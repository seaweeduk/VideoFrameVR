#pragma once

#include <array>

// Warmth (blue to orange), tint (green to magenta) and exposure (darker to
// brighter by scaling the light, where Brightness lifts or lowers every
// level alike), each -100..100: gains on the picture's red, green and blue
// light, applied after brightness, contrast and saturation. Warmth and tint
// keep the light's luminance; exposure scales it.
struct ColourBalance {
  int warmth = 0, tint = 0, exposure = 0;

  bool neutral() const { return warmth == 0 && tint == 0 && exposure == 0; }
  bool operator==(const ColourBalance& o) const {
    return warmth == o.warmth && tint == o.tint && exposure == o.exposure;
  }
  bool operator!=(const ColourBalance& o) const { return !(*this == o); }

  // The red, green and blue gains on linear light; exactly 1 when neutral.
  std::array<float, 3> gains() const;
  // Lights the picture as a room is lit: its light's colour (linear red,
  // green and blue, any scale) for warmth and tint, unless the cameras see
  // no colour (`colour` false), and where its white sits (linear, 1 = the
  // display's) for exposure. Each as far as its range goes.
  void match(const float light[3], bool colour, float white);
};
