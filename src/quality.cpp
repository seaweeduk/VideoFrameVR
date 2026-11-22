#include "quality.h"

#include "eye_resolution.h"
#include "eye_targets.h"
#include "file_util.h"
#include "foveation.h"
#include "frame_reuse.h"
#include "gpu_context.h"
#include "menu_view.h"
#include "options.h"
#include "platform.h"
#include "refresh_rate.h"
#include "settings.h"
#include "video_player.h"
#include "xr_context.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

Quality::Quality(const Options& opts, Settings& settings, XrContext& xr, Foveation& foveation,
                 FrameReuse& frameReuse, const GpuCapabilities& caps, bool refreshRateAvailable,
                 int steamVrRate)
    : opts(opts), settings(settings), xr(xr), foveation(foveation), frameReuse(frameReuse),
      refreshRateAvailable(refreshRateAvailable), resolutionStops(eyeResolutionStops(caps)),
      presets(resolutionStops), resolutionBase(kPanelWidth > 0 ? kPanelWidth : xr.recommendedEyeWidth()) {
  // A profile from before is settled here.
  if (settleEyeResolution(settings, xr.recommendedEyeWidth(), resolutionStops)) saveSettings(settings);
  if (presets.settle(settings)) saveSettings(settings);
  // A rate chosen in SteamVR's per-app page shows here; another than the
  // menu's choices too, none of them lit.
  if (steamVrRate > 0 && steamVrRate != settings.refreshRate) {
    settings.refreshRate = steamVrRate;
    settings.performancePreset = presets.matching(settings);
    saveSettings(settings);
  }
  if (refreshRateAvailable)
    std::printf(steamVrRate > 0 ? "Refresh rate: %d Hz, SteamVR's for this app\n"
                                : "Refresh rate: %d Hz, set in SteamVR for this app (it had none)\n",
                settings.refreshRate);
  else if (kStandaloneHeadset)
    std::printf("Refresh rate: not started from Steam, left to SteamVR\n");
  antiShimmer = opts.antiShimmerSet ? opts.antiShimmer : settings.antiShimmer;
}

void Quality::startUserPresetListing() { userPresetsListing = listUserPresets(); }

int Quality::chosenResolution() const {
  return resolutionStops.empty() ? settings.videoResolution : settings.videoResolutionPixels;
}

// Half a pixel more, so the size is not floored to one less.
float Quality::pixelScale(int pixels) const { return (pixels + 0.5f) / resolutionBase; }

float Quality::askedScale() const {
  return overrideScale > 0 ? overrideScale
         : resolutionStops.empty() ? settings.videoResolution / 100.0f
                                   : pixelScale(settings.videoResolutionPixels);
}

bool Quality::createEyeImages(GpuContext& gpu, bool foveationAsked) {
  chosenFoveation = opts.foveationSet ? opts.foveation : settings.foveation;
  if (opts.resolutionPixels > 0) overrideScale = pixelScale(opts.resolutionPixels);
  if (opts.resolutionPercent > 0) overrideScale = static_cast<float>(opts.resolutionPercent / 100);
  if (overrideScale > 0)
    std::printf("Eye resolution: %d pixels across for this run (--resolution)\n",
                static_cast<int>(std::floor(resolutionBase * overrideScale)));
  if (kPanelWidth > 0)
    std::printf("Eye resolution: 1x is the panel's %dx%d per eye\n", kPanelWidth, kPanelHeight);
  else
    std::printf("Eye resolution: 1x is the runtime's recommended %dx%d per eye\n",
                xr.recommendedEyeWidth(), xr.recommendedEyeHeight());
  xr.preferAppDensityMaps(!opts.runtimeDensityMaps);
  if (!xr.createEyeImages(gpu, kPanelWidth, kPanelHeight, askedScale(), kInstantResolution / 100.0f))
    return false;
  if (foveationAsked && !xr.foveated()) {
    std::fprintf(stderr, "--foveation=%s: this headset, runtime or GPU cannot foveate\n",
                 foveationLevelName(opts.foveation));
    return false;
  }
  sharpBins = settings.sharpBins;
  foveationOutside = settings.foveationOutside;
  foveationRing = settings.foveationRing;
  if (opts.foveationSet) foveationPreset(chosenFoveation, sharpBins, foveationRing, foveationOutside);
  if (opts.sharpBins) sharpBins = opts.sharpBins;
  if (opts.ring >= 0) foveationRing = opts.ring != 0;
  chosenFoveation = foveationLevel(chosenFoveation != FoveationLevel::Off, sharpBins, foveationRing, foveationOutside);
  foveation.init(xr, chosenFoveation, opts.foveationGaze, sharpBins, foveationRing, foveationOutside);
  showFoveation = foveation.active() && (opts.showFoveation || settings.showFoveation);
  if (opts.showFoveation && !foveation.active()) std::printf("Show foveation: Steam Frame only\n");
  return true;
}

void Quality::reportAntiShimmer(const char* from) {
  const int taps = antiShimmerTaps(antiShimmer);
  std::printf("Anti-shimmer: %s, at most %dx%d taps per pixel with mipmaps off (%s)\n",
              antiShimmerName(antiShimmer), taps, taps, from);
}

void Quality::matchPreset() {
  settings.performancePreset = presets.matching(settings);
  userPresetShown = presets.shownUserPreset(userPresets, settings, pickedUserPreset);
}

void Quality::applyPendingResolution() {
  const int value = pendingResolution;
  pendingResolution = 0;
  if (value == chosenResolution() && overrideScale == 0) return;
  settings.videoResolutionPixels = resolutionStops.empty() ? 0 : value;
  settings.videoResolution = resolutionStops.empty()
                                 ? value : static_cast<int>(std::lround(value * 100.0 / resolutionBase));
  settings.resolutionNotice = false;
  matchPreset();
  saveSettings(settings);
  overrideScale = 0;  // the command line's size gives way to the setting
  xr.requestEyeScale(askedScale());
}

// Off the frame loop: vrcmd runs on the config queue; SteamVR switches
// within about 2 s.
void Quality::setRefreshRate(int hz) {
  if (hz == settings.refreshRate) return;
  settings.refreshRate = hz;
  if (refreshRateAvailable) queueConfigWork([hz] { writeAppRefreshRate(hz); });
}

// A preset's settings, live as their own rows apply them.
void Quality::applyPreset(const Settings& p, VideoPlayer& player) {
  if (p.foveation != chosenFoveation || p.sharpBins != sharpBins ||
      p.foveationRing != foveationRing || p.foveationOutside != foveationOutside) {
    chosenFoveation = p.foveation;
    sharpBins = p.sharpBins;
    foveationRing = p.foveationRing;
    foveationOutside = p.foveationOutside;
    pendingFoveation = true;  // applied before xrWaitFrame
  }
  settings.videoMipmaps = p.videoMipmaps;
  player.setMipmaps(settings.videoMipmaps);
  settings.antiShimmer = p.antiShimmer;
  if (antiShimmer != settings.antiShimmer) {
    antiShimmer = settings.antiShimmer;
    reportAntiShimmer("preset");
  }
  settings.sharpen = p.sharpen;
  player.setSharpened(settings.sharpen > 0);
  settings.toneLook = p.toneLook;
  if (settings.tvGamma != p.tvGamma) {
    settings.tvGamma = p.tvGamma;
    player.setTvGamma(settings.tvGamma);
  }
  settings.focus = p.focus;
  setRefreshRate(p.refreshRate);
  pendingResolution = resolutionStops.empty() ? p.videoResolution : p.videoResolutionPixels;
  applyPendingResolution();
  settings.performancePreset = presets.matching(p);
  userPresetShown = presets.shownUserPreset(userPresets, p, pickedUserPreset);
  saveSettings(settings);
}

bool Quality::handle(const MenuCommand& c, VideoPlayer& player) {
  switch (c.type) {
    case MenuCommand::ToggleTvGamma:
      settings.tvGamma = !settings.tvGamma;
      player.setTvGamma(settings.tvGamma);
      matchPreset();
      saveSettings(settings);
      break;
    case MenuCommand::ToggleVideoMipmaps:
      settings.videoMipmaps = !settings.videoMipmaps;
      matchPreset();
      saveSettings(settings);
      player.setMipmaps(settings.videoMipmaps);
      break;
    case MenuCommand::SetAntiShimmer:
      settings.antiShimmer = static_cast<AntiShimmer>(
          std::clamp(static_cast<int>(c.value), 0, kAntiShimmerCount - 1));
      matchPreset();
      saveSettings(settings);
      if (settings.antiShimmer == antiShimmer) break;
      antiShimmer = settings.antiShimmer;
      reportAntiShimmer("menu");
      break;
    case MenuCommand::SetVideoResolution: {  // applied once it settles (applyPendingResolution)
      // Frame sends a stop index, desktop a percent; the held slider repeats it.
      const int choice = static_cast<int>(c.value);
      const int value = resolutionStops.empty()
                            ? std::clamp(choice, kMinResolution, kMaxResolution)
                            : resolutionStops[std::clamp(choice, 0, static_cast<int>(resolutionStops.size()) - 1)];
      if (value == (pendingResolution > 0 ? pendingResolution : chosenResolution()) &&
          (pendingResolution > 0 || overrideScale == 0))
        break;
      pendingResolution = value;
      pendingResolutionAt = glfwGetTime() + kResolutionSettle;
      break;
    }
    case MenuCommand::SetFoveation:  // applied before xrWaitFrame
      chosenFoveation = static_cast<FoveationLevel>(std::clamp(static_cast<int>(c.value), 0, 3));
      foveationPreset(chosenFoveation, sharpBins, foveationRing, foveationOutside);
      pendingFoveation = true;
      break;
    case MenuCommand::SetRefreshRate:  // value: Hz
      setRefreshRate(static_cast<int>(c.value));
      matchPreset();
      saveSettings(settings);
      break;
    case MenuCommand::ApplyPreset: {  // value: the PerformancePreset
      const auto preset = static_cast<PerformancePreset>(static_cast<int>(c.value));
      if (!presets.offered(preset)) break;
      pickedUserPreset.clear();
      std::printf("Performance preset: %s\n", performancePresetName(preset));
      applyPreset(presets.with(preset, settings), player);
      break;
    }
    case MenuCommand::ListUserPresets: userPresetsListing = listUserPresets(); break;
    case MenuCommand::SaveUserPreset:
      pickedUserPreset = presetName(c.path);
      userPresetsListing = saveUserPreset(c.path, settings);
      break;
    case MenuCommand::ApplyUserPreset:
      for (const UserPreset& preset : userPresets) {
        if (preset.file != c.path) continue;
        pickedUserPreset = preset.name;
        applyPreset(presets.with(preset, settings), player);
        break;
      }
      break;
    case MenuCommand::RenameUserPreset: {
      const size_t line = c.path.find('\n');
      if (line == std::string::npos) break;
      for (const UserPreset& preset : userPresets)
        if (preset.file == c.path.substr(0, line) && preset.name == pickedUserPreset)
          pickedUserPreset = presetName(c.path.substr(line + 1));
      userPresetsListing = renameUserPreset(c.path.substr(0, line), c.path.substr(line + 1));
      break;
    }
    case MenuCommand::DeleteUserPreset: userPresetsListing = deleteUserPreset(c.path); break;
    case MenuCommand::MoveUserPreset:
      userPresetsListing = moveUserPreset(c.path, static_cast<int>(c.value));
      break;
    case MenuCommand::ToggleShowFoveation:
      if (!foveation.active()) break;
      settings.showFoveation = showFoveation = !showFoveation;
      saveSettings(settings);
      break;
    case MenuCommand::ToggleFoveationLearned:
      if (!foveation.active()) break;
      settings.foveationLearned = !settings.foveationLearned;
      saveSettings(settings);
      break;
    case MenuCommand::SetSharpBins: {
      const int bins = c.value == 1 ? 1 : 3;
      if (bins == sharpBins) break;
      settings.sharpBins = sharpBins = bins;
      pendingFoveation = true;
      break;
    }
    case MenuCommand::ToggleFoveationRing:
      settings.foveationRing = foveationRing = !foveationRing;
      pendingFoveation = true;
      break;
    case MenuCommand::SetFoveationOutside:
      foveationOutside = c.value == 2 ? 2 : 4;
      pendingFoveation = true;
      break;
    case MenuCommand::ToggleToneLook:
      settings.toneLook = !settings.toneLook;
      matchPreset();
      saveSettings(settings);
      break;
    case MenuCommand::ToggleFrameReuse:
      settings.frameReuse = !frameReuse.enabled();
      saveSettings(settings);
      frameReuse.setEnabled(kStandaloneHeadset && settings.frameReuse);
      frameReuse.forget();  // nothing drawn before the switch is shown again
      std::printf("Frame reuse: %s\n", frameReuse.enabled() ? "on" : "off");
      break;
    default: return false;
  }
  return true;
}

bool Quality::beforeWaitFrame(EyeTargets& eyeTargets) {
  if (pendingFoveation) {
    pendingFoveation = false;
    settings.foveation = chosenFoveation = foveationLevel(chosenFoveation != FoveationLevel::Off,
                                                         sharpBins, foveationRing, foveationOutside);
    settings.sharpBins = sharpBins;
    settings.foveationRing = foveationRing;
    settings.foveationOutside = foveationOutside;
    foveation.setLevel(chosenFoveation);
    foveation.setShape(sharpBins, foveationRing, foveationOutside);
    matchPreset();
    saveSettings(settings);
    foveation.prepare();
    if (!eyeTargets.update()) return false;
  }
  return true;
}

void Quality::afterMenu(MenuCommand::Type draggingSlider) {
  const bool resolutionWasHeld = resolutionHeld;
  resolutionHeld = draggingSlider == MenuCommand::SetVideoResolution;
  if (pendingResolution > 0 && ((resolutionWasHeld && !resolutionHeld) || glfwGetTime() >= pendingResolutionAt))
    applyPendingResolution();
}

void Quality::overrideResolution(int pixels) {
  overrideScale = pixelScale(pixels);
  xr.requestEyeScale(overrideScale);
}

void Quality::pollUserPresets() {
  if (userPresetsListing.valid() &&
      userPresetsListing.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    std::vector<UserPreset> listed = userPresetsListing.get();
    // The menu redraws for a listing only if something in it changed.
    if (!userPresetsListed || !sameUserPresets(listed, userPresets)) {
      userPresets = std::move(listed);
      ++userPresetsListed;
    }
    userPresetShown = presets.shownUserPreset(userPresets, settings, pickedUserPreset);
  }
}

void Quality::fill(MenuView& view) const {
  view.tvGamma = settings.tvGamma;
  view.videoMipmaps = settings.videoMipmaps;
  view.antiShimmer = antiShimmer;
  view.sharpen = settings.sharpen;
  view.toneLook = settings.toneLook;
  view.videoResolution = pendingResolution > 0 ? pendingResolution : chosenResolution();
  view.resolutionStops = resolutionStops.empty() ? nullptr : &resolutionStops;
  view.videoResolutionActive = static_cast<int>(std::lround(askedScale() * 100));
  view.videoResolutionEffective = static_cast<int>(std::lround(xr.requestedEyeScale() * 100));
  view.resolutionOverride =
      overrideScale > 0 ? static_cast<int>(std::floor(resolutionBase * overrideScale)) : 0;
  view.resolutionBase = resolutionBase;
  view.foveationAvailable = foveation.active();
  view.foveation = foveation.level();
  view.showFoveation = showFoveation;
  view.foveationLearned = settings.foveationLearned;
  view.sharpBins = sharpBins;
  view.foveationRing = foveationRing;
  view.foveationOutside = foveationOutside;
  view.frameReuse = frameReuse.enabled();
  view.refreshRate = refreshRateAvailable ? settings.refreshRate : 0;
  view.performancePreset = settings.performancePreset;
  view.userPresets = &userPresets;
  view.userPresetsListed = userPresetsListed;
  view.userPresetShown = userPresetShown;
  view.presets = &presets;
  view.foveationMaps = foveation.equivalentMaps();
  view.sharpDegrees = foveation.binDegrees();
  view.resolutionNotice = settings.resolutionNotice;
}
