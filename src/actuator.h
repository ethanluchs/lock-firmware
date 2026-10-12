#pragma once
// The one actuator interface (ACT-001). Swapping LED for servo only changes
// which driver is linked in (build flag ACTUATOR_SERVO), never the callers.

#include "lock_types.h"

namespace act {

// Called once per finished move with the position reached. Runs in the
// esp_timer task: must not block.
using DoneFn = void (*)(lock::LockState reached);

class Actuator {
 public:
  virtual ~Actuator() = default;
  virtual bool begin(DoneFn onDone) = 0;
  // `target` is Locked or Unlocked. A move already in progress is abandoned
  // and replaced; completion is reported for the new target only.
  virtual void startMove(lock::LockState target) = 0;
  virtual const char *name() const = 0;
};

// The driver selected at build time.
Actuator &instance();

}  // namespace act
