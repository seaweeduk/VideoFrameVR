#include "thumbnailer.h"

#include "file_util.h"
#include "platform.h"
#include "settings.h"  // cacheDir
#include "remote_avio.h"
#include "stash_images.h"  // encodeJpeg, decodeJpeg
#include "stash_stream.h"
#include "video_format.h"

#include <cairo.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace fs = std::filesystem;

namespace {

constexpr size_t kMemoryBudget = 140 << 20;  // beyond that unwanted ones are dropped
constexpr uintmax_t kDiskBudget = uintmax_t{500} << 20;  // beyond that the least used go
using Clock = std::chrono::steady_clock;
using Size = Thumbnailer::Size;
// Lanczos, as Stash's covers are made.
constexpr int kSharp = SWS_LANCZOS | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP;
constexpr auto kRetryInterval = std::chrono::seconds(60);
// Workers, each on its own file: from a network share each mostly waits on
// round trips. On the Frame they run on these cores of those Steam gives the
// app (the compositor's render thread uses 4-6).
constexpr int kWorkers = 3;
constexpr int kFrameCores[] = {3, 7};

struct Interrupt {
  const std::atomic<bool>& quit;
  const std::atomic<bool>& playing;  // a video started: the decode stops
  Clock::time_point deadline = Clock::now() + std::chrono::seconds(10);
  static int check(void* opaque) {
    const auto& self = *static_cast<Interrupt*>(opaque);
    return self.quit.load() || self.playing.load() || Clock::now() >= self.deadline;
  }
};

}  // namespace

SwsContext* frameScaler(SwsContext* cached, const AVFrame* frame, int srcWidth, int width,
                        int height, int dstFormat, int flags) {
  auto format = static_cast<AVPixelFormat>(frame->format);
  bool fullRange = frame->color_range == AVCOL_RANGE_JPEG;
  switch (format) {
    case AV_PIX_FMT_YUVJ420P: format = AV_PIX_FMT_YUV420P; fullRange = true; break;
    case AV_PIX_FMT_YUVJ422P: format = AV_PIX_FMT_YUV422P; fullRange = true; break;
    case AV_PIX_FMT_YUVJ444P: format = AV_PIX_FMT_YUV444P; fullRange = true; break;
    case AV_PIX_FMT_YUVJ440P: format = AV_PIX_FMT_YUV440P; fullRange = true; break;
    case AV_PIX_FMT_YUVJ411P: format = AV_PIX_FMT_YUV411P; fullRange = true; break;
    default: break;
  }
  SwsContext* sws = sws_getCachedContext(cached, srcWidth, frame->height, format, width, height,
                                         static_cast<AVPixelFormat>(dstFormat), flags, nullptr,
                                         nullptr, nullptr);
  if (!sws) return nullptr;
  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(format);
  if (desc && !(desc->flags & AV_PIX_FMT_FLAG_RGB)) {
    int *inverse, *table, srcRange, dstRange, brightness, contrast, saturation;
    if (sws_getColorspaceDetails(sws, &inverse, &srcRange, &table, &dstRange, &brightness,
                                 &contrast, &saturation) >= 0) {
      const int* coefficients =
          sws_getCoefficients(frame->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_DEFAULT);
      sws_setColorspaceDetails(sws, coefficients, fullRange ? 1 : 0, table, dstRange, brightness,
                               contrast, saturation);
    }
  }
  return sws;
}

// swscale may write past the end of a row (it works in SIMD blocks), so it
// gets padded rows; the result is then copied into tightly packed pixels.
bool scaleInto(SwsContext* sws, const AVFrame* frame, int width, int height,
               std::vector<uint32_t>& pixels) {
  const int stride = (width * 4 + 63) & ~63;
  std::vector<uint8_t> padded(static_cast<size_t>(stride) * (height + 2) + 256);
  uint8_t* planes[4] = {padded.data(), nullptr, nullptr, nullptr};
  int strides[4] = {stride, 0, 0, 0};
  if (sws_scale(sws, frame->data, frame->linesize, 0, frame->height, planes, strides) <= 0)
    return false;
  pixels.resize(static_cast<size_t>(width) * height);
  for (int y = 0; y < height; ++y)
    std::memcpy(&pixels[static_cast<size_t>(y) * width], padded.data() + static_cast<size_t>(y) * stride,
                static_cast<size_t>(width) * 4);
  return true;
}

namespace {

// Picks the GPU (CUDA) output format when the decoder offers it.
AVPixelFormat pickCuda(AVCodecContext*, const AVPixelFormat* formats) {
  for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f)
    if (*f == AV_PIX_FMT_CUDA) return *f;
  return formats[0];
}

// Decodes one frame (20% into a video, the picture of a photo) and makes the
// `size` picture of it. With `gpu` (a CUDA device), videos are decoded on
// the graphics card.
bool decodeFrame(const std::string& path, AVBufferRef* gpu, Size size,
                 std::vector<uint32_t>& pixels, Interrupt& interrupt) {
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
  AVFrame* downloaded = av_frame_alloc();
  bool ok = false;
  do {
    if (avformat_find_stream_info(format, nullptr) < 0) break;
    const AVCodec* decoder = nullptr;
    const int stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (stream < 0 || !decoder) break;
    codec = avcodec_alloc_context3(decoder);
    if (!codec || avcodec_parameters_to_context(codec, format->streams[stream]->codecpar) < 0)
      break;
    if (gpu) {
      codec->hw_device_ctx = av_buffer_ref(gpu);
      codec->get_format = pickCuda;
    } else {
      // On the CPU: one thread (bursts on all cores make the headset's
      // frames late). Not keyframes only: many files mark pictures as
      // keyframes the decoder doesn't take as such, and it read on for
      // tens of MB to the next one.
      codec->thread_count = 1;
    }
    if (avcodec_open2(codec, decoder, nullptr) < 0) break;
    // Timestamps count from the file's start time: hours in on a Blu-ray
    // disc's transport streams.
    const int64_t start = format->start_time != AV_NOPTS_VALUE ? format->start_time : 0;
    if (!isPhotoFile(path) && format->duration > 0)
      av_seek_frame(format, -1, start + format->duration / 5, AVSEEK_FLAG_BACKWARD);

    bool got = false, ended = false;
    for (int reads = 0; !got && reads < 3000 && !Interrupt::check(&interrupt); ++reads) {
      if (!ended && av_read_frame(format, packet) < 0) {
        ended = true;
        avcodec_send_packet(codec, nullptr);  // drain what the decoder holds
      } else if (!ended) {
        if (packet->stream_index == stream) avcodec_send_packet(codec, packet);
        av_packet_unref(packet);
      }
      got = avcodec_receive_frame(codec, frame) == 0;
      if (ended && !got) break;
    }
    if (!got || Interrupt::check(&interrupt)) break;
    if (frame->format == AV_PIX_FMT_CUDA) {  // copy the picture from the GPU
      if (av_hwframe_transfer_data(downloaded, frame, 0) < 0) break;
      std::swap(frame, downloaded);
    }

    // One eye of a 3D video (guessed as the player does, from the name and
    // size), of that the middle filling 16:9, scaled in one pass.
    const VideoFormat guessed = guessVideoFormat(path, frame->width, frame->height);
    const int eyeW = frame->width / (guessed.stereo == StereoLayout::SideBySide ? 2 : 1);
    const int eyeH = frame->height / (guessed.stereo == StereoLayout::TopBottom ? 2 : 1);
    const int w = std::min(eyeW, eyeH * 16 / 9), h = std::min(eyeH, eyeW * 9 / 16);
    if (w < 2 || h < 2) break;
    frame->crop_left = ((eyeW - w) / 2) & ~1;
    frame->crop_top = ((eyeH - h) / 2) & ~1;
    frame->crop_right = frame->width - frame->crop_left - w;
    frame->crop_bottom = frame->height - frame->crop_top - h;
    if (av_frame_apply_cropping(frame, AV_FRAME_CROP_UNALIGNED) < 0) break;
    SwsContext* sws = frameScaler(nullptr, frame, frame->width, size.width, size.height,
                                  AV_PIX_FMT_BGRA, kSharp);
    if (!sws) break;
    ok = scaleInto(sws, frame, size.width, size.height, pixels);
    sws_freeContext(sws);
  } while (false);
  av_frame_free(&frame);
  av_frame_free(&downloaded);
  av_packet_free(&packet);
  avcodec_free_context(&codec);
  avformat_close_input(&format);
  return ok;
}

const cairo_user_data_key_t kPixelsKey{};

void freePixels(void* pixels) { delete static_cast<std::vector<uint32_t>*>(pixels); }

// The picture as an opaque surface that owns its pixels, made at `to` from
// the gallery's (`from`) on the worker, so the GL thread pastes it as it is.
cairo_surface_t* makeSurface(std::vector<uint32_t> pixels, Size from, Size to) {
  if (to.width != from.width || to.height != from.height) {
    pixels.resize(pixels.size() + 64);  // swscale may read past the last row
    AVFrame* frame = av_frame_alloc();
    if (!frame) return nullptr;
    frame->data[0] = reinterpret_cast<uint8_t*>(pixels.data());
    frame->linesize[0] = from.width * 4;
    frame->width = from.width;
    frame->height = from.height;
    frame->format = AV_PIX_FMT_BGRA;
    std::vector<uint32_t> shrunk;
    SwsContext* sws = frameScaler(nullptr, frame, from.width, to.width, to.height,
                                  AV_PIX_FMT_BGRA, kSharp);
    const bool ok = sws && scaleInto(sws, frame, to.width, to.height, shrunk);
    sws_freeContext(sws);
    av_frame_free(&frame);
    if (!ok) return nullptr;
    pixels = std::move(shrunk);
  }
  auto* owned = new std::vector<uint32_t>(std::move(pixels));
  cairo_surface_t* surface = cairo_image_surface_create_for_data(
      reinterpret_cast<unsigned char*>(owned->data()), CAIRO_FORMAT_RGB24, to.width, to.height,
      to.width * 4);
  cairo_surface_set_user_data(surface, &kPixelsKey, owned, freePixels);
  return surface;
}

size_t bytesOf(cairo_surface_t* surface) {
  return surface ? static_cast<size_t>(cairo_image_surface_get_stride(surface)) *
                       cairo_image_surface_get_height(surface)
                 : 0;
}

// A file's thumbnail in the disk cache, named by a hash (FNV-1a, 64 bit) of
// its path (a Stash scene's: its file's oshash), size and modification time:
// no names show, and a changed file gets a new one.
std::string diskName(const std::string& path, const struct stat& file) {
  const std::string key = path + '\n' + std::to_string(file.st_size) + ' ' +
                          std::to_string(file.st_mtim.tv_sec) + '.' +
                          std::to_string(file.st_mtim.tv_nsec);
  uint64_t h = 14695981039346656037ull;
  for (const unsigned char c : key) h = (h ^ c) * 1099511628211ull;
  char name[24];
  std::snprintf(name, sizeof(name), "%016llx.jpg", static_cast<unsigned long long>(h));
  return name;
}

// The gallery's picture from the disk cache, if it is there at that size.
bool readDisk(const fs::path& file, Size size, std::vector<uint32_t>& pixels) {
  std::ifstream in(file, std::ios::binary);
  const std::string data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  int width = 0, height = 0;
  return !data.empty() && decodeJpeg(data, pixels, width, height) && width == size.width &&
         height == size.height;
}

// Through a temporary file renamed over the target, so a crash never leaves
// half a file under the real name. The bytes written, 0 on failure.
uintmax_t writeDisk(const fs::path& file, const std::vector<uint32_t>& pixels, Size size) {
  std::string data;
  if (!encodeJpeg(pixels, size.width, size.height, data)) return 0;
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  fs::path temp = file;
  temp += ".tmp";
  std::ofstream out(temp, std::ios::binary | std::ios::trunc);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  out.close();
  if (out) fs::rename(temp, file, ec);
  if (!out || ec) {
    fs::remove(temp, ec);
    return 0;
  }
  return data.size();
}

// Over budget, the least recently used thumbnails go (each is touched when
// read), down to 90% so it isn't trimmed again with every new one; also
// temporary files a crash left. The bytes the cache then holds.
uintmax_t trimDisk(const fs::path& dir) {
  struct File {
    fs::file_time_type used;
    uintmax_t bytes;
    fs::path path;
  };
  std::vector<File> files;
  uintmax_t total = 0;
  std::error_code ec, e;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().extension() == ".tmp") {
      fs::remove(it->path(), e);
      continue;
    }
    File f{it->last_write_time(e), it->file_size(e), it->path()};
    if (e) continue;
    total += f.bytes;
    files.push_back(std::move(f));
  }
  if (total <= kDiskBudget) return total;
  std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.used < b.used; });
  for (const File& f : files) {
    if (total <= kDiskBudget / 10 * 9) break;
    if (fs::remove(f.path, e)) total -= f.bytes;
  }
  return total;
}

}  // namespace

struct Thumbnailer::Worker {
  struct Cached {
    struct stat file{};
    Clock::time_point checked;
    bool ok = false;
    int width = 0;  // of the surface made
  };
  Size gallery, list;
  fs::path dir;  // the disk cache
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::string> queue;
  std::set<std::string> busy;  // files a worker is on
  std::vector<std::string> wanted;
  int width = 0;  // how wide the wanted ones are drawn
  bool exact = false;           // ... and made at that width (wantAhead())
  std::set<std::string> ahead;  // ... but for these, made at the list's
  std::map<std::string, Cached> cache;
  std::vector<Frame> finished;
  std::atomic<bool> quit{false};
  std::atomic<bool> playing{false};
  int stopped = 0;  // workers
  // Writing to the disk cache and trimming it, one worker at a time.
  std::mutex diskMutex;
  bool trimmed = false;  // once at start
  uintmax_t diskBytes = 0;
  // The CUDA device all workers decode with, made with the first video.
  // It holds about 260 MB of video memory, so it is let go once no video
  // has been decoded for kRetryInterval, and made again when needed.
  std::mutex gpuMutex;
  AVBufferRef* gpu = nullptr;
  bool gpuTried = false;
  Clock::time_point gpuUsed;
  ~Worker() {
    av_buffer_unref(&gpu);
    for (const Frame& frame : finished)
      if (frame.surface) cairo_surface_destroy(frame.surface);
  }
};

Thumbnailer::~Thumbnailer() {
  if (!workers_.empty()) {
    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->quit = true;
    state_->wake.notify_all();
    // Filesystem syscalls need not honor FFmpeg's interrupt callback. The
    // workers own their state, so even a stuck mount cannot hold up shutdown.
    const bool stopped = state_->wake.wait_for(lock, std::chrono::milliseconds(500), [this] {
      return state_->stopped == static_cast<int>(workers_.size());
    });
    lock.unlock();
    for (std::thread& worker : workers_) stopped ? worker.join() : worker.detach();
  }
  for (auto& [path, surface] : done_)
    if (surface) cairo_surface_destroy(surface);
}

// 16:9, rounded up: the picture covers a box that size (drawThumbnail()).
void Thumbnailer::setSizes(int galleryWidth, int listWidth) {
  gallery_ = {galleryWidth, (galleryWidth * 9 + 15) / 16};
  list_ = {listWidth, (listWidth * 9 + 15) / 16};
}

void Thumbnailer::start() {
  av_log_set_level(AV_LOG_QUIET);  // FFmpeg messages could name files
  state_ = std::make_shared<Worker>();
  state_->gallery = gallery_;
  state_->list = list_;
  state_->dir = cacheDir() / "thumbnails";
  for (int i = 0; i < kWorkers; ++i) workers_.emplace_back(&Thumbnailer::work, state_);
}

cairo_surface_t* Thumbnailer::find(const std::string& path) const {
  const auto it = done_.find(path);
  return it != done_.end() ? it->second : nullptr;
}

void Thumbnailer::want(const std::vector<std::string>& paths, int width) {
  request(paths, paths.size(), width, false);
}

void dropThumbnail(const std::string& path, uint64_t size, int64_t seconds, long nanos) {
  struct stat file {};
  file.st_size = static_cast<off_t>(size);
  file.st_mtim.tv_sec = seconds;
  file.st_mtim.tv_nsec = nanos;
  std::error_code ec;
  fs::remove(cacheDir() / "thumbnails" / diskName(path, file), ec);
}

void Thumbnailer::wantAhead(const std::vector<std::string>& paths, size_t shown, int width) {
  request(paths, shown, width, true);
}

void Thumbnailer::request(const std::vector<std::string>& paths, size_t shown, int width, bool exact) {
  wanted_ = std::set<std::string>(paths.begin(), paths.end());
  if (!state_) return;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->wanted = paths;
    state_->width = width;
    state_->exact = exact;
    state_->ahead = std::set<std::string>(paths.begin() + std::min(shown, paths.size()), paths.end());
    state_->queue.assign(paths.begin(), paths.end());
  }
  state_->wake.notify_all();
}

void Thumbnailer::setPlaying(bool playing) {
  if (!state_ || playing == playing_) return;
  playing_ = playing;
  state_->playing = playing;
  if (playing) return;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);  // those left out meanwhile
    state_->queue.assign(state_->wanted.begin(), state_->wanted.end());
  }
  state_->wake.notify_all();
}

void Thumbnailer::work(const std::shared_ptr<Worker>& worker) {
  // Behind playback and the menu; on the Frame on kFrameCores.
  const auto tid = static_cast<pid_t>(syscall(SYS_gettid));
  setpriority(PRIO_PROCESS, static_cast<id_t>(tid), 19);
  syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, tid, 3 << 13 /* IOPRIO_CLASS_IDLE */);
  if (kStandaloneHeadset) {
    cpu_set_t cores;
    CPU_ZERO(&cores);
    for (const int core : kFrameCores) CPU_SET(core, &cores);
    sched_setaffinity(tid, sizeof(cores), &cores);
  }
  const Size gallery = worker->gallery;
  // Thumbnails made before are read back from disk, much quicker than made.
  const fs::path& dir = worker->dir;
  {
    std::lock_guard<std::mutex> lock(worker->diskMutex);
    if (!std::exchange(worker->trimmed, true)) worker->diskBytes = trimDisk(dir);
  }
  while (true) {
    {
      std::lock_guard<std::mutex> lock(worker->gpuMutex);
      if (worker->gpuTried && Clock::now() - worker->gpuUsed > kRetryInterval) {
        av_buffer_unref(&worker->gpu);
        worker->gpuTried = false;
      }
    }
    std::string path;
    Size size;
    Worker::Cached cached;
    {
      std::unique_lock<std::mutex> lock(worker->mutex);
      if (!worker->wake.wait_for(lock, kRetryInterval,
                                [&] { return worker->quit || !worker->queue.empty(); })) {
        worker->queue.assign(worker->wanted.begin(), worker->wanted.end());
      }
      if (worker->quit) break;
      if (worker->queue.empty()) continue;
      path = worker->queue.front();
      worker->queue.pop_front();
      if (!worker->busy.insert(path).second) continue;  // another worker has it
      if (worker->ahead.count(path)) {
        size = worker->list;
      } else if (worker->exact) {
        const int w = std::min(worker->width, gallery.width);
        size = {w, (w * 9 + 15) / 16};
      } else {
        size = worker->width <= worker->list.width ? worker->list : gallery;
      }
      const auto it = worker->cache.find(path);
      if (it != worker->cache.end()) {
        cached = it->second;
        // Redraws request the same files repeatedly. Revisit checks are cheap
        // on the worker; failures get at most one attempt per minute.
        const auto interval = cached.ok ? std::chrono::seconds(1) : kRetryInterval;
        if ((!cached.ok || cached.width == size.width) && Clock::now() - cached.checked < interval) {
          worker->busy.erase(path);
          continue;
        }
      }
    }
    // Leaves the file to the others again however this pass ends.
    struct Done {
      Worker& worker;
      const std::string& path;
      ~Done() {
        std::lock_guard<std::mutex> lock(worker.mutex);
        worker.busy.erase(path);
      }
    } done{*worker, path};
    Interrupt interrupt{worker->quit, worker->playing};
    struct stat file{};
    bool exists;
    std::string key = path;  // of its disk cache's name
    if (remote::isRemote(path)) {
      remote::Entry entry;
      exists = remote::stat(path, entry) == remote::Status::Ok;
      file.st_size = entry.size;
      file.st_mtim.tv_sec = entry.modified;
    } else if (isStashScene(path)) {
      // A scene read from Stash's stream, as the catalog knows its file: one
      // made before asks Stash nothing.
      const FileFingerprint known = stashSceneFile(path);
      exists = !known.oshash.empty();
      file.st_size = static_cast<off_t>(known.size);
      file.st_mtim.tv_sec = known.mtime;
      key = "stash:" + known.oshash;
    } else {
      exists = stat(path.c_str(), &file) == 0;
    }
    if (worker->quit) break;
    if (exists && cached.ok && cached.width == size.width && file.st_size == cached.file.st_size &&
        file.st_mtim.tv_sec == cached.file.st_mtim.tv_sec &&
        file.st_mtim.tv_nsec == cached.file.st_mtim.tv_nsec) {
      std::lock_guard<std::mutex> lock(worker->mutex);
      worker->cache[path].checked = Clock::now();
      continue;
    }
    Frame frame;
    frame.path = path;
    std::vector<uint32_t> pixels;  // at the gallery's size
    const fs::path stored = dir / diskName(key, file);
    bool ok = exists && readDisk(stored, gallery, pixels);
    std::error_code ec;
    if (ok) fs::last_write_time(stored, fs::file_time_type::clock::now(), ec);  // used: kept longer
    if (exists && !ok && worker->playing) continue;  // made once it stops (setPlaying())
    if (exists && !ok) {
      // Photos on the CPU; videos on the GPU, else (e.g. H.264 wider than
      // NVDEC handles) on the CPU. A video that failed both is retried on the
      // CPU only, so its retries don't keep the CUDA context.
      const bool photo = isPhotoFile(path);
      const bool retry = cached.checked != Clock::time_point{} && !cached.ok;
      AVBufferRef* gpu = nullptr;  // this decode's reference to the shared device
      if (!photo && !retry && !Interrupt::check(&interrupt)) {
        std::lock_guard<std::mutex> lock(worker->gpuMutex);
        if (!worker->gpuTried) {
          worker->gpuTried = true;
          worker->gpuUsed = Clock::now();
          if (av_hwdevice_ctx_create(&worker->gpu, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) < 0)
            worker->gpu = nullptr;
        }
        if (worker->gpu) {
          gpu = av_buffer_ref(worker->gpu);
          worker->gpuUsed = Clock::now();
        }
      }
      ok = gpu && decodeFrame(path, gpu, gallery, pixels, interrupt);
      if (gpu) {
        av_buffer_unref(&gpu);
        std::lock_guard<std::mutex> lock(worker->gpuMutex);
        worker->gpuUsed = Clock::now();
      }
      if (!ok) ok = decodeFrame(path, nullptr, gallery, pixels, interrupt);
      if (!ok && worker->playing) continue;  // stopped, not failed
      if (ok) {
        std::lock_guard<std::mutex> lock(worker->diskMutex);
        worker->diskBytes += writeDisk(stored, pixels, gallery);
        if (worker->diskBytes > kDiskBudget) worker->diskBytes = trimDisk(dir);
      }
    }
    if (ok) frame.surface = makeSurface(std::move(pixels), gallery, size);
    std::lock_guard<std::mutex> lock(worker->mutex);
    worker->cache[path] = {file, Clock::now(), frame.surface != nullptr, size.width};
    if (!worker->quit) worker->finished.push_back(std::move(frame));
  }
  std::lock_guard<std::mutex> lock(worker->mutex);
  ++worker->stopped;
  worker->wake.notify_all();
}

bool Thumbnailer::update() {
  if (!state_) return false;
  std::vector<Frame> frames;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    frames.swap(state_->finished);
  }
  if (frames.empty()) return false;
  for (const Frame& frame : frames) {
    auto& surface = done_[frame.path];
    doneBytes_ -= bytesOf(surface);
    if (surface) cairo_surface_destroy(surface);
    surface = frame.surface;
    doneBytes_ += bytesOf(surface);
  }
  if (doneBytes_ > kMemoryBudget) {
    // Make room: drop the thumbnails that are not wanted at the moment.
    for (auto it = done_.begin(); it != done_.end();) {
      if (wanted_.count(it->first)) {
        ++it;
        continue;
      }
      doneBytes_ -= bytesOf(it->second);
      if (it->second) cairo_surface_destroy(it->second);
      {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->cache.erase(it->first);
      }
      it = done_.erase(it);
    }
  }
  return true;
}
