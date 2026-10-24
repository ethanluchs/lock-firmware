#pragma once
// Single home for build-time configuration (DIAG-003: version lives only here).

#include <stddef.h>
#include <stdint.h>

namespace cfg {

// ---- Firmware version (DIAG-003) --------------------------------------------
// Used by the boot banner and the BLE info characteristic (BLE-003).
constexpr uint8_t kFwMajor = 0;
constexpr uint8_t kFwMinor = 2;
constexpr uint8_t kFwPatch = 0;
#define APP_FW_VERSION_STR "0.2.0"

// ---- BLE ----------------------------------------------------------------------
constexpr const char *kDeviceName = "VT-LOCK-001";
// Big enough that a full log page (129 B) fits one ATT read when the client
// negotiates it; smaller MTUs still work through long reads.
constexpr uint16_t kLocalMtu = 185;

// ---- Timing -----------------------------------------------------------------
// LED stand-in actuation time (STATE-003, §13 #7).
constexpr uint32_t kLedActuationMs = 1000;
// ~4 Hz blink while moving (ACT-002): toggle every 125 ms.
constexpr uint32_t kLedBlinkHalfPeriodMs = 125;
// Idle client disconnect (BLE-005, §13 #5).
constexpr uint32_t kIdleTimeoutMs = 30000;

// ---- Pins ---------------------------------------------------------------------
constexpr uint8_t kLedPin = 2;    // onboard LED; bench only (ACT-004)
constexpr uint8_t kServoPin = 25; // not a strapping/flash pin (ACT-004)

// ---- Servo placeholders: OPEN-1 (all values unconfirmed) ----------------------
constexpr uint32_t kServoPwmHz = 50;
constexpr uint8_t kServoPwmBits = 14;          // 20 ms period / 16384 steps
// SG90 bench values (unloaded, laptop USB via VIN); final values pending OPEN-1.
// Pulses/angles: clean sweep, no end-stop buzz. Original placeholders were
// 500 / 2500 us, locked 0, unlocked 90. Travel time is NOT yet measured: it
// finishes visibly before 1 s but is left at 1000 ms (it is T_act, STATE-003).
constexpr uint32_t kServoMinPulseUs = 1000;    // OPEN-1: pulse at 0 deg
constexpr uint32_t kServoMaxPulseUs = 2000;    // OPEN-1: pulse at 180 deg
constexpr uint8_t kServoLockedDeg = 0;         // OPEN-1
constexpr uint8_t kServoUnlockedDeg = 180;     // OPEN-1
constexpr uint32_t kServoTravelMs = 1000;      // OPEN-1

// T_act: the single compile-time actuation time (STATE-003).
#ifdef ACTUATOR_SERVO
constexpr uint32_t kActuationMs = kServoTravelMs;
#else
constexpr uint32_t kActuationMs = kLedActuationMs;
#endif

// ---- Event log ------------------------------------------------------------------
constexpr size_t kLogCapacity = 256;  // LOG-004
constexpr const char *kLogFsLabel = "spiffs";      // partition label in huge_app.csv
constexpr const char *kLogFsBasePath = "/littlefs";
constexpr const char *kLogFilePath = "/eventlog.bin";

// ---- Control task -------------------------------------------------------------
constexpr size_t kEventQueueLen = 16;
// Slots commands may not use, so connect/disconnect/actuation events always fit.
constexpr size_t kEventQueueReserve = 4;
// Commands that arrive while the queue is full wait here to be answered BUSY.
constexpr size_t kOverflowLen = 32;
constexpr uint32_t kControlStackBytes = 6144;
constexpr uint8_t kControlPriority = 3;  // above loopTask (1)
constexpr uint8_t kControlCore = 1;      // Bluedroid runs on core 0
// Upper bound on one queue wait; keeps the watchdog fed (TWDT is 5 s).
constexpr uint32_t kControlMaxWaitMs = 1000;

// ---- Failure handling -------------------------------------------------------
constexpr uint32_t kBleFailRebootDelayMs = 3000;  // FAIL-006

}  // namespace cfg
