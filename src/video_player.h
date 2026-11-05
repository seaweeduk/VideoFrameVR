#pragma once

#include "gl_util.h"
#include "video_export.h"
#include "video_texture.h"

#include <cstdint>
#include <atomic>
#include <future>
#include <limits>
#include <string>
#include <vector>

struct mpv_handle;
struct mpv_node;
struct mpv_render_context;
class DirectFrame;
class DirectVideo;

struct Track {
  int64_t id = 0;
  std::string type;  // "audio", "sub" or "video"
  std::string title;
  std::string lang;
  std::string codec;
  bool selected = false;
};

struct Chapter {
  double time = 0;  // seconds
  std::string title;
};

struct PlaybackState {
  double position = 0;  // seconds
  double duration = 0;  // seconds, 0 if unknown
  double bufferedUntil = 0;  // seconds: how far ahead the stream is already downloaded
  bool paused = false;
  double volume = 100;
  bool muted = false;
  std::string title;
  std::vector<Track> tracks;
  std::string subtitleText;  // the subtitle line showing right now, as plain text
  bool endReached = false;            // stopped at the end of the file
  // What is playing, for the menu's info line.
  std::string videoCodec;   // e.g. "hevc"
  std::string pixelFormat;  // e.g. "p010" (10 bit)
  double fps = 0;
  std::string audioCodec;   // e.g. "opus"
  int audioChannels = 0;
  std::vector<Chapter> chapters;
  int chapter = -1;  // the current one, -1 = before the first / none
};

// Decodes media with libmpv and renders each new video frame, including
// mpv's OSD, with OpenGL into an image the Vulkan scene samples
// (VideoTexture). All methods must be called on the thread that owns the GL
// context.
class VideoPlayer {
 public:
  ~VideoPlayer();

  bool init(const std::string& hwdec, GlInterop& interop, bool timing = false,
            bool decoderQueue = true);
  // Frame: mpv hands out decoded frames for the eyes to sample instead of
  // drawing them, where they allow it (docs/vulkan-renderer.md 11).
  void setDirectVideo(DirectVideo* direct) { direct_ = direct; }
  // Opens a file, from `start` seconds on (mpv's start option: no visible seek).
  void load(const std::string& path, double start = 0);
  // True once for each load() that delivered its first video frame:
  // a play, in the library's sense.
  bool takeOpened();
  // Whether mpv has said where the file of the last load() is (its
  // position) since it opened; playback().position is 0 until then.
  bool positionKnown() const { return positionKnown_; }
  // Stops playback and closes the file (e.g. before deleting it).
  void stop();
  // Runs an mpv input command, e.g. "osd-msg cycle pause".
  void command(const char* cmd);
  // Seeks never queue behind one still on its way: one asked for meanwhile
  // waits until the first frame of the last has arrived, merged with any
  // others asked for since, so a held stick keeps the picture changing. An
  // exact seek on its way is replaced at once (it is slow and unwanted now).
  // seekBy goes to the keyframe in its direction (fast); seekTo to the
  // keyframe before the time, or exactly to it (decoding up to it).
  void seekBy(double seconds);
  void seekTo(double seconds, bool exact);
  // Loads an external subtitle file and selects it.
  void addSubtitle(const std::string& path);
  // Whether mpv draws subtitles into the video frame.
  void setSubtitlesInFrame(bool visible);
  // Size (1 = normal) and height (percent of the picture above the usual
  // place) of the subtitles mpv draws into the frame.
  void setSubtitleStyle(double scale, int raise);
  // Picture adjustments, -100..100 each (0 = unchanged).
  void setPicture(int brightness, int contrast, int saturation);
  // SDR video keeps its own values (as VR players show it) unless tvGamma:
  // then mpv converts it from a TV's gamma (BT.1886) to sRGB, darkening the
  // shadows. HDR is tone-mapped to sRGB either way.
  void setTvGamma(bool tvGamma);
  // The scene samples the video through mipmaps (trilinear, the default) or
  // from its full-size level only (bilinear); they are
  // made for each new frame only while on.
  void setMipmaps(bool enabled);
  // Sharpening on: the scene has no pipelines for it with Main 10 frames as
  // the decoder wrote them (Q10C, DirectVideo), so mpv converts them.
  void setSharpened(bool sharpened) { sharpened_ = sharpened; }
  const PlaybackState& playback() const { return playback_; }
  void pollEvents();
  // Standalone session visibility, not focus. A hidden session releases its
  // video decoder without selecting a software replacement; waking restores
  // the selected track and the exact saved position and pause state.
  void setPresentationActive(bool active);
  // Renders the next frame if mpv has one ready: it is texture() from then
  // on, or with setPipelined() from the next showRendered() on. Its mipmaps
  // are made only while setMipmaps() is on.
  void render();
  // Pipelined, a new frame is shown from the next loop iteration on
  // (showRendered()), so the headset frame drawn meanwhile does not wait for
  // its decoding, conversion and mpv render on the GPU. The sound is delayed
  // by frameSeconds (one headset frame) to match.
  void setPipelined(bool enabled, double frameSeconds);
  bool pipelined() const { return pipelined_; }
  // Pipelined: the frame render() drew last is texture() from now on.
  void showRendered();
  // --timing: video submissions/drops, separate from the XR-loop frame rate.
  // Uses cached asynchronous mpv properties; never waits for the player core.
  void logTiming(bool controlsVisible, bool previewBusy, bool xrRendering, bool xrFocused);
  // What --timing counts, kept as running totals for the performance
  // readout while setCounting() is on (always with --timing).
  struct Counts {
    uint64_t newFrames = 0;  // new video frames rendered or handed out, not redraws or repeats
    int64_t voDrops = -1, decoderDrops = -1;  // mpv's for this file, -1 = not known yet
    uint64_t restarts = 0;   // files started and seeks sent: what was before is not this playback
    bool seek = false;       // ... the last of them was a seek
    std::string decoder = "unknown";  // mpv's hwdec-current: "no" for software
    std::string hwPixelFormat;  // the decoded format under a hardware one (PlaybackState::pixelFormat)
  };
  void setCounting(bool on);
  const Counts& counts() const { return counts_; }
  // A seek is on its way or waiting.
  bool seeking() const { return seekStage_ != SeekStage::None || seekWaiting_; }
  // The frame shown was handed out by mpv for the eyes to sample (direct),
  // not drawn by mpv.
  bool directFrame() const { return directCurrent_ != nullptr; }
  DirectFrame* directCurrent() const { return directCurrent_; }
  // Before GL reads glTexture(): acquires it for GL and makes its mipmaps
  // match the current frame, if they do not yet.
  bool prepareRead();

  bool hasVideo() const { return video_.valid(); }
  // True from load() until the new file's first frame is in texture(); the
  // texture still holds the previous video meanwhile.
  bool loading() const { return loading_; }
  bool failed() const { return failed_; }
  // The current frame for the scene, nullptr before the first one.
  const Texture* texture();
  // ... and for GL (depth estimation), after prepareRead(); 0 if none,
  // also for a frame mpv handed out (directCurrent()), which stays out of GL
  // (DirectDepth reads it).
  GLuint glTexture();
  int width() const { return width_; }
  int height() const { return height_; }
  float aspect() const {
    return height_ > 0 ? static_cast<float>(width_) / height_ : 16.0f / 9.0f;
  }
  bool shutdownRequested() const { return shutdown_; }

 private:
  bool updateVideoSize(const mpv_node& params, bool preparing = false);
  void logPixelPath(int64_t displayW, int64_t displayH, int maxSize, int levels);
  void updateMipmaps(SharedImage* image);
  void applyTargetTrc();
  void releaseTarget();
  bool wantExport() const;
  void showDirect(DirectFrame* frame);
  void dropDirect();
  void returnFrames();
  // Sends a loadfile command; its reply names mpv's playlist entry for it.
  void loadFile(const char** cmd);
  struct SeekRequest {
    bool absolute = false;
    double amount = 0;  // seconds: the time, or the distance from the position
    bool exact = false;
    int merged = 1;  // requests it stands for
  };
  void requestSeek(const SeekRequest& request);
  void sendSeek();       // the waiting request, if nothing holds it back
  void resetSeeks();
  void cancelVideoSleep();
  bool videoReleaseComplete();
  void failVideoResume(const char* reason);
  void closeVideoForSleep(bool snapshot);
  void seekAfterSleep();
  void finishVideoResume();

  mpv_handle* mpv_ = nullptr;
  mpv_render_context* render_ = nullptr;
  GlInterop* interop_ = nullptr;
  DirectVideo* direct_ = nullptr;
  video_export export_{};
  // The frame shown and the next one, when mpv handed them out (else the
  // video_ slots have them).
  DirectFrame* directCurrent_ = nullptr;
  DirectFrame* directPending_ = nullptr;
  bool exportRefused_ = false;  // this video's colour is not one the shaders reproduce
  bool q10cRefused_ = false;    // its Main 10 frames cannot be sampled as the decoder wrote them
  VideoTexture video_;
  int width_ = 0;
  int height_ = 0;
  uint64_t sizeRequest_ = 0;
  uint64_t loadRequest_ = 0;
  PlaybackState playback_;
  std::string path_;  // of the loaded file, for redacting it from log output
  // Subtitles to add once the file is loaded: a remote file's beside it, or a Stash
  // scene's captions (in memory, titled by their file's name).
  struct Sidecar {
    std::string url, title, lang;
  };
  std::future<std::vector<Sidecar>> sidecarSubtitles_;
  std::atomic<bool> remoteFailed_{false};  // a remote file's read, or Stash's stream
  bool subtitlesInFrame_ = true;
  bool loading_ = false;
  bool failed_ = false;
  bool fileLoaded_ = false;     // the new file has been opened
  bool newVideoSized_ = false;  // ...and its video has been configured
  bool loadRestarted_ = false;  // its first decoded frame has reached the render API
  double loadedAt_ = 0;         // seconds, steady clock
  double loadSentAt_ = 0;       // when the last load() sent its loadfile, 0 after stop()
  bool loadStarted_ = false;    // mpv started a file since
  bool closeTimedOut_ = false;  // failed for the last file not closing in time
  // mpv's playlist entry of the last load() until it opens (-1 = none), and
  // of the file started last: a file replaced before it started is not a new
  // play. Only events of that entry count for the load. mpv replies to
  // loadfile before it starts the new file, but the last file's end can come
  // before the reply: until then no file event counts.
  int64_t countedEntry_ = -1;
  int64_t startedEntry_ = -1;
  bool loadReplyPending_ = false;
  bool opened_ = false;
  bool current_ = false;  // the file of the last load() has opened ...
  bool positionKnown_ = false;  // ... and its position came since
  bool framePending_ = false;
  bool pipelined_ = false;
  bool firstFrameDeferred_ = false;  // the new video's first frame waits for showRendered()
  bool mipmaps_ = true;
  bool sharpened_ = false;
  bool tvGamma_ = false;
  std::string sourceTrc_;            // the video's transfer, as mpv names it
  std::string targetTrc_ = "srgb";   // what mpv converts it to
  bool shutdown_ = false;
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
  bool timing_ = false;
  bool counting_ = false;        // --timing or the performance readout
  bool countsObserved_ = false;  // mpv's properties for counts_ are observed
  Counts counts_;
  struct VideoTiming {
    uint64_t file = 0;  // anonymous load number, not a path or library identity
    double since = 0;
    int frames = 0, redraws = 0, repeats = 0, unknown = 0;
    int loops = 0, controls = 0, previews = 0, xrFrames = 0, xrFocused = 0;
    int64_t lastVoDrops = -1, lastDecoderDrops = -1;
    double avSync = std::numeric_limits<double>::quiet_NaN();
    double cacheSeconds = std::numeric_limits<double>::quiet_NaN();
    double speed = 1;
    int buffering = -1, seeking = -1;
  } timingState_;
};
