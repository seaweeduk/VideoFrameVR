#pragma once

#include "gl_interop.h"
#include "preview_source.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

struct mpv_handle;
struct mpv_render_context;
class MpvGlContext;

// The seek preview's pictures from a hardware decoder: a second, silent mpv
// instance that shows single frames on request (it opens links too). Frames
// are scaled into an image shared with Vulkan (docs/vulkan-renderer.md,
// 3.5), sRGB: sampling returns linear colour. Nothing is logged.
// A frame costs little here, so every place pointed at gets one: first a
// quick keyframe, then, once no other is asked for, the exact frame.
class FrameGrabber : public PreviewSource {
 public:
  FrameGrabber() = default;
  FrameGrabber(const FrameGrabber&) = delete;
  FrameGrabber& operator=(const FrameGrabber&) = delete;
  ~FrameGrabber() override;

  // Frames are scaled down to fit maxWidth x maxHeight on the CPU. Pass a
  // copy-back hwdec (e.g. "nvdec-copy", "vaapi-copy"). False: unusable.
  bool init(GlInterop& interop, const std::string& hwdec, int maxWidth, int maxHeight,
            const MpvGlContext& glContext);

  // Streams take seconds to open, so one opens right away; a file opens
  // fast enough when first pointed at.
  void setVideo(const std::string& location, bool stream) override;
  void point(double seconds, double fraction) override;
  // Polls mpv and renders a new frame if one is ready.
  void update(const PlaybackState& playback) override;
  State state() const override {
    return unavailable_ ? State::Unavailable : hasFrame_ ? State::Ready : State::Loading;
  }
  // Includes an outstanding exact seek even after the preview is hidden.
  bool busy() const override {
    return !path_.empty() && !failed_ && (!restarted_ || seeking_ || pendingSeek_ >= 0);
  }
  // A 3D disc's base view alone (open()): half the reading and decoding.
  bool baseViewOnly() const override;
  const Texture* image() const override { return hasFrame_ ? &image_->texture() : nullptr; }
  int width() const override { return width_; }
  int height() const override { return height_; }

 private:
  // Opens a file or stream, paused at `start`.
  void open(const std::string& path, double start);
  void close();
  void seek(double seconds);
  void poll();
  // Stops the decoder, which frees its GPU memory, and keeps the file open;
  // the next seek() starts it again, which takes about as long as a seek.
  // Not while busy().
  void rest();
  void updateVideoSize();
  void releaseTarget();
  void sendSeek(double seconds, bool exact = false);
  void wake();
  void setVideoTrack(const char* track);

  mpv_handle* mpv_ = nullptr;
  mpv_render_context* render_ = nullptr;
  GlInterop* interop_ = nullptr;
  int maxWidth_ = 0, maxHeight_ = 0;
  std::string video_;  // the playing video (setVideo()), opened or not yet
  std::string path_;   // the one open
  double asked_ = -1;  // seconds: where a frame was last asked for
  bool unavailable_ = false;  // the playing video failed, and was closed
  std::chrono::steady_clock::time_point lastUse_;  // pointed at, or the video changed
  std::unique_ptr<SharedImage> image_;  // one level
  int width_ = 0, height_ = 0;
  int64_t videoWidth_ = 0, videoHeight_ = 0, rotate_ = 0;  // observed; 0 = no video
  bool fileLoaded_ = false;
  bool restarted_ = false;  // the file's first frame has been decoded
  bool seeking_ = false;
  double pendingSeek_ = -1;  // seconds, < 0 = none
  double target_ = -1;       // the last seek sent, seconds
  bool exact_ = true;        // ... and whether it was exact
  bool framePending_ = false;
  bool hasFrame_ = false;
  bool failed_ = false;
  int64_t entry_ = 0;     // mpv's number for the video opened last (0: not known)
  bool started_ = false;  // ... which mpv has begun to open
  bool asleep_ = false;  // vid=no: the file is open, nothing decodes
};
