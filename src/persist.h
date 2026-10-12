#pragma once
// NVS (Preferences) storage for the boot counter. The lock always drives to
// Locked on boot (STATE-005), so no target state is persisted. Not
// thread-safe: only setup() uses it.

#include <stdint.h>

namespace persist {

bool begin();

// Increments and stores the boot counter; returns the new value (1 on first boot).
uint32_t incrementBootCount();

}  // namespace persist
