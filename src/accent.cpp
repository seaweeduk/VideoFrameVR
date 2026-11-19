#include "accent.h"

#include <algorithm>
#include <cmath>

// Colours are made in OKLCH, where lightness looks the same across hues: the
// fill of every hue is as light as keeps white text on it as readable as on
// today's blue (4.05:1), and as vivid as sRGB allows at that lightness.

namespace {

constexpr const char* kCodes[kAccentPresets + 1] = {"blue", "teal",  "green",  "orange",
                                                    "red",  "pink", "purple", "custom"};
constexpr double kPresetHues[kAccentPresets] = {259.4, 200, 145, 50, 25, 350, 305};

struct Linear {
  double r, g, b;
};

Linear oklchToLinear(double L, double C, double hue) {
  const double h = hue * M_PI / 180;
  const double a = C * std::cos(h), b = C * std::sin(h);
  const double l = std::pow(L + 0.3963377774 * a + 0.2158037573 * b, 3);
  const double m = std::pow(L - 0.1055613458 * a - 0.0638541728 * b, 3);
  const double s = std::pow(L - 0.0894841775 * a - 1.2914855480 * b, 3);
  return {4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
          -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
          -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s};
}

bool inGamut(const Linear& c) {
  constexpr double e = 1e-6;
  return c.r >= -e && c.r <= 1 + e && c.g >= -e && c.g <= 1 + e && c.b >= -e && c.b <= 1 + e;
}

// The most chroma sRGB can show at this lightness and hue.
double maxChroma(double L, double hue) {
  double lo = 0, hi = 0.4;
  for (int i = 0; i < 24; ++i) {
    const double mid = (lo + hi) / 2;
    (inGamut(oklchToLinear(L, mid, hue)) ? lo : hi) = mid;
  }
  return lo;
}

// The most vivid colour of this hue and lightness sRGB can show, or this
// share of it.
Linear vivid(double L, double hue, double share = 1) {
  return oklchToLinear(L, maxChroma(L, hue) * share, hue);
}

double whiteContrast(const Linear& c) {
  const double y = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
  return 1.05 / (std::max(0.0, y) + 0.05);
}

// The lightness at which white text on the hue's fill has this contrast.
double lightnessFor(double hue, double contrast) {
  double lo = 0.3, hi = 0.9;
  for (int i = 0; i < 24; ++i) {
    const double mid = (lo + hi) / 2;
    (whiteContrast(vivid(mid, hue)) > contrast ? lo : hi) = mid;
  }
  return lo;
}

void toSrgb(const Linear& c, double out[3]) {
  int i = 0;
  for (double v : {c.r, c.g, c.b}) {
    v = std::clamp(v, 0.0, 1.0);
    out[i++] = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
  }
}

// An sRGB colour's OKLCH lightness and hue, and its chroma as a share of the
// most vivid at that lightness and hue.
void toOklch(const double srgb[3], double& L, double& hue, double& share) {
  double c[3];
  for (int i = 0; i < 3; ++i)
    c[i] = srgb[i] <= 0.04045 ? srgb[i] / 12.92 : std::pow((srgb[i] + 0.055) / 1.055, 2.4);
  const double l = std::cbrt(0.4122214708 * c[0] + 0.5363325363 * c[1] + 0.0514459929 * c[2]);
  const double m = std::cbrt(0.2119034982 * c[0] + 0.6806995451 * c[1] + 0.1073969566 * c[2]);
  const double s = std::cbrt(0.0883024619 * c[0] + 0.2817188376 * c[1] + 0.6299787005 * c[2]);
  L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
  const double a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
  const double b = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
  hue = std::atan2(b, a) * 180 / M_PI;
  const double most = maxChroma(L, hue);
  share = most > 0 ? std::min(1.0, std::hypot(a, b) / most) : 0;
}

// The filled controls' shades, as steps from the fill: lightness, hue and
// share of the most vivid. On the default blue they are the menu design's
// own colours (#3b8cff to #1b6cf0 at rest, #1a5fd2 to #1d68e4 pressed;
// shadows #001446 and #145aff).
struct Step {
  double lightness, hue, share;
};
constexpr Step kFilledTop{0.0480, -1.57, 1.0}, kFilledBottom{-0.0362, 0.90, 0.904};
constexpr Step kPressedTop{-0.0867, 1.01, 0.881}, kPressedBottom{-0.0534, 0.93, 0.883};
constexpr Step kShadow{-0.3849, 2.23, 1.0}, kGlow{-0.0580, 3.77, 1.0};

void finish(AccentColours& c) {
  double L, hue, share;
  toOklch(c.fill, L, hue, share);
  auto shade = [&](const Step& s, double out[3]) {
    const double l = std::clamp(L + s.lightness, 0.05, 0.97);
    toSrgb(vivid(l, hue + s.hue, share * s.share), out);
  };
  shade(kFilledTop, c.filled.top);
  shade(kFilledBottom, c.filled.bottom);
  shade(kPressedTop, c.pressed.top);
  shade(kPressedBottom, c.pressed.bottom);
  shade(kShadow, c.shadow);
  shade(kGlow, c.glow);
}

// brightness 50: as readable as the presets; 0 dark; 100 as light as white
// text still reads on (3:1). saturation: percent of the most vivid.
AccentColours make(double hue, int brightness, int saturation) {
  const double share = std::clamp(saturation, 0, 100) / 100.0;
  const double normal = lightnessFor(hue, 4.05);
  const double t = std::clamp(brightness, 0, 100) / 50.0 - 1;
  const double L = t < 0 ? normal + t * (normal - 0.40) : normal + t * (lightnessFor(hue, 3.0) - normal);
  AccentColours c;
  toSrgb(vivid(L, hue, share), c.fill);
  // Text on the dark panels: lighter than the fill, following it a little.
  toSrgb(vivid(std::clamp(0.711 + 0.6 * (L - normal), 0.64, 0.84), hue, share), c.text);
  finish(c);
  return c;
}

}  // namespace

AccentColours accentColours(const Accent& accent) {
  if (accent.preset == kAccentCustom) return make(accent.hue, accent.brightness, accent.saturation);
  if (accent.preset <= 0 || accent.preset >= kAccentPresets) {
    static AccentColours blue = [] {
      AccentColours c{{0.118, 0.471, 1.0}, {0.36, 0.64, 1.0}, {}, {}, {}, {}};  // as it always was
      finish(c);
      return c;
    }();
    return blue;
  }
  static AccentColours presets[kAccentPresets] = {};
  static bool made[kAccentPresets] = {};
  if (!made[accent.preset]) {
    presets[accent.preset] = make(kPresetHues[accent.preset], 50, 100);
    made[accent.preset] = true;
  }
  return presets[accent.preset];
}

const char* accentCode(int preset) {
  return kCodes[std::clamp(preset, 0, kAccentCustom)];
}

int accentPreset(const std::string& code) {
  for (int i = 0; i <= kAccentCustom; ++i)
    if (code == kCodes[i]) return i;
  return 0;
}
