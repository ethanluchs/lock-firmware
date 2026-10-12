#pragma once
// Lock state machine and command response matrix (docs/v1-spec.md §4, §5.1).
//
// Pure logic: it decides, it does not act. The caller (control task) owns the
// actuator. Required call pattern:
//   boot:      sm.beginBoot(); actuator.startMove(Locked)
//   command:   o = sm.handleCommand(data, len)
//              if o.deferred: nothing to report yet (result comes later)
//              else: report o.result; if o.startMove: actuator.startMove(o.target)
//   settled:   sm.onActuationComplete(); report the settle
//              while (sm.popPending(&o)): report o.result; if o.startMove: move
// popPending() stops returning commands as soon as one starts a move, so the
// loop always leaves at most one move in flight.

#include <stddef.h>
#include <stdint.h>

#include "lock_types.h"

namespace lock {

// Valid commands that arrive during a move wait here and run, in order, after
// it settles. Beyond this many waiting, new ones are answered Busy.
constexpr size_t kMaxPendingCommands = 4;

struct CommandOutcome {
  CmdResult result;  // meaningless when deferred
  uint8_t opcode;    // data[0], or kNone when len == 0
  bool deferred;     // queued behind the current move; result comes from popPending()
  bool startMove;    // true iff result == Ok and !deferred
  LockState target;  // Locked or Unlocked; meaningful only when startMove
};

class LockStateMachine {
 public:
  // Boot (STATE-005): the lock always drives to Locked on power-up, whatever
  // it was doing before. Enters Locking; the caller starts the move to Locked.
  // Clears any pending commands.
  void beginBoot();

  // Applies the response matrix, checked in this order:
  //   len != 1                       -> Malformed       (any state)
  //   opcode not LOCK/UNLOCK         -> UnknownCommand  (any state)
  //   transitional, queue has room   -> deferred (no result yet)
  //   transitional, queue full       -> Busy
  //   already in requested state     -> NoChange
  //   otherwise                      -> Ok, state becomes Unlocking/Locking
  // `data` may be null when len == 0.
  CommandOutcome handleCommand(const uint8_t *data, size_t len);

  // Actuator finished. If transitional, settles into the target state and
  // returns true. Otherwise does nothing and returns false. Does not touch the
  // pending queue; call popPending() afterwards.
  bool onActuationComplete();

  // If the lock is settled and a command is pending, removes the oldest,
  // applies the matrix to it (result NoChange or Ok, never deferred), writes
  // the outcome and returns true. Returns false while a move is in progress or
  // when nothing is pending.
  bool popPending(CommandOutcome *out);

  size_t pendingCount() const { return pendingCount_; }
  LockState state() const { return state_; }

  static bool isTransitional(LockState s);
  // Target of a transitional state (Unlocking -> Unlocked, Locking -> Locked).
  // Settled states return themselves.
  static LockState targetOf(LockState s);

 private:
  CommandOutcome apply(uint8_t opcode);

  // Until beginBoot() runs, the lock is treated as heading to Locked.
  LockState state_ = LockState::Locking;
  uint8_t pending_[kMaxPendingCommands] = {};
  size_t pendingHead_ = 0;
  size_t pendingCount_ = 0;
};

}  // namespace lock
