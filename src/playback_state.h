#pragma once

#include "video_format.h"

#include <cstdint>
#include <string>
#include <vector>

struct Track {
  int64_t id = 0;
  std::string type;  // "audio", "sub" or "video"
  std::string title;
  std::string lang;
  std::string codec;
  bool selected = false;
  int64_t sourceId = -1;  // the container's ID for it (a transport stream's PID), -1 if none
};

struct Chapter {
  double time = 0;  // seconds
  std::string title;
};

struct PlaybackState {
  double position = 0;  // seconds
  double duration = 0;  // seconds, 0 if unknown
  double bufferedUntil = 0;  // seconds: how far ahead the stream is already downloaded
  bool readAhead = false;  // it has read as far ahead as it keeps, and waits (mpv's demuxer-cache-idle)
  bool paused = false;
  double volume = 100;
  bool muted = false;
  std::string title;
  std::vector<Track> tracks;
  std::string subtitleText;  // the subtitle line showing right now, as plain text
  bool endReached = false;            // stopped at the end of the file
  // What is playing, for the menu's info line.
  std::string videoCodec;   // e.g. "hevc"
  std::string pixelFormat;  // e.g. "p010" (10 bit)
  double fps = 0;
  std::string audioCodec;   // e.g. "opus"
  int audioChannels = 0;
  std::vector<Chapter> chapters;
  int chapter = -1;  // the current one, -1 = before the first / none
  unsigned chaptersRevision = 0;  // counts the times chapters changed
  TwoViews views = TwoViews::No;  // 3D Blu-ray (MVC) video: as mpv decodes it
};
