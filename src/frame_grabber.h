#pragma once

#include "gl_interop.h"

#include <cstdint>
#include <memory>
#include <string>

struct mpv_handle;
struct mpv_render_context;
class MpvGlContext;

// A second, silent mpv instance that shows single frames on request, for the
// seek bar preview (it opens links too). Frames are scaled into an image
// shared with Vulkan (docs/vulkan-renderer.md, 3.5), sRGB: sampling returns
// linear colour. Nothing is logged.
// All methods must be called on the thread that owns the GL context; update()
// inside GL's access window (gl_interop.h), before GlInterop::publish().
class FrameGrabber {
 public:
  FrameGrabber() = default;
  FrameGrabber(const FrameGrabber&) = delete;
  FrameGrabber& operator=(const FrameGrabber&) = delete;
  ~FrameGrabber();

  // Frames are scaled down to fit maxWidth x maxHeight on the CPU. Pass a
  // copy-back hwdec (e.g. "nvdec-copy", "v4l2m2m-copy") or "no".
  bool init(GlInterop& interop, const std::string& hwdec, int maxWidth, int maxHeight,
            const MpvGlContext& glContext);
  // Opens a file or stream, paused at `start` (mpv syntax: seconds or "20%").
  void open(const std::string& path, const std::string& start);
  void close();
  const std::string& path() const { return path_; }
  // Stops the decoder, which frees its GPU memory (about 1.8 GB for 8K HEVC
  // with NVDEC), and keeps the file open; the next seek() starts it again,
  // which takes about as long as a seek. Only while not busy().
  void sleep();
  bool asleep() const { return asleep_; }
  // Shows the frame near `seconds`. While a seek runs, only the latest request is kept.
  void seek(double seconds);
  // Polls mpv and renders a new frame if one is ready. True if the image changed.
  bool update();
  // A frame of the current file is in image().
  bool hasFrame() const { return hasFrame_; }
  // The current file could not be opened.
  bool failed() const { return failed_; }
  // Includes an outstanding exact seek even after the preview is hidden.
  bool busy() const {
    return !path_.empty() && !failed_ && (!restarted_ || seeking_ || pendingSeek_ >= 0);
  }
  // The whole frame (both eyes of a 3D video; the scene picks one), while
  // hasFrame(). It stays valid until the next update().
  const Texture* image() const { return hasFrame_ ? &image_->texture() : nullptr; }
  int width() const { return width_; }
  int height() const { return height_; }

 private:
  void updateVideoSize();
  void releaseTarget();
  void sendSeek(double seconds, bool exact = false);
  void wake();
  void setVideoTrack(const char* track);

  mpv_handle* mpv_ = nullptr;
  mpv_render_context* render_ = nullptr;
  GlInterop* interop_ = nullptr;
  int maxWidth_ = 0, maxHeight_ = 0;
  std::string path_;
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
  bool asleep_ = false;  // vid=no: the file is open, nothing decodes
};
