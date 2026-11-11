#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>

// Measures how long each part of the frame loop blocks and prints averages
// every two seconds (enabled with --timing).
class FrameTimer {
 public:
  // Fence: waiting for a frame slot's last GPU work; Acquire: for the eye
  // images; Record and Submit: the eye passes' commands.
  enum Section { Events, XrWait, Mpv, Draw, Fence, XrEnd, Swap, Acquire, Record, Submit, kCount };

  explicit FrameTimer(bool enabled) : enabled_(enabled) {}
  void start() { last_ = Clock::now(); }
  void lap(Section s) {
    const auto now = Clock::now();
    totals_[s] += std::chrono::duration<double, std::milli>(now - last_).count();
    last_ = now;
  }
  // Re-attributes part of an already measured section to another one.
  void move(Section from, Section to, double ms) {
    totals_[from] -= ms;
    totals_[to] += ms;
  }
  // One frame's GPU milliseconds (SceneRenderer::GpuTimes), averaged over
  // the frames that have them.
  void gpu(double upload, double glass, double eye0, double eye1, double mirror, double frame) {
    gpu_[0] += upload;
    gpu_[1] += glass;
    gpu_[2] += eye0;
    gpu_[3] += eye1;
    gpu_[4] += mirror;
    gpu_[5] += frame;
    if (frame > gpuMax_) gpuMax_ = frame;
    ++gpuFrames_;
  }
  // The frame's eyes were drawn, not shown again (frame_reuse.h).
  void drew() { ++drawn_; }
  // Vulkan device memory we allocated, in use and waiting to be freed
  // (GpuContext), as of the report.
  void memory(unsigned long long live, unsigned long long retired) {
    live_ = live;
    retired_ = retired;
  }
  // True when it reported (every two seconds, also without --timing).
  bool finishFrame() {
    ++frames_;
    const Clock::time_point now = Clock::now();
    longest_ = std::max(longest_, std::chrono::duration<double, std::milli>(now - lastFinish_).count());
    lastFinish_ = now;
    const double elapsed = std::chrono::duration<double>(Clock::now() - reportStart_).count();
    if (elapsed < 2.0) return false;
    if (enabled_) {
      static const char* names[kCount] = {"events", "xrWait", "mpv",     "draw",   "fence",
                                          "xrEnd",  "swap",   "acquire", "record", "submit"};
      std::printf("[timing] %.1f fps longest %.1f ms |", frames_ / elapsed, longest_);
      for (int i = 0; i < kCount; ++i) std::printf(" %s %.2f ms", names[i], totals_[i] / frames_);
      if (gpuFrames_ > 0)
        std::printf(" | gpu upload %.2f glass %.2f eye0 %.2f eye1 %.2f mirror %.2f frame %.2f max %.2f ms",
                    gpu_[0] / gpuFrames_, gpu_[1] / gpuFrames_, gpu_[2] / gpuFrames_,
                    gpu_[3] / gpuFrames_, gpu_[4] / gpuFrames_, gpu_[5] / gpuFrames_, gpuMax_);
      std::printf(" | vk memory %.0f MiB live %.0f retired | drawn %.1f fps\n", live_ / 1048576.0,
                  retired_ / 1048576.0, drawn_ / elapsed);
      std::fflush(stdout);
    }
    for (double& t : totals_) t = 0;
    for (double& t : gpu_) t = 0;
    gpuMax_ = 0;
    gpuFrames_ = 0;
    frames_ = 0;
    drawn_ = 0;
    longest_ = 0;
    reportStart_ = Clock::now();
    return true;
  }

 private:
  using Clock = std::chrono::steady_clock;
  bool enabled_;
  Clock::time_point last_ = Clock::now();
  Clock::time_point reportStart_ = Clock::now();
  Clock::time_point lastFinish_ = Clock::now();
  double longest_ = 0;  // the longest frame since the report, from one frame's end to the next
  double totals_[kCount] = {};
  int frames_ = 0;
  int drawn_ = 0;
  double gpu_[6] = {}, gpuMax_ = 0;
  int gpuFrames_ = 0;
  unsigned long long live_ = 0, retired_ = 0;
};
