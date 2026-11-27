#include "seek_queue.h"

#include <mpv/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace {

// A keyframe seek on its way holds the next back this long at most, waiting
// for its picture; one that never arrives is forgotten after kSeekGiveUp.
constexpr double kSeekPatience = 1.5;
constexpr double kSeekGiveUp = 6.0;
// Seek replies carry this plus the seek's number.
constexpr uint64_t kSeekReply = uint64_t(1) << 32;

double nowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

void SeekQueue::add(const SeekRequest& request) {
  if (!seekWaiting_) {
    waitingSeek_ = request;
    seekWaiting_ = true;
  } else if (request.absolute) {
    const int merged = waitingSeek_.merged + 1;
    waitingSeek_ = request;
    waitingSeek_.merged = merged;
  } else {
    // A distance adds to what waits; it is the stick's, so a keyframe seek.
    waitingSeek_.amount += request.amount;
    waitingSeek_.exact = false;
    ++waitingSeek_.merged;
  }
}

bool SeekQueue::send(bool ready, double position, double duration, bool dropFirst) {
  const double now = nowSeconds();
  if (seekStage_ != SeekStage::None && now - seekSentAt_ > kSeekGiveUp) {
    if (timing_)
      std::printf("[seek-timing] #%llu no frame after %.0f ms, given up\n",
                  static_cast<unsigned long long>(seekCount_), (now - seekSentAt_) * 1000);
    seekStage_ = SeekStage::None;
  }
  // A new file's seeks wait for its first frame.
  if (!seekWaiting_ || !ready) return false;
  if (seekStage_ != SeekStage::None && !seekExact_ && now - seekSentAt_ < kSeekPatience) return false;
  SeekRequest r = waitingSeek_;
  seekWaiting_ = false;
  // Dropping its buffers loses where mpv is: a distance goes from here, as
  // the keyframe seek it would have been.
  if (dropFirst && !r.absolute) {
    r.amount += position;
    r.absolute = true;
    r.exact = false;
  }
  if (r.absolute) {
    r.amount = std::max(0.0, r.amount);  // mpv counts negative times from the end
    if (duration > 0) r.amount = std::min(r.amount, duration);
  }
  const std::string amount = std::to_string(r.amount);
  const char* flags = !r.absolute ? "relative+keyframes" : r.exact ? "absolute+exact" : "absolute+keyframes";
  const char* cmd[] = {"seek", amount.c_str(), flags, nullptr};
  if (dropFirst) {  // asynchronous too: mpv runs both in order
    const char* drop[] = {"drop-buffers", nullptr};
    mpv_command_async(mpv_, 0, drop);
  }
  // Asynchronous: mpv's core may be busy, and the frame loop never waits for it.
  if (mpv_command_async(mpv_, kSeekReply + seekCount_ + 1, cmd) < 0) return false;
  ++seekCount_;
  if (timing_)
    std::printf("[seek-timing] #%llu %s %.2f (%d asked)%s from pos=%.2f\n",
                static_cast<unsigned long long>(seekCount_), flags, r.amount, r.merged,
                seekStage_ != SeekStage::None ? ", replacing the one on its way" : "",
                position);
  seekStage_ = SeekStage::Sent;
  seekTarget_ = r.absolute ? r.amount : position + r.amount;
  seekExact_ = r.exact;
  seekSentAt_ = now;
  return true;
}

bool SeekQueue::stepBack(double position) {
  const char* cmd[] = {"frame-back-step", nullptr};
  if (mpv_command_async(mpv_, kSeekReply + seekCount_ + 1, cmd) < 0) return false;
  ++seekCount_;
  if (timing_)
    std::printf("[seek-timing] #%llu frame-back-step from pos=%.2f\n",
                static_cast<unsigned long long>(seekCount_), position);
  seekStage_ = SeekStage::Sent;
  seekTarget_ = position;
  seekExact_ = true;
  seekSentAt_ = nowSeconds();
  return true;
}

void SeekQueue::reset() {
  seekStage_ = SeekStage::None;
  seekWaiting_ = false;
}

bool SeekQueue::reply(uint64_t userdata, int error) {
  if (userdata != kSeekReply + seekCount_ || seekStage_ != SeekStage::Sent) return false;
  seekStage_ = error < 0 ? SeekStage::None : SeekStage::Accepted;
  if (error < 0 && timing_)
    std::printf("[seek-timing] #%llu refused: %s\n",
                 static_cast<unsigned long long>(seekCount_), mpv_error_string(error));
  return error < 0;
}

void SeekQueue::seekEvent() {
  // After the reply, mpv's next seek is this one (or merged with it).
  if (seekStage_ == SeekStage::Accepted) seekStage_ = SeekStage::Started;
}

bool SeekQueue::restarted(double position, const std::string& decoder, bool direct) {
  // mpv restarts once the seek's first frame was drawn (handed to
  // render()): the picture has changed, the next seek may go.
  if (seekStage_ != SeekStage::Started) return false;
  seekStage_ = SeekStage::None;
  if (timing_)
    std::printf("[seek-timing] #%llu first frame after %.0f ms, mpv at %.2f, hw=%s%s\n",
                static_cast<unsigned long long>(seekCount_), (nowSeconds() - seekSentAt_) * 1000,
                position, decoder.c_str(),
                 direct ? " direct" : "");
  return true;
}

double SeekQueue::destination(double position) const {
  double to = seekStage_ != SeekStage::None ? seekTarget_ : position;
  if (seekWaiting_) to = waitingSeek_.absolute ? waitingSeek_.amount : to + waitingSeek_.amount;
  return to;
}
