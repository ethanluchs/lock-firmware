// LED stand-in driver (ACT-002): on = unlocked, off = locked, ~4 Hz blink
// while moving, done after T_act. Default driver unless ACTUATOR_SERVO is set.
#ifndef ACTUATOR_SERVO

#include <Arduino.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#include "actuator.h"
#include "app_config.h"

namespace act {
namespace {

constexpr uint32_t kTicksPerMove = cfg::kActuationMs / cfg::kLedBlinkHalfPeriodMs;
static_assert(kTicksPerMove >= 1, "T_act shorter than one blink half-period");

class LedActuator : public Actuator {
 public:
  bool begin(DoneFn onDone) override {
    onDone_ = onDone;
    pinMode(cfg::kLedPin, OUTPUT);
    digitalWrite(cfg::kLedPin, LOW);
    esp_timer_create_args_t args = {};
    args.callback = &LedActuator::tickThunk;
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "led_act";
    return esp_timer_create(&args, &timer_) == ESP_OK;
  }

  void startMove(lock::LockState target) override {
    bool arm;
    portENTER_CRITICAL(&mux_);
    target_ = target;
    ticks_ = 0;
    moving_ = true;
    level_ = !level_;  // first visible toggle immediately
    arm = !armed_;
    armed_ = true;
    portEXIT_CRITICAL(&mux_);
    digitalWrite(cfg::kLedPin, level_ ? HIGH : LOW);
    // If a tick is already pending it re-arms itself; arming twice would fail.
    if (arm) esp_timer_start_once(timer_, cfg::kLedBlinkHalfPeriodMs * 1000ULL);
  }

  const char *name() const override { return "LED"; }

 private:
  static void tickThunk(void *arg) { static_cast<LedActuator *>(arg)->tick(); }

  // One-shot re-armed per tick, so no periodic wakeups exist while idle.
  void tick() {
    bool rearm = false;
    bool done = false;
    bool level;
    lock::LockState reached = lock::LockState::Locked;
    portENTER_CRITICAL(&mux_);
    if (!moving_) {
      armed_ = false;
      portEXIT_CRITICAL(&mux_);
      return;
    }
    if (++ticks_ >= kTicksPerMove) {
      moving_ = false;
      armed_ = false;
      done = true;
      reached = target_;
      level_ = (target_ == lock::LockState::Unlocked);
    } else {
      level_ = !level_;
      rearm = true;
    }
    level = level_;
    portEXIT_CRITICAL(&mux_);

    digitalWrite(cfg::kLedPin, level ? HIGH : LOW);
    if (rearm) esp_timer_start_once(timer_, cfg::kLedBlinkHalfPeriodMs * 1000ULL);
    if (done && onDone_) onDone_(reached);
  }

  DoneFn onDone_ = nullptr;
  esp_timer_handle_t timer_ = nullptr;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  lock::LockState target_ = lock::LockState::Locked;
  uint32_t ticks_ = 0;
  bool moving_ = false;
  bool armed_ = false;
  bool level_ = false;
};

LedActuator g_led;

}  // namespace

Actuator &instance() {
  return g_led;
}

}  // namespace act

#endif  // !ACTUATOR_SERVO
