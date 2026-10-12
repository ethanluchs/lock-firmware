#include "diagnostics.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

#include "app_config.h"

namespace diag {

void begin() {
  Serial.begin(115200);
}

void logf(const char *tag, const char *fmt, ...) {
  char buf[160];
  int n = snprintf(buf, sizeof(buf), "[%s] ", tag);
  if (n < 0) return;
  size_t used = static_cast<size_t>(n) < sizeof(buf) ? static_cast<size_t>(n) : sizeof(buf) - 1;

  va_list ap;
  va_start(ap, fmt);
  int m = vsnprintf(buf + used, sizeof(buf) - used, fmt, ap);
  va_end(ap);
  if (m > 0) {
    used += static_cast<size_t>(m);
    if (used > sizeof(buf) - 1) used = sizeof(buf) - 1;
  }
  // One write per line keeps lines from different tasks from interleaving.
  if (used > sizeof(buf) - 2) used = sizeof(buf) - 2;
  buf[used++] = '\n';
  Serial.write(reinterpret_cast<const uint8_t *>(buf), used);
}

esp_reset_reason_t resetReason() {
  return esp_reset_reason();
}

const char *resetReasonString(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_UNKNOWN: return "UNKNOWN";
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "OTHER";
  }
}

void printBanner(esp_reset_reason_t reason, uint32_t bootCount, lock::LockState bootTarget) {
  logf(kBoot, "==============================");
  logf(kBoot, "VT-LOCK firmware " APP_FW_VERSION_STR);
  logf(kBoot, "reset reason: %s (%d)", resetReasonString(reason), static_cast<int>(reason));
  logf(kBoot, "boot count:   %lu", static_cast<unsigned long>(bootCount));
  logf(kBoot, "boot target:  %s (always locks on boot)", lock::toString(bootTarget));
#ifdef ACTUATOR_SERVO
  logf(kBoot, "actuator:     servo on GPIO %u", cfg::kServoPin);
#else
  logf(kBoot, "actuator:     LED on GPIO %u", cfg::kLedPin);
#endif
  logf(kBoot, "==============================");
}

}  // namespace diag
