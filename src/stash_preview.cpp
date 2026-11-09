#include "stash_preview.h"

#include "stash_http.h"
#include "thumbnailer.h"  // frameScaler

#include <cairo.h>
#include <curl/curl.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Surface = std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)>;
constexpr size_t kMaxDownload = 32u << 20;
constexpr int kFrameSize = 640, kFrameRate = 24;

struct Cancellation {
  const std::atomic<uint64_t>& generation;
  uint64_t expected;
  bool stopped() const { return generation.load() != expected; }
};

// Keep the connection between hovers. Polling bounds cancellation latency
// even when the server is connected but has stopped sending anything.
class Download {
 public:
  explicit Download(const std::string& apiKey) {
    if (!apiKey.empty()) headers_ = curl_slist_append(nullptr, ("ApiKey: " + apiKey).c_str());
    if (!curl_) return;
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers_);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl_, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, append);
  }
  ~Download() {
    curl_easy_cleanup(curl_);
    curl_multi_cleanup(multi_);
    curl_slist_free_all(headers_);
  }

  // `path` on Stash's server (stashPerform()).
  bool get(const StashAddress& address, const std::string& path, std::vector<uint8_t>& bytes,
           const Cancellation& cancel) {
    if (!curl_ || !multi_ || cancel.stopped()) return false;
    Body body{bytes, cancel};
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &body);
    auto perform = [&](CURL* curl) {
      if (curl_multi_add_handle(multi_, curl) != CURLM_OK) return CURLE_FAILED_INIT;
      int running = 0;
      CURLMcode result;
      do {
        result = curl_multi_perform(multi_, &running);
        if (result != CURLM_OK || !running || cancel.stopped()) break;
        result = curl_multi_poll(multi_, nullptr, 0, 25, nullptr);
      } while (result == CURLM_OK && !cancel.stopped());
      CURLcode done = CURLE_ABORTED_BY_CALLBACK;  // stopped before it finished
      int left = 0;
      while (CURLMsg* message = curl_multi_info_read(multi_, &left))
        if (message->msg == CURLMSG_DONE) done = message->data.result;
      curl_multi_remove_handle(multi_, curl);
      return result == CURLM_OK ? done : CURLE_FAILED_INIT;
    };
    const StashReply reply = stashPerform(curl_, address, path, [&bytes] { bytes.clear(); }, true, perform);
    return reply.code == CURLE_OK && reply.status == 200 && !bytes.empty() && !cancel.stopped();
  }

 private:
  struct Body {
    std::vector<uint8_t>& bytes;
    const Cancellation& cancel;
  };
  static size_t append(char* data, size_t size, size_t count, void* target) {
    auto& body = *static_cast<Body*>(target);
    const size_t length = size * count;
    if (body.cancel.stopped() || length > kMaxDownload - body.bytes.size()) return 0;
    body.bytes.insert(body.bytes.end(), data, data + length);
    return length;
  }
  CURL* curl_ = curl_easy_init();
  CURLM* multi_ = curl_multi_init();
  curl_slist* headers_ = nullptr;
};

// Seekable, memory-only MP4 input. Stash puts the index at the end, so the
// complete download is kept until the pointer leaves; looping rereads it.
class Decoder {
 public:
  explicit Decoder(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}
  ~Decoder() {
    sws_freeContext(sws_);
    av_frame_free(&frame_);
    av_packet_free(&packet_);
    avcodec_free_context(&codec_);
    avformat_close_input(&input_);
    if (io_) av_freep(&io_->buffer);
    avio_context_free(&io_);
  }

  bool open() {
    auto* buffer = static_cast<uint8_t*>(av_malloc(32768));
    if (!buffer) return false;
    io_ = avio_alloc_context(buffer, 32768, 0, this, read, nullptr, seek);
    if (!io_) {
      av_free(buffer);
      return false;
    }
    input_ = avformat_alloc_context();
    if (!input_) return false;
    input_->pb = io_;
    input_->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_open_input(&input_, nullptr, av_find_input_format("mov"), nullptr) < 0 ||
        avformat_find_stream_info(input_, nullptr) < 0)
      return false;
    const AVCodec* decoder = nullptr;
    stream_ = av_find_best_stream(input_, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (stream_ < 0 || !decoder) return false;
    codec_ = avcodec_alloc_context3(decoder);
    if (!codec_ || avcodec_parameters_to_context(codec_, input_->streams[stream_]->codecpar) < 0)
      return false;
    codec_->thread_count = 1;
    codec_->max_pixels = 4096 * 4096;
    return frame_ && packet_ && avcodec_open2(codec_, decoder, nullptr) >= 0;
  }

  bool next(const Cancellation& cancel) {
    while (!cancel.stopped()) {
      const int result = avcodec_receive_frame(codec_, frame_);
      if (result == 0) return true;
      if (result != AVERROR(EAGAIN)) return false;
      int readResult;
      do {
        av_packet_unref(packet_);
        readResult = av_read_frame(input_, packet_);
      } while (readResult >= 0 && packet_->stream_index != stream_ && !cancel.stopped());
      const int sent = avcodec_send_packet(codec_, readResult < 0 ? nullptr : packet_);
      av_packet_unref(packet_);
      if (sent < 0) return false;
    }
    return false;
  }

  double time() const {
    const int64_t pts = frame_->best_effort_timestamp;
    return pts == AV_NOPTS_VALUE ? NAN : pts * av_q2d(input_->streams[stream_]->time_base);
  }
  double step() const {
    const double fps = av_q2d(input_->streams[stream_]->avg_frame_rate);
    return fps > 0 ? 1 / fps : 1.0 / kFrameRate;
  }
  bool rewind() {
    const int64_t start = input_->streams[stream_]->start_time;
    if (av_seek_frame(input_, stream_, start == AV_NOPTS_VALUE ? 0 : start, AVSEEK_FLAG_BACKWARD) < 0)
      return false;
    avcodec_flush_buffers(codec_);
    return true;
  }

  bool picture(const VideoFormat& format, int cover, Surface& surface) {
    // A shallow view changes only the crop, not the decoder's frame buffers.
    AVFrame eye = *frame_;
    if (format.stereo == StereoLayout::SideBySide) eye.crop_right = eye.width - eye.width / 2;
    if (format.stereo == StereoLayout::TopBottom) eye.crop_bottom = eye.height - eye.height / 2;
    if (av_frame_apply_cropping(&eye, AV_FRAME_CROP_UNALIGNED) < 0) return false;
    double sar = av_q2d(frame_->sample_aspect_ratio);
    if (!(sar > 0)) sar = 1;
    double w = eye.width * sar, h = eye.height;
    if (isHalfPacked(format)) {
      if (format.stereo == StereoLayout::SideBySide) w *= 2;
      if (format.stereo == StereoLayout::TopBottom) h *= 2;
    }
    const double scale = cover > 0 ? std::max(cover / w, cover * 9.0 / 16 / h)
                                   : std::min({1.0, kFrameSize / w, kFrameSize / h});
    const int width = std::max(2, static_cast<int>(std::ceil(w * scale - 0.01)));
    const int height = std::max(2, static_cast<int>(std::ceil(h * scale - 0.01)));
    sws_ = frameScaler(sws_, &eye, eye.width, width, height, AV_PIX_FMT_BGRA, SWS_BILINEAR);
    if (!sws_) return false;
    if (!surface || cairo_image_surface_get_width(surface.get()) != width ||
        cairo_image_surface_get_height(surface.get()) != height) {
      // swscale's SIMD writes need padded rows and a little space past the
      // final row. Cairo owns that buffer through its surface's user data.
      static cairo_user_data_key_t pixelsKey;
      const int stride = (width * 4 + 63) & ~63;
      auto* pixels =
          static_cast<uint8_t*>(av_mallocz(static_cast<size_t>(stride) * (height + 2) + 256));
      if (!pixels) return false;
      // Opaque: pasted, not blended.
      surface.reset(cairo_image_surface_create_for_data(pixels, CAIRO_FORMAT_RGB24, width, height,
                                                        stride));
      if (cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS ||
          cairo_surface_set_user_data(surface.get(), &pixelsKey, pixels, av_free) !=
              CAIRO_STATUS_SUCCESS) {
        av_free(pixels);
        surface.reset();
        return false;
      }
    }
    cairo_surface_flush(surface.get());
    uint8_t* planes[4] = {cairo_image_surface_get_data(surface.get()), nullptr, nullptr, nullptr};
    int strides[4] = {cairo_image_surface_get_stride(surface.get()), 0, 0, 0};
    const bool ok = sws_scale(sws_, eye.data, eye.linesize, 0, eye.height, planes, strides) > 0;
    cairo_surface_mark_dirty(surface.get());
    return ok;
  }

 private:
  static int read(void* opaque, uint8_t* buffer, int size) {
    auto& self = *static_cast<Decoder*>(opaque);
    const size_t count = std::min(static_cast<size_t>(size), self.bytes_.size() - self.position_);
    if (!count) return AVERROR_EOF;
    std::memcpy(buffer, self.bytes_.data() + self.position_, count);
    self.position_ += count;
    return static_cast<int>(count);
  }
  static int64_t seek(void* opaque, int64_t offset, int whence) {
    auto& self = *static_cast<Decoder*>(opaque);
    if (whence == AVSEEK_SIZE) return static_cast<int64_t>(self.bytes_.size());
    whence &= ~AVSEEK_FORCE;
    if (whence == SEEK_CUR) offset += static_cast<int64_t>(self.position_);
    else if (whence == SEEK_END) offset += static_cast<int64_t>(self.bytes_.size());
    else if (whence != SEEK_SET) return AVERROR(EINVAL);
    if (offset < 0 || static_cast<uint64_t>(offset) > self.bytes_.size()) return AVERROR(EINVAL);
    self.position_ = static_cast<size_t>(offset);
    return offset;
  }
  const std::vector<uint8_t>& bytes_;
  size_t position_ = 0;
  AVIOContext* io_ = nullptr;
  AVFormatContext* input_ = nullptr;
  AVCodecContext* codec_ = nullptr;
  AVFrame* frame_ = av_frame_alloc();
  AVPacket* packet_ = av_packet_alloc();
  SwsContext* sws_ = nullptr;
  int stream_ = -1;
};

}  // namespace

struct StashPreview::State {
  StashAddress address;
  std::string apiKey;
  int64_t scene = 0;
  VideoFormat format;
  int width = 0;
  std::atomic<uint64_t> generation{0};
  std::atomic<bool> ready{false};
  bool quit = false;
  std::mutex mutex;
  std::condition_variable wake;
  std::thread worker;
  Surface pending{nullptr, cairo_surface_destroy}, shown{nullptr, cairo_surface_destroy};

  void work() {
    Download download(apiKey);
    uint64_t seen = 0;
    for (;;) {
      int64_t id;
      VideoFormat videoFormat;
      int width;
      {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait(lock, [&] { return quit || generation.load() != seen; });
        if (quit) return;
        seen = generation.load();
        id = scene;
        videoFormat = format;
        width = this->width;
      }
      if (!id) continue;
      const Cancellation cancel{generation, seen};
      std::vector<uint8_t> bytes;
      // Stash's generated MP4 endpoint, not its full-video stream route.
      if (!download.get(address, "/scene/" + std::to_string(id) + "/preview", bytes, cancel))
        continue;
      Decoder decoder(bytes);
      if (cancel.stopped() || !decoder.open()) continue;
      Surface buffer{nullptr, cairo_surface_destroy};
      do {
        auto began = Clock::now();
        double origin = NAN, nextTime = 0, fallback = 0;
        bool gotFrame = false;
        while (decoder.next(cancel)) {
          const double pts = decoder.time();
          if (std::isnan(origin)) origin = std::isfinite(pts) ? pts : 0;
          const double time = std::isfinite(pts) ? std::max(0.0, pts - origin) : fallback;
          fallback = time + decoder.step();
          if (time + 1e-6 < nextTime) continue;
          nextTime = (std::floor(time * kFrameRate) + 1) / kFrameRate;
          if (!decoder.picture(videoFormat, width, buffer)) break;
          if (!gotFrame) began = Clock::now();
          gotFrame = true;
          std::unique_lock<std::mutex> lock(mutex);
          if (wake.wait_until(lock, began + std::chrono::duration<double>(time),
                              [&] { return cancel.stopped(); }))
            break;
          pending.swap(buffer);
          ready = true;
        }
        if (!gotFrame || cancel.stopped()) break;
        // Hold the last frame for its duration before looping.
        std::unique_lock<std::mutex> lock(mutex);
        if (wake.wait_until(lock, began + std::chrono::duration<double>(fallback),
                            [&] { return cancel.stopped(); }))
          break;
        lock.unlock();
      } while (!cancel.stopped() && decoder.rewind());
    }
  }
};

StashPreview::StashPreview(const std::string& url, std::string apiKey, bool unverified)
    : state_(std::make_unique<State>()) {
  state_->address = StashAddress(url, unverified);
  state_->apiKey = std::move(apiKey);
}

StashPreview::~StashPreview() {
  stop();
  if (state_->worker.joinable()) state_->worker.join();
}

void StashPreview::request(int64_t scene, const VideoFormat& format, int width) {
  State& s = *state_;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.scene == scene && s.format.projection == format.projection &&
        s.format.stereo == format.stereo && s.format.packing == format.packing && s.width == width)
      return;
    s.scene = scene;
    s.format = format;
    s.width = width;
    ++s.generation;
    s.ready = false;
    s.pending.reset();
  }
  s.shown.reset();
  if (scene && !s.worker.joinable()) s.worker = std::thread([&s] { s.work(); });
  s.wake.notify_one();
}

void StashPreview::cancel() { request(0, {}); }

bool StashPreview::update() {
  State& s = *state_;
  if (!s.ready) return false;
  std::unique_lock<std::mutex> lock(s.mutex, std::try_to_lock);
  if (!lock || !s.ready) return false;
  s.shown.swap(s.pending);
  s.ready = false;
  return true;
}

cairo_surface_t* StashPreview::frame() const { return state_->shown.get(); }

void StashPreview::stop() {
  State& s = *state_;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    s.quit = true;
    ++s.generation;
  }
  s.wake.notify_one();
}
