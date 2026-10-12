// Hobby-servo driver (ACT-003): 50 Hz LEDC PWM between two configured angles,
// done after the travel time. All angles/pulses/timing are OPEN-1
// placeholders in app_config.h. Selected with -DACTUATOR_SERVO.
// Power the servo from its own 5 V supply with shared ground (PWR-002).
#ifdef ACTUATOR_SERVO

#include <Arduino.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#include "actuator.h"
#include "app_config.h"
#include "diagnostics.h"

namespace act {
namespace {

constexpr uint32_t kPeriodUs = 1000000UL / cfg::kServoPwmHz;
constexpr uint32_t kDutyMax = (1UL << cfg::kServoPwmBits) - 1;

uint32_t dutyForDeg(uint8_t deg) {
  if (deg > 180) deg = 180;
  uint32_t us = cfg::kServoMinPulseUs +
                (cfg::kServoMaxPulseUs - cfg::kServoMinPulseUs) * deg / 180;
  return us * kDutyMax / kPeriodUs;
}

class ServoActuator : public Actuator {
 public:
  bool begin(DoneFn onDone) override {
    onDone_ = onDone;
    if (!ledcAttach(cfg::kServoPin, cfg::kServoPwmHz, cfg::kServoPwmBits)) {
      diag::logf(diag::kAct, "servo PWM attach failed on GPIO %u", cfg::kServoPin);
      return false;
    }
    esp_timer_create_args_t args = {};
    args.callback = &ServoActuator::doneThunk;
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "servo_act";
    return esp_timer_create(&args, &timer_) == ESP_OK;
  }

  void startMove(lock::LockState target) override {
    uint8_t deg = (target == lock::LockState::Unlocked) ? cfg::kServoUnlockedDeg
                                                       : cfg::kServoLockedDeg;
    esp_timer_stop(timer_);  // abandon any move in progress; error if idle is fine
    portENTER_CRITICAL(&mux_);
    target_ = target;
    portEXIT_CRITICAL(&mux_);
    ledcWrite(cfg::kServoPin, dutyForDeg(deg));
    // No position feedback (§11): "done" means travel time elapsed.
    esp_timer_start_once(timer_, cfg::kServoTravelMs * 1000ULL);
  }

  const char *name() const override { return "servo"; }

 private:
  static void doneThunk(void *arg) {
    ServoActuator *self = static_cast<ServoActuator *>(arg);
    portENTER_CRITICAL(&self->mux_);
    lock::LockState reached = self->target_;
    portEXIT_CRITICAL(&self->mux_);
    if (self->onDone_) self->onDone_(reached);
  }

  DoneFn onDone_ = nullptr;
  esp_timer_handle_t timer_ = nullptr;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  lock::LockState target_ = lock::LockState::Locked;
};

ServoActuator g_servo;

}  // namespace

Actuator &instance() {
  return g_servo;
}

}  // namespace act

#endif  // ACTUATOR_SERVO
