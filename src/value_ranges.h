#pragma once

// The values the menu's sliders set, each a range and a step in the units
// its MenuCommand carries; the gestures that set the same values
// (controls.h) go by them too. settings.conf's values are kept within the
// same ranges (settings.cpp), and a range is written only here.

#include <algorithm>
#include <cmath>

struct ValueRange {
  int least, most, step;
};

// Eye image sizes in percent of what 1× is (eye_resolution.h). What may be
// kept in settings.conf (a converted render_scale may fall outside the
// menu's range).
constexpr int kMinResolution = 25, kMaxResolution = 600;
// The desktop menu's range and step: its slider and its − and +.
constexpr int kResolutionLow = 50, kResolutionHigh = 300, kResolutionStep = 1;

// Skip's and Big skip's lengths, in seconds: their ranges and steps
// (Settings → Controls), and a length on one of those steps.
struct SkipRange {
  int least, most, step;
  int snap(double seconds) const {
    // Into the range first, so no length overflows the steps (not a number:
    // the least).
    seconds = seconds >= most ? most : seconds >= least ? seconds : least;
    return std::clamp(static_cast<int>(std::lround(seconds / step)) * step, least, most);
  }
};
constexpr SkipRange kSkipRange{5, 60, 5}, kBigSkipRange{30, 600, 30};

// Funscript timing (ScriptSettings, script_player.h).
constexpr int kScriptOffsetLimit = 1000;  // offsetMs is within this of 0

namespace value_range {

// The limits VideoSettings (video_memory.h) and ScreenSize (flat_screen.h)
// keep their values in, under the same names there.
constexpr int kMinZoomPercent = 50, kMaxZoomPercent = 250, kZoomStepPercent = 1;
constexpr float kMaxOriginForward = 0.8f;
constexpr float kMinScreenDistance = 0.1f, kMaxScreenDistance = 11.0f;  // meters
constexpr float kMinScreenHeight = 0.1f, kMaxScreenHeight = 11.0f;      // meters
constexpr float kMaxScreenCurve = 1.0f;

constexpr int hundredths(float v) { return static_cast<int>(v * 100 + 0.5f); }

constexpr ValueRange kVolume{0, 100, 1};   // the strip's slider, of mpv's 0..130
constexpr ValueRange kKeptVolume{0, 130, 1};  // mpv's, as settings.conf keeps it
// Opacity, Match the room's strengths, the key's sliders, the menu colour's
// brightness and saturation.
constexpr ValueRange kPercent{0, 100, 1};
constexpr ValueRange kAccentHue{0, 359, 1};  // the menu's and the pointer's own colour, degrees
// Brightness, contrast, saturation (mpv's), warmth, tint and exposure.
constexpr ValueRange kPicture{-100, 100, 1};
constexpr ValueRange kSharpen{0, 300, 1};  // hundredths
constexpr ValueRange kZoom{kMinZoomPercent, kMaxZoomPercent, kZoomStepPercent};
// The surround viewpoint, in percent of the sphere's radius toward the picture.
constexpr ValueRange kViewpoint{-hundredths(kMaxOriginForward), hundredths(kMaxOriginForward), 1};
// The flat screen: centimetres, and its curve in hundredths.
constexpr ValueRange kScreenHeight{hundredths(kMinScreenHeight), hundredths(kMaxScreenHeight), 1};
constexpr ValueRange kScreenDistance{hundredths(kMinScreenDistance), hundredths(kMaxScreenDistance), 1};
constexpr ValueRange kScreenCurve{0, hundredths(kMaxScreenCurve), 1};
constexpr ValueRange kScriptOffset{-kScriptOffsetLimit, kScriptOffsetLimit, 10};  // milliseconds
constexpr ValueRange kScriptStroke{0, 100, 1};  // the part of the device's stroke played on, percent
constexpr ValueRange kSubtitleSize{50, 200, 1};        // percent
constexpr ValueRange kSubtitleRaise{-100, 100, 1};     // percent of the picture
constexpr ValueRange kSubtitleDistance{25, 600, 1};    // centimetres
// render_scale of older versions (Settings::legacyRenderScale), percent.
constexpr ValueRange kRenderScale{50, 200, 1};
constexpr ValueRange kMotionDistance{5, 1000, 1};  // head motion emulation (motion.h), centimetres
// Stereo focus (focus_settings.h): the manual distance and the automatic
// focus's least and greatest, centimetres; its transition and sensitivity
// in hundredths; its accuracy; the camera separation in millimetres; the
// pitch and yaw offsets in degrees; the eyes' distance in tenths of a mm
// (4 to 9 cm).
constexpr ValueRange kFocusDistance{10, 1000, 1};
constexpr ValueRange kFocusRange{5, 100000, 1};
constexpr ValueRange kFocusSpeed{10, 1000, 1};
constexpr ValueRange kFocusSensitivity{50, 200, 1};
constexpr ValueRange kFocusAccuracy{1, 128, 1};
constexpr ValueRange kFocusBaseline{10, 120, 1};
constexpr ValueRange kFocusOffset{-45, 45, 1};
constexpr ValueRange kIpd{400, 900, 1};

}  // namespace value_range
