#include "green_screen_check.h"

#include "platform.h"
#include "remote_avio.h"
#include "thumbnailer.h"  // frameScaler

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// The samples are this many pixels across (both eyes): enough for the
// shares, cheap to look through.
constexpr int kSampleWidth = 256;
constexpr double kSamplesAt[] = {0.2, 0.4, 0.6, 0.8};

struct Interrupt {
  const std::atomic<uint64_t>& generation;
  uint64_t mine;
  Clock::time_point deadline = Clock::now() + std::chrono::seconds(60);  // else no result: again next time
  static int check(void* opaque) {
    const auto& self = *static_cast<Interrupt*>(opaque);
    return self.generation.load() != self.mine || Clock::now() >= self.deadline;
  }
};

// One sample as YUV 4:4:4, 8 bits (the decoder's range).
struct Sample {
  int width = 0, height = 0;
  std::vector<uint8_t> y, u, v;
};

// An eye's part of a sample: how much of its picture is a keying green, at
// its top and its sides, and that green's hue (degrees from Cb toward Cr's
// negative: pure green is about 50) and spread (10th to 90th percentile).
struct Eye {
  double share = 0, top = 0, side = 0;
  bool hasHue = false;
  double hue = 0, spread = 0;
};

Eye eyeOf(const Sample& s, int x0, int y0, int w, int h) {
  // tan(35°) and tan(65°): the hues taken as a keying green.
  constexpr double kLow = 0.7002, kHigh = 2.1445;
  long picture = 0, key = 0, topPicture = 0, topKey = 0;
  long leftPicture = 0, leftKey = 0, rightPicture = 0, rightKey = 0;
  std::vector<float> hues;
  hues.reserve(static_cast<size_t>(w) * h);
  for (int y = y0; y < y0 + h; ++y)
    for (int x = x0; x < x0 + w; ++x) {
      const size_t i = static_cast<size_t>(y) * s.width + x;
      const int a = 128 - s.u[i], b = 128 - s.v[i];
      const int saturation2 = a * a + b * b;
      if (s.y[i] <= 20 && saturation2 <= 36) continue;  // the black around a fisheye picture
      const bool green = a > 0 && b > 0 && saturation2 >= 30 * 30 && b >= kLow * a && b <= kHigh * a;
      const bool top = y < y0 + h / 5, left = x < x0 + w / 8, right = x >= x0 + w - w / 8;
      ++picture;
      topPicture += top;
      leftPicture += left;
      rightPicture += right;
      if (!green) continue;
      ++key;
      topKey += top;
      leftKey += left;
      rightKey += right;
      hues.push_back(static_cast<float>(std::atan2(b, a) * 180 / M_PI));
    }
  auto ratio = [](long n, long of) { return of > 0 ? double(n) / of : 0.0; };
  Eye e;
  e.share = ratio(key, picture);
  e.top = ratio(topKey, topPicture);
  e.side = std::max(ratio(leftKey, leftPicture), ratio(rightKey, rightPicture));
  if (hues.size() > 50) {
    auto at = [&](double q) {
      auto it = hues.begin() + static_cast<long>(q * (hues.size() - 1));
      std::nth_element(hues.begin(), it, hues.end());
      return double(*it);
    };
    e.hasHue = true;
    e.hue = at(0.5);
    e.spread = at(0.9) - at(0.1);
  }
  return e;
}

// A green screen sample: every eye mostly keying green of one hue, along the
// top and a side (a set, not a floor), both eyes alike. `hue`: the first eye's.
bool greenSample(const Sample& s, StereoLayout stereo, double& hue) {
  Eye eyes[2];
  int count = 2;
  if (stereo == StereoLayout::SideBySide) {
    eyes[0] = eyeOf(s, 0, 0, s.width / 2, s.height);
    eyes[1] = eyeOf(s, s.width / 2, 0, s.width / 2, s.height);
  } else if (stereo == StereoLayout::TopBottom) {
    eyes[0] = eyeOf(s, 0, 0, s.width, s.height / 2);
    eyes[1] = eyeOf(s, 0, s.height / 2, s.width, s.height / 2);
  } else {
    eyes[0] = eyeOf(s, 0, 0, s.width, s.height);
    count = 1;
  }
  for (int i = 0; i < count; ++i) {
    const Eye& e = eyes[i];
    if (!e.hasHue || e.share < 0.4 || e.top < 0.3 || e.side < 0.3 || e.hue < 40 || e.hue > 62 ||
        e.spread > 20)
      return false;
  }
  if (count == 2 &&
      (std::abs(eyes[0].hue - eyes[1].hue) > 4 || std::abs(eyes[0].share - eyes[1].share) > 0.15))
    return false;
  hue = eyes[0].hue;
  return true;
}

// The keyframe at or before `seconds`, shrunk into `out`.
bool keyframeAt(AVFormatContext* format, AVCodecContext* codec, int stream, double seconds,
                Interrupt& interrupt, SwsContext*& sws, Sample& out) {
  if (av_seek_frame(format, -1, static_cast<int64_t>(seconds * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD) < 0)
    return false;
  avcodec_flush_buffers(codec);
  // Its packet alone, then the end of input: the decoder holds a frame back
  // until the next it decodes, here the next keyframe, seconds of video on.
  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  bool got = false;
  for (int reads = 0; reads < 2000 && !Interrupt::check(&interrupt); ++reads) {
    if (av_read_frame(format, packet) < 0) break;
    const bool key = packet->stream_index == stream && (packet->flags & AV_PKT_FLAG_KEY);
    if (key) avcodec_send_packet(codec, packet);
    av_packet_unref(packet);
    if (!key) continue;
    avcodec_send_packet(codec, nullptr);
    got = avcodec_receive_frame(codec, frame) == 0;
    break;
  }
  bool ok = false;
  if (got && frame->width > 0 && frame->height > 0) {
    out.width = kSampleWidth;
    out.height = std::max(2, static_cast<int>(std::lround(double(kSampleWidth) * frame->height /
                                                          frame->width / 2)) * 2);
    sws = frameScaler(sws, frame, frame->width, out.width, out.height, AV_PIX_FMT_YUV444P, SWS_AREA);
    if (sws) {
      // Padded: swscale writes in SIMD blocks.
      const int stride = (out.width + 63) & ~63;
      std::vector<uint8_t> planes[3];
      uint8_t* data[4] = {};
      int strides[4] = {stride, stride, stride, 0};
      for (int p = 0; p < 3; ++p) {
        planes[p].resize(static_cast<size_t>(stride) * (out.height + 2) + 256);
        data[p] = planes[p].data();
      }
      if (sws_scale(sws, frame->data, frame->linesize, 0, frame->height, data, strides) > 0) {
        std::vector<uint8_t>* dst[3] = {&out.y, &out.u, &out.v};
        for (int p = 0; p < 3; ++p) {
          dst[p]->resize(static_cast<size_t>(out.width) * out.height);
          for (int y = 0; y < out.height; ++y)
            std::copy_n(planes[p].data() + static_cast<size_t>(y) * stride, out.width,
                        dst[p]->data() + static_cast<size_t>(y) * out.width);
        }
        ok = true;
      }
    }
  }
  av_frame_free(&frame);
  av_packet_free(&packet);
  return ok;
}

// The check itself: three samples of the same green hue (at most one
// failing), so it stops at the third pass or the second failure. `looked`: the keyframes decoded; with fewer than
// two (a file that can't be read now), there is no answer.
bool hasGreenScreen(const std::string& path, StereoLayout stereo, Interrupt& interrupt,
                    int& looked) {
  looked = 0;
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
  SwsContext* sws = nullptr;
  bool found = false;
  do {
    if (avformat_find_stream_info(format, nullptr) < 0 || format->duration <= 0) break;
    const AVCodec* decoder = nullptr;
    const int stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (stream < 0 || !decoder) break;
    codec = avcodec_alloc_context3(decoder);
    if (!codec || avcodec_parameters_to_context(codec, format->streams[stream]->codecpar) < 0)
      break;
    // Keyframes only, quickly and roughly, on two threads beside playback.
    codec->thread_count = 2;
    codec->skip_frame = AVDISCARD_NONKEY;
    codec->skip_loop_filter = AVDISCARD_ALL;
    codec->flags2 |= AV_CODEC_FLAG2_FAST;
    if (avcodec_open2(codec, decoder, nullptr) < 0) break;
    const double duration = double(format->duration) / AV_TIME_BASE;
    int passed = 0, failed = 0;
    double lowHue = 1e9, highHue = -1e9;
    for (const double at : kSamplesAt) {
      Sample sample;
      double hue = 0;
      const bool decoded = keyframeAt(format, codec, stream, duration * at, interrupt, sws, sample);
      if (Interrupt::check(&interrupt)) break;
      looked += decoded;
      if (decoded && greenSample(sample, stereo, hue)) {
        ++passed;
        lowHue = std::min(lowHue, hue);
        highHue = std::max(highHue, hue);
        if (passed >= 3) break;
      } else if (++failed >= 2) {
        break;
      }
    }
    found = passed >= 3 && highHue - lowHue <= 6 && !Interrupt::check(&interrupt);
  } while (false);
  sws_freeContext(sws);
  avcodec_free_context(&codec);
  avformat_close_input(&format);
  return found;
}

}  // namespace

GreenScreenCheck::~GreenScreenCheck() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    quit_ = true;
    ++generation_;  // cancels a check in progress
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void GreenScreenCheck::start(const std::string& path, StereoLayout stereo) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    job_ = {path, stereo, ++generation_};
    done_ = false;
  }
  if (!worker_.joinable()) worker_ = std::thread([this] { work(); });
  wake_.notify_all();
}

bool GreenScreenCheck::result(std::string& path, bool& found) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!done_) return false;
  done_ = false;
  path = donePath_;
  found = doneFound_;
  return true;
}

void GreenScreenCheck::work() {
  // Behind playback and everything else: one video's check can wait. On the
  // Frame off the compositor's cores (its decoder's threads follow).
  const auto tid = static_cast<pid_t>(syscall(SYS_gettid));
  setpriority(PRIO_PROCESS, static_cast<id_t>(tid), 19);
  syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, tid, 3 << 13 /* IOPRIO_CLASS_IDLE */);
  if (kStandaloneHeadset) {
    cpu_set_t cores;
    CPU_ZERO(&cores);
    for (const int core : kFrameBackgroundCores) CPU_SET(core, &cores);
    sched_setaffinity(tid, sizeof(cores), &cores);
  }
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [&] { return quit_ || !job_.path.empty(); });
      if (quit_) return;
      job = std::move(job_);
      job_ = {};
    }
    Interrupt interrupt{generation_, job.generation};
    const auto began = Clock::now();
    int looked = 0;
    const bool found = hasGreenScreen(job.path, job.stereo, interrupt, looked);
    // Replaced, cancelled, too slow or unreadable: no answer, so it runs again next time.
    if (Interrupt::check(&interrupt) || looked < 2) {
      if (looked < 2 && !Interrupt::check(&interrupt))
        std::printf("Green screen check: could not read the video\n");
      continue;
    }
    std::printf("Green screen check: %s (%d keyframes, %.1f s)\n", found ? "found" : "none", looked,
                std::chrono::duration<double>(Clock::now() - began).count());
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation_.load() != job.generation) continue;
    donePath_ = job.path;
    doneFound_ = found;
    done_ = true;
  }
}
