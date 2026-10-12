// Host-side unit tests for lib/lock_core (lock_types + lock_state_machine).
// Requirement IDs refer to docs/v1-spec.md; wire values to docs/ble-spec.md v1.1.
//
// Run: pio test -e native -f test_state_machine

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include <deque>

#include "lock_state_machine.h"
#include "lock_types.h"

using lock::CmdResult;
using lock::CommandOutcome;
using lock::kMaxPendingCommands;
using lock::kNone;
using lock::kOpLock;
using lock::kOpUnlock;
using lock::LockState;
using lock::LockStateMachine;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const LockState kAllStates[] = {LockState::Locked, LockState::Unlocking,
                                       LockState::Unlocked, LockState::Locking};

// Drives a fresh machine into `s` using only the public API.
static LockStateMachine machineIn(LockState s) {
  static const uint8_t unlock[] = {kOpUnlock};
  LockStateMachine sm;
  sm.beginBoot();
  if (s != LockState::Locking) sm.onActuationComplete();
  if (s == LockState::Unlocking || s == LockState::Unlocked) sm.handleCommand(unlock, 1);
  if (s == LockState::Unlocked) sm.onActuationComplete();
  TEST_ASSERT_EQUAL_size_t_MESSAGE(0, sm.pendingCount(), "test helper left commands pending");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(static_cast<uint8_t>(s), static_cast<uint8_t>(sm.state()),
                                  "test helper failed to reach the requested state");
  return sm;
}

static void assertState(LockState expected, LockState actual, const char *msg) {
  char buf[160];
  snprintf(buf, sizeof buf, "%s: expected state %s, got %s (%u)", msg, lock::toString(expected),
           lock::toString(actual), static_cast<unsigned>(actual));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(static_cast<uint8_t>(expected), static_cast<uint8_t>(actual), buf);
}

static void assertResult(CmdResult expected, CmdResult actual, const char *msg) {
  char buf[160];
  snprintf(buf, sizeof buf, "%s: expected result %s, got %s (%u)", msg, lock::toString(expected),
           lock::toString(actual), static_cast<unsigned>(actual));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(static_cast<uint8_t>(expected), static_cast<uint8_t>(actual), buf);
}

// Checks one matrix cell: from state `from`, writing data/len must give
// `expectResult`, leave the machine in `expectState`, and fill the outcome
// fields consistently (CMD-001, CMD-004, STATE-002).
static void checkCell(LockState from, const uint8_t *data, size_t len, CmdResult expectResult,
                      LockState expectState, const char *what) {
  LockStateMachine sm = machineIn(from);
  CommandOutcome out = sm.handleCommand(data, len);

  char ctx[160];
  snprintf(ctx, sizeof ctx, "%s from %s", what, lock::toString(from));
  TEST_ASSERT_FALSE_MESSAGE(out.deferred, ctx);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(0, sm.pendingCount(), ctx);
  assertResult(expectResult, out.result, ctx);
  assertState(expectState, sm.state(), ctx);

  // opcode field: data[0], or kNone for an empty write.
  uint8_t expectOpcode = (len > 0 && data != nullptr) ? data[0] : kNone;
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(expectOpcode, out.opcode, ctx);

  // startMove iff Ok.
  TEST_ASSERT_EQUAL_MESSAGE(expectResult == CmdResult::Ok, out.startMove, ctx);
  // target is always a valid settled state; when moving it is the new target.
  TEST_ASSERT_FALSE_MESSAGE(LockStateMachine::isTransitional(out.target), ctx);
  if (out.startMove) {
    assertState(LockStateMachine::targetOf(sm.state()), out.target, ctx);
  }
}

// A valid command during a move is queued: no result, no move, state unchanged.
static void checkDeferredCell(LockState from, const uint8_t *data, const char *what) {
  LockStateMachine sm = machineIn(from);
  CommandOutcome out = sm.handleCommand(data, 1);
  char ctx[160];
  snprintf(ctx, sizeof ctx, "%s from %s", what, lock::toString(from));
  TEST_ASSERT_TRUE_MESSAGE(out.deferred, ctx);
  TEST_ASSERT_FALSE_MESSAGE(out.startMove, ctx);
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(data[0], out.opcode, ctx);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(1, sm.pendingCount(), ctx);
  assertState(from, sm.state(), ctx);
}

// ---------------------------------------------------------------------------
// §5.1 response matrix, every cell (CMD-001, CMD-004, STATE-002, FAIL-005)
// ---------------------------------------------------------------------------

static const uint8_t kUnlock[] = {kOpUnlock};
static const uint8_t kLock[] = {kOpLock};

// Row: UNLOCK (0x01)
void test_matrix_UNLOCK_from_LOCKED_is_OK_and_starts_UNLOCKING() {
  checkCell(LockState::Locked, kUnlock, 1, CmdResult::Ok, LockState::Unlocking, "UNLOCK");
}
void test_matrix_UNLOCK_from_UNLOCKING_is_DEFERRED() {
  checkDeferredCell(LockState::Unlocking, kUnlock, "UNLOCK");
}
void test_matrix_UNLOCK_from_UNLOCKED_is_NO_CHANGE() {
  checkCell(LockState::Unlocked, kUnlock, 1, CmdResult::NoChange, LockState::Unlocked, "UNLOCK");
}
void test_matrix_UNLOCK_from_LOCKING_is_DEFERRED() {
  checkDeferredCell(LockState::Locking, kUnlock, "UNLOCK");
}

// Row: LOCK (0x00)
void test_matrix_LOCK_from_LOCKED_is_NO_CHANGE() {
  checkCell(LockState::Locked, kLock, 1, CmdResult::NoChange, LockState::Locked, "LOCK");
}
void test_matrix_LOCK_from_UNLOCKING_is_DEFERRED() {
  checkDeferredCell(LockState::Unlocking, kLock, "LOCK");
}
void test_matrix_LOCK_from_UNLOCKED_is_OK_and_starts_LOCKING() {
  checkCell(LockState::Unlocked, kLock, 1, CmdResult::Ok, LockState::Locking, "LOCK");
}
void test_matrix_LOCK_from_LOCKING_is_DEFERRED() {
  checkDeferredCell(LockState::Locking, kLock, "LOCK");
}

// Row: any other opcode (0x02..0xFF), every value from every state.
void test_matrix_every_other_1_byte_opcode_is_UNKNOWN_COMMAND_from_every_state() {
  for (LockState s : kAllStates) {
    for (unsigned op = 0x02; op <= 0xFF; ++op) {
      uint8_t b[1] = {static_cast<uint8_t>(op)};
      char what[48];
      snprintf(what, sizeof what, "opcode 0x%02X", op);
      checkCell(s, b, 1, CmdResult::UnknownCommand, s, what);
    }
  }
}

// Row: length != 1 (BLE spec §4.2: empty -> lastOpcode FF; >= 2 -> first byte).
void test_matrix_empty_write_is_MALFORMED_with_opcode_kNone_from_every_state() {
  for (LockState s : kAllStates) {
    uint8_t dummy[1] = {kOpUnlock};
    checkCell(s, dummy, 0, CmdResult::Malformed, s, "empty write (non-null ptr)");
    checkCell(s, nullptr, 0, CmdResult::Malformed, s, "empty write (null ptr)");
  }
}

void test_matrix_multi_byte_write_is_MALFORMED_and_not_executed_from_every_state() {
  // 13 §4: "01 00" must NOT unlock; "00 01" must NOT lock.
  const uint8_t unlockPlus[] = {kOpUnlock, 0x00};
  const uint8_t lockPlus[] = {kOpLock, 0x01};
  const uint8_t longWrite[20] = {kOpUnlock};
  uint8_t big[300];
  memset(big, kOpUnlock, sizeof big);
  for (LockState s : kAllStates) {
    checkCell(s, unlockPlus, 2, CmdResult::Malformed, s, "write 01 00");
    checkCell(s, lockPlus, 2, CmdResult::Malformed, s, "write 00 01");
    checkCell(s, longWrite, sizeof longWrite, CmdResult::Malformed, s, "20-byte write");
    checkCell(s, big, sizeof big, CmdResult::Malformed, s, "300-byte write");
  }
}

// ---------------------------------------------------------------------------
// Check order (header: len -> opcode -> transitional -> no-change -> ok)
// ---------------------------------------------------------------------------

void test_order_2_byte_unknown_opcode_is_MALFORMED_not_UNKNOWN_COMMAND() {
  const uint8_t b[] = {0x07, 0x00};
  for (LockState s : kAllStates) checkCell(s, b, 2, CmdResult::Malformed, s, "write 07 00");
}

void test_order_unknown_opcode_during_transition_is_UNKNOWN_COMMAND_not_deferred() {
  const uint8_t b[] = {0x07};
  checkCell(LockState::Unlocking, b, 1, CmdResult::UnknownCommand, LockState::Unlocking, "07");
  checkCell(LockState::Locking, b, 1, CmdResult::UnknownCommand, LockState::Locking, "07");
}

void test_order_multi_byte_during_transition_is_MALFORMED_not_deferred() {
  const uint8_t lock2[] = {kOpLock, 0x00};
  const uint8_t unlock2[] = {kOpUnlock, 0x00};
  checkCell(LockState::Unlocking, lock2, 2, CmdResult::Malformed, LockState::Unlocking, "00 00");
  checkCell(LockState::Locking, unlock2, 2, CmdResult::Malformed, LockState::Locking, "01 00");
}

void test_order_null_data_with_len_1_is_MALFORMED_with_opcode_kNone() {
  // Defensive: header says data may be null only when len == 0. Must not crash
  // (FAIL-005) and must not be executed.
  for (LockState s : kAllStates) checkCell(s, nullptr, 1, CmdResult::Malformed, s, "null,len=1");
}

// ---------------------------------------------------------------------------
// Opcode field and target field
// ---------------------------------------------------------------------------

void test_outcome_opcode_is_first_byte_of_multi_byte_write() {
  LockStateMachine sm = machineIn(LockState::Unlocked);
  const uint8_t b[] = {0xAB, 0xCD, 0xEF};
  CommandOutcome out = sm.handleCommand(b, 3);
  TEST_ASSERT_EQUAL_HEX8(0xAB, out.opcode);
}

void test_outcome_opcode_0xFF_one_byte_is_UNKNOWN_COMMAND_with_opcode_0xFF() {
  // Note: indistinguishable from kNone on the wire (see report).
  LockStateMachine sm = machineIn(LockState::Locked);
  const uint8_t b[] = {0xFF};
  CommandOutcome out = sm.handleCommand(b, 1);
  assertResult(CmdResult::UnknownCommand, out.result, "0xFF");
  TEST_ASSERT_EQUAL_HEX8(0xFF, out.opcode);
}

void test_outcome_target_is_Unlocked_when_UNLOCK_accepted() {
  LockStateMachine sm = machineIn(LockState::Locked);
  CommandOutcome out = sm.handleCommand(kUnlock, 1);
  TEST_ASSERT_TRUE(out.startMove);
  assertState(LockState::Unlocked, out.target, "UNLOCK target");
}

void test_outcome_target_is_Locked_when_LOCK_accepted() {
  LockStateMachine sm = machineIn(LockState::Unlocked);
  CommandOutcome out = sm.handleCommand(kLock, 1);
  TEST_ASSERT_TRUE(out.startMove);
  assertState(LockState::Locked, out.target, "LOCK target");
}

void test_outcome_startMove_false_when_deferred_or_queue_full() {
  // The control task must not act on a command arriving during a move,
  // whether it is queued or answered BUSY.
  LockStateMachine sm = machineIn(LockState::Unlocking);
  CommandOutcome out = sm.handleCommand(kLock, 1);
  TEST_ASSERT_TRUE(out.deferred);
  TEST_ASSERT_FALSE(out.startMove);
  for (size_t i = 1; i < kMaxPendingCommands; i++) sm.handleCommand(kLock, 1);
  out = sm.handleCommand(kLock, 1);
  TEST_ASSERT_FALSE(out.deferred);
  TEST_ASSERT_FALSE(out.startMove);
  assertResult(CmdResult::Busy, out.result, "queue full");
  assertState(LockState::Unlocked, out.target, "BUSY target stays targetOf(state)");
}

// ---------------------------------------------------------------------------
// Transitions and settling (STATE-002, STATE-003)
// ---------------------------------------------------------------------------

void test_full_cycle_LOCKED_UNLOCKING_UNLOCKED_LOCKING_LOCKED() {
  LockStateMachine sm = machineIn(LockState::Locked);
  assertResult(CmdResult::Ok, sm.handleCommand(kUnlock, 1).result, "unlock");
  assertState(LockState::Unlocking, sm.state(), "after unlock");
  TEST_ASSERT_TRUE(sm.onActuationComplete());
  assertState(LockState::Unlocked, sm.state(), "after settle");
  assertResult(CmdResult::Ok, sm.handleCommand(kLock, 1).result, "lock");
  assertState(LockState::Locking, sm.state(), "after lock");
  TEST_ASSERT_TRUE(sm.onActuationComplete());
  assertState(LockState::Locked, sm.state(), "after settle");
}

void test_onActuationComplete_when_LOCKED_returns_false_and_does_nothing() {
  LockStateMachine sm = machineIn(LockState::Locked);
  TEST_ASSERT_FALSE(sm.onActuationComplete());
  assertState(LockState::Locked, sm.state(), "spurious complete");
}

void test_onActuationComplete_when_UNLOCKED_returns_false_and_does_nothing() {
  LockStateMachine sm = machineIn(LockState::Unlocked);
  TEST_ASSERT_FALSE(sm.onActuationComplete());
  assertState(LockState::Unlocked, sm.state(), "spurious complete");
}

void test_onActuationComplete_twice_second_returns_false() {
  LockStateMachine sm = machineIn(LockState::Locked);
  sm.handleCommand(kUnlock, 1);
  TEST_ASSERT_TRUE(sm.onActuationComplete());
  TEST_ASSERT_FALSE(sm.onActuationComplete());
  assertState(LockState::Unlocked, sm.state(), "double complete");
}

void test_command_during_move_runs_after_settle() {
  // §10 step 6: LOCK during UNLOCKING is queued, then runs once settled.
  LockStateMachine sm = machineIn(LockState::Locked);
  sm.handleCommand(kUnlock, 1);
  TEST_ASSERT_TRUE(sm.handleCommand(kLock, 1).deferred);
  CommandOutcome out;
  TEST_ASSERT_FALSE_MESSAGE(sm.popPending(&out), "popPending during a move");
  TEST_ASSERT_TRUE(sm.onActuationComplete());
  assertState(LockState::Unlocked, sm.state(), "settled before pending runs");
  TEST_ASSERT_TRUE(sm.popPending(&out));
  assertResult(CmdResult::Ok, out.result, "pending lock");
  TEST_ASSERT_TRUE(out.startMove);
  TEST_ASSERT_FALSE(out.deferred);
  TEST_ASSERT_EQUAL_HEX8(kOpLock, out.opcode);
  assertState(LockState::Locked, out.target, "pending lock target");
  assertState(LockState::Locking, sm.state(), "after pending lock");
  TEST_ASSERT_FALSE(sm.popPending(&out));
}

void test_default_constructed_is_LOCKING_and_defers_commands() {
  // Header: before beginBoot the lock is treated as heading to Locked.
  LockStateMachine sm;
  assertState(LockState::Locking, sm.state(), "default");
  TEST_ASSERT_TRUE(sm.handleCommand(kUnlock, 1).deferred);
  TEST_ASSERT_TRUE(sm.onActuationComplete());
  assertState(LockState::Locked, sm.state(), "default settles to Locked");
}

// ---------------------------------------------------------------------------
// Pending-command queue
// ---------------------------------------------------------------------------

void test_pending_runs_FIFO_and_stops_once_a_move_starts() {
  // Moving to Unlocked; queue LOCK, LOCK, UNLOCK, UNLOCK.
  LockStateMachine sm = machineIn(LockState::Unlocking);
  const uint8_t order[] = {kOpLock, kOpLock, kOpUnlock, kOpUnlock};
  for (uint8_t op : order) TEST_ASSERT_TRUE(sm.handleCommand(&op, 1).deferred);
  TEST_ASSERT_EQUAL_size_t(4, sm.pendingCount());
  sm.onActuationComplete();  // Unlocked

  CommandOutcome out;
  TEST_ASSERT_TRUE(sm.popPending(&out));  // LOCK from Unlocked -> Ok
  TEST_ASSERT_EQUAL_HEX8(kOpLock, out.opcode);
  assertResult(CmdResult::Ok, out.result, "1st");
  TEST_ASSERT_FALSE_MESSAGE(sm.popPending(&out), "must stop after a move starts");
  TEST_ASSERT_EQUAL_size_t(3, sm.pendingCount());
  sm.onActuationComplete();  // Locked

  TEST_ASSERT_TRUE(sm.popPending(&out));  // LOCK from Locked -> NoChange
  TEST_ASSERT_EQUAL_HEX8(kOpLock, out.opcode);
  assertResult(CmdResult::NoChange, out.result, "2nd");
  TEST_ASSERT_FALSE(out.startMove);
  assertState(LockState::Locked, sm.state(), "after NoChange");
  TEST_ASSERT_TRUE(sm.popPending(&out));  // UNLOCK -> Ok
  TEST_ASSERT_EQUAL_HEX8(kOpUnlock, out.opcode);
  assertResult(CmdResult::Ok, out.result, "3rd");
  assertState(LockState::Unlocked, out.target, "3rd target");
  TEST_ASSERT_FALSE(sm.popPending(&out));
  sm.onActuationComplete();  // Unlocked

  TEST_ASSERT_TRUE(sm.popPending(&out));  // UNLOCK -> NoChange
  assertResult(CmdResult::NoChange, out.result, "4th");
  TEST_ASSERT_EQUAL_size_t(0, sm.pendingCount());
  TEST_ASSERT_FALSE(sm.popPending(&out));
}

void test_pending_queue_full_answers_BUSY_and_keeps_queued() {
  LockStateMachine sm = machineIn(LockState::Locking);
  for (size_t i = 0; i < kMaxPendingCommands; i++) {
    const uint8_t op = (i % 2) ? kOpLock : kOpUnlock;
    TEST_ASSERT_TRUE(sm.handleCommand(&op, 1).deferred);
  }
  for (int i = 0; i < 3; i++) {
    CommandOutcome out = sm.handleCommand(kUnlock, 1);
    TEST_ASSERT_FALSE(out.deferred);
    TEST_ASSERT_FALSE(out.startMove);
    assertResult(CmdResult::Busy, out.result, "queue full");
    TEST_ASSERT_EQUAL_size_t(kMaxPendingCommands, sm.pendingCount());
  }
  // Running one frees exactly one slot for the next move.
  sm.onActuationComplete();
  CommandOutcome out;
  TEST_ASSERT_TRUE(sm.popPending(&out));  // UNLOCK from Locked -> Ok
  assertResult(CmdResult::Ok, out.result, "first queued");
  TEST_ASSERT_TRUE(sm.handleCommand(kLock, 1).deferred);
  assertResult(CmdResult::Busy, sm.handleCommand(kLock, 1).result, "full again");
}

void test_pending_malformed_and_unknown_stay_immediate_even_when_queue_full() {
  LockStateMachine sm = machineIn(LockState::Unlocking);
  const uint8_t unknown[] = {0x07};
  const uint8_t twoBytes[] = {kOpLock, 0x00};
  for (int round = 0; round < 2; round++) {
    CommandOutcome out = sm.handleCommand(unknown, 1);
    TEST_ASSERT_FALSE(out.deferred);
    assertResult(CmdResult::UnknownCommand, out.result, "unknown during move");
    out = sm.handleCommand(twoBytes, 2);
    TEST_ASSERT_FALSE(out.deferred);
    assertResult(CmdResult::Malformed, out.result, "2 bytes during move");
    out = sm.handleCommand(nullptr, 0);
    TEST_ASSERT_FALSE(out.deferred);
    assertResult(CmdResult::Malformed, out.result, "empty during move");
    TEST_ASSERT_EQUAL_size_t(round == 0 ? 0 : kMaxPendingCommands, sm.pendingCount());
    for (size_t i = 0; i < kMaxPendingCommands; i++) sm.handleCommand(kLock, 1);
  }
}

void test_pending_popPending_false_when_empty_or_null_out() {
  LockStateMachine sm = machineIn(LockState::Locked);
  CommandOutcome out;
  TEST_ASSERT_FALSE(sm.popPending(&out));
  LockStateMachine sm2 = machineIn(LockState::Locking);
  sm2.handleCommand(kUnlock, 1);
  sm2.onActuationComplete();
  TEST_ASSERT_FALSE(sm2.popPending(nullptr));
  TEST_ASSERT_EQUAL_size_t(1, sm2.pendingCount());  // not consumed
}

void test_pending_commands_deferred_during_boot_run_after_boot_settles() {
  // ble-spec §6.3: a client connecting right after power-up meets LOCKING.
  LockStateMachine sm;
  sm.beginBoot();
  TEST_ASSERT_TRUE(sm.handleCommand(kLock, 1).deferred);
  TEST_ASSERT_TRUE(sm.handleCommand(kUnlock, 1).deferred);
  CommandOutcome out;
  TEST_ASSERT_FALSE(sm.popPending(&out));
  TEST_ASSERT_TRUE(sm.onActuationComplete());
  assertState(LockState::Locked, sm.state(), "boot settled");
  TEST_ASSERT_TRUE(sm.popPending(&out));
  assertResult(CmdResult::NoChange, out.result, "LOCK after boot");
  TEST_ASSERT_TRUE(sm.popPending(&out));
  assertResult(CmdResult::Ok, out.result, "UNLOCK after boot");
  assertState(LockState::Unlocking, sm.state(), "unlocking");
  TEST_ASSERT_FALSE(sm.popPending(&out));
}

void test_beginBoot_clears_pending() {
  LockStateMachine sm = machineIn(LockState::Unlocking);
  sm.handleCommand(kLock, 1);
  sm.handleCommand(kUnlock, 1);
  sm.beginBoot();
  TEST_ASSERT_EQUAL_size_t(0, sm.pendingCount());
  sm.onActuationComplete();
  CommandOutcome out;
  TEST_ASSERT_FALSE(sm.popPending(&out));
}

void test_pending_queue_wraps_around_its_ring() {
  // Many fill/drain cycles push the ring indices past their end.
  LockStateMachine sm = machineIn(LockState::Locked);
  for (int cycle = 0; cycle < 50; cycle++) {
    const uint8_t op = (sm.state() == LockState::Locked) ? kOpUnlock : kOpLock;
    assertResult(CmdResult::Ok, sm.handleCommand(&op, 1).result, "start move");
    size_t n = 1 + static_cast<size_t>(cycle) % kMaxPendingCommands;
    for (size_t i = 0; i < n; i++) TEST_ASSERT_TRUE(sm.handleCommand(&op, 1).deferred);
    sm.onActuationComplete();
    CommandOutcome out;
    for (size_t i = 0; i < n; i++) {
      TEST_ASSERT_TRUE(sm.popPending(&out));
      assertResult(CmdResult::NoChange, out.result, "same request again");
      TEST_ASSERT_EQUAL_HEX8(op, out.opcode);
    }
    TEST_ASSERT_FALSE(sm.popPending(&out));
  }
}

// ---------------------------------------------------------------------------
// beginBoot (STATE-005, FAIL-001)
// ---------------------------------------------------------------------------

void test_boot_drives_to_LOCKED_via_LOCKING() {
  LockStateMachine sm;
  sm.beginBoot();
  assertState(LockState::Locking, sm.state(), "after beginBoot");
  TEST_ASSERT_TRUE(sm.onActuationComplete());
  assertState(LockState::Locked, sm.state(), "boot settled");
}

void test_boot_from_every_state_heads_to_LOCKED() {
  // Not expected in production, but must still be well-defined.
  for (LockState s : kAllStates) {
    LockStateMachine sm = machineIn(s);
    sm.beginBoot();
    assertState(LockState::Locking, sm.state(), lock::toString(s));
    TEST_ASSERT_TRUE(sm.onActuationComplete());
    assertState(LockState::Locked, sm.state(), lock::toString(s));
  }
}

// ---------------------------------------------------------------------------
// isTransitional / targetOf
// ---------------------------------------------------------------------------

void test_isTransitional_true_only_for_UNLOCKING_and_LOCKING() {
  TEST_ASSERT_FALSE(LockStateMachine::isTransitional(LockState::Locked));
  TEST_ASSERT_TRUE(LockStateMachine::isTransitional(LockState::Unlocking));
  TEST_ASSERT_FALSE(LockStateMachine::isTransitional(LockState::Unlocked));
  TEST_ASSERT_TRUE(LockStateMachine::isTransitional(LockState::Locking));
}

void test_targetOf_maps_transitional_to_target_and_settled_to_itself() {
  assertState(LockState::Locked, LockStateMachine::targetOf(LockState::Locked), "Locked");
  assertState(LockState::Unlocked, LockStateMachine::targetOf(LockState::Unlocking), "Unlocking");
  assertState(LockState::Unlocked, LockStateMachine::targetOf(LockState::Unlocked), "Unlocked");
  assertState(LockState::Locked, LockStateMachine::targetOf(LockState::Locking), "Locking");
}

// ---------------------------------------------------------------------------
// Wire values never renumbered (ble-spec §4.4, §6.2)
// ---------------------------------------------------------------------------

void test_wire_values_of_states_results_opcodes_and_kNone() {
  TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(LockState::Locked));
  TEST_ASSERT_EQUAL_UINT8(1, static_cast<uint8_t>(LockState::Unlocking));
  TEST_ASSERT_EQUAL_UINT8(2, static_cast<uint8_t>(LockState::Unlocked));
  TEST_ASSERT_EQUAL_UINT8(3, static_cast<uint8_t>(LockState::Locking));
  TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(CmdResult::Ok));
  TEST_ASSERT_EQUAL_UINT8(1, static_cast<uint8_t>(CmdResult::NoChange));
  TEST_ASSERT_EQUAL_UINT8(2, static_cast<uint8_t>(CmdResult::Busy));
  TEST_ASSERT_EQUAL_UINT8(3, static_cast<uint8_t>(CmdResult::UnknownCommand));
  TEST_ASSERT_EQUAL_UINT8(4, static_cast<uint8_t>(CmdResult::Malformed));
  TEST_ASSERT_EQUAL_HEX8(0x00, kOpLock);
  TEST_ASSERT_EQUAL_HEX8(0x01, kOpUnlock);
  TEST_ASSERT_EQUAL_HEX8(0xFF, kNone);
}

// ---------------------------------------------------------------------------
// toString (DIAG-001)
// ---------------------------------------------------------------------------

void test_toString_LockState_all_values() {
  TEST_ASSERT_EQUAL_STRING("LOCKED", lock::toString(LockState::Locked));
  TEST_ASSERT_EQUAL_STRING("UNLOCKING", lock::toString(LockState::Unlocking));
  TEST_ASSERT_EQUAL_STRING("UNLOCKED", lock::toString(LockState::Unlocked));
  TEST_ASSERT_EQUAL_STRING("LOCKING", lock::toString(LockState::Locking));
}

void test_toString_LockState_out_of_range_is_question_mark() {
  const uint8_t bad[] = {4, 5, 0x7F, 0x80, 0xFE, 0xFF};
  for (uint8_t v : bad) {
    TEST_ASSERT_EQUAL_STRING("?", lock::toString(static_cast<LockState>(v)));
  }
}

void test_toString_CmdResult_all_values() {
  TEST_ASSERT_EQUAL_STRING("OK", lock::toString(CmdResult::Ok));
  TEST_ASSERT_EQUAL_STRING("NO_CHANGE", lock::toString(CmdResult::NoChange));
  TEST_ASSERT_EQUAL_STRING("BUSY", lock::toString(CmdResult::Busy));
  TEST_ASSERT_EQUAL_STRING("UNKNOWN_COMMAND", lock::toString(CmdResult::UnknownCommand));
  TEST_ASSERT_EQUAL_STRING("MALFORMED", lock::toString(CmdResult::Malformed));
}

void test_toString_CmdResult_out_of_range_is_question_mark() {
  const uint8_t bad[] = {5, 6, 0x7F, 0x80, 0xFE, 0xFF};
  for (uint8_t v : bad) {
    TEST_ASSERT_EQUAL_STRING("?", lock::toString(static_cast<CmdResult>(v)));
  }
}

// ---------------------------------------------------------------------------
// Property: random commands interleaved with random completes
// (STATE-001, STATE-002, CMD-001, FAIL-005)
// ---------------------------------------------------------------------------

// Deterministic xorshift32 so failures reproduce exactly.
static uint32_t rngState;
static uint32_t rng() {
  uint32_t x = rngState;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return rngState = x;
}

// Independent reference model of §5.1 for a settled state.
static CmdResult modelSettled(LockState s, uint8_t op) {
  LockState want = (op == kOpUnlock) ? LockState::Unlocked : LockState::Locked;
  return (s == want) ? CmdResult::NoChange : CmdResult::Ok;
}

static bool isOneOfFourStates(LockState s) {
  return s == LockState::Locked || s == LockState::Unlocking || s == LockState::Unlocked ||
         s == LockState::Locking;
}

// Allowed edges from §4.1 (self-loops allowed: a rejected command changes nothing).
static bool edgeAllowed(LockState from, LockState to) {
  if (from == to) return true;
  return (from == LockState::Locked && to == LockState::Unlocking) ||
         (from == LockState::Unlocking && to == LockState::Unlocked) ||
         (from == LockState::Unlocked && to == LockState::Locking) ||
         (from == LockState::Locking && to == LockState::Locked);
}

static LockState moveFor(uint8_t op) {
  return (op == kOpUnlock) ? LockState::Unlocking : LockState::Locking;
}

static void checkOutcome(const CommandOutcome &out, CmdResult expect, uint8_t op, LockState model,
                         const char *ctx) {
  assertResult(expect, out.result, ctx);
  TEST_ASSERT_FALSE_MESSAGE(out.deferred, ctx);
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(op, out.opcode, ctx);
  TEST_ASSERT_EQUAL_MESSAGE(expect == CmdResult::Ok, out.startMove, ctx);
  TEST_ASSERT_FALSE_MESSAGE(LockStateMachine::isTransitional(out.target), ctx);
  if (out.startMove) assertState(LockStateMachine::targetOf(model), out.target, ctx);
}

// Follows the header's call pattern (every settle is followed by a popPending
// drain). The model is the state plus a FIFO of deferred opcodes.
void test_property_random_sequences_follow_matrix_and_queue_model() {
  const uint32_t seeds[] = {0x1u, 0xC0FFEEu, 0xDEADBEEFu, 0x12345678u, 0xA5A5A5A5u};
  for (uint32_t seed : seeds) {
    rngState = seed;
    LockStateMachine sm;
    sm.beginBoot();
    LockState model = LockState::Locking;
    std::deque<uint8_t> queue;
    size_t moves = 0, deferrals = 0, busies = 0, pops = 0;

    for (int step = 0; step < 20000; ++step) {
      char ctx[96];
      snprintf(ctx, sizeof ctx, "seed 0x%08X step %d", static_cast<unsigned>(seed), step);
      LockState before = sm.state();
      if (rng() % 8 < 2) {
        bool moved = sm.onActuationComplete();
        TEST_ASSERT_EQUAL_MESSAGE(LockStateMachine::isTransitional(model), moved, ctx);
        model = LockStateMachine::targetOf(model);
        assertState(model, sm.state(), ctx);
        TEST_ASSERT_TRUE_MESSAGE(edgeAllowed(before, sm.state()), ctx);
        CommandOutcome out;
        while (true) {
          LockState prev = sm.state();
          bool got = sm.popPending(&out);
          bool expectGot = !LockStateMachine::isTransitional(model) && !queue.empty();
          TEST_ASSERT_EQUAL_MESSAGE(expectGot, got, ctx);
          if (!got) break;
          uint8_t op = queue.front();
          queue.pop_front();
          CmdResult expect = modelSettled(model, op);
          if (expect == CmdResult::Ok) {
            model = moveFor(op);
            moves++;
          }
          checkOutcome(out, expect, op, model, ctx);
          assertState(model, sm.state(), ctx);
          TEST_ASSERT_TRUE_MESSAGE(edgeAllowed(prev, sm.state()), ctx);
          pops++;
        }
      } else {
        uint8_t buf[4];
        size_t lenPick = rng() % 10;  // mostly 1-byte writes
        size_t len = (lenPick < 7) ? 1 : (lenPick == 7 ? 0 : (lenPick == 8 ? 2 : 4));
        for (size_t i = 0; i < sizeof buf; ++i) {
          uint32_t r = rng() % 8;
          buf[i] = (r < 3) ? kOpLock : (r < 6) ? kOpUnlock : static_cast<uint8_t>(rng());
        }
        const uint8_t *data = (len == 0 && (rng() & 1)) ? nullptr : buf;
        CommandOutcome out = sm.handleCommand(data, len);
        uint8_t op = len > 0 ? buf[0] : kNone;
        if (len != 1) {
          checkOutcome(out, CmdResult::Malformed, op, model, ctx);
        } else if (op != kOpLock && op != kOpUnlock) {
          checkOutcome(out, CmdResult::UnknownCommand, op, model, ctx);
        } else if (LockStateMachine::isTransitional(model)) {
          if (queue.size() < kMaxPendingCommands) {
            TEST_ASSERT_TRUE_MESSAGE(out.deferred, ctx);
            TEST_ASSERT_FALSE_MESSAGE(out.startMove, ctx);
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(op, out.opcode, ctx);
            queue.push_back(op);
            deferrals++;
          } else {
            checkOutcome(out, CmdResult::Busy, op, model, ctx);
            busies++;
          }
        } else {
          CmdResult expect = modelSettled(model, op);
          if (expect == CmdResult::Ok) {
            model = moveFor(op);
            moves++;
          }
          checkOutcome(out, expect, op, model, ctx);
        }
        TEST_ASSERT_TRUE_MESSAGE(edgeAllowed(before, sm.state()), ctx);
      }
      TEST_ASSERT_TRUE_MESSAGE(isOneOfFourStates(sm.state()), ctx);
      assertState(model, sm.state(), ctx);
      TEST_ASSERT_EQUAL_size_t_MESSAGE(queue.size(), sm.pendingCount(), ctx);
      // With the call pattern followed, nothing waits while the lock is settled.
      if (!LockStateMachine::isTransitional(model)) TEST_ASSERT_TRUE_MESSAGE(queue.empty(), ctx);
    }
    // The run must reach every branch, or it proves little.
    TEST_ASSERT_TRUE(moves > 100);
    TEST_ASSERT_TRUE(deferrals > 100);
    TEST_ASSERT_TRUE(busies > 10);
    TEST_ASSERT_TRUE(pops > 100);
  }
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_matrix_UNLOCK_from_LOCKED_is_OK_and_starts_UNLOCKING);
  RUN_TEST(test_matrix_UNLOCK_from_UNLOCKING_is_DEFERRED);
  RUN_TEST(test_matrix_UNLOCK_from_UNLOCKED_is_NO_CHANGE);
  RUN_TEST(test_matrix_UNLOCK_from_LOCKING_is_DEFERRED);
  RUN_TEST(test_matrix_LOCK_from_LOCKED_is_NO_CHANGE);
  RUN_TEST(test_matrix_LOCK_from_UNLOCKING_is_DEFERRED);
  RUN_TEST(test_matrix_LOCK_from_UNLOCKED_is_OK_and_starts_LOCKING);
  RUN_TEST(test_matrix_LOCK_from_LOCKING_is_DEFERRED);
  RUN_TEST(test_matrix_every_other_1_byte_opcode_is_UNKNOWN_COMMAND_from_every_state);
  RUN_TEST(test_matrix_empty_write_is_MALFORMED_with_opcode_kNone_from_every_state);
  RUN_TEST(test_matrix_multi_byte_write_is_MALFORMED_and_not_executed_from_every_state);
  RUN_TEST(test_order_2_byte_unknown_opcode_is_MALFORMED_not_UNKNOWN_COMMAND);
  RUN_TEST(test_order_unknown_opcode_during_transition_is_UNKNOWN_COMMAND_not_deferred);
  RUN_TEST(test_order_multi_byte_during_transition_is_MALFORMED_not_deferred);
  RUN_TEST(test_order_null_data_with_len_1_is_MALFORMED_with_opcode_kNone);
  RUN_TEST(test_outcome_opcode_is_first_byte_of_multi_byte_write);
  RUN_TEST(test_outcome_opcode_0xFF_one_byte_is_UNKNOWN_COMMAND_with_opcode_0xFF);
  RUN_TEST(test_outcome_target_is_Unlocked_when_UNLOCK_accepted);
  RUN_TEST(test_outcome_target_is_Locked_when_LOCK_accepted);
  RUN_TEST(test_outcome_startMove_false_when_deferred_or_queue_full);
  RUN_TEST(test_full_cycle_LOCKED_UNLOCKING_UNLOCKED_LOCKING_LOCKED);
  RUN_TEST(test_onActuationComplete_when_LOCKED_returns_false_and_does_nothing);
  RUN_TEST(test_onActuationComplete_when_UNLOCKED_returns_false_and_does_nothing);
  RUN_TEST(test_onActuationComplete_twice_second_returns_false);
  RUN_TEST(test_command_during_move_runs_after_settle);
  RUN_TEST(test_default_constructed_is_LOCKING_and_defers_commands);
  RUN_TEST(test_pending_runs_FIFO_and_stops_once_a_move_starts);
  RUN_TEST(test_pending_queue_full_answers_BUSY_and_keeps_queued);
  RUN_TEST(test_pending_malformed_and_unknown_stay_immediate_even_when_queue_full);
  RUN_TEST(test_pending_popPending_false_when_empty_or_null_out);
  RUN_TEST(test_pending_commands_deferred_during_boot_run_after_boot_settles);
  RUN_TEST(test_beginBoot_clears_pending);
  RUN_TEST(test_pending_queue_wraps_around_its_ring);
  RUN_TEST(test_boot_drives_to_LOCKED_via_LOCKING);
  RUN_TEST(test_boot_from_every_state_heads_to_LOCKED);
  RUN_TEST(test_isTransitional_true_only_for_UNLOCKING_and_LOCKING);
  RUN_TEST(test_targetOf_maps_transitional_to_target_and_settled_to_itself);
  RUN_TEST(test_wire_values_of_states_results_opcodes_and_kNone);
  RUN_TEST(test_toString_LockState_all_values);
  RUN_TEST(test_toString_LockState_out_of_range_is_question_mark);
  RUN_TEST(test_toString_CmdResult_all_values);
  RUN_TEST(test_toString_CmdResult_out_of_range_is_question_mark);
  RUN_TEST(test_property_random_sequences_follow_matrix_and_queue_model);
  return UNITY_END();
}
