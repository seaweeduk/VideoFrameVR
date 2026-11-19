#include "setup_placement.h"

#include "menu.h"
#include "platform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr float kDegree = kPi / 180;
constexpr double kStill = 0.5;              // seconds held still before it is placed
constexpr float kStillSpeed = 15 * kDegree;  // per second: still enough
constexpr float kHeadTurn = 10 * kDegree;    // turned since start: a head, not a headset on a desk
constexpr float kAway = 45 * kDegree, kBack = 35 * kDegree;  // off the line of sight
constexpr double kHintAfter = 2;
constexpr float kLyingBack = 40 * kDegree, kSittingUp = 25 * kDegree;  // looking up
constexpr double kLyingAfter = 2, kSittingAfter = 1;
constexpr double kFade = 0.2;  // seconds out, and as long in
// The cues hang this far in front of the eyes, as the messages' (notices.h),
// sized as the prototype draws them at 1.5 m; the arrow out at about these
// angles.
constexpr float kNear = 0.7f, kCueScale = kNear / 1.5f / 1000;
constexpr float kEdgeX = 0.53f, kEdgeY = 0.36f;  // tangents: 28° across, 20° up or down

float angleBetween(Quat a, Quat b) {
  const float d = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
  return 2 * std::acos(std::min(1.0f, d));
}

Vec3 forward(Quat q) { return transformDir(rotation(q), {0.0f, 0.0f, -1.0f}); }

}  // namespace

void SetupPlacement::place(const Frame& f, MenuPanels& panel) const {
  panel.placeInFrontOf(f.headPos, tilted_ ? menuFrame(f.headRot) : headFrame(f.headRot));
}

void SetupPlacement::update(const Frame& f, MenuPanels& panel) {
  if (!f.headValid) return;
  now_ = f.now;
  if (!placed_) {
    if (!started_) {
      started_ = true;
      first_ = last_ = f.headRot;
      lastAt_ = stillSince_ = f.now;
    }
    const double dt = f.now - lastAt_;
    if (dt > 0 && angleBetween(last_, f.headRot) / dt > kStillSpeed) stillSince_ = f.now;
    last_ = f.headRot;
    lastAt_ = f.now;
    if (angleBetween(first_, f.headRot) > kHeadTurn) moved_ = true;
    const bool worn = kStandaloneHeadset || f.presence == 1 || (f.presence < 0 && moved_);
    if (!f.pressed && !(worn && f.now - stillSince_ >= kStill)) return;
    std::printf("Setup: placed (%s)\n", f.pressed ? "a button" : f.presence == 1 ? "worn" : "moved, then still");
    placed_ = true;
    panel.open = true;
    place(f, panel);
    front_ = panel.front();
    return;
  }

  if (fade_ == Fade::Out && f.now - fadeAt_ >= kFade) {
    place(f, panel);
    fade_ = Fade::In;
    fadeAt_ = f.now;
    away_ = false;
  } else if (fade_ == Fade::In && f.now - fadeAt_ >= kFade) {
    fade_ = Fade::None;
  }
  const Vec3 ahead = forward(f.headRot);
  if (fade_ == Fade::None) {
    // Lying back (or sitting up again) for a while, or B: out, and back in
    // front of them.
    const float up = std::asin(std::clamp(ahead.y, -1.0f, 1.0f));
    if (tilted_ ? up >= kSittingUp : up <= kLyingBack)
      pitchSince_ = -1;
    else if (pitchSince_ < 0)
      pitchSince_ = f.now;
    const bool turn = pitchSince_ >= 0 && f.now - pitchSince_ >= (tilted_ ? kSittingAfter : kLyingAfter);
    if (turn || f.bring) {
      tilted_ = up > (tilted_ ? kSittingUp : kLyingBack);
      pitchSince_ = -1;
      fade_ = Fade::Out;
      fadeAt_ = f.now;
    }
  }
  front_ = panel.front();
  const Vec3 toPanel = normalize(front_ - f.headPos);
  const float off = std::acos(std::clamp(dot(ahead, toPanel), -1.0f, 1.0f));
  if (!away_ && off > kAway) {
    away_ = true;
    awaySince_ = f.now;
  } else if (away_ && off < kBack) {
    away_ = false;
  }
}

float SetupPlacement::opacity() const {
  const float t = std::clamp(static_cast<float>((now_ - fadeAt_) / kFade), 0.0f, 1.0f);
  return fade_ == Fade::Out ? 1 - t : fade_ == Fade::In ? t : 1;
}

void SetupPlacement::addCues(std::vector<OverlayDraw>& overlays, Menu& menu, Vec3 headPos,
                             Quat headRot, double now) const {
  if (!placed_ || !away_ || fade_ != Fade::None) return;
  const Mat4 gaze = rotation(headRot);
  // Toward the panel across the view; from behind, sideways (right if
  // it is straight behind).
  const Vec3 d = transformDir(transposedRotation(gaze), front_ - headPos);
  float x = d.x, y = d.y;
  if (d.z >= 0) {
    x = std::abs(d.x) < 0.1f * length(d) || d.x > 0 ? 1.0f : -1.0f;
    y = 0;
  }
  const float len = std::sqrt(x * x + y * y);
  if (len <= 0) return;
  x /= len, y /= len;
  const Vec3 at = normalize(Vec3{kEdgeX * x, kEdgeY * y, -1.0f}) * kNear;
  auto add = [&](Menu::SetupCue cue, Vec3 center, float turn) {
    int w = 0, h = 0;
    PanelItem item;
    item.texture = menu.setupCue(cue, w, h);
    if (!item.texture) return 0.0f;
    const Quat roll{0.0f, 0.0f, std::sin(turn / 2), std::cos(turn / 2)};
    item.model = translation(headPos + transformDir(gaze, center)) * gaze * rotation(roll) *
                 scaling(w * kCueScale, h * kCueScale, 1);
    overlays.push_back(item);
    return h * kCueScale;
  };
  const float arrowH = add(Menu::SetupCue::Arrow, at, std::atan2(y, x));
  int w = 0, h = 0;
  menu.setupCue(Menu::SetupCue::Label, w, h);
  add(Menu::SetupCue::Label, at + Vec3{0.0f, -(arrowH + h * kCueScale) / 2, 0.0f}, 0);
  if (now - awaySince_ >= kHintAfter)
    add(Menu::SetupCue::Hint, {0.0f, -0.12f, -kNear}, 0);
}
