#pragma once
// The control task: sole owner of the state machine, the event log and the
// actuator (CMD-002, PWR-003). Everything else talks to it
// through the non-blocking post*() calls below.

#include <esp_system.h>
#include <stddef.h>
#include <stdint.h>

#include "event_log.h"
#include "lock_protocol.h"
#include "lock_types.h"

namespace control {

// ---- Boot sequence (setup(), in this order) ---------------------------------
// Creates the event queue and log mutex, mounts log storage and recovers the
// log (EventLog::begin). Also initializes the actuator.
bool init(uint32_t bootCount);
// STATE-005: the lock always locks on boot. Enters Locking, starts the
// actuator toward Locked, then logs Boot + StateChange. Returns Locked.
lock::LockState boot(esp_reset_reason_t reason);
// Creates the control task (subscribed to the task watchdog).
bool start();

// ---- Producers (any task; never block) --------------------------------------
// BLE command write (CMD-003). Never dropped: if the queue is full the command
// is parked and answered BUSY by the control task (CMD-005).
void postCommand(const uint8_t *data, size_t len);
// `accepted`: this link became the active client (ble_service decides; a
// second client while one is active is refused and disconnected there).
void postConnect(uint16_t connId, bool accepted);
// `wasActive`: the active client's link went down (not a refused one's).
void postDisconnect(uint16_t connId, uint16_t hciReason, bool wasActive);

// ---- Readers (any task) -----------------------------------------------------
// EventLog::readFrom under the log mutex. Safe while the control task appends.
size_t readLog(uint32_t cursor, lock::log::LogEntry *out, size_t maxEntries);
// Current encoded status (used for the characteristic's initial value).
void currentStatus(uint8_t out[lock::proto::kStatusLen]);

}  // namespace control
