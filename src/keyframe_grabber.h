#pragma once

#include "preview_pace.h"
#include "preview_source.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

class CairoCanvas;
class Uploader;

// The seek preview's pictures where they are decoded on the CPU (the Frame:
// playback needs its one hardware decoder): the keyframe before the moment
// pointed at, read and shrunk on a worker (keyframe_reader.h) and put into a
// canvas's image here. The worker steps back behind the headset's
// frames (Background::Decoding).
// A picture takes a few tenths of a second of an 8K video, so they are
// asked for sparingly (preview_pace.h), and the video is opened before it
// is pointed at: a file at once, one on a network share or a stream once
// playback has read as far ahead as it keeps. Until then playback's own
// reads come first, and opening took 8 s instead of 0.7.
class KeyframeGrabber : public PreviewSource {
 public:
  // Pictures are shrunk to fit maxWidth × maxHeight. steady: the seek bar's
  // dead zone, of its width (Menu::seekHover()).
  KeyframeGrabber(Uploader& uploader, int maxWidth, int maxHeight, double steady);
  // Stops the worker's reads, and waits for it half a second at most.
  ~KeyframeGrabber() override;
  KeyframeGrabber(const KeyframeGrabber&) = delete;
  KeyframeGrabber& operator=(const KeyframeGrabber&) = delete;

  void setVideo(const std::string& location, bool stream) override;
  void point(double seconds, double fraction) override;
  void update(const PlaybackState& playback) override;
  State state() const override {
    return unavailable_ ? State::Unavailable : hasFrame_ ? State::Ready : State::Loading;
  }
  bool busy() const override { return !path_.empty() && answered_ != asked_; }
  bool baseViewOnly() const override { return true; }
  const Texture* image() const override;
  int width() const override;
  int height() const override;

 private:
  struct Shared;
  using Clock = std::chrono::steady_clock;
  // The worker opens `path` (none if empty), without a picture yet.
  void open(const std::string& path);
  void ask(double seconds);
  void take();
  void rest();

  std::shared_ptr<Shared> shared_;  // with the worker
  std::thread worker_;
  Uploader& uploader_;
  std::unique_ptr<CairoCanvas> canvas_;  // the picture, at its size
  double steady_ = 0;
  PreviewPace pace_;
  std::string video_;  // the playing video (setVideo()), opened or not yet
  bool far_ = false;   // ... on a network share, or a stream: opened once playback allows
  Clock::time_point videoAt_;  // ... since when
  Clock::time_point lastUse_;  // pointed at, or the video changed
  bool unavailable_ = false;   // the playing video gave no picture, and was closed
  std::string path_;           // the one the worker has, or is opening
  double target_ = -1;         // the moment asked for last, seconds
  uint64_t asked_ = 0, answered_ = 0;  // requests of this video, and the last the worker finished
  bool opened_ = false;  // the worker has said whether it opened
  bool hasFrame_ = false;
  bool failed_ = false;
  bool resting_ = false;
};
