#include "lock_state_machine.h"

namespace lock {

void LockStateMachine::beginBoot() {
  // STATE-005: power-up always ends Locked; saved state is not trusted.
  state_ = LockState::Locking;
  // Commands from before the reboot (or before begin) belong to no client now.
  pendingHead_ = 0;
  pendingCount_ = 0;
}

CommandOutcome LockStateMachine::handleCommand(const uint8_t *data, size_t len) {
  CommandOutcome out;
  out.opcode = (len > 0 && data != nullptr) ? data[0] : kNone;
  out.deferred = false;
  out.startMove = false;
  // Not meaningful unless startMove, but keep it a valid settled state.
  out.target = targetOf(state_);

  if (len != 1 || data == nullptr) {
    out.result = CmdResult::Malformed;
    return out;
  }
  if (out.opcode != kOpLock && out.opcode != kOpUnlock) {
    out.result = CmdResult::UnknownCommand;
    return out;
  }

  if (isTransitional(state_)) {
    if (pendingCount_ >= kMaxPendingCommands) {
      out.result = CmdResult::Busy;
      return out;
    }
    pending_[(pendingHead_ + pendingCount_) % kMaxPendingCommands] = out.opcode;
    pendingCount_++;
    // Result is not reported for a deferred command; Busy is the least
    // misleading value if a caller reports it anyway.
    out.result = CmdResult::Busy;
    out.deferred = true;
    return out;
  }

  return apply(out.opcode);
}

bool LockStateMachine::onActuationComplete() {
  if (!isTransitional(state_)) return false;
  state_ = targetOf(state_);
  return true;
}

bool LockStateMachine::popPending(CommandOutcome *out) {
  // Never start a second move while one is in flight.
  if (out == nullptr || isTransitional(state_) || pendingCount_ == 0) return false;
  uint8_t opcode = pending_[pendingHead_];
  pendingHead_ = (pendingHead_ + 1) % kMaxPendingCommands;
  pendingCount_--;
  *out = apply(opcode);
  return true;
}

// Caller guarantees a settled state and a valid LOCK/UNLOCK opcode.
CommandOutcome LockStateMachine::apply(uint8_t opcode) {
  CommandOutcome out;
  out.opcode = opcode;
  out.deferred = false;
  out.startMove = false;
  out.target = state_;

  LockState requested = (opcode == kOpUnlock) ? LockState::Unlocked : LockState::Locked;
  if (state_ == requested) {
    out.result = CmdResult::NoChange;
    return out;
  }

  state_ = (requested == LockState::Unlocked) ? LockState::Unlocking : LockState::Locking;
  out.result = CmdResult::Ok;
  out.startMove = true;
  out.target = requested;
  return out;
}

bool LockStateMachine::isTransitional(LockState s) {
  return s == LockState::Unlocking || s == LockState::Locking;
}

LockState LockStateMachine::targetOf(LockState s) {
  switch (s) {
    case LockState::Unlocking: return LockState::Unlocked;
    case LockState::Locking: return LockState::Locked;
    default: return s;
  }
}

}  // namespace lock
