#include "lock_types.h"

namespace lock {

const char *toString(LockState s) {
  switch (s) {
    case LockState::Locked: return "LOCKED";
    case LockState::Unlocking: return "UNLOCKING";
    case LockState::Unlocked: return "UNLOCKED";
    case LockState::Locking: return "LOCKING";
  }
  // Values outside the enum can arrive from storage or the wire.
  return "?";
}

const char *toString(CmdResult r) {
  switch (r) {
    case CmdResult::Ok: return "OK";
    case CmdResult::NoChange: return "NO_CHANGE";
    case CmdResult::Busy: return "BUSY";
    case CmdResult::UnknownCommand: return "UNKNOWN_COMMAND";
    case CmdResult::Malformed: return "MALFORMED";
  }
  return "?";
}

}  // namespace lock
