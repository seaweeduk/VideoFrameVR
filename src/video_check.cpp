#include "video_check.h"

#include "platform.h"
#include "remote_avio.h"
#include "thumbnailer.h"  // frameScaler

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// The green screen samples are this many pixels across (both eyes): enough
// for the shares, cheap to look through.
constexpr int kGreenSampleWidth = 256;
constexpr double kGreenSamplesAt[] = {0.2, 0.4, 0.6, 0.8};
// A flat fill: its pixels are within this many code values of its colour in
// each of Y, U and V; a fisheye's picture is taken inside this much of its
// circle (its rim is blurred, and outside it is black or a mask).
constexpr int kFillTolerance = 2;
constexpr double kFisheyeArea = 0.94;
// The mask layer samples: points on a grid this many across the frame (a
// mask's blue areas span a few or more), read from the keyframe as decoded.
// Three through the video: the layered videos known have it in at least two
// (the live check, MaskProbe, catches it elsewhere).
constexpr int kLayerGrid = 1024;
constexpr double kLayerSamplesAt[] = {0.25, 0.55, 0.85};

struct Interrupt {
  const std::atomic<uint64_t>& generation;
  uint64_t mine;
  Clock::time_point deadline = Clock::now() + std::chrono::seconds(60);  // else no result: again next time
  static int check(void* opaque) {
    const auto& self = *static_cast<Interrupt*>(opaque);
    return self.generation.load() != self.mine || Clock::now() >= self.deadline;
  }
};

// One sample, 8 bits: YUV 4:4:4 planes (the decoder's range), or RGB in
// plane 0.
struct Sample {
  int width = 0, height = 0;
  std::vector<uint8_t> plane[3];
};

// A decoded frame read straight from its planes: its format if that is
// planar YUV of 8 to 16 bits, else nullptr.
const AVPixFmtDescriptor* planarYuv(const AVFrame* f) {
  const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(f->format));
  const bool planar =
      d && (d->flags & AV_PIX_FMT_FLAG_PLANAR) &&
      !(d->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_BE | AV_PIX_FMT_FLAG_HWACCEL)) &&
      d->nb_components >= 3 && d->comp[0].plane == 0 && d->comp[1].plane == 1 &&
      d->comp[2].plane == 2 && d->comp[0].shift == 0 && d->comp[0].depth <= 16;
  return planar ? d : nullptr;
}

int planeValue(const AVFrame* f, int depth, int plane, int x, int y) {
  const uint8_t* row = f->data[plane] + static_cast<ptrdiff_t>(y) * f->linesize[plane];
  return depth > 8 ? reinterpret_cast<const uint16_t*>(row)[x] : row[x];
}

// A frame's matrix's Kr and Kb: BT.709 (also when unknown), BT.601 or BT.2020.
void lumaWeights(const AVFrame* f, double& kr, double& kb) {
  kr = 0.2126, kb = 0.0722;
  if (f->colorspace == AVCOL_SPC_BT470BG || f->colorspace == AVCOL_SPC_SMPTE170M) kr = 0.299, kb = 0.114;
  if (f->colorspace == AVCOL_SPC_BT2020_NCL || f->colorspace == AVCOL_SPC_BT2020_CL)
    kr = 0.2627, kb = 0.0593;
}

// An eye's part of a sample.
struct Part {
  int x = 0, y = 0, w = 0, h = 0;
};

// The eyes' parts of a sample: how many (one, or two).
int eyeParts(const Sample& s, StereoLayout stereo, Part parts[2]) {
  const int w = s.width, h = s.height;
  if (stereo == StereoLayout::SideBySide) {
    parts[0] = {0, 0, w / 2, h};
    parts[1] = {w / 2, 0, w / 2, h};
    return 2;
  }
  if (stereo == StereoLayout::TopBottom) {
    parts[0] = {0, 0, w, h / 2};
    parts[1] = {0, h / 2, w, h / 2};
    return 2;
  }
  parts[0] = {0, 0, w, h};
  return 1;
}

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
  const std::vector<uint8_t> &luma = s.plane[0], &u = s.plane[1], &v = s.plane[2];
  long picture = 0, key = 0, topPicture = 0, topKey = 0;
  long leftPicture = 0, leftKey = 0, rightPicture = 0, rightKey = 0;
  std::vector<float> hues;
  hues.reserve(static_cast<size_t>(w) * h);
  for (int y = y0; y < y0 + h; ++y)
    for (int x = x0; x < x0 + w; ++x) {
      const size_t i = static_cast<size_t>(y) * s.width + x;
      const int a = 128 - u[i], b = 128 - v[i];
      const int saturation2 = a * a + b * b;
      if (luma[i] <= 20 && saturation2 <= 36) continue;  // the black around a fisheye picture
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
  Part parts[2];
  const int count = eyeParts(s, stereo, parts);
  for (int i = 0; i < count; ++i)
    eyes[i] = eyeOf(s, parts[i].x, parts[i].y, parts[i].w, parts[i].h);
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

// An eye's most common colour (the sample's YUV), and how much of its
// picture, of the picture's top fifth and of its upper corners (a quarter
// high and wide; the lesser of the two) is within kFillTolerance of it.
struct Fill {
  int y = 0, u = 0, v = 0;
  double share = 0, top = 0, corner = 0;
};

bool nearFill(const Sample& s, size_t i, const Fill& f) {
  return std::abs(s.plane[0][i] - f.y) <= kFillTolerance &&
         std::abs(s.plane[1][i] - f.u) <= kFillTolerance &&
         std::abs(s.plane[2][i] - f.v) <= kFillTolerance;
}

Fill fillOf(const Sample& s, const Part& eye, bool fisheye) {
  const int x0 = eye.x, y0 = eye.y, w = eye.w, h = eye.h;
  const std::vector<uint8_t> &luma = s.plane[0], &u = s.plane[1], &v = s.plane[2];
  // The picture: a fisheye's circle (an ellipse in an eye that isn't square), else all.
  auto inPicture = [&](int x, int y) {
    if (!fisheye) return true;
    const double dx = (x - x0 + 0.5) / w * 2 - 1, dy = (y - y0 + 0.5) / h * 2 - 1;
    return dx * dx + dy * dy <= kFisheyeArea * kFisheyeArea;
  };
  auto each = [&](auto&& f) {
    for (int y = y0; y < y0 + h; ++y)
      for (int x = x0; x < x0 + w; ++x)
        if (inPicture(x, y)) f(x, y, static_cast<size_t>(y) * s.width + x);
  };
  // Its most common value, without a sort: the fullest of 4096 bins (the
  // top 4 bits of Y, U and V), then the most common exact value in it.
  std::array<uint32_t, 4096> coarse{}, fine{};
  auto high = [&](size_t i) { return (luma[i] >> 4) << 8 | (u[i] >> 4) << 4 | v[i] >> 4; };
  auto low = [&](size_t i) { return (luma[i] & 15) << 8 | (u[i] & 15) << 4 | (v[i] & 15); };
  long picture = 0;
  each([&](int, int, size_t i) { ++coarse[high(i)], ++picture; });
  Fill f;
  if (picture == 0) return f;
  const int bin = static_cast<int>(std::max_element(coarse.begin(), coarse.end()) - coarse.begin());
  each([&](int, int, size_t i) {
    if (high(i) == bin) ++fine[low(i)];
  });
  const int exact = static_cast<int>(std::max_element(fine.begin(), fine.end()) - fine.begin());
  f.y = (bin >> 8) << 4 | exact >> 8;
  f.u = (bin >> 4 & 15) << 4 | (exact >> 4 & 15);
  f.v = (bin & 15) << 4 | (exact & 15);
  // Its centre: the mean of what is near it.
  long n = 0, sy = 0, su = 0, sv = 0;
  each([&](int, int, size_t i) {
    if (nearFill(s, i, f)) ++n, sy += luma[i], su += u[i], sv += v[i];
  });
  f.y = static_cast<int>(std::floor(double(sy) / n + 0.5));
  f.u = static_cast<int>(std::floor(double(su) / n + 0.5));
  f.v = static_cast<int>(std::floor(double(sv) / n + 0.5));
  // The parts, in the picture's box (a fisheye's circle's).
  const int r0 = fisheye ? static_cast<int>(std::lround(h * (1 - kFisheyeArea) / 2)) : 0;
  const int c0 = fisheye ? static_cast<int>(std::lround(w * (1 - kFisheyeArea) / 2)) : 0;
  const int bh = h - 2 * r0, bw = w - 2 * c0;
  long fill = 0, top = 0, topFill = 0, left = 0, leftFill = 0, right = 0, rightFill = 0;
  each([&](int x, int y, size_t i) {
    const bool is = nearFill(s, i, f);
    const int ry = y - y0, rx = x - x0;
    const bool t = ry < r0 + bh / 5, corner = ry < r0 + bh / 4;
    const bool l = corner && rx < c0 + bw / 4, r = corner && rx >= c0 + bw - bw / 4;
    fill += is;
    top += t, topFill += t && is;
    left += l, leftFill += l && is;
    right += r, rightFill += r && is;
  });
  auto ratio = [](long a, long b) { return b > 0 ? double(a) / b : 0.0; };
  f.share = ratio(fill, picture);
  f.top = ratio(topFill, top);
  f.corner = std::min(ratio(leftFill, left), ratio(rightFill, right));
  return f;
}

bool sameFill(const Fill& a, const Fill& b) {
  return std::abs(a.y - b.y) <= 3 && std::abs(a.u - b.u) <= 3 && std::abs(a.v - b.v) <= 3;
}

// A flat fill sample: in every eye one saturated colour covering much of
// the picture, most of its top and of both its upper corners, the same in
// both eyes. Natural footage never comes near; flat grey, white or black
// would (a dark studio, a smaller picture's border), so they aren't
// taken. `fill`: the first eye's.
bool fillSample(const Sample& s, const VideoFormat& format, Fill& fill) {
  const bool fisheye = format.projection == Projection::Fisheye180 ||
                       format.projection == Projection::Fisheye190 ||
                       format.projection == Projection::Fisheye200;
  Fill eyes[2];
  Part parts[2];
  const int count = eyeParts(s, format.stereo, parts);
  for (int i = 0; i < count; ++i) {
    eyes[i] = fillOf(s, parts[i], fisheye);
    if (eyes[i].share < 0.35 || eyes[i].top < 0.6 || eyes[i].corner < 0.6) return false;
  }
  if (count == 2 && !sameFill(eyes[0], eyes[1])) return false;
  fill = eyes[0];
  return std::hypot(fill.u - 128, fill.v - 128) >= 24;
}

// A fill's colour as the scene shows the frame, so as the colour key
// compares it: 8-bit display RGB of the decoded keyframe's own values (not
// the sample's: swscale may have changed their matrix), the median of those
// under the eye's fill pixels in its sample (at each one's middle), with the
// frame's matrix and range (DirectVideo::computeColor's, as mpv's). False
// for a frame not in planar YUV.
bool fillColour(const AVFrame* f, const Sample& s, const Part& eye, const Fill& fill,
                KeyColour& out) {
  const AVPixFmtDescriptor* d = planarYuv(f);
  if (!d) return false;
  const int depth = d->comp[0].depth;
  std::vector<int> values[3];
  for (int y = eye.y; y < eye.y + eye.h; ++y)
    for (int x = eye.x; x < eye.x + eye.w; ++x) {
      if (!nearFill(s, static_cast<size_t>(y) * s.width + x, fill)) continue;
      const int fx = static_cast<int>((2 * x + 1) * int64_t(f->width) / (2 * s.width));
      const int fy = static_cast<int>((2 * y + 1) * int64_t(f->height) / (2 * s.height));
      values[0].push_back(planeValue(f, depth, 0, fx, fy));
      const int cx = fx >> d->log2_chroma_w, cy = fy >> d->log2_chroma_h;
      for (int p = 1; p < 3; ++p) values[p].push_back(planeValue(f, depth, p, cx, cy));
    }
  if (values[0].empty()) return false;
  double m[3];
  for (int p = 0; p < 3; ++p) {
    auto middle = values[p].begin() + values[p].size() / 2;
    std::nth_element(values[p].begin(), middle, values[p].end());
    m[p] = *middle;
  }
  const bool full = f->color_range == AVCOL_RANGE_JPEG;
  const double unit = 1 << (depth - 8);
  const double l = (m[0] - (full ? 0 : 16 * unit)) / ((full ? 255 : 219) * unit);
  const double cb = (m[1] - 128 * unit) / ((full ? 254 : 224) * unit);
  const double cr = (m[2] - 128 * unit) / ((full ? 254 : 224) * unit);
  double kr, kb;
  lumaWeights(f, kr, kb);
  const double r = l + 2 * (1 - kr) * cr, b = l + 2 * (1 - kb) * cb;
  const double g = (l - kr * r - kb * b) / (1 - kr - kb);
  auto byte = [](double c) {
    return static_cast<uint8_t>(std::lround(std::clamp(c, 0.0, 1.0) * 255));
  };
  out = {byte(r), byte(g), byte(b)};
  return true;
}

// A mask layer sample: in each eye's packed mask (a side-by-side fisheye
// video's, where SceneRenderer reads it: centred on the middle of the top
// edge for the left eye, on the corners for the right one, a fifth of the
// frame wide and two fifths high, wrapping around the edges; inside its
// circle), a few points of saturated blue with little red or green. Straight
// from the keyframe's planes, each point turned to display RGB on its own
// (its chroma its 2x2 block's): nothing else of the frame is read. False in
// `read` for a frame not in planar 4:2:0, 8 to 16 bits.
bool layerSample(const AVFrame* f, bool& read) {
  constexpr int kEnough = 4;  // points in each eye
  const AVPixFmtDescriptor* d = planarYuv(f);
  read = d && d->log2_chroma_w == 1 && d->log2_chroma_h == 1;
  if (!read) return false;
  const int depth = d->comp[0].depth;
  auto value = [&](int plane, int x, int y) -> double { return planeValue(f, depth, plane, x, y); };
  const bool full = f->color_range == AVCOL_RANGE_JPEG;
  const double unit = (1 << (depth - 8)), top = (1 << depth) - 1;
  const double lumaOffset = full ? 0 : 16 * unit, lumaScale = full ? top : 219 * unit;
  const double chromaScale = full ? top : 224 * unit;
  double kr, kb;
  lumaWeights(f, kr, kb);
  const double step = double(f->width) / kLayerGrid;
  int found[2] = {};
  for (int j = 0; j < static_cast<int>(f->height / step); ++j) {
    const int y = static_cast<int>(j * step + step / 2);
    const double fy = (y + 0.5) / f->height;
    const double my = (fy < 0.5 ? fy : fy - 1.0) / 0.4;  // from the top edge, wrapping
    if (std::abs(my) > 0.5) continue;
    for (int i = 0; i < kLayerGrid; ++i) {
      const int x = static_cast<int>(i * step + step / 2);
      const double fx = (x + 0.5) / f->width;
      int eye = -1;
      for (int e = 0; e < 2; ++e) {
        double dx = fx - (e == 0 ? 0.5 : 0.0);
        dx -= std::round(dx);  // wrapping
        if (dx * dx / 0.04 + my * my <= 0.25) eye = e;
      }
      if (eye < 0) continue;
      const double l = (value(0, x, y) - lumaOffset) / lumaScale;
      const double cb = (value(1, x / 2, y / 2) - 128 * unit) / chromaScale;
      const double cr = (value(2, x / 2, y / 2) - 128 * unit) / chromaScale;
      const double r = l + 2 * (1 - kr) * cr, b = l + 2 * (1 - kb) * cb;
      const double g = (l - kr * r - kb * b) / (1 - kr - kb);
      found[eye] += b >= 0.6 && r <= 0.2 && g <= 0.2;
    }
  }
  return found[0] >= kEnough && found[1] >= kEnough;
}

// A video opened for its keyframes alone, decoded quickly and roughly on two
// threads beside playback.
class Keyframes {
 public:
  Keyframes(const std::string& path, Interrupt& interrupt) : interrupt_(interrupt) {
    format_ = avformat_alloc_context();
    if (!format_) return;
    format_->interrupt_callback = {Interrupt::check, &interrupt_};
    if (!remote_.attach(format_, path)) {
      avformat_free_context(format_);
      format_ = nullptr;
      return;
    }
    if (avformat_open_input(&format_, path.c_str(), nullptr, nullptr) < 0) {
      format_ = nullptr;  // freed by avformat_open_input
      return;
    }
    if (avformat_find_stream_info(format_, nullptr) < 0 || format_->duration <= 0) return;
    const AVCodec* decoder = nullptr;
    stream_ = av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (stream_ < 0 || !decoder) return;
    codec_ = avcodec_alloc_context3(decoder);
    if (!codec_ ||
        avcodec_parameters_to_context(codec_, format_->streams[stream_]->codecpar) < 0) {
      close();
      return;
    }
    codec_->thread_count = 2;
    codec_->skip_frame = AVDISCARD_NONKEY;
    codec_->skip_loop_filter = AVDISCARD_ALL;
    codec_->flags2 |= AV_CODEC_FLAG2_FAST;
    if (avcodec_open2(codec_, decoder, nullptr) < 0) {
      close();
      return;
    }
    duration_ = double(format_->duration) / AV_TIME_BASE;
  }
  ~Keyframes() {
    av_frame_free(&frame_);
    sws_freeContext(sws_);
    avcodec_free_context(&codec_);
    avformat_close_input(&format_);
  }
  Keyframes(const Keyframes&) = delete;
  Keyframes& operator=(const Keyframes&) = delete;

  bool open() const { return codec_ != nullptr; }
  // The keyframe at or before this part of the video, as decoded, until
  // the next one; nullptr if there is none.
  const AVFrame* at(double fraction) {
    if (av_seek_frame(format_, -1, static_cast<int64_t>(duration_ * fraction * AV_TIME_BASE),
                      AVSEEK_FLAG_BACKWARD) < 0)
      return nullptr;
    avcodec_flush_buffers(codec_);
    if (!frame_ && !(frame_ = av_frame_alloc())) return nullptr;
    av_frame_unref(frame_);
    // Its packet alone, then the end of input: the decoder holds a frame back
    // until the next it decodes, here the next keyframe, seconds of video on.
    AVPacket* packet = av_packet_alloc();
    bool got = false;
    for (int reads = 0; packet && reads < 2000 && !Interrupt::check(&interrupt_); ++reads) {
      if (av_read_frame(format_, packet) < 0) break;
      const bool key = packet->stream_index == stream_ && (packet->flags & AV_PKT_FLAG_KEY);
      if (key) avcodec_send_packet(codec_, packet);
      av_packet_unref(packet);
      if (!key) continue;
      avcodec_send_packet(codec_, nullptr);
      got = avcodec_receive_frame(codec_, frame_) == 0;
      break;
    }
    av_packet_free(&packet);
    return got && frame_->width > 0 && frame_->height > 0 ? frame_ : nullptr;
  }
  // A frame shrunk to `width` pixels across into `out`, as YUV 4:4:4 planes or RGB.
  bool shrink(const AVFrame* frame, int width, bool rgb, Sample& out) {
    out.width = width;
    out.height = std::max(
        2, static_cast<int>(std::lround(double(width) * frame->height / frame->width / 2)) * 2);
    sws_ = frameScaler(sws_, frame, frame->width, out.width, out.height,
                       rgb ? AV_PIX_FMT_RGB24 : AV_PIX_FMT_YUV444P, SWS_AREA);
    if (!sws_) return false;
    // Padded: swscale writes in SIMD blocks.
    const int planes = rgb ? 1 : 3, bytes = rgb ? 3 * out.width : out.width;
    const int stride = (bytes + 63) & ~63;
    std::vector<uint8_t> scaled[3];
    uint8_t* data[4] = {};
    int strides[4] = {};
    for (int p = 0; p < planes; ++p) {
      scaled[p].resize(static_cast<size_t>(stride) * (out.height + 2) + 256);
      data[p] = scaled[p].data();
      strides[p] = stride;
    }
    if (sws_scale(sws_, frame->data, frame->linesize, 0, frame->height, data, strides) <= 0)
      return false;
    for (int p = 0; p < planes; ++p) {
      out.plane[p].resize(static_cast<size_t>(bytes) * out.height);
      for (int y = 0; y < out.height; ++y)
        std::copy_n(scaled[p].data() + static_cast<size_t>(y) * stride, bytes,
                    out.plane[p].data() + static_cast<size_t>(y) * bytes);
    }
    return true;
  }

 private:
  void close() { avcodec_free_context(&codec_); }

  Interrupt& interrupt_;
  RemoteAvio remote_;  // outlives format_ (destroyed after it)
  AVFormatContext* format_ = nullptr;
  AVCodecContext* codec_ = nullptr;
  SwsContext* sws_ = nullptr;
  AVFrame* frame_ = nullptr;  // the last keyframe decoded
  int stream_ = -1;
  double duration_ = 0;
};

// The green screen check: three samples of the same green hue (at most one
// failing), so it stops at the third pass or the second failure. `looked`: the keyframes decoded; with fewer
// than two (a file that can't be read now), there is no answer. With it the
// fill check: `filled` if its first two samples have a flat fill of one
// colour, `fill` (display RGB). A fill ends the check there, with no green
// screen: its colour key wins over the green keyer.
bool hasGreenScreen(Keyframes& video, const VideoFormat& format, Interrupt& interrupt, int& looked,
                    bool& filled, KeyColour& fill) {
  int passed = 0, failed = 0;
  double lowHue = 1e9, highHue = -1e9;
  Fill fills[2];
  bool fillSoFar = true;
  for (size_t i = 0; i < std::size(kGreenSamplesAt); ++i) {
    Sample sample;
    double hue = 0;
    const AVFrame* frame = video.at(kGreenSamplesAt[i]);
    const bool decoded = frame && video.shrink(frame, kGreenSampleWidth, false, sample);
    if (Interrupt::check(&interrupt)) break;
    if (i < 2 && fillSoFar) {
      Part eyes[2];
      eyeParts(sample, format.stereo, eyes);
      fillSoFar = decoded && fillSample(sample, format, fills[i]);
      filled = fillSoFar && i == 1 && sameFill(fills[0], fills[1]) &&
               fillColour(frame, sample, eyes[0], fills[1], fill);
    }
    looked += decoded;
    if (filled) break;
    if (decoded && greenSample(sample, format.stereo, hue)) {
      ++passed;
      lowHue = std::min(lowHue, hue);
      highHue = std::max(highHue, hue);
      if (passed >= 3) break;
    } else if (++failed >= 2) {
      break;
    }
  }
  return passed >= 3 && highHue - lowHue <= 6 && !Interrupt::check(&interrupt);
}

// The mask layer check: found in the first sample that has it.
bool hasMaskLayers(Keyframes& video, Interrupt& interrupt, int& looked) {
  for (const double at : kLayerSamplesAt) {
    const AVFrame* frame = video.at(at);
    if (Interrupt::check(&interrupt)) break;
    bool read = false;
    const bool found = frame && layerSample(frame, read);
    looked += read;
    if (found) return true;
  }
  return false;
}

}  // namespace

VideoCheck::~VideoCheck() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    quit_ = true;
    ++generation_;  // cancels a check in progress
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void VideoCheck::start(const std::string& path, const VideoFormat& format, Kind kind) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    job_ = {path, format, kind, ++generation_};
    done_ = false;
  }
  if (!worker_.joinable()) worker_ = std::thread([this] { work(); });
  wake_.notify_all();
}

bool VideoCheck::result(Result& out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!done_) return false;
  done_ = false;
  out = result_;
  return true;
}

void VideoCheck::work() {
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
    const bool green = job.kind == Kind::GreenScreen;
    const char* what = green ? "Green screen check" : "Mask layer check";
    int looked = 0;
    bool found = false, filled = false;
    KeyColour fill;
    {
      Keyframes video(job.path, interrupt);
      if (video.open())
        found = green ? hasGreenScreen(video, job.format, interrupt, looked, filled, fill)
                      : hasMaskLayers(video, interrupt, looked);
    }
    // Replaced, cancelled, too slow or unreadable: no answer, so it runs
    // again next time. (A layer found in one keyframe is an answer.)
    const int least = green ? 2 : found ? 1 : 3;
    if (Interrupt::check(&interrupt) || looked < least) {
      if (looked < least && !Interrupt::check(&interrupt))
        std::printf("%s: could not read the video\n", what);
      std::lock_guard<std::mutex> lock(mutex_);
      if (generation_.load() != job.generation) continue;  // replaced: the new one answers
      result_ = {job.path, job.kind};
      result_.answered = false;
      done_ = true;
      continue;
    }
    char fillText[64] = "";
    if (filled)
      std::snprintf(fillText, sizeof fillText, ", flat background: found, RGB %d %d %d", fill.r,
                    fill.g, fill.b);
    else if (green)
      std::snprintf(fillText, sizeof fillText, ", flat background: none");
    std::printf("%s: %s%s (%d keyframes, %.1f s)\n", what, found ? "found" : "none", fillText,
                looked, std::chrono::duration<double>(Clock::now() - began).count());
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation_.load() != job.generation) continue;
    result_ = {job.path, job.kind, found, filled, fill};
    done_ = true;
  }
}
