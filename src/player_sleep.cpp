#include "player_sleep.h"
#include "playback_state.h"
#include "video_export.h"

#include <mpv/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

namespace {

// Replies carry these plus the load's number (VideoPlayer's own use other bits).
constexpr uint64_t kSleepVideoReply = uint64_t(1) << 35;
constexpr uint64_t kWakeVideoReply = uint64_t(1) << 36;

double nowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void setPause(mpv_handle* mpv, bool paused) {
  const char* text = paused ? "yes" : "no";
  mpv_set_property_async(mpv, 0, "pause", MPV_FORMAT_STRING, &text);
}

}  // namespace

bool PlayerSleep::command(const char* cmd, PlaybackState& shown) {
  if (videoSleep_ != VideoSleep::Awake) {
    if (std::strcmp(cmd, "cycle pause") == 0) {
      sleepPaused_ = !sleepPaused_;
      shown.paused = sleepPaused_;
      return true;
    }
    if (std::strcmp(cmd, "set pause yes") == 0 || std::strcmp(cmd, "set pause no") == 0) {
      sleepPaused_ = std::strcmp(cmd, "set pause yes") == 0;
      shown.paused = sleepPaused_;
      return true;
    }
  }
  return false;
}

bool PlayerSleep::seek(const SeekQueue::SeekRequest& request, PlaybackState& shown) {
  if (videoSleep_ != VideoSleep::Awake) {
    sleepPosition_ = request.absolute ? request.amount : sleepPosition_ + request.amount;
    sleepPosition_ = std::max(0.0, sleepPosition_);
    if (shown.duration > 0) sleepPosition_ = std::min(sleepPosition_, shown.duration);
    shown.position = sleepPosition_;
    sleepSeekChanged_ = true;
    return true;
  }
  return false;
}

bool PlayerSleep::save(const PlaybackState& playback, double destination) {
  sleepTrack_ = "auto";
  bool selected = false;
  for (const auto& track : playback.tracks)
    if (track.type == "video" && track.selected) {
      sleepTrack_ = std::to_string(track.id);
      selected = true;
    }
  if (!selected) return false;
  sleepPosition_ = destination;
  sleepPosition_ = std::max(0.0, sleepPosition_);
  if (playback.duration > 0) sleepPosition_ = std::min(sleepPosition_, playback.duration);
  sleepPaused_ = playback.paused;
  return true;
}

int PlayerSleep::close(uint64_t load) {
  export_.close_pending = 1;
  export_.buffers_released = 0;
  sleepCloseReplied_ = false;
  videoReleased_ = false;
  setPause(mpv_, true);
  const char* no = "no";
  const int ret = mpv_set_property_async(mpv_, kSleepVideoReply + load,
                                        "vid", MPV_FORMAT_STRING, &no);
  sleepCloseInFlight_ = ret >= 0;
  if (ret < 0) return ret;
  videoSleep_ = VideoSleep::Closing;
  sleepActionAt_ = nowSeconds();
  return ret;
}

void PlayerSleep::fail(uint64_t load) {
  export_.close_pending = 1;
  export_.buffers_released = 0;
  sleepCloseReplied_ = false;
  videoReleased_ = false;
  const char* no = "no";
  sleepCloseInFlight_ = mpv_set_property_async(mpv_, kSleepVideoReply + load,
                                             "vid", MPV_FORMAT_STRING, &no) >= 0;
  setPause(mpv_, true);
  videoSleep_ = VideoSleep::Closed;
}

int PlayerSleep::open(uint64_t load) {
  export_.close_pending = 0;
  const char* track = sleepTrack_.c_str();
  const int ret = mpv_set_property_async(mpv_, kWakeVideoReply + load,
                                        "vid", MPV_FORMAT_STRING, &track);
  if (ret < 0) return ret;
  videoSleep_ = VideoSleep::Opening;
  sleepActionAt_ = nowSeconds();
  resumeFrameSeen_ = false;
  resumePictureShown_ = false;
  return ret;
}

void PlayerSleep::resumeSeekSent(uint64_t seek) {
  videoSleep_ = VideoSleep::Seeking;
  resumeSeek_ = seek;
  sleepSeekChanged_ = false;
  resumeRestarted_ = false;
  resumeFrameSeen_ = false;
  resumePictureShown_ = false;
}

void PlayerSleep::resumed() {
  videoSleep_ = VideoSleep::Awake;
  setPause(mpv_, sleepPaused_);
  std::printf("Video decoder resumed at the saved position\n");
}

void PlayerSleep::cancel() {
  deferredLoadPath_.clear();
  deferredStop_ = false;
  videoSleep_ = VideoSleep::Awake;
  if (videoReleased_) export_.close_pending = 0;
  resumeSeek_ = 0;
  hiddenSince_ = -1;
}

void PlayerSleep::setActive(bool active) {
  if (active && !presentationActive_) sleepActionAt_ = nowSeconds();
  presentationActive_ = active;
}

bool PlayerSleep::shown() {
  hiddenSince_ = -1;
  return videoSleep_ == VideoSleep::Closed;
}

bool PlayerSleep::timedOut() const {
  return videoSleep_ != VideoSleep::Awake && nowSeconds() - sleepActionAt_ > 10;
}

bool PlayerSleep::hidden() {
  if (hiddenSince_ < 0) hiddenSince_ = nowSeconds();
  return videoSleep_ == VideoSleep::Awake && nowSeconds() - hiddenSince_ >= 0.25;
}

PlayerSleep::Reply PlayerSleep::reply(uint64_t userdata, int error, uint64_t load) {
  if (userdata == kSleepVideoReply + load && export_.close_pending) {
    sleepCloseInFlight_ = false;
    if (error < 0) {
      std::fprintf(stderr, "Video track could not sleep: %s\n", mpv_error_string(error));
      videoSleep_ = VideoSleep::Closed;
      return Reply::CloseFailed;
    } else {
      sleepCloseReplied_ = true;
      videoSleep_ = VideoSleep::Closed;
      std::printf("Video track closed while the session is hidden\n");
      return Reply::Closed;
    }
  } else if (userdata == kWakeVideoReply + load &&
             videoSleep_ == VideoSleep::Opening) {
    return error < 0 ? Reply::OpenFailed : Reply::Opened;
  }
  return Reply::None;
}

void PlayerSleep::restarted(uint64_t seek) {
  if (videoSleep_ == VideoSleep::Seeking && seek == resumeSeek_) {
    resumeRestarted_ = true;
  }
}

bool PlayerSleep::holdsPicture(bool seekStarted) const {
  return videoSleep_ == VideoSleep::Seeking && !seekStarted && !resumeRestarted_;
}

void PlayerSleep::frameRendered(uint64_t seek, bool seekStarted, bool newFrame) {
  if (videoSleep_ == VideoSleep::Seeking && seek == resumeSeek_ &&
      (seekStarted || resumeRestarted_) && newFrame)
    resumeFrameSeen_ = true;
}

void PlayerSleep::pictureShown() {
  if (videoSleep_ == VideoSleep::Seeking && resumeFrameSeen_) resumePictureShown_ = true;
}

bool PlayerSleep::closeUnsent() const {
  return export_.close_pending && !sleepCloseReplied_ && !sleepCloseInFlight_;
}

bool PlayerSleep::setReleased(bool released) {
  videoReleased_ = released;
  return videoReleased_;
}

void PlayerSleep::deferLoad(const std::string& path, double start) {
  deferredLoadPath_ = path;
  deferredLoadStart_ = start;
  deferredStop_ = false;
  deferredActionAt_ = nowSeconds();
}

void PlayerSleep::deferStop() {
  deferredLoadPath_.clear();
  deferredStop_ = true;
  deferredActionAt_ = nowSeconds();
}

PlayerSleep::Deferred PlayerSleep::takeDeferred() {
  Deferred next;
  next.path = std::move(deferredLoadPath_);
  deferredLoadPath_.clear();
  next.stop = deferredStop_;
  deferredStop_ = false;
  next.start = deferredLoadStart_;
  return next;
}

bool PlayerSleep::giveUpDeferred() {
  if (nowSeconds() - deferredActionAt_ <= 10) return false;
  deferredLoadPath_.clear();
  deferredStop_ = false;
  return true;
}
