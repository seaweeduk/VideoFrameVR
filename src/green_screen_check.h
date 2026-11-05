#pragma once

#include "video_format.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

// Whether a video was shot in front of a green screen, so that passthrough
// can switch itself on (autoPassthrough(), see_through.h). A worker thread
// with its own demuxer decodes a few keyframes spread through the video (20,
// 40, 60 and 80 %: intros often have no green), shrunk in YUV, and looks for
// a keying green in each eye: saturated and of one hue (shading changes its
// strength, not its hue), covering much of the picture, along its top and
// sides, the same in both eyes and in every sample. Natural greens (grass,
// leaves) are far duller and never come near. It errs towards missing: at
// most one sample may fail, and two failures end the check early, so most
// videos cost two keyframes (about 6 s of two low-priority threads on the
// Frame for 8K). Never on the render loop. Main thread only.
class GreenScreenCheck {
 public:
  // Stored with each result: a newer check runs again on videos the older
  // one looked at.
  static constexpr int kVersion = 1;

  GreenScreenCheck() = default;
  GreenScreenCheck(const GreenScreenCheck&) = delete;
  GreenScreenCheck& operator=(const GreenScreenCheck&) = delete;
  ~GreenScreenCheck();

  // Checks this video (a local file or a remote one), replacing a check in progress.
  void start(const std::string& path, StereoLayout stereo);
  // A finished check, once: its video and whether it has a green screen.
  bool result(std::string& path, bool& found);

 private:
  struct Job {
    std::string path;
    StereoLayout stereo = StereoLayout::Mono;
    uint64_t generation = 0;
  };
  void work();

  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool quit_ = false;
  Job job_;                       // to do; empty path = nothing
  std::atomic<uint64_t> generation_{0};  // of the latest start(); a newer one cancels
  bool done_ = false;             // a result for donePath_
  std::string donePath_;
  bool doneFound_ = false;
};
