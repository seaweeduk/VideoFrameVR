#pragma once

#include "flat_screen.h"
#include "video_format.h"
#include "video_memory.h"

#include <functional>
#include <string>

class Library;
class VideoPlayer;
struct Settings;

void printFormat(const VideoFormat& f, const char* reason);

// The running video: its file, format, screen and what is remembered about
// it, and the changes to it and to the volume and Sharpen that are stored a
// moment later rather than at every step. Main thread only.
class RunningVideo {
 public:
  RunningVideo(VideoPlayer& player, Settings& settings, Library& library)
      : player(player), settings(settings), library(library) {}

  std::string currentFile;
  std::string streamOshash;  // a Stash stream playing: its file's oshash as opened
  ScreenPlacement screen;
  VideoFormat format;
  bool formatChosenByUser = false;  // stops auto-detection from overriding the user
  bool rememberFormat = false;      // the user changed the format by key: store it
  VideoSettings video;  // what is remembered about the running video
  // Settings (VideoSettings::Part) of the running video changed in a run of
  // small steps (a slider, a held stick, a drag), stored at glfwGetTime()
  // saveAt; 0 = none waiting.
  unsigned unsavedParts = 0;
  double saveAt = 0;
  bool screenChanged = false;  // by key: keep the flat screen's new size
  bool flipVideo = false;

  // After a covered setting changes: the preset they now match, else
  // Custom; set once, before the frames.
  std::function<void()> matchPreset;

  void zoomBy(double percent);
  // Stores these parts (VideoSettings::Part) of the running video's
  // settings: only what the user changed, so the rest of a record still
  // being found stays as it is.
  void storeVideo(unsigned parts);
  // Keeps the volume for all videos, as mpv reports it: a second after it
  // changes, so at most once a second while it keeps changing. Not before
  // mpv has reported the one set at start, so its default can't overwrite it.
  void keepVolume();
  // Stores these parts soon, with whatever else changes meanwhile: at most
  // twice a second while they keep changing.
  void storeVideoSoon(unsigned parts);
  // Stores what storeVideoSoon() still holds back.
  void saveUnsaved();
  // The flat screen's size was changed: for all videos.
  void keepScreenSize();
  // Puts the running video's remembered settings into effect.
  void applyVideoSettings();
  // At once, also while a slider is held (mpv takes them asynchronously,
  // direct frames recompute their colour matrix); stored soon, not per step.
  void applyPicture(unsigned parts);
  // Sharpen changed in a run of steps (its slider, a twist): matched and
  // saved half a second after the first, at most twice a second.
  void keepSharpenSoon();
  void keepSharpen();
  // Each frame: what is due to be stored. `zoomHeld`: a zoom is still held.
  void saveDue(bool zoomHeld);

 private:
  VideoPlayer& player;
  Settings& settings;
  Library& library;

  bool volumeApplied = false;
  double volumeSaveAt = 0;
  double sharpenSaveAt = 0;  // 0 = none
};
