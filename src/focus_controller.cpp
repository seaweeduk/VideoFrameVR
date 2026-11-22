#include "focus_controller.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kAgreement = 0.02f;  // radians of half-convergence angle
constexpr float kRate = 4.0f;        // per second at transition speed 1
// Aimed by the eyes (ours): a change smaller than this (radians of
// half-convergence) is followed faster, up to 4 times for a quarter of it, as
// the eyes pursue a moving object; larger jumps glide at the usual rate.
constexpr float kPursuit = 0.01f, kMaxPursuitGain = 4.0f;
constexpr float kMinDistance = 0.01f;

}  // namespace

void FocusController::clearProbes() {
  probes_[0] = probes_[1] = probes_[2] = 0;
  phase_ = 0;
  phaseStep_ = 1;
}

void FocusController::submit(const DepthEstimate& e, int phase) {
  const bool ok = e.status == DepthStatus::Ok;
  probes_[std::clamp(phase, -1, 1) + 1] = ok ? e.angle : 0.0f;
  if (phase_ == 1) phaseStep_ = -1;
  if (phase_ == -1) phaseStep_ = 1;
  phase_ += phaseStep_;

  const float minus = probes_[0], center = probes_[1], plus = probes_[2];
  // A side probe without a depth (ours): the other one decides with the
  // centre, so that a plain or repeating spot beside the target does not
  // keep the focus where it was.
  if (center <= 0 || (minus <= 0 && plus <= 0)) return;
  const float none = 2 * kAgreement;
  const float d = std::min(minus > 0 ? std::abs(center - minus) : none,
                           plus > 0 ? std::abs(center - plus) : none);
  if (d < kAgreement && (minus <= 0 || std::abs(minus - accepted_) > d) &&
      (plus <= 0 || std::abs(plus - accepted_) > d)) {
    accepted_ = center;
  }
}

float FocusController::advance(const FocusSettings& s, double now, bool eyes) {
  const float dt = lastAdvance_ < 0 ? 0.0f : static_cast<float>(std::clamp(now - lastAdvance_, 0.0, 0.25));
  lastAdvance_ = now;
  const float halfBaseline = 0.5f * s.baselineMm / 1000.0f;
  const float candidate = halfBaseline / std::tan(accepted_);
  // Nearer than the range: the focus holds. Farther (ours): it goes to the
  // farthest distance, so that looking from a near object to a far
  // background does not leave the near focus on it.
  state_.holding = candidate < s.minCm / 100.0f;
  const float goal = std::max(accepted_, std::atan(halfBaseline / (s.maxCm / 100.0f)));
  // The exponential form approximates 4 × speed × dt for small steps
  // and stays stable for long frames.
  float rate = kRate * s.speed / 100.0f;
  if (eyes) rate *= std::clamp(kPursuit / std::abs(goal - shown_), 1.0f, kMaxPursuitGain);
  if (!state_.holding) shown_ += (goal - shown_) * (1.0f - std::exp(-rate * dt));
  state_.distance = std::max(kMinDistance, halfBaseline / std::tan(shown_));
  return state_.distance;
}
