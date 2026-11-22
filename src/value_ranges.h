#pragma once

// The values the menu's sliders set, each a range and a step in the units
// its MenuCommand carries; the gestures that set the same values
// (controls.h) go by them too.

#include "flat_screen.h"
#include "script_player.h"
#include "video_memory.h"

struct ValueRange {
  int least, most, step;
};

namespace value_range {

constexpr int hundredths(float v) { return static_cast<int>(v * 100 + 0.5f); }

constexpr ValueRange kVolume{0, 100, 1};   // the strip's slider, of mpv's 0..130
constexpr ValueRange kPercent{0, 100, 1};  // opacity, Match the room's strengths, the key's sliders
// Brightness, contrast, saturation (mpv's), warmth, tint and exposure.
constexpr ValueRange kPicture{-100, 100, 1};
constexpr ValueRange kSharpen{0, 300, 1};  // hundredths
constexpr ValueRange kZoom{VideoSettings::kMinZoomPercent, VideoSettings::kMaxZoomPercent,
                           VideoSettings::kZoomStepPercent};
// The surround viewpoint, in percent of the sphere's radius toward the picture.
constexpr ValueRange kViewpoint{-hundredths(VideoSettings::kMaxOriginForward),
                                hundredths(VideoSettings::kMaxOriginForward), 1};
// The flat screen: centimetres, and its curve in hundredths.
constexpr ValueRange kScreenHeight{hundredths(ScreenSize::kMinHeight), hundredths(ScreenSize::kMaxHeight), 1};
constexpr ValueRange kScreenDistance{hundredths(ScreenSize::kMinDistance),
                                     hundredths(ScreenSize::kMaxDistance), 1};
constexpr ValueRange kScreenCurve{0, hundredths(ScreenSize::kMaxCurve), 1};
constexpr ValueRange kScriptOffset{-kScriptOffsetLimit, kScriptOffsetLimit, 10};  // milliseconds
constexpr ValueRange kSubtitleSize{50, 200, 1};        // percent
constexpr ValueRange kSubtitleRaise{-100, 100, 1};     // percent of the picture
constexpr ValueRange kSubtitleDistance{25, 600, 1};    // centimetres

}  // namespace value_range
