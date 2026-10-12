#include "control.h"

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>

#include "actuator.h"
#include "app_config.h"
#include "ble_service.h"
#include "diagnostics.h"
#include "lock_state_machine.h"
#include "log_storage_fs.h"

using lock::CmdResult;
using lock::LockState;
using lock::LockStateMachine;
using lock::log::EventType;

namespace control {
namespace {

enum class EvType : uint8_t {
  Command,
  Connected,
  Disconnected,
  ActuationDone,
};

struct Event {
  EvType type;
  uint8_t opcode;   // Command: data[0], or kNone for an empty write
  uint8_t target;   // ActuationDone: LockState reached
  uint16_t len;     // Command: write length (capped)
  uint16_t connId;  // Connected / Disconnected
  uint16_t reason;  // Disconnected: HCI reason
  bool flag;        // Connected: accepted as active client; Disconnected: was the active one
  uint32_t ms;      // when it happened (millis)
};

// Commands that found the queue full (CMD-005); answered BUSY in order.
struct Parked {
  uint8_t opcode;
  uint8_t len;
};

QueueHandle_t g_queue = nullptr;
TaskHandle_t g_task = nullptr;

// Guards g_log. Holders only touch RAM (LogStorageFs defers flash writes to
// flush()), so the BLE task never waits on flash while reading the log.
SemaphoreHandle_t g_logMutex = nullptr;
LogStorageFs g_storage;
lock::log::EventLog g_log(g_storage);

LockStateMachine g_sm;  // control task only (and setup() before it starts)

portMUX_TYPE g_statusMux = portMUX_INITIALIZER_UNLOCKED;
lock::proto::Status g_status = {LockState::Locking, lock::kNone, lock::kNone, 0};

portMUX_TYPE g_parkMux = portMUX_INITIALIZER_UNLOCKED;
Parked g_parked[cfg::kOverflowLen];
size_t g_parkHead = 0;
size_t g_parkCount = 0;
uint32_t g_parkLost = 0;  // beyond the parking area: still answered BUSY, opcode unknown

// Backstops for events that could not be queued (should never happen with
// the reserved slots, but a lost actuation-done would wedge the lock).
std::atomic<int> g_pendingDone{-1};
std::atomic<uint32_t> g_droppedEvents{0};

// Connection tracking: control task only.
bool g_connected = false;
uint16_t g_connId = 0;
uint32_t g_lastCmdMs = 0;

// ---- helpers -------------------------------------------------------------------

void logEvent(EventType type, const uint8_t *data, size_t len) {
  xSemaphoreTake(g_logMutex, portMAX_DELAY);
  g_log.append(type, millis(), data, len);
  xSemaphoreGive(g_logMutex);
}

void flushLog() {
  // LOG-005: runs after the actuator was started, never before.
  // This task is the only appender, so everything up to upTo is covered by
  // the flush below; it becomes readable over BLE only once it is on flash.
  static bool lastOk = true;
  xSemaphoreTake(g_logMutex, portMAX_DELAY);
  uint32_t upTo = g_log.nextSeq() - 1;
  xSemaphoreGive(g_logMutex);

  bool ok = g_storage.flush();
  if (ok) {
    xSemaphoreTake(g_logMutex, portMAX_DELAY);
    g_log.markDurable(upTo);
    xSemaphoreGive(g_logMutex);
  }
  if (ok != lastOk) {  // report transitions only; flush runs every loop
    if (ok) {
      diag::logf(diag::kLog, "flash writes recovered");
    } else {
      diag::logf(diag::kLog, "flash write failed; new entries not readable until flushed");
    }
    lastOk = ok;
  }
}

void logStateChange(uint8_t from, LockState to) {
  uint8_t d[2] = {from, static_cast<uint8_t>(to)};
  logEvent(EventType::StateChange, d, sizeof(d));
}

void logCommandResult(uint8_t opcode, CmdResult r, uint16_t len) {
  uint8_t d[3] = {opcode, static_cast<uint8_t>(r), static_cast<uint8_t>(len > 255 ? 255 : len)};
  logEvent(EventType::CommandResult, d, sizeof(d));
}

void logConn(EventType type, uint16_t connId, uint16_t reason, bool withReason) {
  uint8_t d[4] = {static_cast<uint8_t>(connId), static_cast<uint8_t>(connId >> 8),
                  static_cast<uint8_t>(reason), static_cast<uint8_t>(reason >> 8)};
  logEvent(type, d, withReason ? 4 : 2);
}

// Updates the status value (always) and notifies (when asked).
void publishStatus(bool notify) {
  uint8_t buf[lock::proto::kStatusLen];
  portENTER_CRITICAL(&g_statusMux);
  g_status.state = g_sm.state();
  lock::proto::encodeStatus(g_status, buf);
  portEXIT_CRITICAL(&g_statusMux);
  ble::publishStatus(buf, notify);
}

void recordResult(uint8_t opcode, CmdResult r) {
  portENTER_CRITICAL(&g_statusMux);
  g_status.lastOpcode = opcode;
  g_status.lastResult = static_cast<uint8_t>(r);
  g_status.resultSeq++;  // wraps
  portEXIT_CRITICAL(&g_statusMux);
}

void onActuatorDone(LockState reached) {
  // esp_timer task: must not block.
  Event ev = {};
  ev.type = EvType::ActuationDone;
  ev.target = static_cast<uint8_t>(reached);
  ev.ms = millis();
  if (xQueueSend(g_queue, &ev, 0) != pdTRUE) {
    g_pendingDone.store(static_cast<int>(reached));
  }
}

// ---- event handlers (control task) ---------------------------------------------

// Reports one command result, immediate or popped from the pending queue:
// start the actuator, notify (resultSeq+1 with the state after it), then log
// CommandResult and, if it started a move, StateChange.
void finishCommand(const lock::CommandOutcome &out, uint16_t len, LockState before) {
  if (out.startMove) {
    act::instance().startMove(out.target);
    diag::logf(diag::kAct, "moving to %s", lock::toString(out.target));
  }

  // One notification carries the result and (if accepted) the new state.
  recordResult(out.opcode, out.result);
  publishStatus(true);

  diag::logf(diag::kCmd, "op 0x%02X len %u -> %s", out.opcode, len, lock::toString(out.result));
  logCommandResult(out.opcode, out.result, len);
  if (out.startMove) {
    diag::logf(diag::kState, "%s -> %s", lock::toString(before), lock::toString(g_sm.state()));
    logStateChange(static_cast<uint8_t>(before), g_sm.state());
  }
}

void handleCommand(uint8_t opcode, uint16_t len, uint32_t ms) {
  // BLE-005: only commands reset the idle timer (any write, valid or not).
  if (g_connected) g_lastCmdMs = ms;

  LockState before = g_sm.state();
  uint8_t byte0 = opcode;
  lock::CommandOutcome out = g_sm.handleCommand(len > 0 ? &byte0 : nullptr, len);

  if (out.deferred) {
    // Queued behind the current move: no notification and no log entry
    // until it is popped after the settle.
    diag::logf(diag::kCmd, "op 0x%02X len %u -> queued (%u pending)", out.opcode, len,
               static_cast<unsigned>(g_sm.pendingCount()));
    return;
  }
  finishCommand(out, len, before);
}

void answerBusy(uint8_t opcode, uint16_t len) {
  if (g_connected) g_lastCmdMs = millis();
  recordResult(opcode, CmdResult::Busy);
  publishStatus(true);
  diag::logf(diag::kCmd, "op 0x%02X len %u -> BUSY (queue full)", opcode, len);
  logCommandResult(opcode, CmdResult::Busy, len);
}

void handleSettle(LockState reached) {
  LockState before = g_sm.state();
  if (!LockStateMachine::isTransitional(before) || LockStateMachine::targetOf(before) != reached) {
    // Stale completion from an abandoned move; the state machine decides.
    diag::logf(diag::kAct, "ignoring completion at %s (state %s)", lock::toString(reached),
               lock::toString(before));
    return;
  }
  g_sm.onActuationComplete();
  publishStatus(true);  // resultSeq unchanged: this is a state change, not a result
  diag::logf(diag::kAct, "reached %s", lock::toString(reached));
  diag::logf(diag::kState, "%s -> %s", lock::toString(before), lock::toString(g_sm.state()));
  logStateChange(static_cast<uint8_t>(before), g_sm.state());

  // Commands queued during the move, oldest first, one result each.
  // popPending() stops after one that starts a move: at most one in flight.
  lock::CommandOutcome o;
  LockState prev = g_sm.state();
  while (g_sm.popPending(&o)) {
    finishCommand(o, 1, prev);  // only valid 1-byte commands are ever queued
    prev = g_sm.state();
  }
}

void handleConnect(uint16_t connId, bool accepted, uint32_t ms) {
  if (!accepted) {
    // One client at a time. ble_service already refused and disconnected the
    // newcomer; the active client's id and idle timer are untouched.
    diag::logf(diag::kBle, "second client (conn %u) refused", connId);
    logConn(EventType::Connect, connId, 0, false);
    return;
  }
  g_connected = true;
  g_connId = connId;
  g_lastCmdMs = ms;
  diag::logf(diag::kBle, "client connected (conn %u)", connId);
  logConn(EventType::Connect, connId, 0, false);
}

void handleDisconnect(uint16_t connId, uint16_t reason, bool wasActive) {
  if (wasActive) {
    if (g_connected && connId == g_connId) g_connected = false;
    diag::logf(diag::kBle, "client disconnected (conn %u, reason 0x%02X); advertising", connId,
               reason);
  } else {
    diag::logf(diag::kBle, "refused client disconnected (conn %u, reason 0x%02X)", connId, reason);
  }
  logConn(EventType::Disconnect, connId, reason, true);
}

void checkIdle() {
  if (!g_connected) return;
  uint32_t now = millis();
  if (now - g_lastCmdMs < cfg::kIdleTimeoutMs) return;
  diag::logf(diag::kBle, "idle %lu ms, disconnecting conn %u",
             static_cast<unsigned long>(now - g_lastCmdMs), g_connId);
  logConn(EventType::IdleTimeout, g_connId, 0, false);
  ble::disconnect(g_connId);
  // If the link somehow survives, try again after another T_idle.
  g_lastCmdMs = now;
}

// Answer parked commands, oldest first. Only called with the queue empty, so
// every command queued before them has already been answered.
void drainParked() {
  for (;;) {
    Parked p;
    bool have = false;
    uint32_t lost = 0;
    portENTER_CRITICAL(&g_parkMux);
    if (g_parkCount > 0) {
      p = g_parked[g_parkHead];
      g_parkHead = (g_parkHead + 1) % cfg::kOverflowLen;
      g_parkCount--;
      have = true;
    } else if (g_parkLost > 0) {
      g_parkLost--;
      lost = 1;
    }
    portEXIT_CRITICAL(&g_parkMux);
    if (have) {
      answerBusy(p.opcode, p.len);
    } else if (lost) {
      answerBusy(lock::kNone, 0);  // details lost, but the result is still reported
    } else {
      return;
    }
  }
}

TickType_t nextWait() {
  uint32_t wait = cfg::kControlMaxWaitMs;
  if (g_connected) {
    uint32_t idle = millis() - g_lastCmdMs;
    uint32_t left = idle >= cfg::kIdleTimeoutMs ? 0 : cfg::kIdleTimeoutMs - idle;
    if (left < wait) wait = left;
  }
  return pdMS_TO_TICKS(wait);
}

void dispatch(const Event &ev) {
  switch (ev.type) {
    case EvType::Command:
      handleCommand(ev.opcode, ev.len, ev.ms);
      break;
    case EvType::Connected:
      handleConnect(ev.connId, ev.flag, ev.ms);
      break;
    case EvType::Disconnected:
      handleDisconnect(ev.connId, ev.reason, ev.flag);
      break;
    case EvType::ActuationDone:
      handleSettle(static_cast<LockState>(ev.target));
      break;
  }
}

void controlTask(void *) {
  // FAIL-002: a hung loop stops feeding the TWDT and the chip resets.
  if (esp_task_wdt_add(nullptr) != ESP_OK) {
    diag::logf(diag::kBoot, "WARNING: control task not on watchdog");
  }
  for (;;) {
    esp_task_wdt_reset();
    Event ev;
    // PWR-003: block on the queue; the timeout doubles as the idle-timer tick.
    if (xQueueReceive(g_queue, &ev, nextWait()) == pdTRUE) dispatch(ev);

    int done = g_pendingDone.exchange(-1);
    if (done >= 0) handleSettle(static_cast<LockState>(done));
    if (uxQueueMessagesWaiting(g_queue) == 0) drainParked();
    checkIdle();

    uint32_t dropped = g_droppedEvents.exchange(0);
    if (dropped) diag::logf(diag::kBle, "WARNING: %lu connection events dropped (queue full)",
                            static_cast<unsigned long>(dropped));
    flushLog();
  }
}

}  // namespace

// ---- public API ------------------------------------------------------------------

bool init(uint32_t bootCount) {
  g_queue = xQueueCreate(cfg::kEventQueueLen, sizeof(Event));
  g_logMutex = xSemaphoreCreateMutex();
  if (!g_queue || !g_logMutex) return false;

  g_storage.begin();  // failure is logged; the log then runs RAM-only
  g_log.begin(static_cast<uint16_t>(bootCount));
  diag::logf(diag::kLog, "event log ready, next seq %lu, %u entries",
             static_cast<unsigned long>(g_log.nextSeq()), static_cast<unsigned>(g_log.size()));

  if (!act::instance().begin(&onActuatorDone)) {
    diag::logf(diag::kAct, "%s actuator init failed", act::instance().name());
    return false;
  }
  return true;
}

LockState boot(esp_reset_reason_t reason) {
  constexpr LockState target = LockState::Locked;  // STATE-005: always lock on boot
  g_sm.beginBoot();
  act::instance().startMove(target);
  diag::logf(diag::kAct, "%s: boot, locking", act::instance().name());

  portENTER_CRITICAL(&g_statusMux);
  g_status.state = g_sm.state();
  portEXIT_CRITICAL(&g_statusMux);

  // data[1] is always Locked, data[2] reserved (0): see event_log.h.
  uint8_t boot[3] = {static_cast<uint8_t>(reason), static_cast<uint8_t>(target), 0};
  logEvent(EventType::Boot, boot, sizeof(boot));
  // No prior state exists at boot; kNone marks "from: boot".
  logStateChange(lock::kNone, g_sm.state());
  diag::logf(diag::kState, "BOOT -> %s", lock::toString(g_sm.state()));
  flushLog();
  return target;
}

bool start() {
  BaseType_t ok = xTaskCreatePinnedToCore(controlTask, "control", cfg::kControlStackBytes, nullptr,
                                          cfg::kControlPriority, &g_task, cfg::kControlCore);
  return ok == pdPASS;
}

void postCommand(const uint8_t *data, size_t len) {
  Event ev = {};
  ev.type = EvType::Command;
  ev.opcode = (len > 0 && data) ? data[0] : lock::kNone;
  ev.len = static_cast<uint16_t>(len > 0xFFFF ? 0xFFFF : len);
  ev.ms = millis();

  // Once anything is parked, later commands park too, so answers stay in order.
  portENTER_CRITICAL(&g_parkMux);
  bool parking = (g_parkCount > 0 || g_parkLost > 0);
  portEXIT_CRITICAL(&g_parkMux);

  if (!parking && uxQueueSpacesAvailable(g_queue) > cfg::kEventQueueReserve &&
      xQueueSend(g_queue, &ev, 0) == pdTRUE) {
    return;
  }

  portENTER_CRITICAL(&g_parkMux);
  if (g_parkCount < cfg::kOverflowLen) {
    size_t idx = (g_parkHead + g_parkCount) % cfg::kOverflowLen;
    g_parked[idx].opcode = ev.opcode;
    g_parked[idx].len = static_cast<uint8_t>(len > 255 ? 255 : len);
    g_parkCount++;
  } else {
    g_parkLost++;
  }
  portEXIT_CRITICAL(&g_parkMux);
}

void postConnect(uint16_t connId, bool accepted) {
  Event ev = {};
  ev.type = EvType::Connected;
  ev.connId = connId;
  ev.flag = accepted;
  ev.ms = millis();
  if (xQueueSend(g_queue, &ev, 0) != pdTRUE) g_droppedEvents.fetch_add(1);
}

void postDisconnect(uint16_t connId, uint16_t hciReason, bool wasActive) {
  Event ev = {};
  ev.type = EvType::Disconnected;
  ev.connId = connId;
  ev.flag = wasActive;
  ev.reason = hciReason;
  ev.ms = millis();
  if (xQueueSend(g_queue, &ev, 0) != pdTRUE) g_droppedEvents.fetch_add(1);
}

size_t readLog(uint32_t cursor, lock::log::LogEntry *out, size_t maxEntries) {
  // Held only around RAM work by both sides, so this wait is microseconds.
  xSemaphoreTake(g_logMutex, portMAX_DELAY);
  size_t n = g_log.readFrom(cursor, out, maxEntries);
  xSemaphoreGive(g_logMutex);
  return n;
}

void currentStatus(uint8_t out[lock::proto::kStatusLen]) {
  portENTER_CRITICAL(&g_statusMux);
  lock::proto::encodeStatus(g_status, out);
  portEXIT_CRITICAL(&g_statusMux);
}

}  // namespace control
