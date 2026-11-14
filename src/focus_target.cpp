#include "focus_target.h"
#include "strings.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kMaxAge = 0.1;      // seconds: older gaze samples are not used
constexpr double kHold = 0.35;       // a gaze gap this short (a blink) is held
constexpr double kReacquire = 0.15;  // gaze tracked this long before it is followed again
constexpr double kBlend = 0.4;       // glide between head and gaze direction
constexpr float kMinForward = 0.2f;  // gaze more than ~78° off the head's axis is ignored
// One Euro filter (ours): cutoff 1.5 Hz at rest plus 12 Hz per rad/s of eye
// speed, the speed itself filtered at 1 Hz. Quiet fixations, quick saccades.
constexpr float kMinCutoff = 1.5f, kBeta = 12.0f, kSpeedCutoff = 1.0f;
constexpr float kPi = 3.14159265f;
const Vec3 kForward = {0.0f, 0.0f, -1.0f};

Vec3 blend(Vec3 from, Vec3 to, double t) {
  const float s = static_cast<float>(std::clamp(t, 0.0, 1.0));
  const float k = s * s * (3 - 2 * s);
  return normalize(from * (1 - k) + to * k);
}

}  // namespace

void FocusTargetSelector::smooth(Vec3 g, double time) {
  if (time == sampleAt_) return;  // the same sample again
  const float dt = static_cast<float>(time - sampleAt_);
  const bool fresh = sampleAt_ < 0 || !(dt > 0 && dt <= kHold);
  sampleAt_ = time;
  if (fresh) {
    gaze_ = raw_ = g;
    velocity_ = {};
    return;
  }
  auto alpha = [dt](float cutoff) { return dt / (dt + 1 / (2 * kPi * cutoff)); };
  velocity_ = velocity_ + ((g - raw_) * (1 / dt) - velocity_) * alpha(kSpeedCutoff);
  raw_ = g;
  gaze_ = normalize(gaze_ + (g - gaze_) * alpha(kMinCutoff + kBeta * length(velocity_)));
}

FocusTarget FocusTargetSelector::update(bool eyeTracking, const GazeState& gaze, Quat head,
                                        double now, const GazeFit& fit) {
  const float dt = last_ < 0 ? 0.0f : static_cast<float>(std::clamp(now - last_, 0.0, 0.1));
  last_ = now;
  FocusTarget t;
  bool good = gaze.available && gaze.active && gaze.valid && gaze.tracked &&
              (!gaze.ageKnown || gaze.ageSeconds <= kMaxAge);
  if (good) {
    // The gaze in head axes, so that holding and gliding move with the head.
    const Vec3 world = transformDir(rotation(gaze.orientation), kForward);
    const Vec3 local = normalize(transformDir(rotation(conjugate(head)), world));
    good = local.z < -kMinForward;
    if (good) {
      trackerGaze_ = local;
      smooth(applyGazeFit(local, fit), gaze.ageKnown ? gaze.sampleSeconds : now);
      goodAt_ = now;
    }
  }
  trackerGood_ = good;
  gazeKnown_ = gaze.available && goodAt_ >= 0 && now - goodAt_ <= kHold;
  if (!eyeTracking || !gaze.available) {
    phase_ = Phase::Head;
    status_ = eyeTracking ? TargetStatus::EyesUnsupported : TargetStatus::Head;
    target_ = kForward;
    return t;
  }
  if (phase_ == Phase::Head) phase_ = Phase::Fallback;  // just chosen: as after a loss

  switch (phase_) {
    case Phase::Following:
      if (!good) {
        phase_ = Phase::Holding;
        since_ = now;
      }
      break;
    case Phase::Holding:
      if (good) {
        phase_ = Phase::Following;  // a blink: carry on without a glide
        since_ = now - kBlend;
      } else if (now - since_ > kHold) {
        phase_ = Phase::Fallback;
      }
      break;
    case Phase::Fallback:
      if (good) {
        phase_ = Phase::Reacquiring;
        since_ = now;
      }
      break;
    case Phase::Reacquiring:
      if (!good) {
        phase_ = Phase::Fallback;  // the glide to the head direction goes on
      } else if (now - since_ > kReacquire) {
        phase_ = Phase::Following;
        since_ = now;
        blendFrom_ = target_;
      }
      break;
    case Phase::Head:
      break;
  }

  switch (phase_) {
    case Phase::Following:
      target_ = blend(blendFrom_, gaze_, (now - since_) / kBlend);
      status_ = TargetStatus::Eyes;
      t.eyes = true;
      break;
    case Phase::Holding:
      t.usable = false;
      t.eyes = true;  // the depth stays as it is through the gap
      status_ = TargetStatus::EyesHolding;
      break;
    default:
      // Gliding to the head direction from wherever the target was, measured
      // on the way.
      target_ = normalize(target_ + (kForward - target_) *
                                        (1.0f - std::exp(-3.0f * dt / static_cast<float>(kBlend))));
      status_ = gaze.active ? TargetStatus::EyesFallback : TargetStatus::EyesInactive;
      break;
  }
  t.direction = target_;
  return t;
}

const char* targetStatusText(TargetStatus status) {
  const auto& t = strings().focusStatus;
  switch (status) {
    case TargetStatus::Head: return t.targets[0];
    case TargetStatus::Eyes: return t.targets[1];
    case TargetStatus::EyesHolding: return t.targets[2];
    case TargetStatus::EyesFallback:
      return t.targets[3];
    case TargetStatus::EyesInactive:
      return t.targets[4];
    case TargetStatus::EyesUnsupported:
      return t.targets[5];
  }
  return "";
}
