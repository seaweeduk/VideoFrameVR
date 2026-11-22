#include "running_video.h"

#include "library.h"
#include "playback_state.h"
#include "settings.h"
#include "video_player.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

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
