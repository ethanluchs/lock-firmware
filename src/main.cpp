// VT-LOCK V1 firmware entry point. setup() runs the boot sequence, then the
// control task (control.cpp) does all the work.

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "app_config.h"
#include "ble_service.h"
#include "control.h"
#include "diagnostics.h"
#include "persist.h"

void setup() {
  diag::begin();
  delay(1000);  // let the serial monitor attach
  diag::logf(diag::kBoot, "starting " APP_FW_VERSION_STR);

  esp_reset_reason_t reason = diag::resetReason();

  if (!persist::begin()) diag::logf(diag::kBoot, "WARNING: NVS unavailable; boot count won't persist");
  uint32_t bootCount = persist::incrementBootCount();

  if (!control::init(bootCount)) {
    // Queue/mutex/actuator failure: nothing safe to do but retry from scratch.
    diag::logf(diag::kBoot, "control init failed; restarting in %lu ms",
               static_cast<unsigned long>(cfg::kBleFailRebootDelayMs));
    delay(cfg::kBleFailRebootDelayMs);
    ESP.restart();
  }

  // STATE-005 / FAIL-001: always drive to Locked, before the radio is up.
  lock::LockState target = control::boot(reason);
  diag::printBanner(reason, bootCount, target);

  if (!ble::init()) {
    // FAIL-006: never run without a radio.
    diag::logf(diag::kBle, "BLE stack failed to start; restarting in %lu ms",
               static_cast<unsigned long>(cfg::kBleFailRebootDelayMs));
    delay(cfg::kBleFailRebootDelayMs);
    ESP.restart();
  }

  if (!control::start()) {
    diag::logf(diag::kBoot, "control task create failed; restarting");
    delay(cfg::kBleFailRebootDelayMs);
    ESP.restart();
  }
  diag::logf(diag::kBoot, "ready");
}

void loop() {
  // All work happens in the control task; free loopTask's stack.
  vTaskDelete(nullptr);
}
