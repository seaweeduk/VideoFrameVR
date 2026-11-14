#include "room_light.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

constexpr const char* kDevice = "/dev/video99";
// The view's frames show the headset's picture this long after it was
// submitted at most: later ones show the room alone.
constexpr auto kSettle = std::chrono::milliseconds(150);
constexpr int kFrames = 2;  // averaged
// The view starts about a second after the device is opened.
constexpr auto kStartTimeout = std::chrono::seconds(3);
constexpr auto kBlankTimeout = std::chrono::milliseconds(800);
// A camera that sees colour: its pixels' chromaticities spread more than
// this (grey cameras, tinted or not, give one chromaticity).
constexpr double kColourSpread = 0.015;
// An auto-exposed camera puts the room's average near 18 % of its white.
constexpr double kMiddleGrey = 0.18;
// Lit: at least this luminance. A view with less than kNoRoom of its pixels
// lit shows no room (the cameras black after the headset moved: under 1 %),
// and gives no reading. One with less than kMinLit lit is a dark room (no
// light on: 2 to 9 %): read for how dark it is, but its few lit pixels are
// mostly noise and don't give the light's colour. A dim room has over 90 %
// lit and is read as any other.
constexpr float kLit = 0.01f;
constexpr double kNoRoom = 0.01, kMinLit = 0.10;

struct Sums {
  double log = 0;  // of the luminance
  long n = 0;
  double light[3] = {0, 0, 0};  // the lit pixels' channels
  double chroma[2] = {0, 0}, chroma2[2] = {0, 0};  // r and b of r+g+b, and their squares
  long lit = 0;    // ... lit and not clipped
  long shown = 0;  // lit, clipped or not
};

}  // namespace

bool RoomLight::available() { return access(kDevice, R_OK) == 0; }

RoomLight::~RoomLight() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
}

bool RoomLight::start() {
  if (running_) return false;
  if (thread_.joinable()) thread_.join();
  running_ = true;
  blankSince_ = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ended_ = false;
    result_.reset();
  }
  try {
    thread_ = std::thread(&RoomLight::run, this);
  } catch (const std::system_error&) {
    running_ = false;
    std::lock_guard<std::mutex> lock(mutex_);
    ended_ = true;
  }
  return true;
}

void RoomLight::blankSubmitted() {
  Clock::rep none = 0;
  blankSince_.compare_exchange_strong(none, Clock::now().time_since_epoch().count());
}

bool RoomLight::take(std::optional<Reading>& reading) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ended_) return false;
  ended_ = false;
  reading = result_;
  result_.reset();
  return true;
}

void RoomLight::run() {
  std::optional<Reading> reading = read();
  blank_ = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    result_ = reading;
    ended_ = true;
  }
  running_ = false;
}

std::optional<RoomLight::Reading> RoomLight::read() {
  const int fd = open(kDevice, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    std::printf("Room light: %s can't be opened (%s)\n", kDevice, std::strerror(errno));
    return {};
  }
  struct Closer {
    int fd;
    ~Closer() { close(fd); }
  } closer{fd};
  v4l2_format format{};
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  const v4l2_pix_format& pix = format.fmt.pix;
  if (ioctl(fd, VIDIOC_G_FMT, &format) < 0 ||
      (pix.pixelformat != V4L2_PIX_FMT_RGB24 && pix.pixelformat != V4L2_PIX_FMT_BGR24) ||
      pix.width < 16 || pix.height < 16 || pix.bytesperline < pix.width * 3 ||
      pix.sizeimage < pix.bytesperline * pix.height) {
    std::printf("Room light: %s gives no RGB picture\n", kDevice);
    return {};
  }
  const bool bgr = pix.pixelformat == V4L2_PIX_FMT_BGR24;
  std::vector<uint8_t> frame(pix.sizeimage);
  // A frame by the deadline; with `fresh`, one that differs from the last
  // read. The device never reports itself readable (poll), and a read gives
  // its latest frame at once, again if no new one came (6 MB each time): it
  // is read three times a frame, and frames are told apart by a sample of
  // their bytes.
  uint64_t last = 0;
  auto next = [&](Clock::time_point deadline, bool fresh) {
    while (!stop_ && Clock::now() < deadline) {
      const ssize_t n = ::read(fd, frame.data(), frame.size());
      if (n < 0 && errno != EAGAIN && errno != EINTR) return false;
      if (n == static_cast<ssize_t>(frame.size())) {
        uint64_t sample = 14695981039346656037ull;
        for (size_t i = 0; i < frame.size(); i += 1021) sample = (sample ^ frame[i]) * 1099511628211ull;
        const bool same = sample == last;
        last = sample;
        if (!fresh || !same) return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  };

  // Until the view runs: the first frames are left over (black), read again
  // as new ones come.
  const auto startBy = Clock::now() + kStartTimeout;
  for (bool live = false, first = true; !live; first = false) {
    if (!next(startBy, !first)) {
      std::printf("Room light: %s shows no picture\n", kDevice);
      return {};
    }
    for (size_t i = 0; i < frame.size() && !live; i += 4099) live = frame[i] != 0;
  }

  float linear[256];
  for (int i = 0; i < 256; ++i) {
    const float e = i / 255.0f;
    linear[i] = e <= 0.04045f ? e / 12.92f : std::pow((e + 0.055f) / 1.055f, 2.4f);
  }
  // The middle of the view (its edges are dark around the lenses' field),
  // every fourth pixel: its mean luminance, and its sums added to `into`.
  Sums sums, unused;
  auto measure = [&](Sums& into) {
    double sum = 0;
    long count = 0;
    for (uint32_t y = pix.height / 4; y < pix.height * 3 / 4; y += 4) {
      const uint8_t* row = frame.data() + size_t(y) * pix.bytesperline;
      for (uint32_t x = pix.width / 4; x < pix.width * 3 / 4; x += 4) {
        const uint8_t* p = row + x * 3;
        const uint8_t r8 = bgr ? p[2] : p[0], b8 = bgr ? p[0] : p[2];
        const float r = linear[r8], g = linear[p[1]], b = linear[b8];
        const float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        into.log += std::log(l + 1e-3);
        ++into.n;
        sum += l;
        ++count;
        into.shown += l >= kLit;
        // The light's colour: the average of what is lit and not clipped
        // (grey world). Weighting bright pixels more (shades of grey) let a
        // few white things, a lamp or a screen, say the light is white.
        if (std::max({r8, p[1], b8}) >= 250 || l < kLit) continue;
        const float c[3] = {r, g, b};
        for (int k = 0; k < 3; ++k) into.light[k] += c[k];
        const double s = double(r) + g + b;
        into.chroma[0] += r / s;
        into.chroma[1] += b / s;
        into.chroma2[0] += (r / s) * (r / s);
        into.chroma2[1] += (b / s) * (b / s);
        ++into.lit;
      }
    }
    return count ? float(sum / count) : 0.0f;
  };

  // Blank, then the frames from kSettle after the first blank frame on. The
  // trace (when each frame came after it, and its mean luminance) shows
  // where the player's picture left the view.
  blank_ = true;
  const auto blankBy = Clock::now() + kBlankTimeout;
  std::string trace;
  int frames = 0;
  while (frames < kFrames) {
    if (!next(blankBy, true)) {
      std::printf("Room light: no frame of the room alone (%s)\n", trace.c_str());
      return {};
    }
    const Clock::rep since = blankSince_.load();
    if (!since) continue;
    const auto after = Clock::now() - Clock::time_point(Clock::duration(since));
    const bool room = after >= kSettle;
    const float luma = measure(room ? sums : unused);
    frames += room;
    char step[48];
    std::snprintf(step, sizeof(step), "%s+%lld ms %.3f%s", trace.empty() ? "" : ", ",
                  static_cast<long long>(
                      std::chrono::duration_cast<std::chrono::milliseconds>(after).count()),
                  luma, room ? "*" : "");
    trace += step;
  }

  const double shown = 100.0 * sums.shown / std::max(sums.n, 1L);
  if (sums.shown < kNoRoom * sums.n) {
    std::printf("Room light: frames %s; no room in view (%.1f %% lit)\n", trace.c_str(), shown);
    return {};
  }
  const bool dark = sums.shown < kMinLit * sums.n;
  Reading reading;
  reading.white = static_cast<float>(std::exp(sums.log / sums.n) / kMiddleGrey);
  double spread = 0;
  if (sums.lit > 0) {
    for (int k = 0; k < 3; ++k)
      reading.light[k] = static_cast<float>(sums.light[k] / sums.lit);
    for (int k = 0; k < 2; ++k) {
      const double mean = sums.chroma[k] / sums.lit;
      spread = std::max(spread, std::sqrt(std::max(sums.chroma2[k] / sums.lit - mean * mean, 0.0)));
    }
  }
  reading.colour = !dark && sums.lit >= 1000 && spread > kColourSpread;
  std::printf("Room light: frames %s; light %.3f %.3f %.3f, spread %.3f (%s), white %.3f, "
              "%.1f %% lit\n",
              trace.c_str(), reading.light[0], reading.light[1], reading.light[2], spread,
              dark ? "dark room" : reading.colour ? "colour" : "no colour", reading.white, shown);
  return reading;
}
