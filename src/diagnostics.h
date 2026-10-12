#pragma once
// Serial diagnostics (DIAG-001/002, FAIL-003).

#include <esp_system.h>
#include <stdint.h>

#include "lock_types.h"

namespace diag {

// Subsystem tags (DIAG-001).
constexpr const char *kBoot = "boot";
constexpr const char *kBle = "ble";
constexpr const char *kCmd = "cmd";
constexpr const char *kState = "state";
constexpr const char *kAct = "act";
constexpr const char *kLog = "log";

void begin();

// Prints "[tag] <formatted>\n". Fixed stack buffer, no heap; long lines are
// truncated. Safe from any task (not from ISRs).
void logf(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

esp_reset_reason_t resetReason();
const char *resetReasonString(esp_reset_reason_t r);

// DIAG-002 banner. `bootTarget` is always Locked (STATE-005).
void printBanner(esp_reset_reason_t reason, uint32_t bootCount, lock::LockState bootTarget);

}  // namespace diag
