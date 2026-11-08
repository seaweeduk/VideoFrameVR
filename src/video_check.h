#pragma once

#include "video_format.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

// Looks at a few keyframes of a video, once, on a worker thread with its own
// demuxer, for one of two things. Never on the render loop. Main thread only.
//
// A green screen, so that passthrough can switch itself on (autoPassthrough(),
// see_through.h): keyframes spread through the video (20, 40, 60 and 80 %:
// intros often have no green), shrunk in YUV, are searched for a keying green
// in each eye: saturated and of one hue (shading changes its strength, not its
// hue), covering much of the picture, along its top and sides, the same in
// both eyes and in every sample. Natural greens (grass, leaves) are far duller
// and never come near. It errs towards missing: at most one sample may fail,
// and two failures end the check early, so most videos cost two keyframes
// (about 6 s of two low-priority threads on the Frame for 8K).
//
// A packed mask's second layer (SLR's layered masks, VideoSettings::
// maskLayersFound): a saturated blue, with neither red nor green, in both
// eyes' masks. Older masks never have it: red ones only bleed a little blue
// at their edges, and white ones have as much red and green as blue. The
// first keyframe that has it ends the check (layered videos so far show it
// by a quarter of the way in); one without looks at six. A layer that first
// shows later is found while the video plays (MaskProbe).
class VideoCheck {
 public:
  enum class Kind { GreenScreen, MaskLayers };
  // Stored with each result: a newer check runs again on videos the older
  // one looked at.
  static constexpr int kGreenVersion = 1;
  static constexpr int kLayersVersion = 1;

  VideoCheck() = default;
  VideoCheck(const VideoCheck&) = delete;
  VideoCheck& operator=(const VideoCheck&) = delete;
  ~VideoCheck();

  // Checks this video (a local file or a remote one), replacing a check in progress.
  void start(const std::string& path, StereoLayout stereo, Kind kind);
  // A finished check, once: its video, what it looked for and whether it found it.
  struct Result {
    std::string path;
    Kind kind = Kind::GreenScreen;
    bool found = false;
  };
  bool result(Result& out);

 private:
  struct Job {
    std::string path;
    StereoLayout stereo = StereoLayout::Mono;
    Kind kind = Kind::GreenScreen;
    uint64_t generation = 0;
  };
  void work();

  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool quit_ = false;
  Job job_;                       // to do; empty path = nothing
  std::atomic<uint64_t> generation_{0};  // of the latest start(); a newer one cancels
  bool done_ = false;             // a result in done_
  Result result_;
};
