#pragma once
// Shared lock vocabulary (docs/v1-spec.md §4, §5.1).
// Pure C++: no Arduino or ESP-IDF includes, so it builds for the native test env.
// Numeric values are on the wire (docs/ble-spec.md v1.1) and in the event log:
// never renumber.

#include <stdint.h>

namespace lock {

enum class LockState : uint8_t {
  Locked = 0,
  Unlocking = 1,
  Unlocked = 2,
  Locking = 3,
};

enum class CmdResult : uint8_t {
  Ok = 0,              // accepted; a transition has started
  NoChange = 1,        // already in the requested state
  Busy = 2,            // transition in progress (or command queue full)
  UnknownCommand = 3,  // opcode not recognised
  Malformed = 4,       // write was not exactly 1 byte
};

constexpr uint8_t kOpLock = 0x00;
constexpr uint8_t kOpUnlock = 0x01;

// "No value" marker for opcode/result fields (e.g. empty write, no command yet).
constexpr uint8_t kNone = 0xFF;

// Short uppercase names for logs, e.g. "UNLOCKING", "NO_CHANGE".
// Unknown values return "?".
const char *toString(LockState s);
const char *toString(CmdResult r);

}  // namespace lock
