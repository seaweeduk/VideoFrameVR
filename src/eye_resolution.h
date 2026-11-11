#pragma once

#include "platform.h"
#include "settings.h"

#include <vector>

struct GpuCapabilities;

// Eye image sizes in percent of what 1× is: on the desktop the size SteamVR
// recommends (which follows its resolution setting for the app), on the
// native Steam Frame build (platform.h) the Frame's panel. OpenXR reports no panel
// size; the Frame's is known, and SteamVR there recommends only 1728 of it.
// New profiles start with the default; one size covers video and menus.
// Change it here.
// Frame: its panel per eye; desktop: 0, 1× = SteamVR's recommendation.
constexpr int kPanelWidth = kStandaloneHeadset ? 2160 : 0;
constexpr int kPanelHeight = kStandaloneHeadset ? 2160 : 0;
// Frame: 3780 (owner, 2026-10-27; docs/frame-plan.md W11).
constexpr int kDefaultVideoResolution = kStandaloneHeadset ? 175 : 100;
// Changing the size while running (docs/vulkan-renderer.md, 5.5): once a
// larger size is asked for, the eye images are made at least this large, so
// later changes up to it are instant. Frame: 3780, past the 2592 / 3456 /
// 3760 comparison; desktop: 1× is already SteamVR's 1:1 estimate there.
constexpr int kInstantResolution = kStandaloneHeadset ? 175 : 125;

// What may be kept in settings.conf (a converted render_scale may fall
// outside the menu's range).
constexpr int kMinResolution = 25, kMaxResolution = 600;
// The desktop menu's range and step: its slider and its − and +.
constexpr int kResolutionLow = 50, kResolutionHigh = 300, kResolutionStep = 1;

// The next multiple of the step up (direction > 0) or down, within the
// menu's range; a value already outside it stays.
int nextResolutionStep(int percent, int direction);

// Frame menu/Auto sizes in pixels, from this GPU's bin model; empty on desktop
// or an unknown GPU. Command-line sizes do not use these stops.
std::vector<int> eyeResolutionStops(const GpuCapabilities& caps);

// Converts old render_scale values, then snaps Frame settings to the nearest
// stop. True if settings changed and should be saved.
bool settleEyeResolution(Settings& settings, int recommendedWidth, const std::vector<int>& stops = {});
