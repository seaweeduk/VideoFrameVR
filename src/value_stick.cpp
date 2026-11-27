#include "value_stick.h"

#include <cmath>

int ValueStick::update(const ControllerState& input, const Frame& frame) {
  float x = 0.0f;
  bool centred = true;
  for (const HandState& h : input.hands) {
    if (std::abs(h.stickX) > std::abs(h.stickY) && std::abs(h.stickX) > std::abs(x)) x = h.stickX;
    centred = centred && std::abs(h.stickX) < adjust::kStickRelease &&
              std::abs(h.stickY) < adjust::kStickRelease;
  }
  if (!frame.menuOpen || frame.typing || centred) held_ = false;
  if (!held_ && frame.menuOpen && !frame.typing && !frame.seeking &&
      std::abs(x) >= adjust::kStickPress && frame.pointedValue)
    held_ = true;
  int step = 0;
  if (less_.update(held_ && x <= -adjust::kStickPress, frame.now)) step = -1;
  if (more_.update(held_ && x >= adjust::kStickPress, frame.now)) step = 1;
  return step;
}
