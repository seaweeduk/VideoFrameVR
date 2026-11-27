#include "frame_grabber.h"
#include "bluray.h"
#include "mpv_gl_context.h"
#include "remote.h"
#include "stash_stream.h"

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

FrameGrabber::~FrameGrabber() {
  if (render_) mpv_render_context_free(render_);
  releaseTarget();
  if (mpv_) mpv_terminate_destroy(mpv_);
}

bool FrameGrabber::init(GlInterop& interop, const std::string& hwdec, int maxWidth,
                        int maxHeight, const MpvGlContext& glContext) {
  std::setlocale(LC_NUMERIC, "C");
  interop_ = &interop;
  maxWidth_ = maxWidth;
  maxHeight_ = maxHeight;
  mpv_ = mpv_create();
  if (!mpv_) return false;
  if (!glContext.configure(mpv_)) return false;
  mpv_set_option_string(mpv_, "vo", "libmpv");
  mpv_set_option_string(mpv_, "hwdec", hwdec.c_str());
  // The same video track as the player's (VideoPlayer::init).
  mpv_set_option_string(mpv_, "flatten-editions", "yes");
  mpv_set_option_string(mpv_, "pause", "yes");
  mpv_set_option_string(mpv_, "keep-open", "yes");
  mpv_set_option_string(mpv_, "idle", "yes");
  mpv_set_option_string(mpv_, "aid", "no");
  mpv_set_option_string(mpv_, "sid", "no");
  mpv_set_option_string(mpv_, "osd-level", "0");
  mpv_set_option_string(mpv_, "video-timing-offset", "0");
  // Seek to keyframes only: fast, and close enough for a preview.
  mpv_set_option_string(mpv_, "hr-seek", "no");
  mpv_set_option_string(mpv_, "demuxer-max-bytes", "32MiB");
  // Shrink each frame on the CPU, in mpv's own threads, before it reaches
  // OpenGL: the GL thread (which also draws the headset's frames) then only
  // handles a small picture, not an 8K one. Copy-back decoding hands the
  // decoded frames to that filter.
  const std::string scale = "scale=w=" + std::to_string(maxWidth) + ":h=" +
                            std::to_string(maxHeight) + ":force_original_aspect_ratio=decrease";
  mpv_set_option_string(mpv_, "vf", scale.c_str());
  // Cheap scaling: these frames are small, quality filters only cost GPU time.
  mpv_set_option_string(mpv_, "scale", "bilinear");
  mpv_set_option_string(mpv_, "dscale", "bilinear");
  mpv_set_option_string(mpv_, "cscale", "bilinear");
  mpv_set_option_string(mpv_, "correct-downscaling", "no");
  mpv_set_option_string(mpv_, "linear-downscaling", "no");
  mpv_set_option_string(mpv_, "sigmoid-upscaling", "no");
  mpv_set_option_string(mpv_, "deband", "no");
  mpv_set_option_string(mpv_, "dither-depth", "no");
  mpv_set_option_string(mpv_, "ytdl", "no");  // as VideoPlayer: links open without yt-dlp
  // Silent: no file names in the console output.
  mpv_set_option_string(mpv_, "terminal", "no");
  mpv_set_option_string(mpv_, "msg-level", "all=no");
  if (mpv_initialize(mpv_) < 0) {
    std::fprintf(stderr, "Frame grabber: mpv_initialize failed\n");
    return false;
  }
  remote::addTo(mpv_, true);
  addStashStreams(mpv_, true);
  bdmv::addTo(mpv_, true);
  // Observed rather than read: reading waits while mpv opens a decoder
  // (about 0.2 s), and this runs on the thread that draws the headset's frames.
  mpv_observe_property(mpv_, 0, "dwidth", MPV_FORMAT_INT64);
  mpv_observe_property(mpv_, 0, "dheight", MPV_FORMAT_INT64);
  mpv_observe_property(mpv_, 0, "video-out-params/rotate", MPV_FORMAT_INT64);
  // A stream that can't seek (a server without ranges), or only within what
  // is cached, can't show other places: it fails rather than wait forever.
  mpv_observe_property(mpv_, 0, "seekable", MPV_FORMAT_FLAG);
  mpv_observe_property(mpv_, 0, "partially-seekable", MPV_FORMAT_FLAG);
  if (!glContext.createRenderContext(mpv_, &render_)) {
    std::fprintf(stderr, "Frame grabber: mpv_render_context_create failed\n");
    return false;
  }
  return true;
}

void FrameGrabber::setVideo(const std::string& location, bool stream) {
  video_ = location;
  unavailable_ = false;
  lastUse_ = std::chrono::steady_clock::now();
  if (!location.empty() && stream) {
    open(location, 0);
    asked_ = 0;
  } else {
    close();
  }
}

void FrameGrabber::point(double seconds, double) {
  lastUse_ = std::chrono::steady_clock::now();
  if (video_.empty() || unavailable_) return;
  if (path_ != video_)
    open(video_, seconds);
  else if (std::abs(seconds - asked_) > 0.5)
    seek(seconds);
  else
    return;
  asked_ = seconds;
}

void FrameGrabber::open(const std::string& path, double start) {
  wake();
  path_ = path;
  fileLoaded_ = false;
  restarted_ = false;
  seeking_ = false;
  pendingSeek_ = -1;
  target_ = -1;
  exact_ = true;
  hasFrame_ = false;
  failed_ = false;
  started_ = false;
  entry_ = 0;
  const std::string options = "start=" + std::to_string(start);
  // A 3D Blu-ray's base view, as thumbnails show: half the reading and decoding.
  const std::string url = bdmv::isDiscImage(path) ? bdmv::mpvUrl(path, true) : remote::mpvUrl(path);
  const char* cmd[] = {"loadfile", url.c_str(), "replace", "-1", options.c_str(), nullptr};
  // Returns at once; mpv opens the file in its own thread. What it still has
  // to say of the video before is told from this one's by its number.
  mpv_node result;
  if (mpv_command_ret(mpv_, cmd, &result) < 0) return;
  if (result.format == MPV_FORMAT_NODE_MAP)
    for (int i = 0; i < result.u.list->num; ++i)
      if (std::strcmp(result.u.list->keys[i], "playlist_entry_id") == 0 &&
          result.u.list->values[i].format == MPV_FORMAT_INT64)
        entry_ = result.u.list->values[i].u.int64;
  mpv_free_node_contents(&result);
}

void FrameGrabber::close() {
  if (path_.empty()) return;
  const char* cmd[] = {"stop", nullptr};
  mpv_command(mpv_, cmd);
  wake();
  path_.clear();
  fileLoaded_ = false;
  restarted_ = false;
  hasFrame_ = false;
  failed_ = false;
}

// First a quick keyframe seek (it lands on the keyframe in the direction of
// the seek, which in videos with few keyframes can be far off); once the
// pointer rests, update() follows with an exact one.
void FrameGrabber::sendSeek(double seconds, bool exact) {
  const std::string target = std::to_string(seconds);
  const char* cmd[] = {"seek", target.c_str(), exact ? "absolute+exact" : "absolute+keyframes",
                       nullptr};
  mpv_command_async(mpv_, 0, cmd);
  seeking_ = true;
  target_ = seconds;
  exact_ = exact;
}

// Asynchronous: mpv closes or opens the decoder in its own thread meanwhile
// (about 0.1-0.2 s), and handles later commands after it, in order.
void FrameGrabber::setVideoTrack(const char* track) {
  mpv_set_property_async(mpv_, 0, "vid", MPV_FORMAT_STRING, &track);
}

bool FrameGrabber::baseViewOnly() const { return bdmv::isDiscImage(video_); }

void FrameGrabber::rest() {
  if (path_.empty() || asleep_ || busy()) return;
  setVideoTrack("no");
  asleep_ = true;
}

// "vid" is not per file: a file opened without a video track would fail.
void FrameGrabber::wake() {
  if (!asleep_) return;
  setVideoTrack("auto");
  asleep_ = false;
  target_ = -1;  // the frame shown is from before; seek again
}

void FrameGrabber::seek(double seconds) {
  if (path_.empty()) return;
  wake();
  if (seconds == target_ && pendingSeek_ < 0) return;  // already there or on its way
  if (!fileLoaded_ || seeking_)
    pendingSeek_ = seconds;
  else
    sendSeek(seconds);
}

void FrameGrabber::update(const PlaybackState&) {
  poll();
  // One that can't show other places (a stream that can't seek) lets its
  // connection go.
  if (failed_ && !path_.empty()) {
    close();
    unavailable_ = true;
  }
  if (std::chrono::steady_clock::now() - lastUse_ > kIdle) rest();
}

void FrameGrabber::poll() {
  bool sizeChanged = false;
  while (true) {
    mpv_event* ev = mpv_wait_event(mpv_, 0);
    if (ev->event_id == MPV_EVENT_NONE) break;
    switch (ev->event_id) {
      case MPV_EVENT_START_FILE:
        started_ = !entry_ ||
                   static_cast<mpv_event_start_file*>(ev->data)->playlist_entry_id == entry_;
        break;
      case MPV_EVENT_FILE_LOADED:
        fileLoaded_ = true;
        if (pendingSeek_ >= 0 && !seeking_) {
          sendSeek(pendingSeek_);
          pendingSeek_ = -1;
        }
        break;
      case MPV_EVENT_PROPERTY_CHANGE: {
        const auto* prop = static_cast<mpv_event_property*>(ev->data);
        const std::string name = prop->name;
        if (name == "seekable" || name == "partially-seekable") {
          // Without a value (no file open) it says nothing, nor of the
          // video before.
          if (prop->format != MPV_FORMAT_FLAG || !started_) break;
          const bool on = *static_cast<int*>(prop->data) != 0;
          if (name == "seekable" ? !on : on) failed_ = true;
          break;
        }
        const int64_t value = prop->format == MPV_FORMAT_INT64 ? *static_cast<int64_t*>(prop->data) : 0;
        (name == "dwidth" ? videoWidth_ : name == "dheight" ? videoHeight_ : rotate_) = value;
        sizeChanged = true;
        break;
      }
      case MPV_EVENT_PLAYBACK_RESTART:
        if (fileLoaded_) restarted_ = true;
        seeking_ = false;
        if (pendingSeek_ >= 0) {
          sendSeek(pendingSeek_);
          pendingSeek_ = -1;
        } else if (!exact_ && target_ >= 0) {
          sendSeek(target_, true);  // the pointer rests: the exact frame
        }
        break;
      case MPV_EVENT_END_FILE: {
        const auto* end = static_cast<mpv_event_end_file*>(ev->data);
        if (end->reason == MPV_END_FILE_REASON_ERROR &&
            (!entry_ || end->playlist_entry_id == entry_))
          failed_ = true;
        // A playlist: what it holds comes under numbers of its own.
        if (end->reason == MPV_END_FILE_REASON_REDIRECT && end->playlist_entry_id == entry_)
          entry_ = 0;
        break;
      }
      default:
        break;
    }
  }
  if (sizeChanged) updateVideoSize();
  if (mpv_render_context_update(render_) & MPV_RENDER_UPDATE_FRAME) framePending_ = true;
  // Before the first restart there is no frame of this file yet.
  if (!framePending_ || !image_ || !restarted_) return;
  framePending_ = false;

  // After the frames that showed the last one are done with it.
  interop_->acquireForWrite(*image_);
  mpv_opengl_fbo fbo{static_cast<int>(image_->framebuffer()), width_, height_, 0};
  int flipY = 0;  // texel row 0 = top of the image
  int block = 0;
  mpv_render_param params[] = {
      {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
      {MPV_RENDER_PARAM_FLIP_Y, &flipY},
      {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
      {MPV_RENDER_PARAM_INVALID, nullptr},
  };
  glDisable(GL_FRAMEBUFFER_SRGB);  // mpv writes sRGB values itself
  mpv_render_context_render(render_, params);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  hasFrame_ = true;
}

void FrameGrabber::updateVideoSize() {
  int64_t w = videoWidth_, h = videoHeight_;
  if (w <= 0 || h <= 0) return;
  // Turned by 90° while rendering: the target takes the turned shape (as VideoPlayer's).
  if (rotate_ % 180 == 90) std::swap(w, h);
  const double s = std::min({1.0, double(maxWidth_) / w, double(maxHeight_) / h});
  const int tw = std::max(2, static_cast<int>(w * s)), th = std::max(2, static_cast<int>(h * s));
  if (image_ && tw == width_ && th == height_) return;
  releaseTarget();
  image_ = interop_->create(static_cast<uint32_t>(tw), static_cast<uint32_t>(th), 1,
                            "the seek preview");
  if (!image_) return;
  width_ = tw;
  height_ = th;
  framePending_ = true;
}

void FrameGrabber::releaseTarget() {
  if (image_) interop_->retire(std::move(image_));
  width_ = height_ = 0;
  hasFrame_ = false;
}
