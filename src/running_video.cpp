#include "running_video.h"

#include "file_util.h"
#include "focus_settings.h"
#include "library.h"
#include "menu_command.h"
#include "menu_view.h"
#include "passthrough.h"
#include "playback_state.h"
#include "see_through.h"
#include "settings.h"
#include "video_player.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

void printFormat(const VideoFormat& f, const char* reason) {
  std::printf("Format (%s): %s, %s%s%s\n", reason, projectionName(f.projection), stereoName(f.stereo),
              f.packing != Packing::Auto ? ", packing " : "",
              f.packing != Packing::Auto ? packingName(f.packing) : "");
}

void RunningVideo::zoomBy(double percent) {
  video.zoom = std::clamp(video.zoom + static_cast<float>(percent / 100),
                          VideoSettings::kMinZoomPercent / 100.0f,
                          VideoSettings::kMaxZoomPercent / 100.0f);
  screen.zoom = video.zoom;
  unsavedParts |= VideoSettings::kZoom;  // stored with the rest if the video changes first
}

void RunningVideo::storeVideo(unsigned parts) { library.setSettings(currentFile, video, parts); }

void RunningVideo::keepVolume() {
  volumeSaveAt = 0;
  const int volume = static_cast<int>(std::lround(player.playback().volume));
  if (!volumeApplied || volume == settings.volume) return;
  settings.volume = volume;
  saveSettings(settings);
}

void RunningVideo::storeVideoSoon(unsigned parts) {
  unsavedParts |= parts;
  if (saveAt == 0) saveAt = glfwGetTime() + 0.5;
}

void RunningVideo::saveUnsaved() {
  // A zoom being held: in whole percents, as when let go.
  if (unsavedParts & VideoSettings::kZoom) {
    video.zoom = std::round(video.zoom * 100) / 100;
    screen.zoom = video.zoom;
  }
  // The screen's size is a setting for all videos (kScreen only marks it).
  if (const unsigned parts = unsavedParts & ~VideoSettings::kScreen) storeVideo(parts);
  if (unsavedParts & VideoSettings::kScreen) saveSettings(settings);  // settings.screen
  unsavedParts = 0;
  saveAt = 0;
}

void RunningVideo::keepScreenSize() {
  screen.size = screen.size.clamped();
  settings.screen = screen.size;
  storeVideoSoon(VideoSettings::kScreen);
}

void RunningVideo::applyVideoSettings() {
  if (video.hasFormat) {
    format = video.format;
    formatChosenByUser = true;
    printFormat(format, "remembered");
  }
  screen.zoom = video.zoom;
  screen.size = settings.screen;
  player.setPicture(video.brightness, video.contrast, video.saturation);
  player.setTvGamma(settings.tvGamma);
}

void RunningVideo::applyPicture(unsigned parts) {
  if (parts != VideoSettings::kZoom)
    player.setPicture(video.brightness, video.contrast, video.saturation);
  screen.zoom = video.zoom;
  storeVideoSoon(parts);
}

void RunningVideo::keepSharpenSoon() {
  if (sharpenSaveAt == 0) sharpenSaveAt = glfwGetTime() + 0.5;
}

void RunningVideo::keepSharpen() {
  if (sharpenSaveAt == 0) return;
  sharpenSaveAt = 0;
  matchPreset();
  saveSettings(settings);
}

void RunningVideo::saveDue(bool zoomHeld) {
  if (saveAt > 0 && glfwGetTime() >= saveAt) {
    // A zoom still held waits for its letting go.
    const unsigned held = zoomHeld ? unsavedParts & VideoSettings::kZoom : 0;
    unsavedParts &= ~held;
    saveUnsaved();
    unsavedParts |= held;
  }
  if (const int volume = static_cast<int>(std::lround(player.playback().volume));
      !volumeApplied)
    volumeApplied = volume == settings.volume;
  else if (volume != settings.volume && volumeSaveAt == 0)
    volumeSaveAt = glfwGetTime() + 1;
  if (volumeSaveAt > 0 && glfwGetTime() >= volumeSaveAt) keepVolume();
  if (sharpenSaveAt > 0 && glfwGetTime() >= sharpenSaveAt) keepSharpen();
}

void RunningVideo::moveViewpoint(int direction, float dt) {
  constexpr float kMax = VideoSettings::kMaxOriginForward;
  video.originForward = std::clamp(video.originForward + direction * 0.05f * dt, -kMax, kMax);
  storeVideoSoon(VideoSettings::kOrigin);
}

void RunningVideo::applyPassthroughToggle(bool available, Passthrough& passthrough) {
  if (togglePassthrough && !available) {
    // The video keeps its choice for a runtime that shows the room.
    togglePassthrough = false;
    std::printf("Passthrough: the runtime offers no alpha blending, so the camera cannot "
                "show\n");
  }
  if (togglePassthrough) {
    togglePassthrough = false;
    video.passthrough = !video.passthrough;
    video.passthroughChosen = true;  // never switched by itself again
    storeVideo(VideoSettings::kPassthrough);
    std::printf("Passthrough: %s\n", video.passthrough ? "on" : "off");
    passthrough.switchedByHand();
  }
}

bool RunningVideo::handle(const MenuCommand& c, Passthrough& passthrough) {
  switch (c.type) {
    case MenuCommand::SetBrightness:
      video.brightness = static_cast<int>(c.value);
      applyPicture(VideoSettings::kBrightness);
      break;
    case MenuCommand::SetContrast:
      video.contrast = static_cast<int>(c.value);
      applyPicture(VideoSettings::kContrast);
      break;
    case MenuCommand::SetSaturation:
      video.saturation = static_cast<int>(c.value);
      applyPicture(VideoSettings::kSaturation);
      break;
    case MenuCommand::SetWarmth:  // the colour balance: applied while drawing
    case MenuCommand::SetTint:
    case MenuCommand::SetExposure:
      if (const unsigned part = passthrough.setBalance(c)) storeVideoSoon(part);
      break;
    case MenuCommand::SetOpacity:  // applied while drawing
      video.opacity = static_cast<int>(c.value);
      storeVideoSoon(VideoSettings::kOpacity);
      break;
    case MenuCommand::SetZoom:
      video.zoom = static_cast<float>(c.value / 100.0);
      applyPicture(VideoSettings::kZoom);
      break;
    case MenuCommand::ResetPicture:
      video.brightness = video.contrast = video.saturation = 0;
      video.balance = {};
      passthrough.pictureReset();
      video.zoom = 1.0f;
      video.originForward = 0;
      applyPicture(VideoSettings::kPicture | VideoSettings::kOrigin);
      break;
    case MenuCommand::SetOriginForward:
      video.originForward = std::clamp(static_cast<float>(c.value / 100.0),
                                       -VideoSettings::kMaxOriginForward,
                                       VideoSettings::kMaxOriginForward);
      storeVideoSoon(VideoSettings::kOrigin);
      break;
    case MenuCommand::SetScreenDistance:
    case MenuCommand::SetScreenHeight:
    case MenuCommand::SetScreenCurve: {
      ScreenSize& size = screen.size;
      (c.type == MenuCommand::SetScreenDistance ? size.distance
       : c.type == MenuCommand::SetScreenHeight ? size.height
                                                : size.curve) = static_cast<float>(c.value / 100);
      keepScreenSize();
      break;
    }
    case MenuCommand::ResetScreen:
      screen.size = settings.screen = ScreenSize{};
      storeVideoSoon(VideoSettings::kScreen);
      break;
    case MenuCommand::SetSharpen:
      settings.sharpen =
          std::clamp(static_cast<int>(c.value), value_range::kSharpen.least, value_range::kSharpen.most);
      player.setSharpened(settings.sharpen > 0);
      keepSharpenSoon();
      break;
    case MenuCommand::TogglePackedMask: {
      // Nothing is stored when the choice is what the name says.
      const bool on = !packedMask(currentFile, video);
      video.packedMask = on == hasPackedAlpha(currentFile) ? std::nullopt
                                                           : std::optional<bool>(on);
      storeVideo(VideoSettings::kPackedMask);
      std::printf("Passthrough: mask in the video %s\n", on ? "on" : "off");
      break;
    }
    case MenuCommand::ToggleSecondLayer:
      video.secondLayerHidden = !video.secondLayerHidden;
      storeVideo(VideoSettings::kSecondLayer);
      std::printf("Passthrough: the mask's second layer %s\n",
                  video.secondLayerHidden ? "hidden" : "shown");
      break;
    case MenuCommand::ToggleGreenScreen:
    case MenuCommand::SetKeyStrength:
    case MenuCommand::SetKeySoftness:
    case MenuCommand::SetKeySpill:
    case MenuCommand::SetKeyShrink:
    case MenuCommand::SetKeyBackground: {
      GreenScreen& key = video.greenScreen;
      const int value = static_cast<int>(c.value);
      KeySliders& sliders = video.keySliders();
      if (c.type == MenuCommand::ToggleGreenScreen) key.on = !key.on;
      else if (c.type == MenuCommand::SetKeyBackground)
        key.source = value == static_cast<int>(KeySource::GreenScreen) ? KeySource::GreenScreen
                                                                       : KeySource::Automatic;
      else if (c.type == MenuCommand::SetKeyStrength) sliders.strength = value;
      else if (c.type == MenuCommand::SetKeySoftness) sliders.softness = value;
      else if (c.type == MenuCommand::SetKeyShrink) sliders.shrink = value;
      else sliders.spill = value;
      storeVideoSoon(VideoSettings::kGreenScreen);
      break;
    }
    case MenuCommand::ResetPassthrough: {
      // What the group shows: the key's sliders only while the green
      // screen is on (passthrough on, not the video's own mask) and removes
      // something, Match the room's strengths only while it is on.
      passthrough.resetMatch();
      unsigned parts = VideoSettings::kOpacity;
      const bool own = packedMask(currentFile, video) && maskFormat(format);
      if (video.passthrough && video.greenScreen.on && !own && video.keyRemoves()) {
        video.keySliders() = GreenScreen().sliders(video.colourKeyed());
        parts |= VideoSettings::kGreenScreen;
      }
      video.opacity = 100;
      storeVideoSoon(parts);
      break;
    }
    case MenuCommand::TogglePassthrough: togglePassthrough = true; break;
    case MenuCommand::SetFocus:
      if (applyFocusControl(static_cast<FocusControl>(c.option), c.value, video.hasFocus,
                            video.focus, settings.focus, settings.focusPrefs)) {
        matchPreset();  // a user's preset keeps the focus defaults
        saveSettings(settings);
      }
      storeVideo(VideoSettings::kFocus);
      break;
    default: return false;
  }
  return true;
}

void RunningVideo::fill(MenuView& view) const {
  view.format = format;
  view.halfPacked = isHalfPacked(format);
  view.screen = ScreenView::of(screen.size);
  view.currentFile = currentFile;
  view.greenScreen = video.greenScreen;
  view.colourKey = video.colourKeyed();
  view.keyColour = video.keyColour();
  view.fillChecked = video.fillCheckVersion > 0;
  view.greenScreenFound = video.greenCheckVersion > 0 && video.greenScreenFound;
  view.packedAlpha = packedMask(currentFile, video);
  view.maskFormat = maskFormat(format);
  view.maskLayers = video.maskLayersFound;
  view.secondLayerHidden = video.secondLayerHidden;
  view.opacity = video.opacity;
  view.photo = !currentFile.empty() && isPhotoFile(currentFile);
  view.brightness = video.brightness;
  view.contrast = video.contrast;
  view.saturation = video.saturation;
  view.zoom = video.zoom;
  view.originForward = static_cast<int>(std::lround(video.originForward * 100));
}
