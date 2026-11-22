#include "screen_control.h"

#include "vr_input.h"
#include "vr_space.h"

#include <algorithm>
#include <cmath>

namespace {

// Screen distance and height increments:
// meters a second for each stick pushed up (down: back), however far.
constexpr float kDistanceRate = 0.5f;
constexpr float kHeightRate = 0.5f;

// Sticks pushed up less those pushed down, adding both hands' bindings.
int verticalPushes(const ControllerState& input) {
  int pushes = 0;
  for (const HandState& h : input.hands) {
    const StickDirections d = stickDirections(h);
    pushes += d.up - d.down;
  }
  return pushes;
}

}  // namespace

bool ScreenControl::update(const ControllerState& input, const Frame& frame,
                           ScreenPlacement& screen) {
  const ScreenSize before = screen.size;
  if (hand_ >= 0 && !input.hands[hand_].select) {
    hand_ = -1;
    corner_ = {};
  }
  {
    const ScreenSurface surface(screen, frame.aspect);
    for (int i = 0; i < 2; ++i) {
      const HandState& h = input.hands[i];
      Pointing& p = pointing_[i];
      p = {};
      if (!h.poseValid || frame.onMenu[i]) continue;
      p.onScreen = surface.hit(h.aimPosition, aimDirection(h), p.x, p.y);
      if (p.onScreen) p.corner = cornerAt(surface, p.x, p.y);
    }
    for (int i = 0; i < 2 && hand_ < 0 && !frame.menuHeld; ++i) {
      const HandState& h = input.hands[i];
      if (h.selectPressed && h.poseValid && !frame.onMenu[i] && !h.grip && !frame.triggerTaken[i])
        takeHold(i, h, surface, screen);
    }
  }

  if (hand_ >= 0 && input.hands[hand_].poseValid) {
    if (corner_.valid())
      resize(input.hands[hand_], frame.aspect, screen);
    else
      carry(input.hands[hand_], frame.up, screen);
  }

  // A corner held takes the stick's job of sizing the screen.
  if (frame.sticksFree && !frame.menuHeld && !corner_.valid()) {
    const float push = verticalPushes(input) * frame.dt;
    const int grips = input.hands[0].grip + input.hands[1].grip;
    if (push != 0 && grips < 2) {
      if (grips == 0)
        screen.size.distance += push * kDistanceRate;
      else
        screen.size.height += push * kHeightRate;
      screen.size = screen.size.clamped();
      shown_ = grips == 0 ? GestureValue::ScreenDistance : GestureValue::ScreenHeight;
      shownAt_ = frame.now;
    }
  }

  ScreenCorner corner = corner_;
  for (const Pointing& p : pointing_)
    if (!corner.valid()) corner = p.corner;
  handle_ = cornerHandle(ScreenSurface(screen, frame.aspect), corner);
  return screen.size != before;
}

void ScreenControl::reset() {
  hand_ = -1;
  corner_ = {};
  pointing_[0] = pointing_[1] = {};
  handle_.clear();
  shownAt_ = -1e9;
}

void ScreenControl::takeHold(int hand, const HandState& h, const ScreenSurface& surface,
                             const ScreenPlacement& screen) {
  hand_ = hand;
  const Pointing& p = pointing_[hand];
  corner_ = p.corner;
  if (corner_.valid()) {
    start_ = p;
    startCornerX_ = corner_.sx * surface.width() / 2;
    startCornerY_ = corner_.sy * surface.height() / 2;
    startHeight_ = screen.size.height;
  } else {
    startInverse_ = poseInverse(h.aimPosition, h.aimOrientation);
    startCentre_ = transformPoint(screen.pose(), {0.0f, 0.0f, -screen.size.distance});
  }
}

void ScreenControl::carry(const HandState& h, Vec3 up, ScreenPlacement& screen) {
  // The centre moves with the hand; the screen turns about the viewer to face it.
  const Vec3 centre =
      transformPoint(poseMatrix(h.aimPosition, h.aimOrientation) * startInverse_, startCentre_);
  const Vec3 offset = centre - screen.anchorPosition;
  const float distance = length(offset);
  Mat4 basis;
  if (distance > ScreenSize::kMinDistance &&
      facingBasis(offset * (-1.0f / distance), up, basis)) {
    screen.anchorRotation = basis;
    screen.turn = 0.0f;
    screen.size.distance = distance;
    screen.size = screen.size.clamped();
  }
}

void ScreenControl::resize(const HandState& h, float aspect, ScreenPlacement& screen) const {
  // The place pointed at along the line from the centre through the corner,
  // as a part of where it was taken: the screen grows by as much.
  float x, y;
  if (!ScreenSurface(screen, aspect).hit(h.aimPosition, aimDirection(h), x, y)) return;
  const float taken = start_.x * startCornerX_ + start_.y * startCornerY_;
  if (taken <= 1e-6f) return;
  const float now = x * startCornerX_ + y * startCornerY_;
  screen.size.height = startHeight_ * now / taken;
  screen.size = screen.size.clamped();
}
