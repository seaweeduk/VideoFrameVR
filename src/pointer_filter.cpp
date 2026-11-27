#include "pointer_filter.h"

#include "menu_feel.h"

#include <cmath>

namespace {

constexpr double kMaxGap = 0.1;  // seconds between samples; longer starts again
constexpr float kPi = 3.14159265f;

}  // namespace

Vec3 PointerFilter::update(Vec3 direction, double seconds) {
  if (!std::isfinite(direction.x + direction.y + direction.z)) {
    reset();  // a bad sample is never kept
    return direction;
  }
  const MenuFeel& feel = menuFeel();
  const double gap = seconds - at_;
  if (gap == 0 && at_ >= 0) return steady_;  // the same sample again
  const bool fresh = at_ < 0 || !(gap > 0 && gap <= kMaxGap) || feel.pointMinCutoff <= 0;
  at_ = seconds;
  if (fresh) {
    steady_ = direction;
    speed_ = {};
    return steady_;
  }
  const float dt = static_cast<float>(gap);
  auto alpha = [dt](double cutoff) {
    return dt / (dt + 1 / (2 * kPi * static_cast<float>(cutoff)));
  };
  // The rate from the steadied direction, not the last raw one, as SteamVR's
  // overlay filter does: noise alone barely opens it.
  speed_ = speed_ + ((direction - steady_) * (1 / dt) - speed_) * alpha(feel.pointSpeedCutoff);
  const double cutoff = feel.pointMinCutoff + feel.pointBeta * length(speed_);
  steady_ = normalize(steady_ + (direction - steady_) * alpha(cutoff));
  return steady_;
}
