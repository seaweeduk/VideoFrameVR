#include "colour_balance.h"

#include <cmath>

namespace {

// Stops at each end of the sliders: warmth turns red up and blue down by
// this much each, tint green by this much against the red/blue average
// (magenta above 0), exposure all three.
constexpr float kWarmthStops = 1.0f, kTintStops = 1.0f, kExposureStops = 2.0f;

}  // namespace

std::array<float, 3> ColourBalance::gains() const {
  if (neutral()) return {1.0f, 1.0f, 1.0f};
  const float w = warmth / 100.0f * kWarmthStops, t = tint / 100.0f * kTintStops;
  std::array<float, 3> g = {std::exp2(w + t / 3), std::exp2(-2 * t / 3), std::exp2(-w + t / 3)};
  const float scale = std::exp2(exposure / 100.0f * kExposureStops) /
                      (0.2126f * g[0] + 0.7152f * g[1] + 0.0722f * g[2]);
  for (float& v : g) v *= scale;
  return g;
}
