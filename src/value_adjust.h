#pragma once

// Adjusting a value row (Menu::GroupLayout::value()): Magnify, the menu
// fading away while a value is held (menu_adjust.cpp), and the thumbstick's
// steps (main.cpp). Every timing and size of them, to tune.
// Sizes are layout pixels, times seconds.
namespace adjust {

// Pressing anywhere on a value's field changes nothing: Magnify opens at
// once (for a value with fine steps: kMagnifyBelow), centred on the laser
// with the value as it is under it, and the value follows the laser from
// there.

// The magnified scale: kLoupeW wide (at most the page's width less
// kLoupeMargin each side), kLoupeH high, centred on the laser and the row.
constexpr double kLoupeW = 640, kLoupeH = 128, kLoupeMargin = 16;
constexpr double kLoupeCorner = 22;
// Its surface drawn this far around it too, under its outline (drawLoupe()).
constexpr double kLoupeUnder = 4;
// Its ruler, kRulerInset in from its sides, kRulerTop down, kRulerH high;
// the value above it, kValueSize, its labels kLabelSize. The setting's name
// at its top left, kNameSize, at most kNameShare of its width; the value
// keeps kNameGap clear of it.
constexpr double kRulerInset = 16, kRulerTop = 60, kRulerH = 56, kRulerCorner = 10;
constexpr int kValueSize = 28, kLabelSize = 19, kNameSize = 22;
constexpr double kNameShare = 0.45, kNameGap = 16;
// It shows about one step every kStepPx across, never fewer than kMinSteps.
constexpr double kStepPx = 24;
constexpr int kMinSteps = 10;
// Only a value whose row's own field gives a step less than kMagnifyBelow
// (a logarithmic one: at its top end, where its steps are narrowest) opens
// the scale; one with fewer, wider steps (Resolution's stops on the Frame)
// is dragged along its field, snapping to them.
constexpr double kMagnifyBelow = kStepPx;
// Within kEdge of either end of the ruler it slides on, at most kPanMost
// times kPanRate of what it shows each second, the faster the nearer the end.
// Kept at an end longer than kPanBoostAfter it speeds up, twice as fast
// every kPanDouble, until it would cross the whole range in kPanRangeTime:
// a far value is a moment away, a near one still easy to stop at.
constexpr double kEdge = 56, kPanMost = 1.5, kPanRate = 0.9;
constexpr double kPanBoostAfter = 0.5, kPanDouble = 0.5, kPanRangeTime = 2;

// While a value is held the rest of the menu fades away over kFadeSeconds
// (and comes back as fast on letting go), all but Magnify's scale; a value
// that doesn't magnify keeps its row, on a surface of its own, its corners
// kRowCorner (shaders/panel.frag).
constexpr double kFadeSeconds = 0.16;
constexpr double kRowCorner = 14;

// The thumbstick on a pointed value: pushed sideways past kStickPress, a
// step down or up; held, more after kStickDelay, one every kStickRepeat.
// It stays the value's (nothing else takes it) until both sticks are back
// within kStickRelease.
constexpr float kStickPress = 0.6f, kStickRelease = 0.3f;
constexpr double kStickDelay = 0.4, kStickRepeat = 0.06;

}  // namespace adjust
