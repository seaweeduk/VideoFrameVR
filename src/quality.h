#pragma once

#include "anti_shimmer.h"
#include "foveation_level.h"
#include "menu_command.h"
#include "presets.h"

#include <future>
#include <string>
#include <vector>

class EyeTargets;
class Foveation;
class FrameReuse;
class GpuContext;
class VideoPlayer;
class XrContext;
struct GpuCapabilities;
struct MenuView;
struct Options;
struct Settings;

// The quality settings: Resolution, foveation, the performance presets and
// the user's named presets, the refresh rate, Anti-shimmer, frame reuse,
// and the other settings the presets cover. Main thread only.
class Quality {
 public:
  // After the GPU: the profile's resolution and preset settled, and
  // SteamVR's rate for this app taken (`steamVrRate`, 0: none).
  Quality(const Options& opts, Settings& settings, XrContext& xr, Foveation& foveation,
          FrameReuse& frameReuse, const GpuCapabilities& caps, bool refreshRateAvailable,
          int steamVrRate);

  // The eye images at the size asked for, and their foveation.
  bool createEyeImages(GpuContext& gpu, bool foveationAsked);
  void reportAntiShimmer(const char* from);
  // After a covered setting changes: the preset they now match, else Custom.
  void matchPreset();
  // The command, if it is one of these settings'; false for any other.
  bool handle(const MenuCommand& c, VideoPlayer& player);
  // A changed foveation, applied before xrWaitFrame. False if the eye
  // targets could not follow.
  bool beforeWaitFrame(EyeTargets& eyeTargets);
  // After the menu's commands: a Resolution that settled is switched to.
  void afterMenu(MenuCommand::Type draggingSlider);
  // --resolution-cycle: eye pixels across, as the command line's size.
  void overrideResolution(int pixels);
  // Once at startup, after the command line's file is opened: the first
  // listing of the user's presets.
  void startUserPresetListing();
  // Each frame, before fill(): a listing of the user's presets that arrived.
  void pollUserPresets();
  void fill(MenuView& view) const;

  AntiShimmer drawnAntiShimmer() const { return antiShimmer; }
  bool foveationShown() const { return showFoveation; }

 private:
  int chosenResolution() const;
  float pixelScale(int pixels) const;
  float askedScale() const;
  void applyPendingResolution();
  void setRefreshRate(int hz);
  void applyPreset(const Settings& p, VideoPlayer& player);

  const Options& opts;
  Settings& settings;
  XrContext& xr;
  Foveation& foveation;
  FrameReuse& frameReuse;
  const bool refreshRateAvailable;
  // The eye images' size counts from SteamVR's recommendation, on the Frame
  // from its panel (eye_resolution.h).
  const std::vector<int> resolutionStops;
  const Presets presets;
  const int resolutionBase;

  // What this run uses; the command line's overrides stay out of settings.
  FoveationLevel chosenFoveation = FoveationLevel::Off;
  int sharpBins = 0, foveationOutside = 0;
  bool foveationRing = false;
  bool showFoveation = false;
  // The eye size asked for, as a scale of 1x: the command line's for this
  // run (--resolution, --resolution-cycle) until the menu's Resolution is
  // changed, else the setting. Applied while running (XrContext::requestEyeScale).
  float overrideScale = 0;
  // The Anti-shimmer this run draws with: the command line's until the menu
  // changes it. Pushed with each video draw, so a change applies from the next frame.
  AntiShimmer antiShimmer = AntiShimmer::On;

  // The Resolution row's choice: shown at once, switched to (and saved)
  // once it settles: a pause after its last change, also while its slider
  // is held, so a drag across several stops or several clicks make one
  // switch; at once on letting go. 0 = none.
  static constexpr double kResolutionSettle = 0.35;
  int pendingResolution = 0;
  double pendingResolutionAt = 0;
  bool resolutionHeld = false;
  bool pendingFoveation = false;
  // The user's named presets as last listed, and the listing on its way;
  // the one picked last (its name), and the one shown as chosen.
  std::vector<UserPreset> userPresets;
  std::future<std::vector<UserPreset>> userPresetsListing;
  int userPresetsListed = 0;
  std::string pickedUserPreset;
  int userPresetShown = -1;
};
