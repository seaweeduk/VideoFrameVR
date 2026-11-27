#include "picture_drag.h"

#include "flat_screen.h"

#include <algorithm>
#include <cmath>

namespace {

// A smoothed copy of the hand's turn catches up with the real one at this
// rate (per second, about a 250 ms lag), which calms hand jitter. Frame gaps
// longer than kDragMaxStep count as that long.
constexpr float kDragResponse = 4.0f;
constexpr double kDragMaxStep = 0.1;  // seconds

}  // namespace

void PictureDrag::update(const ControllerState& input, const Frame& frame, ScreenPlacement& screen) {
  if (hand_ >= 0 && (!input.hands[hand_].select || frame.noDrag || frame.menuHand == hand_))
    hand_ = -1;
  for (int h = 0; h < 2 && hand_ < 0 && frame.headValid && !frame.noDrag; ++h) {
    const HandState& t = input.hands[h];
    if (!t.selectPressed || !t.poseValid || frame.menuHand == h || frame.onMenu[h] ||
        frame.triggerTaken[h] || frame.keyPicking)
      continue;
    hand_ = h;
    tracking_ = false;
    buzz(h, 0.25f, 0.012);
  }
  if (hand_ >= 0) {
    const HandState& t = input.hands[hand_];
    const double time = frame.displayTime;
    if (!t.poseValid) {
      tracking_ = false;  // hold still until the hand is tracked again
    } else if (!tracking_) {
      tracking_ = true;
      smoothHand_ = normalize(t.aimOrientation);
    } else {
      // The picture turns as the smoothed hand did this frame: sideways,
      // up/down, and twisting the wrist rolls it (to level a tilted horizon).
      const double step = std::clamp(time - lastTime_, 0.0, kDragMaxStep);
      const float alpha = 1.0f - std::exp(-kDragResponse * static_cast<float>(step));
      const Quat next = slerpShortest(smoothHand_, t.aimOrientation, alpha);
      screen.anchorRotation = rotation(next * conjugate(smoothHand_)) * screen.rotation();
      screen.turn = 0.0f;
      smoothHand_ = next;
    }
    lastTime_ = time;
  }
}
