#include "list_scroll.h"

#include <algorithm>
#include <cmath>

bool ListScroll::update(const ControllerState& input, const Frame& frame, double& rows) {
  float tilt = 0.0f;
  for (int i = 0; i < 2; ++i) {
    const HandState& h = input.hands[i];
    if (frame.valueStick || frame.menuHand == i || frame.screenHand == i ||
        std::abs(h.stickY) <= std::abs(h.stickX))
      continue;
    if (std::abs(h.stickY) > std::abs(tilt)) tilt = h.stickY;
  }
  const float amount = std::max(0.0f, (std::abs(tilt) - 0.15f) / 0.85f);
  const float sign = tilt > 0 ? -1.0f : 1.0f;
  if (amount == 0 || sign != pushSign_)
    pushSeconds_ = 0;
  else if (amount > 0.7f)
    pushSeconds_ = std::min(pushSeconds_ + frame.dt, 3.0);
  else
    pushSeconds_ = std::max(0.0, pushSeconds_ - 4.0 * frame.dt);
  pushSign_ = amount > 0 ? sign : 0.0f;
  const double ramp = std::clamp((pushSeconds_ - 1.0) / 2.0, 0.0, 1.0);
  const double boost = 1.0 + 3.0 * ramp * ramp * (3.0 - 2.0 * ramp);
  if (amount == 0) return false;
  rows = sign * amount * amount * 14.0 * boost * frame.dt;
  return true;
}

void ListScroll::reset() {
  pushSeconds_ = 0;
  pushSign_ = 0;
}
