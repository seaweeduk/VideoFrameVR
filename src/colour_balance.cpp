#include "colour_balance.h"

#include <algorithm>
#include <cmath>

namespace {

// Stops at each end of the sliders: warmth turns red up and blue down by
// this much each, tint green by this much against the red/blue average
// (magenta above 0), exposure all three.
constexpr float kWarmthStops = 1.0f, kTintStops = 1.0f, kExposureStops = 2.0f;

int toSlider(float stops, float range) {
  return static_cast<int>(std::lround(std::clamp(stops / range, -1.0f, 1.0f) * 100.0f));
}

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

void ColourBalance::match(const float light[3], bool colour, float white, float colourShare,
                          float brightnessShare) {
  if (colour) {
    float l[3];
    for (int i = 0; i < 3; ++i) l[i] = std::log2(std::max(light[i], 1e-6f));
    warmth = toSlider((l[0] - l[2]) / 2 * colourShare, kWarmthStops);
    tint = toSlider(((l[0] + l[2]) / 2 - l[1]) * colourShare, kTintStops);
  }
  exposure = toSlider(std::log2(std::max(white, 1e-6f)) * brightnessShare, kExposureStops);
}
