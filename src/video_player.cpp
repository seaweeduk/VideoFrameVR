#include "video_player.h"
#include "bluray.h"
#include "remote.h"
#include "stash_stream.h"

#include <thread>

#include "direct_video.h"
#include "platform.h"

#include <mpv/client.h>
#include <mpv/render_gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <utility>
#include <vector>

namespace {

constexpr const char* kBt1886 = "bt.1886";  // mpv's name for SDR video's transfer

constexpr uint64_t kSizeReply = uint64_t(1) << 33;
constexpr uint64_t kTracksReply = uint64_t(1) << 34;
constexpr uint64_t kSleepVideoReply = uint64_t(1) << 35;
constexpr uint64_t kWakeVideoReply = uint64_t(1) << 36;
constexpr uint64_t kLoadReply = uint64_t(1) << 37;
constexpr uint64_t kViewsReply = uint64_t(1) << 38;
constexpr uint64_t kPixelPathReply = 2;
constexpr uint64_t kCommandReply = 3;
// How long the last file may take to close before a switch counts as failed.
constexpr double kCloseTimeout = 10.0;

void setProperty(mpv_handle* mpv, const char* name, const std::string& value) {
  const char* text = value.c_str();
  mpv_set_property_async(mpv, 0, name, MPV_FORMAT_STRING, &text);
}

void* getProcAddress(void*, const char* name) {
  return reinterpret_cast<void*>(glfwGetProcAddress(name));
}

void replaceAll(std::string& text, const std::string& from, const std::string& to) {
  if (from.empty()) return;
  for (size_t pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos + to.size()))
    text.replace(pos, from.size(), to);
}

// Keeps file names and folder structure out of the console output: the
// loaded file becomes <video>, any other absolute path becomes <path>.
std::string redactPaths(std::string text, const std::string& videoPath) {
  replaceAll(text, videoPath, "<video>");
  // A track's title, quoted in mpv's track lines, can be a file's or a
  // scene's name.
  const bool track = text.find("--vid=") != std::string::npos ||
                     text.find("--aid=") != std::string::npos ||
                     text.find("--sid=") != std::string::npos;
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    const char prev = i > 0 ? text[i - 1] : ' ';
    const bool tokenStart = std::isspace(static_cast<unsigned char>(prev)) ||
                            std::strchr("'\"(=:[", prev) != nullptr;
    if (track && text[i] == '\'') {
      const size_t end = text.find('\'', i + 1);
      if (end != std::string::npos) {
        out += "'<title>'";
        i = end + 1;
        continue;
      }
    }
    // A path, or a link (letters, then ://).
    size_t scheme = i;
    while (tokenStart && scheme < text.size() && std::isalpha(static_cast<unsigned char>(text[scheme])))
      ++scheme;
    const bool link = scheme > i && text.compare(scheme, 3, "://") == 0;
    if (!tokenStart || (text[i] != '/' && !link)) {
      out += text[i++];
      continue;
    }
    // Quoted, it ends at its quote (names have spaces), else at a space.
    const bool quoted = prev == '\'' || prev == '"';
    const size_t close = quoted ? text.find(prev, i) : std::string::npos;
    if (close != std::string::npos)
      i = close;
    else
      while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) &&
             text[i] != '\'' && text[i] != '"')
        ++i;
    out += link ? "<link>" : "<path>";
  }
  return out;
}

// Reads mpv's "chapter-list" property: an array of maps with time and title.
std::vector<Chapter> parseChapters(const mpv_node& list) {
  std::vector<Chapter> chapters;
  if (list.format != MPV_FORMAT_NODE_ARRAY) return chapters;
  for (int i = 0; i < list.u.list->num; ++i) {
    const mpv_node& item = list.u.list->values[i];
    if (item.format != MPV_FORMAT_NODE_MAP) continue;
    Chapter c;
    for (int k = 0; k < item.u.list->num; ++k) {
      const std::string key = item.u.list->keys[k];
      const mpv_node& v = item.u.list->values[k];
      if (key == "time" && v.format == MPV_FORMAT_DOUBLE) c.time = v.u.double_;
      else if (key == "title" && v.format == MPV_FORMAT_STRING) c.title = v.u.string;
    }
    chapters.push_back(c);
  }
  return chapters;
}

// Reads mpv's "track-list" property: an array of maps, one per track.
std::vector<Track> parseTracks(const mpv_node& list) {
  std::vector<Track> tracks;
  if (list.format != MPV_FORMAT_NODE_ARRAY) return tracks;
  for (int i = 0; i < list.u.list->num; ++i) {
    const mpv_node& item = list.u.list->values[i];
    if (item.format != MPV_FORMAT_NODE_MAP) continue;
    Track t;
    for (int k = 0; k < item.u.list->num; ++k) {
      const std::string key = item.u.list->keys[k];
      const mpv_node& v = item.u.list->values[k];
      if (key == "id" && v.format == MPV_FORMAT_INT64) t.id = v.u.int64;
      else if (key == "type" && v.format == MPV_FORMAT_STRING) t.type = v.u.string;
      else if (key == "title" && v.format == MPV_FORMAT_STRING) t.title = v.u.string;
      else if (key == "lang" && v.format == MPV_FORMAT_STRING) t.lang = v.u.string;
      else if (key == "codec" && v.format == MPV_FORMAT_STRING) t.codec = v.u.string;
      else if (key == "selected" && v.format == MPV_FORMAT_FLAG) t.selected = v.u.flag != 0;
      else if (key == "src-id" && v.format == MPV_FORMAT_INT64) t.sourceId = v.u.int64;
    }
    tracks.push_back(t);
  }
  return tracks;
}

}  // namespace

VideoPlayer::~VideoPlayer() {
  if (render_) mpv_render_context_free(render_);
  releaseTarget();
  if (mpv_) mpv_terminate_destroy(mpv_);
}

bool VideoPlayer::init(const std::string& hwdec, GlInterop& interop, bool timing,
                       bool decoderQueue) {
  timing_ = timing;
  counting_ = timing;
  interop_ = &interop;
  export_.magic = VIDEO_EXPORT_MAGIC;
  export_.size = sizeof(export_);
  export_.fence_fd = -1;
  video_.init(interop);
  // libmpv refuses to start unless numbers are formatted the C way.
  std::setlocale(LC_NUMERIC, "C");

  mpv_ = mpv_create();
  if (!mpv_) {
    std::fprintf(stderr, "mpv_create failed\n");
    return false;
  }
  mpv_set_option_string(mpv_, "vo", "libmpv");
  mpv_set_option_string(mpv_, "hwdec", hwdec.c_str());
  v4l2_ = hwdec == "v4l2m2m";
  if (hwdec == "v4l2m2m-copy" || hwdec == "v4l2m2m") {
    if (kStandaloneHeadset && decoderQueue) {
      // V4L2 calls can block mpv's playback core past a frame's deadline.
      mpv_set_option_string(mpv_, "vd-queue-enable", "yes");
      mpv_set_option_string(mpv_, "vd-queue-max-samples", "2");
    }
    // Frame's decoder must know bit depth/chroma before selecting V4L2.
    // mpv's native MKV demuxer and abbreviated lavf probing can omit them,
    // bypassing the private decoder's unsupported-format fallback.
    mpv_set_option_string(mpv_, "demuxer", "lavf");
    mpv_set_option_string(mpv_, "demuxer-lavf-probe-info", "yes");
    // Iris can report a few damaged leading frames after an HEVC seek.
    // The private mpv patch discards these; allow recovery before falling back.
    mpv_set_option_string(mpv_, "hwdec-software-fallback", "8");
    // Iris stalled on a 4K VP9 file without reporting an error, so mpv never
    // fell back and the picture froze. Software decodes 4K60 VP9 at about
    // 170 fps on the Frame's cores; H.264 and HEVC stay on the hardware.
    mpv_set_option_string(mpv_, "hwdec-codecs", "h264,hevc");
  }
  if (hwdec.rfind("nvdec", 0) == 0) {
    // Spare decoder frames beyond the video's reference frames, each a full
    // frame of video memory (8K 10-bit: about 290 MB). mpv keeps 6 for
    // interpolation, which this player doesn't use; 2 played five 6K-8K HEVC
    // files with seeks as 6 did. 3 keeps one in hand: 0.5-0.9 GB less at 8K.
    mpv_set_option_string(mpv_, "hwdec-extra-frames", "3");
  }
  mpv_set_option_string(mpv_, "keep-open", "yes");
  // A still image (e.g. a cover) stays rather than ending playback after a second.
  mpv_set_option_string(mpv_, "image-display-duration", "inf");
  mpv_set_option_string(mpv_, "idle", "yes");
  // Output sRGB into an sRGB texture: sampling and mipmapping decode it to
  // linear light, like the old float texture, at a fraction of the cost (an 8K
  // frame drew in ~13 ms into RGBA16F, ~0.6 ms into SRGB8_ALPHA8).
  // HDR is tone-mapped by mpv. SDR keeps its own values once its transfer is
  // known (applyTargetTrc).
  mpv_set_option_string(mpv_, "target-trc", "srgb");
  mpv_set_option_string(mpv_, "target-prim", "bt.709");
  // The frame is drawn at its own size, so only chroma is scaled. mpv's
  // default high-quality kernels cost 14-28 ms per 8K frame; bilinear 0.6 ms.
  mpv_set_option_string(mpv_, "scale", "bilinear");
  mpv_set_option_string(mpv_, "dscale", "bilinear");
  mpv_set_option_string(mpv_, "cscale", "bilinear");
  mpv_set_option_string(mpv_, "correct-downscaling", "no");
  mpv_set_option_string(mpv_, "linear-downscaling", "no");
  mpv_set_option_string(mpv_, "sigmoid-upscaling", "no");
  // HDR video on the headset's SDR displays: the BT.2390 tone curve, with
  // the brightness measured per scene.
  // (The render API draws with vo_gpu, which knows fewer curves than gpu-next.)
  mpv_set_option_string(mpv_, "tone-mapping", "bt.2390");
  mpv_set_option_string(mpv_, "hdr-compute-peak", "yes");
  // Keep the videos' own metadata (titles, descriptions) out of the log.
  mpv_set_option_string(mpv_, "display-tags", "");
  // FFmpeg's demuxer warns about harmless things (e.g. fonts attached to MKV
  // files, "could not find codec parameters"); only its errors are of use.
  mpv_set_option_string(mpv_, "msg-level", "ffmpeg/demuxer=error");
  // The 8-bit target would band 10-bit sources; dithering costs nothing measurable.
  mpv_set_option_string(mpv_, "dither-depth", "8");
  // Required when rendering without blocking for the target time.
  mpv_set_option_string(mpv_, "video-timing-offset", "0");
  // mpv's OSD would be cut in half on stereo video; the VR menu replaces it.
  mpv_set_option_string(mpv_, "osd-level", "0");
  // Links play as mpv opens them itself, never through yt-dlp.
  mpv_set_option_string(mpv_, "ytdl", "no");
  // A large read-ahead buffer, so 8K streams survive slow moments. It fills at
  // once, even from a local disk, so on the Frame, whose memory the GPU,
  // SteamVR and Steam share, it is smaller: 384 MiB is still about 40 s of
  // 80 Mbit/s 8K (1.5 GiB was over 2 minutes).
  mpv_set_option_string(mpv_, "cache", "yes");
  mpv_set_option_string(mpv_, "demuxer-max-bytes", kStandaloneHeadset ? "384MiB" : "1536MiB");
  mpv_set_option_string(mpv_, "demuxer-max-back-bytes", kStandaloneHeadset ? "64MiB" : "256MiB");
  // Info and up, as msg-level above says per module ("info" would ignore it).
  mpv_request_log_messages(mpv_, "terminal-default");
  // Benchmark hook: VIDEOFRAMEVR_MPV_OPTS="key=value,key=value" overrides the above.
  if (const char* extra = std::getenv("VIDEOFRAMEVR_MPV_OPTS")) {
    std::string opts = extra;
    size_t start = 0;
    while (start < opts.size()) {
      size_t end = opts.find(',', start);
      if (end == std::string::npos) end = opts.size();
      const std::string pair = opts.substr(start, end - start);
      const size_t eq = pair.find('=');
      if (eq != std::string::npos) {
        const std::string key = pair.substr(0, eq), value = pair.substr(eq + 1);
        const int rc = mpv_set_option_string(mpv_, key.c_str(), value.c_str());
        std::printf("mpv option %s=%s -> %d\n", key.c_str(), value.c_str(), rc);
      }
      start = end + 1;
    }
  }

  if (mpv_initialize(mpv_) < 0) {
    std::fprintf(stderr, "mpv_initialize failed\n");
    return false;
  }
  remote::addTo(mpv_, false, &remoteFailed_);
  addStashStreams(mpv_, false, &remoteFailed_);
  bdmv::addTo(mpv_, false, &remoteFailed_);
  // This app's own mpv decodes both views of 3D Blu-ray (MVC) video.
  char* mvc = mpv_get_property_string(mpv_, "option-info/vd-lavc-mvc/name");
  bdmv::setPlayBothViews(mvc != nullptr);
  mpv_free(mvc);
  mpv_observe_property(mpv_, 0, "time-pos", MPV_FORMAT_DOUBLE);
  mpv_observe_property(mpv_, 0, "duration", MPV_FORMAT_DOUBLE);
  mpv_observe_property(mpv_, 0, "demuxer-cache-time", MPV_FORMAT_DOUBLE);
  mpv_observe_property(mpv_, 0, "pause", MPV_FORMAT_FLAG);
  mpv_observe_property(mpv_, 0, "volume", MPV_FORMAT_DOUBLE);
  mpv_observe_property(mpv_, 0, "mute", MPV_FORMAT_FLAG);
  mpv_observe_property(mpv_, 0, "media-title", MPV_FORMAT_STRING);
  mpv_observe_property(mpv_, 0, "track-list", MPV_FORMAT_NODE);
  // Available even while mpv itself does not draw the subtitles.
  mpv_observe_property(mpv_, 0, "sub-text", MPV_FORMAT_STRING);
  mpv_observe_property(mpv_, 0, "eof-reached", MPV_FORMAT_FLAG);
  mpv_observe_property(mpv_, 0, "video-format", MPV_FORMAT_STRING);
  mpv_observe_property(mpv_, 0, "video-params", MPV_FORMAT_NODE);
  mpv_observe_property(mpv_, 0, "video-params/pixelformat", MPV_FORMAT_STRING);
  mpv_observe_property(mpv_, 0, "video-params/gamma", MPV_FORMAT_STRING);
  mpv_observe_property(mpv_, 0, "container-fps", MPV_FORMAT_DOUBLE);
  mpv_observe_property(mpv_, 0, "audio-codec-name", MPV_FORMAT_STRING);
  mpv_observe_property(mpv_, 0, "audio-params/channel-count", MPV_FORMAT_INT64);
  mpv_observe_property(mpv_, 0, "chapter-list", MPV_FORMAT_NODE);
  mpv_observe_property(mpv_, 0, "chapter", MPV_FORMAT_INT64);
  mpv_observe_property(mpv_, 0, "video-mvc", MPV_FORMAT_STRING);
  // Always: the strip warns of software decoding. It changes only as a
  // decoder starts.
  mpv_observe_property(mpv_, 1, "hwdec-current", MPV_FORMAT_STRING);
  if (timing_) {
    setCounting(true);
    // Separate userdata keeps diagnostic events out of the playback/UI state.
    mpv_observe_property(mpv_, 1, "avsync", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 1, "demuxer-cache-duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 1, "speed", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 1, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, 1, "seeking", MPV_FORMAT_FLAG);
  }

  mpv_opengl_init_params glInit{getProcAddress, nullptr};
  mpv_render_param params[] = {
      {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
      {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
      {MPV_RENDER_PARAM_INVALID, nullptr},
  };
  if (mpv_render_context_create(&render_, mpv_, params) < 0) {
    std::fprintf(stderr, "mpv_render_context_create failed\n");
    return false;
  }
  return true;
}

void VideoPlayer::setCounting(bool on) {
  counting_ = on || timing_;
  if (!counting_ || countsObserved_ || !mpv_) return;
  countsObserved_ = true;
  mpv_observe_property(mpv_, 1, "frame-drop-count", MPV_FORMAT_INT64);
  mpv_observe_property(mpv_, 1, "decoder-frame-drop-count", MPV_FORMAT_INT64);
  mpv_observe_property(mpv_, 1, "video-params/hw-pixelformat", MPV_FORMAT_STRING);
}

static double nowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void VideoPlayer::load(const std::string& path, double start) {
  if (export_.close_pending && !sleepCloseReplied_ && !sleepCloseInFlight_)
    closeVideoForSleep(false);
  if (export_.close_pending && !videoReleaseComplete()) {
    deferredLoadPath_ = path;
    deferredLoadStart_ = start;
    deferredStop_ = false;
    deferredActionAt_ = nowSeconds();
    loading_ = true;
    opened_ = false;
    newVideoSized_ = false;
    return;
  }
  deferredLoadPath_.clear();
  deferredStop_ = false;
  const bool wasAsleep = videoSleep_ != VideoSleep::Awake;
  cancelVideoSleep();
  ++sizeRequest_;
  ++loadRequest_;
  path_ = path;
  remoteFailed_ = false;
  discChapters_ = false;
  sidecarSubtitles_ = {};
  if (remote::isRemote(path) || isStashScene(path)) {
    std::packaged_task<std::vector<Sidecar>()> find([path] {
      std::vector<Sidecar> subtitles;
      const std::string stem = std::filesystem::path(path).stem().string();
      if (remote::isRemote(path))
        for (const std::string& sub : remote::subtitlesBeside(path)) subtitles.push_back({remote::mpvUrl(sub, true)});
      else
        for (const StashCaption& c : stashCaptions(path))
          subtitles.push_back({"memory://" + c.text,
                               stem + (c.language.empty() ? "" : "." + c.language) + "." + c.type, c.language});
      return subtitles;
    });
    sidecarSubtitles_ = find.get_future();
    std::thread(std::move(find)).detach();
  }
  loading_ = true;
  failed_ = false;
  playback_.title.clear();  // "Loading …" until this file's title, not the last one's
  playback_.views = TwoViews::No;  // until mpv says (setViews())
  exportRefused_ = compressedRefused_ = false;
  fileLoaded_ = false;
  newVideoSized_ = false;
  loadRestarted_ = false;
  firstFrameDeferred_ = false;
  opened_ = false;
  current_ = positionKnown_ = false;
  resetSeeks();
  // The scene shows nothing until the new first frame, and the old decoder's
  // session lasts while any of its frames is held (pollEvents()).
  dropDirect();
  if (direct_) direct_->dropStill();
  // File-local track selection restores itself when this file closes. A
  // sleeping file's vid=no must never disable the next file or reopen the old.
  const std::string discOptions = bdmv::mpvOptions(path);
  baseViewRight_ = !discOptions.empty();
  const std::string startOption = "start=" + std::to_string(start) + ",vid=auto,pause=" +
      (wasAsleep && sleepPaused_ ? "yes" : "no") + discOptions;
  const std::string url = bdmv::isDiscImage(path) ? bdmv::mpvUrl(path) : remote::mpvUrl(path);
  const char* resumed[] = {"loadfile", url.c_str(), "replace", "-1", startOption.c_str(), nullptr};
  loadFile(resumed);
}

void VideoPlayer::loadFile(const char** cmd) {
  countedEntry_ = -1;
  loadReplyPending_ = true;
  loadStarted_ = closeTimedOut_ = false;
  loadSentAt_ = nowSeconds();
  const int error = mpv_command_async(mpv_, kLoadReply + loadRequest_, cmd);
  if (error < 0) {
    std::fprintf(stderr, "Playback failed: %s\n", mpv_error_string(error));
    loadReplyPending_ = false;
    failed_ = true;
    loading_ = false;
  }
}

bool VideoPlayer::takeOpened() {
  if (loading_ || !loadRestarted_ || !newVideoSized_ || failed_) return false;
  const bool opened = opened_;
  opened_ = false;
  return opened;
}

void VideoPlayer::stop() {
  if (export_.close_pending && !sleepCloseReplied_ && !sleepCloseInFlight_)
    closeVideoForSleep(false);
  if (export_.close_pending && !videoReleaseComplete()) {
    deferredLoadPath_.clear();
    deferredStop_ = true;
    deferredActionAt_ = nowSeconds();
    loading_ = opened_ = false;
    return;
  }
  deferredLoadPath_.clear();
  deferredStop_ = false;
  cancelVideoSleep();
  remoteFailed_ = false;
  sidecarSubtitles_ = {};
  ++sizeRequest_;
  ++loadRequest_;
  const char* cmd[] = {"stop", nullptr};
  mpv_command_async(mpv_, 0, cmd);
  path_.clear();
  updateChapters();
  loading_ = false;
  failed_ = false;
  countedEntry_ = -1;
  loadReplyPending_ = false;
  loadSentAt_ = 0;
  closeTimedOut_ = false;
  opened_ = false;
  current_ = positionKnown_ = false;
  resetSeeks();
  releaseTarget();  // no stale frame from the closed file
}

void VideoPlayer::command(const char* cmd) {
  if (videoSleep_ != VideoSleep::Awake) {
    if (std::strcmp(cmd, "cycle pause") == 0) {
      sleepPaused_ = !sleepPaused_;
      playback_.paused = sleepPaused_;
      return;
    }
    if (std::strcmp(cmd, "set pause yes") == 0 || std::strcmp(cmd, "set pause no") == 0) {
      sleepPaused_ = std::strcmp(cmd, "set pause yes") == 0;
      playback_.paused = sleepPaused_;
      return;
    }
  }
  // Words separated by spaces, as all callers write them: mpv_command_string()
  // has no asynchronous form.
  std::vector<std::string> words;
  std::istringstream in(cmd);
  for (std::string word; in >> word;) words.push_back(word);
  std::vector<const char*> args;
  for (const std::string& word : words) args.push_back(word.c_str());
  args.push_back(nullptr);
  const int err = mpv_command_async(mpv_, kCommandReply, args.data());
  if (err < 0) std::fprintf(stderr, "mpv command '%s' failed: %s\n", cmd, mpv_error_string(err));
}

void VideoPlayer::cancelVideoSleep() {
  videoSleep_ = VideoSleep::Awake;
  if (videoReleased_) export_.close_pending = 0;
  resumeSeek_ = 0;
  hiddenSince_ = -1;
}

bool VideoPlayer::videoReleaseComplete() {
  if (videoReleased_) return true;
  if (!sleepCloseReplied_) return false;
  returnFrames();
  if (direct_) direct_->dropUnused();
  const uint32_t session = __atomic_load_n(&export_.session, __ATOMIC_ACQUIRE);
  const bool released = !direct_ || direct_->sessionReleased(session);
  if (released) __atomic_store_n(&export_.session_done, session, __ATOMIC_RELEASE);
  videoReleased_ = export_.buffers_released && released;
  return videoReleased_;
}

void VideoPlayer::failVideoResume(const char* reason) {
  std::fprintf(stderr, "Video decoder transition failed: %s\n", reason);
  dropDirect();
  if (direct_) direct_->cancelPendingStill();
  export_.close_pending = 1;
  export_.buffers_released = 0;
  sleepCloseReplied_ = false;
  videoReleased_ = false;
  const char* no = "no";
  sleepCloseInFlight_ = mpv_set_property_async(mpv_, kSleepVideoReply + loadRequest_,
                                             "vid", MPV_FORMAT_STRING, &no) >= 0;
  setProperty(mpv_, "pause", "yes");
  resetSeeks();
  videoSleep_ = VideoSleep::Closed;
  failed_ = true;
}

void VideoPlayer::setPresentationActive(bool active) {
  if (!mpv_ || failed_) return;
  if (active && !presentationActive_) sleepActionAt_ = nowSeconds();
  presentationActive_ = active;
  if (!active && (videoSleep_ == VideoSleep::Opening || videoSleep_ == VideoSleep::Seeking)) {
    closeVideoForSleep(false);
    return;
  }
  if (videoSleep_ == VideoSleep::Seeking && resumeRestarted_ && resumePictureShown_ && newVideoSized_ &&
      (counts_.decoder == "v4l2m2m" || counts_.decoder == "v4l2m2m-copy"))
    finishVideoResume();
  if (active && videoSleep_ != VideoSleep::Awake &&
      nowSeconds() - sleepActionAt_ > 10) {
    failVideoResume("decoder transition timed out");
    return;
  }
  if (active) {
    hiddenSince_ = -1;
    if (videoSleep_ != VideoSleep::Closed) return;
    if (!deferredLoadPath_.empty() || deferredStop_ || !videoReleaseComplete()) return;
    export_.close_pending = 0;
    const char* track = sleepTrack_.c_str();
    const int ret = mpv_set_property_async(mpv_, kWakeVideoReply + loadRequest_,
                                          "vid", MPV_FORMAT_STRING, &track);
    if (ret < 0) {
      failVideoResume(mpv_error_string(ret));
      return;
    }
    videoSleep_ = VideoSleep::Opening;
    sleepActionAt_ = nowSeconds();
    loading_ = true;
    newVideoSized_ = false;
    ++sizeRequest_;
    resumeFrameSeen_ = false;
    resumePictureShown_ = false;
    return;
  }
  if (hiddenSince_ < 0) hiddenSince_ = nowSeconds();
  if (videoSleep_ != VideoSleep::Awake || nowSeconds() - hiddenSince_ < 0.25 ||
      path_.empty() || !fileLoaded_ || !positionKnown_ || failed_ || loading_)
    return;
  closeVideoForSleep(true);
}

void VideoPlayer::closeVideoForSleep(bool snapshot) {
  if (snapshot) {
    sleepTrack_ = "auto";
    bool selected = false;
    for (const auto& track : playback_.tracks)
      if (track.type == "video" && track.selected) {
        sleepTrack_ = std::to_string(track.id);
        selected = true;
      }
    if (!selected) return;
    sleepPosition_ = seekStage_ != SeekStage::None ? seekTarget_ : playback_.position;
    if (seekWaiting_)
      sleepPosition_ = waitingSeek_.absolute ? waitingSeek_.amount : sleepPosition_ + waitingSeek_.amount;
    sleepPosition_ = std::max(0.0, sleepPosition_);
    if (playback_.duration > 0) sleepPosition_ = std::min(sleepPosition_, playback_.duration);
    sleepPaused_ = playback_.paused;
  }
  resetSeeks();
  ++sizeRequest_;
  dropDirect();
  if (direct_) direct_->cancelPendingStill();
  export_.close_pending = 1;
  export_.buffers_released = 0;
  sleepCloseReplied_ = false;
  videoReleased_ = false;
  setProperty(mpv_, "pause", "yes");
  const char* no = "no";
  const int ret = mpv_set_property_async(mpv_, kSleepVideoReply + loadRequest_,
                                        "vid", MPV_FORMAT_STRING, &no);
  sleepCloseInFlight_ = ret >= 0;
  if (ret < 0) {
    failVideoResume(mpv_error_string(ret));
    return;
  }
  videoSleep_ = VideoSleep::Closing;
  sleepActionAt_ = nowSeconds();
}

void VideoPlayer::seekAfterSleep() {
  resetSeeks();
  videoSleep_ = VideoSleep::Awake;
  // This is a decoder restart, not a new file waiting for its first picture.
  // The resume seek must be sent before that picture can become available.
  loading_ = false;
  seekTo(sleepPosition_, true);
  if (seekStage_ == SeekStage::None) {
    failVideoResume("resume seek could not be submitted");
    return;
  }
  videoSleep_ = VideoSleep::Seeking;
  resumeSeek_ = seekCount_;
  sleepSeekChanged_ = false;
  resumeRestarted_ = false;
  resumeFrameSeen_ = false;
  resumePictureShown_ = false;
}

void VideoPlayer::finishVideoResume() {
  if (sleepSeekChanged_) {
    seekAfterSleep();
  } else if (!presentationActive_) {
    closeVideoForSleep(false);
  } else {
    videoSleep_ = VideoSleep::Awake;
    setProperty(mpv_, "pause", sleepPaused_ ? "yes" : "no");
    std::printf("Video decoder resumed at the saved position\n");
  }
}

// A disc's transport streams carry no languages; its playlist names them by
// PID, which mpv gives as each track's src-id.
void VideoPlayer::labelDiscTracks() {
  if (!bdmv::isDiscImage(path_)) return;
  const auto disc = bdmv::known(path_);
  if (!disc) return;
  for (Track& track : playback_.tracks) {
    if (!track.lang.empty()) continue;
    for (const bdmv::Track& t : disc->title.tracks)
      if (t.pid == track.sourceId) track.lang = t.lang;
  }
}

// How mpv decodes this file's two views (video-mvc; null: not MVC).
void VideoPlayer::setViews(const char* mode) {
  const TwoViews views = !mode                             ? TwoViews::No
                         : std::strcmp(mode, "stereo") == 0 ? TwoViews::Both
                                                            : TwoViews::BaseOnly;
  const TwoViews was = playback_.views;
  playback_.views = views;
  // The disc was read after the file was asked for: its base view is the
  // right eye's. mpv restarts the decoder where it is.
  if (views == TwoViews::Both && !baseViewRight_ && !bdmv::mpvOptions(path_).empty()) {
    baseViewRight_ = true;
    const char* right = "right";
    mpv_set_property_async(mpv_, 0, "file-local-options/vd-lavc-mvc-base-view", MPV_FORMAT_STRING,
                           &right);
  }
  // 3D gave up during playback (mpv says why): its decoder carries on in 2D
  // from the next keyframe. Decoding again from the last one, out of the
  // cache, keeps the picture going.
  if (was == TwoViews::Both && views == TwoViews::BaseOnly && current_ && !loading_ &&
      videoSleep_ == VideoSleep::Awake && !seeking())
    seekTo(playback_.position, true);
}

// mpv's chapters, else a disc playlist's. Also once a file has loaded: mpv
// tells nothing when its list is the last file's (none, for most discs).
void VideoPlayer::updateChapters() {
  playback_.chapters = mpvChapters_;
  discChapters_ = false;
  if (playback_.chapters.empty() && bdmv::isDiscImage(path_))
    if (const auto disc = bdmv::known(path_)) {
      for (const double time : disc->title.chapters) playback_.chapters.push_back({time, {}});
      discChapters_ = !playback_.chapters.empty();
    }
  if (discChapters_)
    updateDiscChapter();
  else
    playback_.chapter = mpvChapter_;
}

void VideoPlayer::updateDiscChapter() {
  if (!discChapters_) return;
  int chapter = -1;
  for (size_t i = 0; i < playback_.chapters.size(); ++i)
    if (playback_.chapters[i].time <= playback_.position + 0.01) chapter = static_cast<int>(i);
  playback_.chapter = chapter;
}

// A keyframe seek on its way holds the next back this long at most, waiting
// for its picture; one that never arrives is forgotten after kSeekGiveUp.
constexpr double kSeekPatience = 1.5;
constexpr double kSeekGiveUp = 6.0;
// Seek replies carry this plus the seek's number.
constexpr uint64_t kSeekReply = uint64_t(1) << 32;

void VideoPlayer::seekBy(double seconds) {
  SeekRequest r;
  r.amount = seconds;
  requestSeek(r);
}

void VideoPlayer::seekTo(double seconds, bool exact) {
  SeekRequest r;
  r.absolute = true;
  r.amount = seconds;
  r.exact = exact;
  requestSeek(r);
}

void VideoPlayer::requestSeek(const SeekRequest& request) {
  if (videoSleep_ != VideoSleep::Awake) {
    sleepPosition_ = request.absolute ? request.amount : sleepPosition_ + request.amount;
    sleepPosition_ = std::max(0.0, sleepPosition_);
    if (playback_.duration > 0) sleepPosition_ = std::min(sleepPosition_, playback_.duration);
    playback_.position = sleepPosition_;
    sleepSeekChanged_ = true;
    return;
  }
  if (path_.empty()) return;
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
  sendSeek();
}

void VideoPlayer::sendSeek() {
  const double now = nowSeconds();
  if (seekStage_ != SeekStage::None && now - seekSentAt_ > kSeekGiveUp) {
    if (timing_)
      std::printf("[seek-timing] #%llu no frame after %.0f ms, given up\n",
                  static_cast<unsigned long long>(seekCount_), (now - seekSentAt_) * 1000);
    seekStage_ = SeekStage::None;
  }
  // A new file's seeks wait for its first frame.
  if (!seekWaiting_ || loading_ || !fileLoaded_) return;
  if (seekStage_ != SeekStage::None && !seekExact_ && now - seekSentAt_ < kSeekPatience) return;
  SeekRequest r = waitingSeek_;
  seekWaiting_ = false;
  if (r.absolute) {
    r.amount = std::max(0.0, r.amount);  // mpv counts negative times from the end
    if (playback_.duration > 0) r.amount = std::min(r.amount, playback_.duration);
  }
  const std::string amount = std::to_string(r.amount);
  const char* flags = !r.absolute ? "relative+keyframes" : r.exact ? "absolute+exact" : "absolute+keyframes";
  const char* cmd[] = {"seek", amount.c_str(), flags, nullptr};
  // Asynchronous: mpv's core may be busy, and the frame loop never waits for it.
  if (mpv_command_async(mpv_, kSeekReply + seekCount_ + 1, cmd) < 0) return;
  ++seekCount_;
  ++counts_.restarts;
  counts_.seek = true;
  if (timing_)
    std::printf("[seek-timing] #%llu %s %.2f (%d asked)%s from pos=%.2f\n",
                static_cast<unsigned long long>(seekCount_), flags, r.amount, r.merged,
                seekStage_ != SeekStage::None ? ", replacing the one on its way" : "",
                playback_.position);
  seekStage_ = SeekStage::Sent;
  seekTarget_ = r.absolute ? r.amount : playback_.position + r.amount;
  seekExact_ = r.exact;
  seekSentAt_ = now;
}

void VideoPlayer::resetSeeks() {
  seekStage_ = SeekStage::None;
  seekWaiting_ = false;
}

void VideoPlayer::addSubtitle(const std::string& path) {
  const std::string url = remote::mpvUrl(path, true);
  const char* cmd[] = {"sub-add", url.c_str(), "select", nullptr};
  mpv_command_async(mpv_, 0, cmd);
}

void VideoPlayer::setSubtitlesInFrame(bool visible) {
  if (visible == subtitlesInFrame_) return;
  subtitlesInFrame_ = visible;
  setProperty(mpv_, "sub-visibility", visible ? "yes" : "no");
}

void VideoPlayer::setSubtitleStyle(double scale, int raise) {
  setProperty(mpv_, "sub-scale", std::to_string(scale));
  // sub-pos: 100 = the bottom edge, smaller = higher up.
  setProperty(mpv_, "sub-pos", std::to_string(std::clamp(100 - raise, 0, 150)));
}

void VideoPlayer::setPicture(int brightness, int contrast, int saturation) {
  picture_[0] = brightness;
  picture_[1] = contrast;
  picture_[2] = saturation;
  if (!plainPicture_) sendPicture();
  if (direct_) direct_->setPicture(brightness, contrast, saturation);
}

void VideoPlayer::setPlainPicture(bool plain) {
  if (plain == plainPicture_) return;
  plainPicture_ = plain;
  sendPicture();
}

void VideoPlayer::sendPicture() {
  const char* names[3] = {"brightness", "contrast", "saturation"};
  for (int i = 0; i < 3; ++i) setProperty(mpv_, names[i], std::to_string(plainPicture_ ? 0 : picture_[i]));
}

void VideoPlayer::setTvGamma(bool tvGamma) {
  tvGamma_ = tvGamma;
  applyTargetTrc();
}

// mpv converts the video's transfer to target-trc. VR players show SDR video
// with its own values, so the target is the source's transfer (no change);
// HDR (and linear) video still goes to sRGB through mpv's tone mapping.
bool VideoPlayer::tvGammaImage() const { return tvGamma_ && sourceTrc_ == kBt1886; }

void VideoPlayer::applyTargetTrc() {
  static const char* const kToSrgb[] = {"pq", "hlg", "v-log", "s-log1", "s-log2", "st428",
                                        "linear"};
  bool toSrgb = tvGamma_ || sourceTrc_.empty() || sourceTrc_ == "auto";
  for (const char* t : kToSrgb) toSrgb = toSrgb || sourceTrc_ == t;
  const std::string target = toSrgb ? "srgb" : sourceTrc_;
  // Frames handed out decode BT.1886 themselves (DirectVideo::setTvGamma).
  if (direct_) direct_->setTvGamma(tvGamma_ && sourceTrc_ == kBt1886);
  if (target == targetTrc_) return;
  targetTrc_ = target;
  setProperty(mpv_, "target-trc", target);
  // Not while a new file's is still unknown: it follows once known.
  if (!sourceTrc_.empty())
    std::printf("Video transfer %s, shown as %s\n", sourceTrc_.c_str(),
                target == sourceTrc_ ? "is (no conversion)" : "sRGB");
}

void VideoPlayer::pollEvents() {
  // A decoder closed while mpv's decoder thread waits (video_export.h). Each
  // frame it decoded keeps its whole session alive, so the frames not shown
  // go back first: the next file's 8K session does not fit beside the old.
  const uint32_t session = __atomic_load_n(&export_.session, __ATOMIC_ACQUIRE);
  if (direct_ && session != export_.session_done) {
    // A seek's: the frame shown until the new session's first goes back too,
    // a copy of it shown meanwhile (DirectVideo::still), and the next one
    // queued from before the seek is not shown.
    if (export_.close_pending) {
      dropDirect();
      direct_->cancelPendingStill();
    } else if (directCurrent_ && direct_->closedSession(*directCurrent_, session)) {
      if (DirectFrame* still = direct_->still(*directCurrent_)) {
        direct_->hold(directCurrent_, false);
        directCurrent_ = still;
      }
    }
    if (directPending_ && direct_->closedSession(*directPending_, session)) {
      direct_->hold(directPending_, false);
      directPending_ = nullptr;
    }
    returnFrames();
    direct_->dropUnused();
    if (direct_->sessionReleased(session))
      __atomic_store_n(&export_.session_done, session, __ATOMIC_RELEASE);
  } else if (export_.close_pending) {
    returnFrames();
    if (direct_) direct_->dropUnused();
  }
  // Files without video never deliver a first frame; stop waiting for one.
  if (loading_ && fileLoaded_ && nowSeconds() - loadedAt_ > 10.0) loading_ = false;
  // Closing the last file can wait for its decoder, which on the Frame may
  // need the frames this app was showing back first: they go back as soon as
  // the GPU is done with them, not only with the next file's first frame.
  if (loading_ && !loadStarted_ && loadSentAt_ > 0) returnFrames();
  // The new file starts once the last one has closed: not at all if closing
  // it got stuck, which would otherwise load forever.
  if (loading_ && !loadStarted_ && loadSentAt_ > 0 && nowSeconds() - loadSentAt_ > kCloseTimeout) {
    std::fprintf(stderr, "Playback failed: the last video did not close\n");
    closeTimedOut_ = failed_ = true;
    loading_ = false;
  }
  while (true) {
    mpv_event* ev = mpv_wait_event(mpv_, 0);
    if (ev->event_id == MPV_EVENT_NONE) break;
    switch (ev->event_id) {
      case MPV_EVENT_LOG_MESSAGE: {
        auto* msg = static_cast<mpv_event_log_message*>(ev->data);
        std::fprintf(stderr, "[mpv/%s] %s", msg->prefix, redactPaths(msg->text, path_).c_str());
        break;
      }
      case MPV_EVENT_START_FILE:
        startedEntry_ = static_cast<mpv_event_start_file*>(ev->data)->playlist_entry_id;
        if (countedEntry_ > 0 && startedEntry_ == countedEntry_) {
          loadStarted_ = true;
          if (closeTimedOut_) {  // late, but it closed after all
            closeTimedOut_ = failed_ = false;
            loading_ = true;
          }
        }
        ++counts_.restarts;
        counts_.seek = false;
        if (timing_) {
          ++timingState_.file;
          timingState_.since = 0;
          timingState_.lastVoDrops = timingState_.lastDecoderDrops = -1;
          std::printf("[video-timing] file=%llu start\n",
                      static_cast<unsigned long long>(timingState_.file));
        }
        break;
      case MPV_EVENT_FILE_LOADED:
        if (loadReplyPending_ || (countedEntry_ > 0 && countedEntry_ != startedEntry_)) break;
        fileLoaded_ = true;
        loadedAt_ = nowSeconds();
        updateChapters();
        if (countedEntry_ > 0) {
          countedEntry_ = -1;
          opened_ = current_ = true;
          mpv_get_property_async(mpv_, kTracksReply + loadRequest_, "track-list", MPV_FORMAT_NODE);
        }
        break;
      case MPV_EVENT_VIDEO_RECONFIG:
        // After stop() mpv still reports the closed file's size while it
        // shuts its video down: no target for a file that is gone.
        if (path_.empty()) {
          releaseTarget();
          break;
        }
        // Until the first frame arrives, the VO may still describe the old file.
        if (loading_ && !loadRestarted_) break;
        // mpv can be waiting for render() while configuring its first frame.
        mpv_get_property_async(mpv_, kSizeReply + ++sizeRequest_, "video-out-params", MPV_FORMAT_NODE);
        break;
      case MPV_EVENT_GET_PROPERTY_REPLY:
        if (ev->reply_userdata == kTracksReply + loadRequest_ && current_ && ev->error >= 0 &&
            videoSleep_ == VideoSleep::Awake) {
          const auto* prop = static_cast<mpv_event_property*>(ev->data);
          if (prop->format == MPV_FORMAT_NODE) {
            const auto tracks = parseTracks(*static_cast<mpv_node*>(prop->data));
            if (std::none_of(tracks.begin(), tracks.end(), [](const Track& t) {
                  return t.type == "video" && t.selected;
                })) {
              std::fprintf(stderr, "Playback failed: no video track could be started\n");
              failed_ = true;
              loading_ = opened_ = false;
            }
            // A disc's main video, never a 3D disc's second view (mpv may
            // list both when they share a transport stream).
            if (const auto disc = bdmv::isDiscImage(path_) ? bdmv::known(path_) : nullptr) {
              const auto& wanted = disc->title.tracks;
              const auto base = std::find_if(wanted.begin(), wanted.end(),
                                             [](const bdmv::Track& t) { return t.kind == bdmv::Track::Video; });
              const auto it = std::find_if(tracks.begin(), tracks.end(), [&](const Track& t) {
                return base != wanted.end() && t.type == "video" && t.sourceId == base->pid;
              });
              if (it != tracks.end() && !it->selected) {
                int64_t id = it->id;
                mpv_set_property_async(mpv_, 0, "vid", MPV_FORMAT_INT64, &id);
              }
            }
          }
        }
        if (ev->reply_userdata == kViewsReply + loadRequest_ && !path_.empty()) {
          const auto* prop = static_cast<mpv_event_property*>(ev->data);
          setViews(ev->error >= 0 && prop->format == MPV_FORMAT_STRING
                       ? *static_cast<char**>(prop->data) : nullptr);
        }
        if (ev->reply_userdata == kSizeReply + sizeRequest_ && !path_.empty()) {
          const auto* prop = static_cast<mpv_event_property*>(ev->data);
          if (ev->error >= 0 && prop->format == MPV_FORMAT_NODE) {
            const bool sized = updateVideoSize(*static_cast<mpv_node*>(prop->data));
            if (fileLoaded_ && sized) {
              newVideoSized_ = true;
              framePending_ = true;
            }
          } else {
            releaseTarget();
          }
        }
        break;
      case MPV_EVENT_PROPERTY_CHANGE: {
        const auto* prop = static_cast<mpv_event_property*>(ev->data);
        const std::string name = prop->name;
        if (ev->reply_userdata == 1) {
          auto& t = timingState_;
          const double number = prop->format == MPV_FORMAT_DOUBLE
                                    ? *static_cast<double*>(prop->data)
                                    : std::numeric_limits<double>::quiet_NaN();
          const int64_t count = prop->format == MPV_FORMAT_INT64
                                    ? *static_cast<int64_t*>(prop->data) : -1;
          const int flag = prop->format == MPV_FORMAT_FLAG
                               ? (*static_cast<int*>(prop->data) != 0) : -1;
          if (name == "hwdec-current")
            counts_.decoder = prop->format == MPV_FORMAT_STRING
                                  ? *static_cast<char**>(prop->data) : "unknown";
          else if (name == "video-params/hw-pixelformat")
            counts_.hwPixelFormat = prop->format == MPV_FORMAT_STRING
                                        ? *static_cast<char**>(prop->data) : "";
          else if (name == "frame-drop-count") counts_.voDrops = count;
          else if (name == "decoder-frame-drop-count") counts_.decoderDrops = count;
          else if (name == "avsync") t.avSync = number;
          else if (name == "demuxer-cache-duration") t.cacheSeconds = number;
          else if (name == "speed") t.speed = number;
          else if (name == "paused-for-cache") t.buffering = flag;
          else if (name == "seeking") t.seeking = flag;
          break;
        }
        if (prop->format == MPV_FORMAT_DOUBLE) {
          const double v = *static_cast<double*>(prop->data);
          if (name == "time-pos") {
            if (videoSleep_ == VideoSleep::Awake) {
              playback_.position = v;
              positionKnown_ = current_;
              updateDiscChapter();
            }
          }
          else if (name == "duration") playback_.duration = v;
          else if (name == "demuxer-cache-time") playback_.bufferedUntil = v;
          else if (name == "volume") playback_.volume = v;
          else if (name == "container-fps") playback_.fps = v;
        } else if (prop->format == MPV_FORMAT_INT64) {
          const int v = static_cast<int>(*static_cast<int64_t*>(prop->data));
          if (name == "audio-params/channel-count") playback_.audioChannels = v;
          else if (name == "chapter") {
            mpvChapter_ = v;
            if (!discChapters_) playback_.chapter = v;
          }
        } else if (prop->format == MPV_FORMAT_FLAG) {
          const bool v = *static_cast<int*>(prop->data) != 0;
          if (name == "pause") playback_.paused = videoSleep_ == VideoSleep::Awake ? v : sleepPaused_;
          else if (name == "mute") playback_.muted = v;
          else if (name == "eof-reached") playback_.endReached = v;
        } else if (prop->format == MPV_FORMAT_STRING) {
          const char* s = *static_cast<char**>(prop->data);
          if (name == "media-title") playback_.title = s;
          else if (name == "sub-text") playback_.subtitleText = s;
          else if (name == "video-format") playback_.videoCodec = s;
          else if (name == "video-params/pixelformat") playback_.pixelFormat = s;
          else if (name == "video-params/gamma") {
            sourceTrc_ = s;
            applyTargetTrc();
          }
          else if (name == "audio-codec-name") playback_.audioCodec = s;
          else if (name == "video-mvc") setViews(s);
        } else if (prop->format == MPV_FORMAT_NODE) {
          if (name == "track-list") {
            playback_.tracks = parseTracks(*static_cast<mpv_node*>(prop->data));
            labelDiscTracks();
          } else if (name == "video-params" && loading_ && fileLoaded_ && !newVideoSized_)
            // Prepare before decoding reaches the VO; video-out-params still
            // supplies the final filtered size before showing the first frame.
            updateVideoSize(*static_cast<mpv_node*>(prop->data), true);
          else if (name == "chapter-list") {
            mpvChapters_ = parseChapters(*static_cast<mpv_node*>(prop->data));
            updateChapters();
          }
        } else if (prop->format == MPV_FORMAT_NONE) {  // property became unavailable
          if (name == "time-pos") {
            if (videoSleep_ == VideoSleep::Awake) {
              playback_.position = 0;
              positionKnown_ = false;
            }
          }
          else if (name == "duration") playback_.duration = 0;
          else if (name == "demuxer-cache-time") playback_.bufferedUntil = 0;
          else if (name == "track-list") playback_.tracks.clear();
          else if (name == "sub-text") playback_.subtitleText.clear();
          else if (name == "eof-reached") playback_.endReached = false;
          else if (name == "video-format") playback_.videoCodec.clear();
          else if (name == "video-params/pixelformat") playback_.pixelFormat.clear();
          else if (name == "video-params/gamma") sourceTrc_.clear();
          else if (name == "container-fps") playback_.fps = 0;
          else if (name == "audio-codec-name") playback_.audioCodec.clear();
          else if (name == "audio-params/channel-count") playback_.audioChannels = 0;
          // Not while the video sleeps: the file's views stay what they are.
          else if (name == "video-mvc" && videoSleep_ == VideoSleep::Awake) setViews(nullptr);
          else if (name == "chapter-list") {
            mpvChapters_.clear();
            updateChapters();
          }
          else if (name == "chapter") {
            mpvChapter_ = -1;
            if (!discChapters_) playback_.chapter = -1;
          }
        }
        break;
      }
      case MPV_EVENT_END_FILE: {
        auto* end = static_cast<mpv_event_end_file*>(ev->data);
        if (end->reason == MPV_END_FILE_REASON_ERROR && !loadReplyPending_ &&
            ((countedEntry_ > 0 && end->playlist_entry_id == countedEntry_) ||
             (current_ && end->playlist_entry_id == startedEntry_))) {
          std::fprintf(stderr, "Playback failed: %s\n", mpv_error_string(end->error));
          loading_ = false;
          failed_ = true;
          opened_ = false;
        }
        break;
      }
      case MPV_EVENT_SET_PROPERTY_REPLY:
        if (ev->reply_userdata == kSleepVideoReply + loadRequest_ && export_.close_pending) {
          sleepCloseInFlight_ = false;
          if (ev->error < 0) {
            std::fprintf(stderr, "Video track could not sleep: %s\n", mpv_error_string(ev->error));
            failed_ = true;
            loading_ = false;
            videoSleep_ = VideoSleep::Closed;
          } else {
            sleepCloseReplied_ = true;
            videoSleep_ = VideoSleep::Closed;
            std::printf("Video track closed while the session is hidden\n");
          }
        } else if (ev->reply_userdata == kWakeVideoReply + loadRequest_ &&
                   videoSleep_ == VideoSleep::Opening) {
          if (ev->error < 0) {
            failVideoResume(mpv_error_string(ev->error));
          } else {
            seekAfterSleep();
          }
        }
        break;
      case MPV_EVENT_COMMAND_REPLY:
        if (ev->reply_userdata == kPixelPathReply && ev->error >= 0) {
          const auto& result = static_cast<mpv_event_command*>(ev->data)->result;
          if (result.format == MPV_FORMAT_STRING) std::printf("%s\n", result.u.string);
        }
        if (ev->reply_userdata == kCommandReply && ev->error < 0)
          std::fprintf(stderr, "mpv command failed: %s\n", mpv_error_string(ev->error));
        if (ev->reply_userdata == kLoadReply + loadRequest_ && loadReplyPending_) {
          loadReplyPending_ = false;
          if (ev->error < 0) {
            std::fprintf(stderr, "Playback failed: %s\n", mpv_error_string(ev->error));
            failed_ = true;
            loading_ = false;
          } else {
            const auto& result = static_cast<mpv_event_command*>(ev->data)->result;
            if (result.format == MPV_FORMAT_NODE_MAP)
              for (int i = 0; i < result.u.list->num; ++i)
                if (std::strcmp(result.u.list->keys[i], "playlist_entry_id") == 0 &&
                    result.u.list->values[i].format == MPV_FORMAT_INT64)
                  countedEntry_ = result.u.list->values[i].u.int64;
          }
        }
        if (ev->reply_userdata == kSeekReply + seekCount_ && seekStage_ == SeekStage::Sent) {
          seekStage_ = ev->error < 0 ? SeekStage::None : SeekStage::Accepted;
          if (ev->error < 0 && timing_)
            std::printf("[seek-timing] #%llu refused: %s\n",
                         static_cast<unsigned long long>(seekCount_), mpv_error_string(ev->error));
          if (ev->error < 0 && videoSleep_ == VideoSleep::Seeking)
            failVideoResume(mpv_error_string(ev->error));
        }
        break;
      case MPV_EVENT_SEEK:
        // After the reply, mpv's next seek is this one (or merged with it).
        if (seekStage_ == SeekStage::Accepted) seekStage_ = SeekStage::Started;
        break;
      case MPV_EVENT_PLAYBACK_RESTART:
        if (loading_ && fileLoaded_) {
          loadRestarted_ = true;
          // Before its size: the format is chosen from both (sized()).
          mpv_get_property_async(mpv_, kViewsReply + loadRequest_, "video-mvc", MPV_FORMAT_STRING);
          mpv_get_property_async(mpv_, kSizeReply + ++sizeRequest_, "video-out-params", MPV_FORMAT_NODE);
          framePending_ = true;  // redraw if the first frame arrived before its size reply
        }
        // mpv restarts once the seek's first frame was drawn (handed to
        // render()): the picture has changed, the next seek may go.
        if (seekStage_ == SeekStage::Started) {
          seekStage_ = SeekStage::None;
          if (timing_)
            std::printf("[seek-timing] #%llu first frame after %.0f ms, mpv at %.2f, hw=%s%s\n",
                        static_cast<unsigned long long>(seekCount_), (nowSeconds() - seekSentAt_) * 1000,
                        playback_.position, counts_.decoder.c_str(),
                         directCurrent_ || directPending_ ? " direct" : "");
          if (videoSleep_ == VideoSleep::Seeking && seekCount_ == resumeSeek_) {
            resumeRestarted_ = true;
          }
        }
        break;
      case MPV_EVENT_SHUTDOWN:
        shutdown_ = true;
        break;
      default:
        break;
    }
  }
  if (export_.close_pending && (!deferredLoadPath_.empty() || deferredStop_)) {
    if (videoReleaseComplete()) {
      const std::string next = std::move(deferredLoadPath_);
      deferredLoadPath_.clear();
      const bool stopRequested = deferredStop_;
      deferredStop_ = false;
      if (stopRequested) stop();
      else load(next, deferredLoadStart_);
    } else if (nowSeconds() - deferredActionAt_ > 10) {
      deferredLoadPath_.clear();
      deferredStop_ = false;
      failed_ = true;
      loading_ = false;
      std::fprintf(stderr, "Playback failed: previous decoder did not release\n");
    }
  }
  // mpv can treat a custom-stream read error as EOF or keep retrying it.
  if (remoteFailed_.load(std::memory_order_relaxed) && !failed_) {
    const char* cmd[] = {"stop", nullptr};
    mpv_command_async(mpv_, 0, cmd);
    failed_ = true;
    loading_ = opened_ = false;
    sidecarSubtitles_ = {};
    resetSeeks();
  }
  if (current_ && fileLoaded_ && sidecarSubtitles_.valid() &&
      sidecarSubtitles_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    for (const Sidecar& sub : sidecarSubtitles_.get()) {
      const char* cmd[] = {"sub-add", sub.url.c_str(), "auto", sub.title.empty() ? nullptr : sub.title.c_str(),
                           sub.lang.c_str(), nullptr};
      mpv_command_async(mpv_, 0, cmd);
    }
  }
  sendSeek();
}

bool VideoPlayer::updateVideoSize(const mpv_node& params, bool preparing) {
  int64_t w = 0, h = 0, rotate = 0;
  if (params.format == MPV_FORMAT_NODE_MAP)
    for (int i = 0; i < params.u.list->num; ++i) {
      const auto& value = params.u.list->values[i];
      if (value.format != MPV_FORMAT_INT64) continue;
      const std::string key = params.u.list->keys[i];
      if (key == "dw") w = value.u.int64;
      else if (key == "dh") h = value.u.int64;
      else if (key == "rotate") rotate = value.u.int64;
    }
  if (w <= 0 || h <= 0) {
    if (!preparing) releaseTarget();
    return false;
  }
  // dwidth/dheight are before rotation, which mpv applies while rendering
  // into our target: a portrait video turned 90° needs a portrait target,
  // or it is shrunk to fit a landscape one.
  if (rotate % 180 == 90) std::swap(w, h);
  const int64_t displayW = w, displayH = h;
  const int maxSize = static_cast<int>(interop_->maxSize());
  if (w > maxSize || h > maxSize) {
    const double s = std::min(double(maxSize) / w, double(maxSize) / h);
    w = static_cast<int64_t>(w * s);
    h = static_cast<int64_t>(h * s);
  }
  if (video_.valid() && w == width_ && h == height_) {
    if (!preparing && loading_) logPixelPath(displayW, displayH, maxSize, video_.levels());
    return true;
  }
  releaseTarget();
  width_ = static_cast<int>(w);
  height_ = static_cast<int>(h);

  // Mipmaps keep an 8K frame from shimmering when shown smaller than native.
  if (!video_.resize(width_, height_)) {
    std::fprintf(stderr, "No image for the video (%dx%d)\n", width_, height_);
    releaseTarget();
    return false;
  }
  framePending_ = true;
  if (!preparing) logPixelPath(displayW, displayH, maxSize, video_.levels());
  return true;
}

void VideoPlayer::setMipmaps(bool enabled) {
  if (enabled == mipmaps_) return;
  mipmaps_ = enabled;
  updateCapture();
  std::printf("Video sampling: %s\n",
              mipmaps_ ? "trilinear mipmaps" : "full-size level, over each pixel's footprint");
  // Paused, no new frame would make them; the scene samples them from now on.
  if (mipmaps_) {
    updateMipmaps(video_.current());
    updateMipmaps(video_.newest());
  }
}

void VideoPlayer::setPipelined(bool enabled, double frameSeconds) {
  pipelined_ = enabled;
  // Each frame is seen one headset frame later; the sound waits as long.
  setProperty(mpv_, "audio-delay", std::to_string(enabled ? frameSeconds : 0.0));
}

// Where the frame's pixels come from and how they are stored, once per new size.
void VideoPlayer::logPixelPath(int64_t displayW, int64_t displayH, int maxSize, int levels) {
  GLfloat maxAniso = 0;
  glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAniso);
  std::printf("Video size: %dx%d\n", width_, height_);
  const std::string source =
      "  source: decoded ${video-dec-params/w}x${video-dec-params/h}, after filters "
      "${video-params/w}x${video-params/h}, crop ${video-params/crop-x},${video-params/crop-y} "
      "${video-params/crop-w}x${video-params/crop-h}, pixel aspect ${video-params/par}, "
      "rotation ${video-out-params/rotate}, display " + std::to_string(displayW) + "x" +
      std::to_string(displayH) + "; mpv hwdec ${hwdec-current}, scale ${scale}, "
      "dscale ${dscale}, cscale ${cscale}";
  const char* cmd[] = {"expand-text", source.c_str(), nullptr};
  mpv_command_async(mpv_, kPixelPathReply, cmd);
  std::printf("  texture: %dx%d sRGB, %d mip levels%s, %s, anisotropy %.0f\n",
              width_, height_, levels,
              width_ != displayW || height_ != displayH
                  ? (" (reduced to the GPU limit " + std::to_string(maxSize) + ")").c_str()
                  : "",
              mipmaps_ ? "trilinear" : "level 0 over each pixel's footprint",
              mipmaps_ ? std::min(16.0f, maxAniso) : 1.0f);
}

void VideoPlayer::releaseTarget() {
  video_.release();
  width_ = height_ = 0;
  dropDirect();
  if (direct_) {
    direct_->dropUnused();
    direct_->dropStill();
  }
}

// Hands mpv the frames done with now, without waiting for its next frame,
// with a call that draws nothing: Frame's mpv takes the releases all the
// same, and lets go of the last frame it drew itself once the file ended.
void VideoPlayer::returnFrames() {
  if (!render_) return;
  if (direct_) direct_->release(export_);
  int skip = 1, block = 0;
  export_.request = 0;
  mpv_render_param params[] = {
      {MPV_RENDER_PARAM_SKIP_RENDERING, &skip},
      {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
      {static_cast<mpv_render_param_type>(VIDEO_EXPORT_PARAM), &export_},
      {MPV_RENDER_PARAM_INVALID, nullptr},
  };
  mpv_render_context_render(render_, params);
  export_.release_count = 0;
}

void VideoPlayer::setDirectVideo(DirectVideo* direct) {
  direct_ = direct;
  updateCapture();
}

void VideoPlayer::setSharpened(bool sharpened) {
  sharpened_ = sharpened;
  updateCapture();
}

// Q08C frames only pay where the eyes sample them: those mpv draws (mipmaps,
// sharpening) each go through the converter first. A change applies from the
// next decoder (another file, a seek that reopens it); until then the video
// keeps its capture, and frames the eyes cannot sample take the converter.
void VideoPlayer::updateCapture() {
  const bool q08c = v4l2_ && direct_ && direct_->acceptsQ08c() && !mipmaps_ && !sharpened_;
  if (q08c == q08cCapture_ || !mpv_) return;
  q08cCapture_ = q08c;
  setProperty(mpv_, "hwdec-v4l2-q08c", q08c ? "yes" : "no");
}

void VideoPlayer::dropDirect() {
  if (!direct_) return;
  direct_->hold(directCurrent_, false);
  direct_->hold(directPending_, false);
  directCurrent_ = directPending_ = nullptr;
}

// Only where the eye shaders show the frame as mpv would draw it: no
// mipmaps to make, no transfer conversion (HDR, TV gamma), no subtitles in
// the frame. DirectVideo::supported() checks the rest per frame.
bool VideoPlayer::wantExport() const {
  // mpv's conversion to the target transfer is skipped for frames handed out:
  // only TV gamma's of BT.1886 is done by the eye shaders instead.
  if (!direct_ || !direct_->available() || mipmaps_ || exportRefused_ || sourceTrc_.empty() ||
      (targetTrc_ != sourceTrc_ && !(tvGamma_ && sourceTrc_ == kBt1886)))
    return false;
  if (subtitlesInFrame_)
    for (const Track& t : playback_.tracks)
      if (t.type == "sub" && t.selected) return false;
  return true;
}

void VideoPlayer::showDirect(DirectFrame* frame) {
  direct_->hold(frame, true);
  if (pipelined_) {
    direct_->hold(directPending_, false);
    directPending_ = frame;
    if (newVideoSized_ && loadRestarted_ && loading_) firstFrameDeferred_ = true;
  } else {
    direct_->hold(directCurrent_, false);
    directCurrent_ = frame;
    if (newVideoSized_ && loadRestarted_) loading_ = false;
  }
}

const Texture* VideoPlayer::texture() {
  if (directCurrent_) return &directCurrent_->texture();
  SharedImage* image = video_.current();
  return image ? &image->texture() : nullptr;
}

GLuint VideoPlayer::glTexture() {
  if (directCurrent_) return 0;
  SharedImage* image = video_.current();
  return image ? image->glTexture() : 0;
}

void VideoPlayer::showRendered() {
  bool advanced = false;
  if (directPending_) {
    direct_->hold(directCurrent_, false);
    directCurrent_ = directPending_;
    directPending_ = nullptr;
    advanced = true;
  } else if (video_.advance()) {
    if (direct_) direct_->hold(directCurrent_, false);
    directCurrent_ = nullptr;
    advanced = true;
  }
  if (advanced && firstFrameDeferred_) {
    firstFrameDeferred_ = false;
    loading_ = false;
  }
}

void VideoPlayer::render() {
  if (!render_) return;
  if (export_.close_pending) {
    returnFrames();
    return;
  }
  // Keep the paused seek's first picture queued until its seek event has been
  // processed. A redraw cannot recover freshness evidence after consuming it.
  if (videoSleep_ == VideoSleep::Seeking && seekStage_ != SeekStage::Started && !resumeRestarted_)
    return;
  if (mpv_render_context_update(render_) & MPV_RENDER_UPDATE_FRAME) framePending_ = true;
  if (!framePending_) return;
  framePending_ = false;
  mpv_render_frame_info info{};
  int infoResult = -1;
  if (counting_ || videoSleep_ == VideoSleep::Seeking)
    infoResult = mpv_render_context_get_info(render_, {MPV_RENDER_PARAM_NEXT_FRAME_INFO, &info});
  if (videoSleep_ == VideoSleep::Seeking && seekCount_ == resumeSeek_ &&
      (seekStage_ == SeekStage::Started || resumeRestarted_) && infoResult >= 0 &&
      (info.flags & MPV_RENDER_FRAME_INFO_PRESENT) &&
      !(info.flags & (MPV_RENDER_FRAME_INFO_REDRAW | MPV_RENDER_FRAME_INFO_REPEAT)))
    resumeFrameSeen_ = true;
  if (!video_.valid() || (loading_ && (!loadRestarted_ || !newVideoSized_))) {
    // While opening, mpv can redraw the old file through an unused converter.
    // Consume these updates without drawing; playback-restart requests the
    // new file's first picture again, once its size is known.
    if (direct_) direct_->release(export_);
    int skip = 1, block = 0;
    export_.request = 0;
    const mpv_render_param exportParam{static_cast<mpv_render_param_type>(VIDEO_EXPORT_PARAM), &export_};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_SKIP_RENDERING, &skip},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
        direct_ ? exportParam : mpv_render_param{MPV_RENDER_PARAM_INVALID, nullptr},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    mpv_render_context_render(render_, params);
    export_.release_count = 0;
    return;
  }

  const auto countFrame = [&](int rendered) {
    if (!counting_ || rendered < 0) return false;
    // An update can just redraw the old frame (subtitles/options, or a new
    // target with no frame queued) or repeat it for display sync. Count only
    // genuinely new queued video submissions.
    auto& t = timingState_;
    if (infoResult < 0) ++t.unknown;
    else if (info.flags & MPV_RENDER_FRAME_INFO_REDRAW) ++t.redraws;
    else if (info.flags & MPV_RENDER_FRAME_INFO_REPEAT) ++t.repeats;
    else if (info.flags & MPV_RENDER_FRAME_INFO_PRESENT) {
      ++t.frames;
      ++counts_.newFrames;
      return true;
    }
    else ++t.redraws;
    return false;
  };
  const mpv_render_param exportParam{static_cast<mpv_render_param_type>(VIDEO_EXPORT_PARAM), &export_};
  // Frames done with go back to mpv with this call (its converter's buffers).
  if (direct_) direct_->release(export_);
  int block = 0;  // never stall the XR frame loop
  bool counted = false;
  if (wantExport()) {
    // mpv hands the frame out, else (exported -1) draws nothing: it is drawn
    // below, from mpv's current frame. It only checks the target's size.
    export_.request = 1;
    export_.accept_q10c = direct_->acceptsQ10c() && !sharpened_ && !compressedRefused_;
    export_.accept_q08c = direct_->acceptsQ08c() && !sharpened_ && !compressedRefused_;
    export_.exported = 0;
    mpv_opengl_fbo fbo{static_cast<int>(video_.backFramebuffer()), width_, height_, GL_SRGB8_ALPHA8};
    mpv_render_param params[] = {{MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
                                 exportParam,
                                 {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
                                 {MPV_RENDER_PARAM_INVALID, nullptr}};
    const int rendered = mpv_render_context_render(render_, params);
    export_.release_count = 0;
    if (rendered >= 0 && export_.exported == 1) {
      if (!DirectVideo::supported(export_)) {
        exportRefused_ = true;  // for this video: each try would cost a conversion
        std::printf("Video: mpv draws this video (its colour needs mpv's conversion)\n");
      }
      if (DirectFrame* frame = direct_->accept(export_)) {
        if (countFrame(rendered)) ++timingState_.direct;
        showDirect(frame);
        if (videoSleep_ == VideoSleep::Seeking && resumeFrameSeen_) resumePictureShown_ = true;
        return;
      }
      // A decoder's Q10C or Q08C buffer: converted by mpv from the next frame on.
      if (export_.modifier != 0) compressedRefused_ = true;
      else exportRefused_ = true;
    } else if (rendered >= 0 && export_.exported == 2 &&
               ((directCurrent_ && directCurrent_->id() == export_.id) ||
                (directPending_ && directPending_->id() == export_.id))) {
      countFrame(rendered);  // the frame handed out last, still shown
      if (videoSleep_ == VideoSleep::Seeking && resumeFrameSeen_) resumePictureShown_ = true;
      if (newVideoSized_ && loadRestarted_ && loading_) {
        if (directCurrent_ && directCurrent_->id() == export_.id) loading_ = false;
        else firstFrameDeferred_ = true;
      }
      return;
    }
    // Drawn below as a redraw of what that call took: counted as that frame.
    countFrame(rendered);
    if (rendered >= 0) counted = true;
  }

  SharedImage& target = video_.back();
  mpv_opengl_fbo fbo{static_cast<int>(target.framebuffer()), width_, height_, GL_SRGB8_ALPHA8};
  int flipY = 0;  // texel row 0 = top of the image
  export_.request = 0;
  mpv_render_param params[] = {
      {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
      {MPV_RENDER_PARAM_FLIP_Y, &flipY},
      {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
      direct_ ? exportParam : mpv_render_param{MPV_RENDER_PARAM_INVALID, nullptr},
      {MPV_RENDER_PARAM_INVALID, nullptr},
  };
  const int rendered = mpv_render_context_render(render_, params);
  export_.release_count = 0;
  if (!counted) countFrame(rendered);
  // Shown only once mpv has drawn it: a failed render keeps the last frame.
  if (rendered < 0) return;
  if (videoSleep_ == VideoSleep::Seeking && resumeFrameSeen_) resumePictureShown_ = true;
  if (pipelined_) {
    video_.deferSwap();
    if (direct_) direct_->hold(directPending_, false);
    directPending_ = nullptr;
    // the new video's first frame, shown from the next showRendered() on
    if (newVideoSized_ && loadRestarted_ && loading_) firstFrameDeferred_ = true;
  } else {
    if (newVideoSized_ && loadRestarted_) loading_ = false;  // this was the new video's first frame
    video_.swap();
    if (direct_) direct_->hold(directCurrent_, false);
    directCurrent_ = nullptr;
  }
  if (mipmaps_) updateMipmaps(video_.newest());
}

void VideoPlayer::updateMipmaps(SharedImage* image) {
  if (!image || video_.mipmapsValid(image)) return;
  interop_->acquireForGl(*image);
  glGenerateTextureMipmap(image->glTexture());
  video_.setMipmapsValid(image);
}

bool VideoPlayer::prepareRead() {
  if (directCurrent_) return false;
  SharedImage* image = video_.current();
  if (!image) return false;
  interop_->acquireForGl(*image);
  updateMipmaps(image);
  return true;
}

void VideoPlayer::logTiming(bool controlsVisible, bool previewBusy, bool xrRendering, bool xrFocused) {
  if (!timing_) return;
  auto& t = timingState_;
  const double now = nowSeconds();
  const auto resetInterval = [&] {
    t.since = now;
    t.frames = t.redraws = t.repeats = t.unknown = t.direct = 0;
    t.loops = t.controls = t.previews = t.xrFrames = t.xrFocused = 0;
  };
  if (path_.empty() || t.since == 0) {
    resetInterval();
    return;
  }
  ++t.loops;
  t.controls += controlsVisible;
  t.previews += previewBusy;
  t.xrFrames += xrRendering;
  t.xrFocused += xrFocused;
  const double elapsed = now - t.since;
  if (elapsed < 2) return;
  const auto delta = [](int64_t value, int64_t previous) -> long long {
    // mpv can reset counters. Do not turn a reset/unavailable value into a
    // negative drop count or mix the previous file into the new interval.
    return previous >= 0 && value >= previous ? value - previous : -1;
  };
  const double newFps = t.unknown ? std::numeric_limits<double>::quiet_NaN()
                                 : t.frames / elapsed;
  std::printf("[video-timing] file=%llu %dx%d %s %s hw=%s pos=%.1f source_fps=%.2f "
              "speed=%.2f span=%.2fs new_fps=%.1f redraw=%d repeat=%d "
              "vo_drop_delta=%lld vo_drop_total=%lld decoder_drop_delta=%lld "
              "decoder_drop_total=%lld av_sync_ms=%.1f cache_s=%.1f "
              "paused=%d buffering=%d seeking=%d loading=%d eof=%d "
              "controls_pct=%.0f preview_busy_pct=%.0f xr_render_pct=%.0f xr_focused_pct=%.0f "
              "direct=%d\n",
              static_cast<unsigned long long>(t.file), width_, height_,
              playback_.videoCodec.c_str(), playback_.pixelFormat.c_str(), counts_.decoder.c_str(),
              playback_.position, playback_.fps, t.speed, elapsed, newFps, t.redraws, t.repeats,
              delta(counts_.voDrops, t.lastVoDrops), static_cast<long long>(counts_.voDrops),
              delta(counts_.decoderDrops, t.lastDecoderDrops),
              static_cast<long long>(counts_.decoderDrops),
              t.avSync * 1000, t.cacheSeconds, playback_.paused, t.buffering, t.seeking,
              loading_, playback_.endReached, 100.0 * t.controls / t.loops,
              100.0 * t.previews / t.loops, 100.0 * t.xrFrames / t.loops,
              100.0 * t.xrFocused / t.loops, t.direct);
  std::fflush(stdout);
  t.lastVoDrops = counts_.voDrops;
  t.lastDecoderDrops = counts_.decoderDrops;
  resetInterval();
}
