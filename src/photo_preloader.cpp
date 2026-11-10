#include "photo_preloader.h"

#include "thumbnailer.h"  // frameScaler, scaleInto
#include "remote_avio.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>

namespace {

constexpr int kMaxSize = 16384;            // at most, else the shared image limit
constexpr size_t kUploadBytes = 8u << 20;  // per frame

struct Interrupt {
  const std::atomic<bool>& quit;
  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  static int check(void* opaque) {
    const auto& self = *static_cast<Interrupt*>(opaque);
    return self.quit.load() || std::chrono::steady_clock::now() >= self.deadline;
  }
};

// Turns an RGBA picture clockwise by 90, 180 or 270 degrees.
void rotate(std::vector<uint32_t>& pixels, int& width, int& height, int degrees) {
  if (degrees != 90 && degrees != 180 && degrees != 270) return;
  std::vector<uint32_t> out(pixels.size());
  const int w = width, h = height;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const uint32_t p = pixels[static_cast<size_t>(y) * w + x];
      if (degrees == 180) out[static_cast<size_t>(h - 1 - y) * w + (w - 1 - x)] = p;
      else if (degrees == 90) out[static_cast<size_t>(x) * h + (h - 1 - y)] = p;
      else out[static_cast<size_t>(w - 1 - x) * h + y] = p;
    }
  pixels.swap(out);
  if (degrees != 180) std::swap(width, height);
}

// Decodes a photo at full size (at most maxSize) into RGBA, upright.
bool decodePhoto(const std::string& path, int maxSize, std::vector<uint32_t>& pixels, int& width,
                 int& height, Interrupt& interrupt) {
  if (Interrupt::check(&interrupt)) return false;
  AVFormatContext* format = avformat_alloc_context();
  if (!format) return false;
  format->interrupt_callback = {Interrupt::check, &interrupt};
  RemoteAvio remote;
  if (!remote.attach(format, path)) {
    avformat_free_context(format);
    return false;
  }
  if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0) return false;
  AVCodecContext* codec = nullptr;
  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  bool ok = false;
  do {
    if (avformat_find_stream_info(format, nullptr) < 0) break;
    const AVCodec* decoder = nullptr;
    const int stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (stream < 0 || !decoder) break;
    codec = avcodec_alloc_context3(decoder);
    if (!codec || avcodec_parameters_to_context(codec, format->streams[stream]->codecpar) < 0)
      break;
    codec->thread_count = 0;
    if (avcodec_open2(codec, decoder, nullptr) < 0) break;
    bool got = false, ended = false;
    for (int reads = 0; !got && reads < 100 && !Interrupt::check(&interrupt); ++reads) {
      if (!ended && av_read_frame(format, packet) < 0) {
        ended = true;
        avcodec_send_packet(codec, nullptr);
      } else if (!ended) {
        if (packet->stream_index == stream) avcodec_send_packet(codec, packet);
        av_packet_unref(packet);
      }
      got = avcodec_receive_frame(codec, frame) == 0;
      if (ended && !got) break;
    }
    if (!got || Interrupt::check(&interrupt)) break;

    const double scale =
        std::min({1.0, double(maxSize) / frame->width, double(maxSize) / frame->height});
    width = std::max(1, static_cast<int>(frame->width * scale));
    height = std::max(1, static_cast<int>(frame->height * scale));
    SwsContext* sws =
        frameScaler(nullptr, frame, frame->width, width, height, AV_PIX_FMT_RGBA, SWS_BICUBIC);
    if (!sws) break;
    const bool scaled = scaleInto(sws, frame, width, height, pixels);
    sws_freeContext(sws);
    if (!scaled) break;
    // Camera orientation (EXIF), as FFmpeg reports it.
    if (const AVFrameSideData* side = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX)) {
      const double angle = av_display_rotation_get(reinterpret_cast<const int32_t*>(side->data));
      if (!std::isnan(angle)) {
        const int clockwise = ((static_cast<int>(std::lround(-angle)) % 360) + 360) % 360;
        rotate(pixels, width, height, clockwise);
      }
    }
    ok = true;
  } while (false);
  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&codec);
  avformat_close_input(&format);
  return ok;
}

}  // namespace

struct PhotoPreloader::Worker {
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::string> wanted;
  std::string decoding;
  Decoded decoded;
  bool done = false;
  std::atomic<bool> quit{false};
  bool stopped = false;
};

PhotoPreloader::~PhotoPreloader() {
  if (worker_.joinable()) {
    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->quit = true;
    state_->wake.notify_all();
    // A filesystem syscall may ignore FFmpeg's interrupt callback. The
    // worker owns only CPU data, so it can safely outlive the GL thread.
    const bool stopped = state_->wake.wait_for(lock, std::chrono::milliseconds(500),
                                              [this] { return state_->stopped; });
    lock.unlock();
    if (stopped) worker_.join();
    else worker_.detach();
  }
  for (Slot& s : slots_)
    if (s.image) interop_->retire(std::move(s.image));
}

void PhotoPreloader::start(GlInterop& interop) {
  interop_ = &interop;
  state_ = std::make_shared<Worker>();
  worker_ = std::thread(&PhotoPreloader::work, state_,
                        std::min(kMaxSize, static_cast<int>(interop.maxSize())));
}

void PhotoPreloader::request(const std::string& current, const std::string& next) {
  current_ = current;
  next_ = next;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->wanted.clear();
    for (const std::string& path : {current, next}) {
      if (path.empty()) continue;
      bool known = state_->decoding == path || (state_->done && state_->decoded.path == path);
      for (const Slot& s : slots_) known = known || s.path == path;  // there, or on its way up
      if (!known) state_->wanted.push_back(path);
    }
  }
  state_->wake.notify_one();
}

void PhotoPreloader::work(const std::shared_ptr<Worker>& worker, int maxSize) {
  Worker& w = *worker;
  while (true) {
    std::string path;
    {
      std::unique_lock<std::mutex> lock(w.mutex);
      w.wake.wait(lock, [&] { return w.quit || !w.wanted.empty(); });
      if (w.quit) break;
      path = w.wanted.front();
      w.wanted.pop_front();
      w.decoding = path;
    }
    Decoded slot;
    slot.path = path;
    Interrupt interrupt{w.quit};
    const bool ok = decodePhoto(path, maxSize, slot.pixels, slot.width, slot.height, interrupt);
    std::unique_lock<std::mutex> lock(w.mutex);
    if (w.quit) break;
    if (ok) {
      // One finished photo waits at a time, until the GL thread takes it.
      w.wake.wait(lock, [&] { return w.quit || !w.done; });
      if (w.quit) break;
      w.decoded = std::move(slot);
      w.done = true;
    }
    w.decoding.clear();
  }
  std::lock_guard<std::mutex> lock(w.mutex);
  w.stopped = true;
  w.wake.notify_all();
}

void PhotoPreloader::update() {
  Decoded incoming;
  bool have = false;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->done) {
      incoming = std::move(state_->decoded);
      state_->done = false;
      have = true;
    }
  }
  if (have) {
    state_->wake.notify_all();  // the worker may wait to hand over the next one
    // Into a slot holding neither the current nor the next photo (else the
    // one not on screen).
    int index = shown_ == 0 ? 1 : 0;
    for (int i = 0; i < 2; ++i)
      if (slots_[i].path != current_ && slots_[i].path != next_) index = i;
    Slot& s = slots_[index];
    const int levels =
        1 + static_cast<int>(std::floor(std::log2(std::max(incoming.width, incoming.height))));
    std::unique_ptr<SharedImage> image =
        interop_->create(static_cast<uint32_t>(incoming.width),
                         static_cast<uint32_t>(incoming.height), static_cast<uint32_t>(levels),
                         "a photo");
    if (image) {
      // An upload not finished yet is abandoned with it.
      if (s.image) interop_->retire(std::move(s.image));
      static_cast<Decoded&>(s) = std::move(incoming);
      s.uploadedRows = 0;
      s.ready = false;
      s.image = std::move(image);
      interop_->acquireForWrite(*s.image);
      interop_->keepInGl(*s.image, true);
    } else {
      // The slot keeps what it had.
      std::fprintf(stderr, "Photo (%dx%d) not shown: no image for it\n", incoming.width,
                   incoming.height);
    }
  }
  // A few megabytes per frame, so a 50 MP photo goes up over a few frames;
  // the photo on screen before the next one.
  const int first = slots_[1].path == current_ ? 1 : 0;
  for (int i : {first, 1 - first}) {
    Slot& s = slots_[i];
    if (s.ready || s.pixels.empty()) continue;
    const int rows = std::max(1, static_cast<int>(kUploadBytes / (static_cast<size_t>(s.width) * 4)));
    const int count = std::min(rows, s.height - s.uploadedRows);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTextureSubImage2D(s.image->glTexture(), 0, 0, s.uploadedRows, s.width, count, GL_RGBA,
                        GL_UNSIGNED_BYTE,
                        s.pixels.data() + static_cast<size_t>(s.uploadedRows) * s.width);
    s.uploadedRows += count;
    if (s.uploadedRows >= s.height) {
      glGenerateTextureMipmap(s.image->glTexture());
      s.pixels.clear();
      s.pixels.shrink_to_fit();
      s.ready = true;
      interop_->keepInGl(*s.image, false);  // to Vulkan at the next publish
    }
    break;  // one slot per frame
  }
}

SharedImage* PhotoPreloader::image(const std::string& path, int& width, int& height) {
  for (int i = 0; i < 2; ++i) {
    Slot& s = slots_[i];
    if (!s.ready || s.path != path || s.image->inGl()) continue;
    shown_ = i;
    width = s.width;
    height = s.height;
    return s.image.get();
  }
  return nullptr;
}
