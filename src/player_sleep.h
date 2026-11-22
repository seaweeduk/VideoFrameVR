#pragma once

#include "seek_queue.h"

#include <cstdint>
#include <string>

struct PlaybackState;
struct mpv_handle;
struct video_export;

// While the session is hidden, VideoPlayer closes the video track (vid=no),
// so the decoder lets go of its memory, and opens it again where it was,
// paused or playing as it was, once the session shows: with the track
// selected before, an exact seek to the saved position, and the resume
// done once that seek's first picture is shown. Meanwhile pause commands
// and seeks change what it restores. A load() or stop() asked while the
// closed decoder still holds its buffers waits for them.
//
// This holds the sleep's state, sends its vid and pause properties and
// follows mpv's replies to them, and sets video_export's close_pending.
// VideoPlayer drives it and does what goes with each step to its frames,
// seeks and loading state. Same thread as VideoPlayer.
class PlayerSleep {
 public:
  explicit PlayerSleep(video_export& exp) : export_(exp) {}
  void init(mpv_handle* mpv) { mpv_ = mpv; }

  bool awake() const { return videoSleep_ == VideoSleep::Awake; }
  // The resume's seek is on its way, or its picture not yet shown.
  bool resumeSeeking() const { return videoSleep_ == VideoSleep::Seeking; }
  // The pause it restores.
  bool paused() const { return sleepPaused_; }
  double position() const { return sleepPosition_; }

  // Asleep, pause commands and seeks change what it restores, and `shown`:
  // true if it took the command.
  bool command(const char* cmd, PlaybackState& shown);
  bool seek(const SeekQueue::SeekRequest& request, PlaybackState& shown);

  // Before the track closes: the track, position and pause to restore.
  // False if no video track is selected.
  bool save(const PlaybackState& playback, double destination);
  // The track closes (vid=no, paused). mpv's error if it couldn't be asked.
  int close(uint64_t load);
  // ... and stays closed, after a step of the resume failed.
  void fail(uint64_t load);
  // The track opens again; mpv's error if it couldn't be asked.
  int open(uint64_t load);
  // Awake, for the resume's seek (VideoPlayer::seekAfterSleep), which goes
  // out as the seeks of a video awake do...
  void wake() { videoSleep_ = VideoSleep::Awake; }
  // ... then, sent as seek number `seek`, waits for its picture.
  void resumeSeekSent(uint64_t seek);
  // A seek asked for while asleep since the resume's seek went.
  bool seekChanged() const { return sleepSeekChanged_; }
  // The resume is done: playing or paused as before.
  void resumed();
  // Another file, or none: awake, and nothing waits.
  void cancel();

  // The session's visibility, each frame (VideoPlayer::setPresentationActive).
  void setActive(bool active);
  bool active() const { return presentationActive_; }
  // The track is opening again, or its resume's seek is under way.
  bool waking() const { return videoSleep_ == VideoSleep::Opening || videoSleep_ == VideoSleep::Seeking; }
  // A step of the sleep or resume took too long.
  bool timedOut() const;
  // Shown: whether the track is closed, so can open again.
  bool shown();
  // Hidden: whether the track, awake, may close now.
  bool hidden();

  // mpv's reply to setting a property.
  enum class Reply { None, Closed, CloseFailed, Opened, OpenFailed };
  Reply reply(uint64_t userdata, int error, uint64_t load);
  // The resume's seek, by number: its playback restart, its first new
  // frame rendered, and that frame shown.
  void restarted(uint64_t seek);
  // While its seek has neither started nor restarted, its picture waits
  // queued in mpv (VideoPlayer::render).
  bool holdsPicture(bool seekStarted) const;
  void frameRendered(uint64_t seek, bool seekStarted, bool newFrame);
  void pictureShown();
  // The resume's picture is shown: it can finish.
  bool resumeShown() const {
    return videoSleep_ == VideoSleep::Seeking && resumeRestarted_ && resumePictureShown_;
  }

  // The closed decoder's release (VideoPlayer::videoReleaseComplete):
  // mpv answered vid=no, then the frames and its session went back.
  bool closeUnsent() const;
  bool closeReplied() const { return sleepCloseReplied_; }
  bool released() const { return videoReleased_; }
  bool setReleased(bool released);

  // A load() or stop() waiting for the release.
  void deferLoad(const std::string& path, double start);
  void deferStop();
  bool deferred() const { return !deferredLoadPath_.empty() || deferredStop_; }
  struct Deferred {
    bool stop = false;
    std::string path;
    double start = 0;
  };
  Deferred takeDeferred();
  // It waited too long: dropped.
  bool giveUpDeferred();

 private:
  video_export& export_;
  mpv_handle* mpv_ = nullptr;
  enum class VideoSleep { Awake, Closing, Closed, Opening, Seeking };
  VideoSleep videoSleep_ = VideoSleep::Awake;
  bool sleepCloseReplied_ = false;
  bool sleepCloseInFlight_ = false;
  bool videoReleased_ = false;
  bool resumeFrameSeen_ = false;
  bool resumePictureShown_ = false;
  bool deferredStop_ = false;
  std::string deferredLoadPath_;
  double deferredLoadStart_ = 0;
  double deferredActionAt_ = 0;
  double hiddenSince_ = -1;
  double sleepPosition_ = 0;
  double sleepActionAt_ = 0;
  bool sleepPaused_ = false;
  bool presentationActive_ = true;
  uint64_t resumeSeek_ = 0;
  bool sleepSeekChanged_ = false;
  bool resumeRestarted_ = false;
  std::string sleepTrack_ = "auto";
};
