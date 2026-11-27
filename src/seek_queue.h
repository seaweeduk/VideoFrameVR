#pragma once

#include <cstdint>
#include <string>

struct mpv_handle;

// VideoPlayer's seeks, sent to mpv one at a time: one asked for while one is
// on its way waits until the first frame of that one has arrived, merged
// with any others asked for since. An exact seek on its way is replaced at
// once. Same thread as VideoPlayer.
class SeekQueue {
 public:
  struct SeekRequest {
    bool absolute = false;
    double amount = 0;  // seconds: the time, or the distance from the position
    bool exact = false;
    int merged = 1;  // requests it stands for
  };

  void init(mpv_handle* mpv, bool timing) {
    mpv_ = mpv;
    timing_ = timing;
  }
  // Waits, merged into the one already waiting.
  void add(const SeekRequest& request);
  // The waiting request, if nothing holds it back; `ready`: the file has
  // loaded and shown its first frame; `dropFirst`: mpv drops its buffers
  // just before it (DownloadEdge), and a distance is sent as the time it
  // reaches from `position`. True if it went to mpv.
  bool send(bool ready, double position, double duration, bool dropFirst = false);
  // mpv's frame-back-step, which goes as an exact seek does. True if sent.
  bool stepBack(double position);
  void reset();

  // A command's reply: true if mpv refused the seek on its way.
  bool reply(uint64_t userdata, int error);
  // mpv's seek event.
  void seekEvent();
  // mpv's playback restart: true if it ended the seek on its way.
  // `decoder` and `direct` are for --timing.
  bool restarted(double position, const std::string& decoder, bool direct);

  // A seek is on its way or waiting.
  bool busy() const { return seekStage_ != SeekStage::None || seekWaiting_; }
  bool onItsWay() const { return seekStage_ != SeekStage::None; }
  bool started() const { return seekStage_ == SeekStage::Started; }
  // Seeks sent so far: the last one's number.
  uint64_t count() const { return seekCount_; }
  // Where the seek on its way and the one waiting take the video from
  // `position`.
  double destination(double position) const;

 private:
  mpv_handle* mpv_ = nullptr;
  bool timing_ = false;
  // The seek on its way: sent, accepted (mpv's reply), started (its seek
  // event); playback's restart ends it.
  enum class SeekStage { None, Sent, Accepted, Started };
  SeekStage seekStage_ = SeekStage::None;
  bool seekExact_ = false;
  double seekSentAt_ = 0;  // seconds, steady clock
  double seekTarget_ = 0;
  uint64_t seekCount_ = 0;
  bool seekWaiting_ = false;
  SeekRequest waitingSeek_;
};
