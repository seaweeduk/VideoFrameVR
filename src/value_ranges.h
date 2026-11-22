#pragma once

// The values the menu's sliders set, each a range and a step in the units
// its MenuCommand carries; the gestures that set the same values
// (controls.h) go by them too.

struct ValueRange {
  int least, most, step;
};

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
constexpr ValueRange kPercent{0, 100, 1};  // opacity, Match the room's strengths, the key's sliders
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
constexpr ValueRange kSubtitleSize{50, 200, 1};        // percent
constexpr ValueRange kSubtitleRaise{-100, 100, 1};     // percent of the picture
constexpr ValueRange kSubtitleDistance{25, 600, 1};    // centimetres

}  // namespace value_range
