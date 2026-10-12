#pragma once
// GATT server per lock_protocol.h (protocol v1.1). Callbacks run in the
// Bluedroid task and only capture data and hand it to the control task.

#include <stdint.h>

#include "lock_protocol.h"

namespace ble {

// Creates the server, service and characteristics and starts advertising.
// Returns false if the BLE stack failed to start (FAIL-006).
bool init();

// Control task: set the status value; notify subscribers if `notify`.
// No-op before init().
void publishStatus(const uint8_t status[lock::proto::kStatusLen], bool notify);

// Control task: drop the link (idle timeout, BLE-005).
void disconnect(uint16_t connId);

}  // namespace ble
