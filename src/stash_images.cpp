#include "stash_images.h"

#include "stash_http.h"
#include "thumbnailer.h"  // scaleInto

#include <cairo.h>
#include <curl/curl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <librsvg/rsvg.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace fs = std::filesystem;
using Kind = StashImages::Kind;

namespace {

constexpr int kTiers[] = {256, 512, 1024, 2048};  // widths of the sized copies
constexpr int kDownloaders = 3;
constexpr int kResizers = 2;
// About eighty 1024-pixel covers, or a thousand of the list's 256-pixel
// ones: a few screens of the grid either way, so scrolling back is instant.
// Wanted images stay even beyond it.
constexpr size_t kMemoryBudget = 192u << 20;
constexpr size_t kMaxDownload = 64u << 20;
constexpr int kMaxSide = 16384;  // larger originals are not decoded
constexpr auto kOfflinePause = std::chrono::seconds(30);
constexpr const char* kJpegQuality = "90";

// StashImages stopped whose workers are not yet gone.
std::mutex stoppingMutex;
std::condition_variable stopped;
int stopping = 0;

const char* folderOf(Kind kind) {
  switch (kind) {
    case Kind::Cover: return "covers";
    case Kind::Performer: return "performers";
    default: return "studios";
  }
}

char letterOf(Kind kind) {
  switch (kind) {
    case Kind::Cover: return 'c';
    case Kind::Performer: return 'p';
    default: return 's';
  }
}

bool plainName(const std::string& s) {
  return !s.empty() && s.size() <= 64 &&
         std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c); });
}

// The URL's t (Stash's image version), else a hash of the URL.
std::string versionOf(const std::string& url) {
  for (const char* key : {"?t=", "&t="}) {
    const size_t pos = url.find(key);
    if (pos == std::string::npos) continue;
    const size_t start = pos + 3;
    const std::string t = url.substr(start, url.find_first_of("&#", start) - start);
    if (plainName(t)) return t;
  }
  char hex[17];
  std::snprintf(hex, sizeof(hex), "%016zx", std::hash<std::string>()(url));
  return hex;
}

// Stash's placeholder for an object without an image.
bool isPlaceholder(const std::string& url) { return url.find("default=true") != std::string::npos; }

int tierFor(int width) {
  for (int t : kTiers)
    if (t >= width) return t;
  return kTiers[std::size(kTiers) - 1];
}

// One image at one size, for the workers.
struct Job {
  Kind kind = Kind::Cover;
  std::string id, version, url;  // url: as Stash gave it
  int tier = 0;
  int width = 0;      // a cover: the width shown, which the copy in memory fits (fitCover())
  bool crop = false;  // a 3D video's cover: its left eye if it is a raw frame
  int videoWidth = 0, videoHeight = 0;
  bool prefetch = false;

  std::string entry() const { return std::string(1, letterOf(kind)) + "/" + id; }
  std::string image() const { return entry() + "@" + version; }  // the downloaded file
  std::string picture() const { return image() + (crop ? "/l" : ""); }  // what is shown
  std::string key() const {
    return picture() + "#" + std::to_string(tier) + (width ? "/" + std::to_string(width) : "");
  }
};

enum class Format { Unknown, Jpeg, Png, Webp, Gif, Svg, Jxl };

Format sniff(const std::string& d) {
  auto at = [&](size_t pos, const char* magic, size_t n) {
    return d.size() >= pos + n && std::memcmp(d.data() + pos, magic, n) == 0;
  };
  if (at(0, "\xFF\xD8\xFF", 3)) return Format::Jpeg;
  if (at(0, "\x89PNG", 4)) return Format::Png;
  if (at(0, "RIFF", 4) && at(8, "WEBP", 4)) return Format::Webp;
  if (at(0, "GIF8", 4)) return Format::Gif;
  if (at(0, "\xFF\x0A", 2) || at(0, "\0\0\0\x0CJXL ", 8)) return Format::Jxl;
  size_t i = at(0, "\xEF\xBB\xBF", 3) ? 3 : 0;
  while (i < d.size() && std::isspace(static_cast<unsigned char>(d[i]))) ++i;
  if (i < d.size() && d[i] == '<' && d.substr(0, 4096).find("<svg") != std::string::npos)
    return Format::Svg;
  return Format::Unknown;
}

// Stash's covers and portraits are photos; an SVG in their place is its
// placeholder for a scene or performer without an image (only studios have
// SVG logos).
Format formatOf(const std::string& data, Kind kind) {
  const Format f = sniff(data);
  return f == Format::Svg && kind != Kind::Studio ? Format::Unknown : f;
}

const char* extensionOf(Format f) {
  switch (f) {
    case Format::Jpeg: return "jpg";
    case Format::Png: return "png";
    case Format::Webp: return "webp";
    case Format::Gif: return "gif";
    case Format::Svg: return "svg";
    case Format::Jxl: return "jxl";
    default: return "bin";
  }
}

AVCodecID codecOf(Format f) {
  switch (f) {
    case Format::Jpeg: return AV_CODEC_ID_MJPEG;
    case Format::Png: return AV_CODEC_ID_PNG;
    case Format::Webp: return AV_CODEC_ID_WEBP;
    case Format::Gif: return AV_CODEC_ID_GIF;
    case Format::Jxl: return AV_CODEC_ID_JPEGXL;
    default: return AV_CODEC_ID_NONE;
  }
}

// A JPEG's size from its frame header, without decoding it.
bool jpegSize(const std::string& d, int& width, int& height) {
  auto byte = [&](size_t i) { return static_cast<unsigned>(static_cast<unsigned char>(d[i])); };
  size_t i = 2;
  while (i + 9 < d.size()) {
    if (byte(i) != 0xFF) return false;
    const unsigned marker = byte(i + 1);
    if (marker == 0xFF) {
      ++i;
      continue;
    }
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {
      i += 2;
      continue;
    }
    if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
      height = static_cast<int>(byte(i + 5) << 8 | byte(i + 6));
      width = static_cast<int>(byte(i + 7) << 8 | byte(i + 8));
      return width > 0 && height > 0;
    }
    i += 2 + (byte(i + 2) << 8 | byte(i + 3));
  }
  return false;
}

bool readFile(const fs::path& path, std::string& data) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !in.bad() && !data.empty();
}

// Through a temporary file renamed over the target, so a crash never leaves
// half a file under the real name.
bool writeFile(const fs::path& path, const std::string& data) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  fs::path temp = path;
  temp += ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) {
      fs::remove(temp, ec);
      return false;
    }
  }
  fs::rename(temp, path, ec);
  if (ec) fs::remove(temp, ec);
  return !ec;
}

// A picture in cairo's premultiplied ARGB32 (BGRA bytes), top row first.
struct Picture {
  int width = 0, height = 0;
  bool transparent = false;
  std::vector<uint32_t> pixels;
};

// A decoded image for swscale: FFmpeg's frame, or gdk-pixbuf's pixels
// (straight BGRA, padded for swscale's reads) wrapped in one.
struct Source {
  AVFrame* frame = nullptr;
  std::vector<uint32_t> pixels;
  Source() = default;
  Source(const Source&) = delete;
  Source& operator=(const Source&) = delete;
  ~Source() { av_frame_free(&frame); }
};

AVFrame* wrapBgra(std::vector<uint32_t>& pixels, int width, int height) {
  AVFrame* frame = av_frame_alloc();
  if (!frame) return nullptr;
  frame->data[0] = reinterpret_cast<uint8_t*>(pixels.data());
  frame->linesize[0] = width * 4;
  frame->width = width;
  frame->height = height;
  frame->format = AV_PIX_FMT_BGRA;
  return frame;
}

// The first picture of an image file. A JPEG decodes at 1/2^lowres size,
// which costs a fraction of a full decode.
AVFrame* decodeFFmpeg(const std::string& data, Format format, int lowres) {
  const AVCodec* decoder = avcodec_find_decoder(codecOf(format));
  if (!decoder) return nullptr;
  AVCodecContext* codec = avcodec_alloc_context3(decoder);
  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  bool ok = false;
  do {
    if (!codec || !packet || !frame) break;
    codec->thread_count = 1;
    codec->lowres = std::min<int>(lowres, decoder->max_lowres);
    if (avcodec_open2(codec, decoder, nullptr) < 0) break;
    if (av_new_packet(packet, static_cast<int>(data.size())) < 0) break;
    std::memcpy(packet->data, data.data(), data.size());
    packet->flags |= AV_PKT_FLAG_KEY;
    if (avcodec_send_packet(codec, packet) < 0) break;
    avcodec_send_packet(codec, nullptr);
    ok = avcodec_receive_frame(codec, frame) == 0 && frame->width > 0 && frame->height > 0 &&
         frame->width <= kMaxSide && frame->height <= kMaxSide;
  } while (false);
  av_packet_free(&packet);
  avcodec_free_context(&codec);
  if (!ok) av_frame_free(&frame);
  return frame;
}

// Whatever FFmpeg cannot read (e.g. an animated WebP), through gdk-pixbuf.
bool decodePixbuf(const std::string& data, Source& source) {
  GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
  const bool loaded =
      gdk_pixbuf_loader_write(loader, reinterpret_cast<const guchar*>(data.data()), data.size(),
                              nullptr) &&
      gdk_pixbuf_loader_close(loader, nullptr);
  GdkPixbuf* pixbuf = loaded ? gdk_pixbuf_loader_get_pixbuf(loader) : nullptr;
  const int w = pixbuf ? gdk_pixbuf_get_width(pixbuf) : 0;
  const int h = pixbuf ? gdk_pixbuf_get_height(pixbuf) : 0;
  const bool ok = w > 0 && h > 0 && w <= kMaxSide && h <= kMaxSide &&
                  gdk_pixbuf_get_bits_per_sample(pixbuf) == 8 && gdk_pixbuf_get_n_channels(pixbuf) >= 3;
  if (ok) {
    const int channels = gdk_pixbuf_get_n_channels(pixbuf);
    const int stride = gdk_pixbuf_get_rowstride(pixbuf);
    const bool alpha = gdk_pixbuf_get_has_alpha(pixbuf);
    const guchar* pixels = gdk_pixbuf_get_pixels(pixbuf);
    source.pixels.assign(static_cast<size_t>(w) * h + 64, 0);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const guchar* p = pixels + static_cast<size_t>(y) * stride + x * channels;
        const uint32_t a = alpha ? p[3] : 255;
        source.pixels[static_cast<size_t>(y) * w + x] =
            a << 24 | uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
      }
    source.frame = wrapBgra(source.pixels, w, h);
  }
  g_object_unref(loader);
  return ok && source.frame;
}

bool decode(const std::string& data, Format format, int lowres, Source& source) {
  source.frame = decodeFFmpeg(data, format, lowres);
  if (!source.frame && lowres > 0) source.frame = decodeFFmpeg(data, format, 0);
  return source.frame || decodePixbuf(data, source);
}

// Converts the left `srcWidth` columns of a frame to BGRA at the given size
// (Lanczos when it shrinks).
bool convert(const AVFrame* frame, int srcWidth, int width, int height, std::vector<uint32_t>& pixels) {
  const bool scaled = width != srcWidth || height != frame->height;
  const int flags = scaled ? SWS_LANCZOS | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP
                           : SWS_BICUBIC;
  SwsContext* sws = frameScaler(nullptr, frame, srcWidth, width, height, AV_PIX_FMT_BGRA, flags);
  if (!sws) return false;
  const bool ok = scaleInto(sws, frame, width, height, pixels);
  sws_freeContext(sws);
  return ok;
}

void premultiply(std::vector<uint32_t>& pixels) {
  for (uint32_t& p : pixels) {
    const uint32_t a = p >> 24;
    if (a == 255) continue;
    auto mul = [a](uint32_t c) { return (c * a + 127) / 255; };
    p = a << 24 | mul(p >> 16 & 255) << 16 | mul(p >> 8 & 255) << 8 | mul(p & 255);
  }
}

// Lanczos overshoots; a premultiplied colour may not exceed its alpha.
void clampToAlpha(std::vector<uint32_t>& pixels) {
  for (uint32_t& p : pixels) {
    const uint32_t a = p >> 24;
    if (a == 255) continue;
    p = a << 24 | std::min(p >> 16 & 255, a) << 16 | std::min(p >> 8 & 255, a) << 8 | std::min(p & 255, a);
  }
}

// The left `srcWidth` columns of a decoded image at width × height.
bool makePicture(const AVFrame* frame, int srcWidth, int width, int height, Picture& out) {
  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
  out.width = width;
  out.height = height;
  out.transparent = false;
  if (!desc || !(desc->flags & (AV_PIX_FMT_FLAG_ALPHA | AV_PIX_FMT_FLAG_PAL)))
    return convert(frame, srcWidth, width, height, out.pixels);
  // With alpha: converted at full size first, to see whether any of it is
  // see-through, and so see-through pixels are premultiplied before scaling
  // (else their hidden colour bleeds into the edges).
  std::vector<uint32_t> full;
  if (!convert(frame, srcWidth, srcWidth, frame->height, full)) return false;
  out.transparent = std::any_of(full.begin(), full.end(), [](uint32_t p) { return p >> 24 != 255; });
  if (out.transparent) premultiply(full);
  if (width == srcWidth && height == frame->height) {
    out.pixels = std::move(full);
    return true;
  }
  full.resize(full.size() + 64);  // swscale may read past the last row
  AVFrame* wrapped = wrapBgra(full, srcWidth, frame->height);
  const bool ok = wrapped && convert(wrapped, srcWidth, width, height, out.pixels);
  av_frame_free(&wrapped);
  if (ok && out.transparent) clampToAlpha(out.pixels);
  return ok;
}

// An SVG logo drawn `width` pixels wide.
bool renderSvg(const std::string& data, int width, Picture& out) {
  GError* error = nullptr;
  RsvgHandle* svg = rsvg_handle_new_from_data(reinterpret_cast<const guint8*>(data.data()),
                                              data.size(), &error);
  if (!svg) {
    g_clear_error(&error);
    return false;
  }
  rsvg_handle_set_dpi(svg, 96);
  double w = 0, h = 0;
  if (!rsvg_handle_get_intrinsic_size_in_pixels(svg, &w, &h) || w <= 0 || h <= 0) {
    gboolean hasViewBox = FALSE;
    RsvgRectangle viewBox{};
    rsvg_handle_get_intrinsic_dimensions(svg, nullptr, nullptr, nullptr, nullptr, &hasViewBox,
                                         &viewBox);
    w = hasViewBox && viewBox.width > 0 ? viewBox.width : 1;
    h = hasViewBox && viewBox.height > 0 ? viewBox.height : 1;
  }
  out.width = width;
  out.height = std::clamp(static_cast<int>(std::lround(width * h / w)), 1, 4 * width);
  cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, out.width, out.height);
  cairo_t* cr = cairo_create(surface);
  const RsvgRectangle viewport{0, 0, double(out.width), double(out.height)};
  const bool ok = rsvg_handle_render_document(svg, cr, &viewport, &error);
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  if (ok && cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
    const unsigned char* bytes = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    out.pixels.resize(static_cast<size_t>(out.width) * out.height);
    for (int y = 0; y < out.height; ++y)
      std::memcpy(&out.pixels[static_cast<size_t>(y) * out.width], bytes + static_cast<size_t>(y) * stride,
                  static_cast<size_t>(out.width) * 4);
    clampToAlpha(out.pixels);  // SVG filters can overshoot too
    out.transparent =
        std::any_of(out.pixels.begin(), out.pixels.end(), [](uint32_t p) { return p >> 24 != 255; });
  }
  g_clear_error(&error);
  cairo_surface_destroy(surface);
  g_object_unref(svg);
  return ok && !out.pixels.empty();
}

// Makes the copy of an original for a job: its left eye if it is a 3D
// video's raw frame, at most job.tier wide, never enlarged. `full` tells
// that it kept the original's (or eye's) width.
bool sizeOriginal(const std::string& data, const Job& job, Picture& out, bool& full) {
  const Format format = formatOf(data, job.kind);
  if (format == Format::Svg) {
    full = false;
    return renderSvg(data, job.tier, out);
  }
  if (format == Format::Unknown) return false;
  auto isRawFrame = [&](int w, int h) {
    return job.crop && w == job.videoWidth && h == job.videoHeight && w >= 2;
  };
  // A big JPEG decodes shrunk by a power of two, to no less than twice the
  // copy's width; Lanczos does the rest.
  int width = 0, height = 0, lowres = 0;
  if (format == Format::Jpeg && jpegSize(data, width, height)) {
    if (width > kMaxSide || height > kMaxSide) return false;
    const int eye = isRawFrame(width, height) ? width / 2 : width;
    const int target = std::min(eye, job.tier);
    while (lowres < 3 && (eye >> (lowres + 1)) >= 2 * target) ++lowres;
  }
  Source source;
  if (!decode(data, format, lowres, source)) return false;
  const AVFrame* frame = source.frame;
  if (frame->width != (width + (1 << lowres) - 1) >> lowres) {  // shrunk otherwise, or not at all
    width = frame->width;
    height = frame->height;
  }
  const bool crop = isRawFrame(width, height);
  const int srcWidth = crop ? frame->width / 2 : frame->width;
  const int eye = crop ? width / 2 : width;
  full = eye <= job.tier;
  const int outWidth = full ? eye : job.tier;
  const int outHeight =
      full ? height : std::clamp(static_cast<int>(std::lround(double(height) * job.tier / eye)), 1, kMaxSide);
  // Exactly the decoded size when nothing shrinks (e.g. a JPEG shrunk
  // to precisely the copy's width).
  if (srcWidth == outWidth && std::abs(frame->height - outHeight) <= 1)
    return makePicture(frame, srcWidth, srcWidth, frame->height, out);
  return makePicture(frame, srcWidth, outWidth, outHeight, out);
}

// A cover's copy in memory, sized from its tier's to just cover the 16:9
// picture a view draws `width` pixels wide (kept whole, the view crops it):
// painted at its own size, it costs a fraction of what scaling the tier's
// costs, again with every strip of a scrolling list (Lanczos is also
// sharper).
bool fitCover(int width, Picture& p) {
  if (width <= 0) return true;
  const int w = std::max(width, static_cast<int>(std::ceil(width * 9.0 / 16 * p.width / p.height)));
  if (w == p.width) return true;
  const int h =
      std::clamp(static_cast<int>(std::ceil(double(p.height) * w / p.width - 0.01)), 1, kMaxSide);
  p.pixels.resize(p.pixels.size() + 64);  // swscale may read past the last row
  AVFrame* frame = wrapBgra(p.pixels, p.width, p.height);
  Picture out;
  out.width = w;
  out.height = h;
  out.transparent = p.transparent;
  const bool ok = frame && convert(frame, p.width, w, h, out.pixels);
  av_frame_free(&frame);
  if (!ok) return false;
  if (out.transparent) clampToAlpha(out.pixels);
  p = std::move(out);
  return true;
}

// A copy, as made by sizeOriginal, read back for display.
bool decodeCopy(const std::string& data, Picture& out) {
  Source source;
  return decode(data, sniff(data), 0, source) &&
         makePicture(source.frame, source.frame->width, source.frame->width, source.frame->height, out);
}

}  // namespace

bool encodeJpeg(const std::vector<uint32_t>& pixels, int width, int height, std::string& out) {
  GdkPixbuf* pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, width, height);
  if (!pixbuf) return false;
  guchar* rows = gdk_pixbuf_get_pixels(pixbuf);
  const int stride = gdk_pixbuf_get_rowstride(pixbuf);
  for (int y = 0; y < height; ++y) {
    guchar* row = rows + static_cast<size_t>(y) * stride;
    for (int x = 0; x < width; ++x) {
      const uint32_t c = pixels[static_cast<size_t>(y) * width + x];
      row[3 * x] = c >> 16 & 255;
      row[3 * x + 1] = c >> 8 & 255;
      row[3 * x + 2] = c & 255;
    }
  }
  gchar* buffer = nullptr;
  gsize size = 0;
  const bool ok = gdk_pixbuf_save_to_buffer(pixbuf, &buffer, &size, "jpeg", nullptr, "quality",
                                            kJpegQuality, nullptr);
  if (ok) out.assign(buffer, size);
  g_free(buffer);
  g_object_unref(pixbuf);
  return ok;
}

namespace {

cairo_status_t appendBytes(void* target, const unsigned char* data, unsigned int length) {
  static_cast<std::string*>(target)->append(reinterpret_cast<const char*>(data), length);
  return CAIRO_STATUS_SUCCESS;
}

bool encodePng(const Picture& p, std::string& out) {
  cairo_surface_t* surface = cairo_image_surface_create_for_data(
      reinterpret_cast<unsigned char*>(const_cast<uint32_t*>(p.pixels.data())), CAIRO_FORMAT_ARGB32,
      p.width, p.height, p.width * 4);
  const bool ok = cairo_surface_write_to_png_stream(surface, appendBytes, &out) == CAIRO_STATUS_SUCCESS;
  cairo_surface_destroy(surface);
  return ok;
}

size_t appendDownload(char* data, size_t size, size_t count, void* target) {
  auto* body = static_cast<std::string*>(target);
  if (body->size() + size * count > kMaxDownload) return 0;  // too big: aborts
  body->append(data, size * count);
  return size * count;
}

int stopWhenQuitting(void* quit, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  return static_cast<std::atomic<bool>*>(quit)->load() ? 1 : 0;
}

// Name parts of a cached file: "<id>-<version>.<ext>" for an original,
// "<id>-<version>[-l]-<width>[f].<ext>" for a copy.
std::vector<std::string> nameParts(const std::string& name) {
  std::vector<std::string> parts;
  const std::string stem = name.substr(0, name.find('.'));
  size_t start = 0;
  while (true) {
    const size_t dash = stem.find('-', start);
    parts.push_back(stem.substr(start, dash - start));
    if (dash == std::string::npos) break;
    start = dash + 1;
  }
  return parts;
}

// Whether version a is newer than b (Stash's are Unix times).
bool newer(const std::string& a, const std::string& b) {
  return a.size() != b.size() ? a.size() > b.size() : a > b;
}

}  // namespace

bool decodeJpeg(const std::string& data, std::vector<uint32_t>& pixels, int& width, int& height) {
  Picture p;
  if (!decodeCopy(data, p)) return false;
  pixels = std::move(p.pixels);
  width = p.width;
  height = p.height;
  return true;
}

struct StashImages::State {
  // A sized copy on disk.
  struct DiskCopy {
    bool crop = false;
    int width = 0;
    bool full = false;  // as wide as the original (or its eye): serves every wider tier
    std::string file;
  };
  // What the disk holds for one scene, performer or studio: one version.
  struct DiskEntry {
    std::string version;
    std::string original;  // file name, empty if not downloaded
    std::vector<DiskCopy> copies;
  };
  struct Download {
    Job job;  // the first that needed it
    std::string image;
    std::vector<Job> jobs;
  };
  struct Result {
    std::string picture, image, key;
    bool ok = false;
    bool full = false;
    int fitted = 0;  // Job::width
    Picture pixels;
  };
  // A surface in memory, GL thread only.
  struct Copy {
    cairo_surface_t* surface = nullptr;
    int width = 0;
    bool full = false;
    int fitted = 0;  // the cover's width shown that it was made for (fitCover())
    size_t bytes = 0;
    uint64_t used = 0;
  };

  fs::path root;
  StashAddress address;
  std::string apiKey;
  std::vector<std::thread> threads;
  std::atomic<bool> quit{false};
  // Wanted images whose download was dropped while Stash could not be
  // reached: the next want() asks for them again, even the same.
  std::atomic<bool> wantedDropped{false};
  std::once_flag scanned;

  std::mutex mutex;  // guards the members below up to the GL thread's
  std::condition_variable resizeWake, downloadWake;
  std::deque<Job> wanted, prefetching;
  std::set<std::string> wantedKeys;  // Job::key() of the last want()
  std::map<std::string, std::string> busy;  // picture → Job::key() a resizer is on, "" prefetching
  std::deque<Download> downloads, prefetchDownloads;
  std::map<std::string, std::vector<Job>> inFlight;  // by image: jobs waiting for it
  std::set<std::string> failedImages;
  std::chrono::steady_clock::time_point offlineUntil;
  std::map<std::string, DiskEntry> disk;  // by Job::entry()
  std::vector<Result> finished;

  // GL thread only.
  std::map<std::string, std::vector<Copy>> memory;  // by Job::picture()
  std::set<std::string> failed;                     // by Job::image()
  std::vector<Request> wantedRequests;
  size_t memoryBytes = 0;
  uint64_t clock = 0;

  fs::path originalPath(Kind kind, const std::string& file) const {
    return root / "originals" / folderOf(kind) / file;
  }
  fs::path copyPath(Kind kind, const std::string& file) const {
    return root / "copies" / folderOf(kind) / file;
  }

  bool makeJob(const Request& r, bool prefetch, Job& job) const;
  const Copy* best(const std::string& picture, int width) const;
  bool satisfied(const Job& job) const;
  void evict();

  void scan();
  bool findCopy(const Job& job, DiskCopy& copy);
  std::string findOriginal(const Job& job);
  bool takeJob(Job& job);
  void resizeLoop();
  void process(const Job& job);
  bool joinDownload(const Job& job);
  void queueDownload(const Job& job);
  void downloadLoop();
  void fail(const std::vector<Job>& jobs);
};

bool StashImages::State::makeJob(const Request& r, bool prefetch, Job& job) const {
  if (r.url.empty() || isPlaceholder(r.url) || !plainName(r.id)) return false;
  job.kind = r.kind;
  job.id = r.id;
  job.version = versionOf(r.url);
  job.url = r.url;
  job.tier = tierFor(r.width);
  job.width = r.kind == Kind::Cover && !prefetch ? r.width : 0;
  job.crop = r.kind == Kind::Cover && r.stereo && r.videoWidth > 0 && r.videoHeight > 0;
  job.videoWidth = r.videoWidth;
  job.videoHeight = r.videoHeight;
  job.prefetch = prefetch;
  return true;
}

// ---- GL thread ----

StashImages::DiskUsage StashImages::diskUsage(const fs::path& root) {
  DiskUsage usage;
  for (const char* part : {"originals", "copies"}) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root / part, ec), end; !ec && it != end; it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      const uint64_t size = it->file_size(ec);
      if (part[0] == 'o') {
        ++usage.originals;
        usage.originalBytes += size;
      } else {
        ++usage.copies;
        usage.copyBytes += size;
      }
    }
  }
  return usage;
}

StashImages::StashImages(fs::path root, const std::string& url, std::string apiKey,
                         bool unverified)
    : state_(std::make_unique<State>()) {
  state_->root = std::move(root);
  state_->address = StashAddress(url, unverified);
  state_->apiKey = std::move(apiKey);
}

StashImages::~StashImages() {
  stop();
  State& s = *state_;
  for (std::thread& t : s.threads) t.join();
  for (auto& [picture, copies] : s.memory)
    for (State::Copy& c : copies) cairo_surface_destroy(c.surface);
  {
    std::lock_guard<std::mutex> lock(stoppingMutex);
    --stopping;
  }
  stopped.notify_all();
}

void StashImages::stop() {
  State& s = *state_;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.quit) return;
    s.quit = true;
  }
  s.resizeWake.notify_all();
  s.downloadWake.notify_all();
  {
    std::lock_guard<std::mutex> lock(stoppingMutex);
    ++stopping;
  }
  stopped.notify_all();  // its own scan() may be waiting
}

void StashImages::start() {
  static std::once_flag curlInit;
  std::call_once(curlInit, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  av_log_set_level(AV_LOG_QUIET);  // FFmpeg messages could name files
  State& s = *state_;
  for (int i = 0; i < kResizers; ++i) s.threads.emplace_back(&State::resizeLoop, &s);
  for (int i = 0; i < kDownloaders; ++i) s.threads.emplace_back(&State::downloadLoop, &s);
}

// The smallest copy at least `width` wide, else the widest.
const StashImages::State::Copy* StashImages::State::best(const std::string& picture, int width) const {
  const auto it = memory.find(picture);
  if (it == memory.end()) return nullptr;
  const Copy* best = nullptr;
  for (const Copy& c : it->second) {
    const bool better = !best || (c.width >= width ? best->width < width || c.width < best->width
                                                   : best->width < width && c.width > best->width);
    if (better) best = &c;
  }
  return best;
}

// The job's own tier is in memory (or the original's width, if no wider; a
// cover's copy fitted to the job's width). A wider copy shows meanwhile, but scaling it down costs more with every
// redraw (a list's row: 1.4 ms from 1024 pixels, 0.27 ms from 256).
bool StashImages::State::satisfied(const Job& job) const {
  const auto it = memory.find(job.picture());
  if (it == memory.end()) return false;
  return std::any_of(it->second.begin(), it->second.end(), [&](const Copy& c) {
    if (job.width) return c.fitted == job.width;
    return c.full ? c.width <= job.tier : c.width == job.tier;
  });
}

cairo_surface_t* StashImages::find(const Request& request) const {
  State& s = *state_;
  Job job;
  if (!s.makeJob(request, false, job)) return nullptr;
  auto* copy = const_cast<State::Copy*>(s.best(job.picture(), request.width));
  if (!copy) return nullptr;
  copy->used = ++s.clock;
  return copy->surface;
}

bool StashImages::failed(const Request& request) const {
  const State& s = *state_;
  Job job;
  return !s.makeJob(request, false, job) || s.failed.count(job.image());
}

void StashImages::want(const std::vector<Request>& requests) {
  State& s = *state_;
  // Views ask again with every layout (while scrolling, every frame), mostly
  // for the same.
  auto same = [](const Request& a, const Request& b) {
    return a.kind == b.kind && a.width == b.width && a.videoWidth == b.videoWidth &&
           a.videoHeight == b.videoHeight && a.stereo == b.stereo && a.id == b.id && a.url == b.url;
  };
  if (requests.size() == s.wantedRequests.size() &&
      std::equal(requests.begin(), requests.end(), s.wantedRequests.begin(), same) &&
      !s.wantedDropped.exchange(false))
    return;
  s.wantedRequests = requests;
  std::vector<Job> jobs;
  std::set<std::string> keys;
  for (const Request& r : requests) {
    Job job;
    if (!s.makeJob(r, false, job) || s.failed.count(job.image()) || s.satisfied(job)) continue;
    if (keys.insert(job.key()).second) jobs.push_back(std::move(job));
  }
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    s.wantedKeys = std::move(keys);
    // Downloads nobody waits for any more are dropped, unless under way.
    std::set<std::string> fetching;
    for (auto it = s.downloads.begin(); it != s.downloads.end();) {
      auto& waiting = it->jobs;
      waiting.erase(std::remove_if(waiting.begin(), waiting.end(),
                                   [&](const Job& j) { return !j.prefetch && !s.wantedKeys.count(j.key()); }),
                    waiting.end());
      if (waiting.empty()) {
        it = s.downloads.erase(it);
        continue;
      }
      fetching.insert(it->image);
      ++it;
    }
    for (const auto& [image, waiting] : s.inFlight) fetching.insert(image);
    for (const State::Download& d : s.prefetchDownloads) fetching.insert(d.image);
    std::set<std::string> finished, failed;  // waiting for update()
    for (const State::Result& r : s.finished) (r.ok ? finished : failed).insert(r.ok ? r.key : r.image);
    // Not those finished, those a resizer is on, or those waiting for a download.
    s.wanted.clear();
    for (Job& job : jobs) {
      const auto working = s.busy.find(job.picture());
      if (finished.count(job.key()) || failed.count(job.image()) ||
          (working != s.busy.end() && working->second == job.key()) ||
          (fetching.count(job.image()) && s.joinDownload(job)))
        continue;
      s.wanted.push_back(std::move(job));
    }
  }
  s.resizeWake.notify_all();
}

void StashImages::prefetch(const std::vector<Request>& requests) {
  State& s = *state_;
  std::deque<Job> jobs;
  std::set<std::string> keys;
  for (const Request& r : requests) {
    Job job;
    if (s.makeJob(r, true, job) && !s.failed.count(job.image()) && keys.insert(job.key()).second)
      jobs.push_back(std::move(job));
  }
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    s.prefetching = std::move(jobs);
    s.prefetchDownloads.clear();
  }
  s.resizeWake.notify_all();
}

namespace {

const cairo_user_data_key_t kPixelsKey{};

void freePixels(void* pixels) { delete static_cast<std::vector<uint32_t>*>(pixels); }

}  // namespace

bool StashImages::update() {
  State& s = *state_;
  std::vector<State::Result> results;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    results.swap(s.finished);
  }
  if (results.empty()) return false;
  for (State::Result& r : results) {
    if (!r.ok) {
      s.failed.insert(r.image);
      continue;
    }
    // The decoded pixels become the surface's own, without a copy. Opaque,
    // they are pasted as they are, not blended (several times cheaper).
    auto* pixels = new std::vector<uint32_t>(std::move(r.pixels.pixels));
    cairo_surface_t* surface = cairo_image_surface_create_for_data(
        reinterpret_cast<unsigned char*>(pixels->data()),
        r.pixels.transparent ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24, r.pixels.width,
        r.pixels.height, r.pixels.width * 4);
    cairo_surface_set_user_data(surface, &kPixelsKey, pixels, freePixels);
    State::Copy copy;
    copy.surface = surface;
    copy.width = r.pixels.width;
    copy.full = r.full;
    copy.fitted = r.fitted;
    copy.bytes = static_cast<size_t>(r.pixels.width) * r.pixels.height * 4;
    copy.used = ++s.clock;
    auto& copies = s.memory[r.picture];
    for (auto it = copies.begin(); it != copies.end(); ++it)
      if (it->width == copy.width) {
        s.memoryBytes -= it->bytes;
        cairo_surface_destroy(it->surface);
        copies.erase(it);
        break;
      }
    copies.push_back(copy);
    s.memoryBytes += copy.bytes;
  }
  s.evict();
  return true;
}

// Over budget, the longest unused surfaces go, except those shown for the
// wanted images.
void StashImages::State::evict() {
  if (memoryBytes <= kMemoryBudget) return;
  // By surface: a Copy's address is taken by the next one when it is erased.
  std::set<const cairo_surface_t*> keep;
  for (const Request& r : wantedRequests) {
    Job job;
    if (makeJob(r, false, job))
      if (const Copy* c = best(job.picture(), r.width)) keep.insert(c->surface);
  }
  std::vector<std::pair<uint64_t, const Copy*>> order;
  for (const auto& [picture, copies] : memory)
    for (const Copy& c : copies)
      if (!keep.count(c.surface)) order.emplace_back(c.used, &c);
  std::sort(order.begin(), order.end());
  std::set<const cairo_surface_t*> drop;
  size_t bytes = memoryBytes;
  for (const auto& [used, c] : order) {
    if (bytes <= kMemoryBudget) break;
    bytes -= c->bytes;
    drop.insert(c->surface);
  }
  for (auto it = memory.begin(); it != memory.end();) {
    auto& copies = it->second;
    for (auto c = copies.begin(); c != copies.end();) {
      if (!drop.count(c->surface)) {
        ++c;
        continue;
      }
      memoryBytes -= c->bytes;
      cairo_surface_destroy(c->surface);
      c = copies.erase(c);
    }
    it = copies.empty() ? memory.erase(it) : std::next(it);
  }
}

// ---- Worker threads ----

// Reads what the cache folder holds, once, before the first job. Leftover
// temporary files and older versions go. Waits first for StashImages stopped
// before (Stash switched off and on, or another server) to be gone: they
// may still be writing files there.
void StashImages::State::scan() {
  {
    std::unique_lock<std::mutex> lock(stoppingMutex);
    stopped.wait(lock, [&] { return stopping == 0 || quit; });
  }
  if (quit) return;
  std::vector<fs::path> stale;
  std::map<std::string, DiskEntry> found;
  for (Kind kind : {Kind::Cover, Kind::Performer, Kind::Studio}) {
    const std::string letter(1, letterOf(kind));
    std::error_code ec;
    for (fs::directory_iterator it(root / "originals" / folderOf(kind), ec), end; !ec && it != end;
         it.increment(ec)) {
      const std::string name = it->path().filename().string();
      const auto parts = nameParts(name);
      if (name.find(".tmp") != std::string::npos || parts.size() != 2 || !plainName(parts[0]) ||
          !plainName(parts[1])) {
        stale.push_back(it->path());
        continue;
      }
      DiskEntry& e = found[letter + "/" + parts[0]];
      if (!e.original.empty() && !newer(parts[1], e.version)) {
        stale.push_back(it->path());
        continue;
      }
      if (!e.original.empty()) stale.push_back(originalPath(kind, e.original));
      e.version = parts[1];
      e.original = name;
    }
    for (fs::directory_iterator it(root / "copies" / folderOf(kind), ec), end; !ec && it != end;
         it.increment(ec)) {
      const std::string name = it->path().filename().string();
      auto parts = nameParts(name);
      const bool crop = parts.size() == 4 && parts[2] == "l";
      DiskCopy copy;
      copy.crop = crop;
      copy.file = name;
      if (parts.size() == (crop ? 4u : 3u)) {
        std::string width = parts.back();
        copy.full = !width.empty() && width.back() == 'f';
        if (copy.full) width.pop_back();
        const bool digits = !width.empty() && width.size() < 6 &&
                            std::all_of(width.begin(), width.end(),
                                        [](unsigned char c) { return std::isdigit(c); });
        copy.width = digits ? std::stoi(width) : 0;
      }
      DiskEntry* e = nullptr;
      if (name.find(".tmp") == std::string::npos && copy.width > 0 && plainName(parts[0]) &&
          plainName(parts[1])) {
        e = &found[letter + "/" + parts[0]];
        if (e->version.empty()) e->version = parts[1];
      }
      if (!e || e->version != parts[1]) {
        stale.push_back(it->path());
        continue;
      }
      e->copies.push_back(copy);
    }
  }
  std::error_code ec;
  for (const fs::path& p : stale) fs::remove(p, ec);
  std::lock_guard<std::mutex> lock(mutex);
  disk = std::move(found);
}

// The copy to show for a job: the tier's own, or a full one no wider.
// Called with the mutex held.
bool StashImages::State::findCopy(const Job& job, DiskCopy& copy) {
  const auto it = disk.find(job.entry());
  if (it == disk.end() || it->second.version != job.version) return false;
  for (const DiskCopy& c : it->second.copies)
    if (c.crop == job.crop && ((!c.full && c.width == job.tier) || (c.full && c.width <= job.tier))) {
      copy = c;
      return true;
    }
  return false;
}

// Called with the mutex held.
std::string StashImages::State::findOriginal(const Job& job) {
  const auto it = disk.find(job.entry());
  return it != disk.end() && it->second.version == job.version ? it->second.original : std::string();
}

// The most urgent job no other resizer is on, the wanted before the
// prefetched; its duplicates among the wanted go (prefetch() made its list
// without any: going through thousands here, with the mutex held, would
// keep the GL thread waiting). Called with the mutex held.
bool StashImages::State::takeJob(Job& job) {
  for (std::deque<Job>* queue : {&wanted, &prefetching}) {
    for (auto it = queue->begin(); it != queue->end(); ++it) {
      if (busy.count(it->picture())) continue;
      job = std::move(*it);
      queue->erase(it);
      if (queue == &wanted) {
        const std::string key = job.key();
        queue->erase(std::remove_if(queue->begin(), queue->end(),
                                    [&](const Job& j) { return j.key() == key; }),
                     queue->end());
      }
      return true;
    }
  }
  return false;
}

void StashImages::State::resizeLoop() {
  while (true) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex);
      resizeWake.wait(lock, [&] { return quit || takeJob(job); });
      if (quit) return;
      busy[job.picture()] = job.prefetch ? std::string() : job.key();
    }
    std::call_once(scanned, [this] { scan(); });
    process(job);
    {
      std::lock_guard<std::mutex> lock(mutex);
      busy.erase(job.picture());
    }
    resizeWake.notify_all();  // a job held back for this picture may go now
  }
}

void StashImages::State::fail(const std::vector<Job>& jobs) {
  std::lock_guard<std::mutex> lock(mutex);
  for (const Job& job : jobs) {
    failedImages.insert(job.image());
    if (job.prefetch) continue;
    Result r;
    r.picture = job.picture();
    r.key = job.key();
    r.image = job.image();
    finished.push_back(std::move(r));
  }
}

// Shows a job's copy, making it from the original first if need be, and
// fetching the original first if need be.
void StashImages::State::process(const Job& job) {
  DiskCopy copy;
  std::string original;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (failedImages.count(job.image())) {
      if (!job.prefetch) {
        Result r;
        r.picture = job.picture();
        r.key = job.key();
        r.image = job.image();
        finished.push_back(std::move(r));
      }
      return;
    }
    if (!findCopy(job, copy)) copy.file.clear();
    original = findOriginal(job);
  }
  Result result;
  result.picture = job.picture();
  result.key = job.key();
  result.image = job.image();
  if (!copy.file.empty()) {
    if (job.prefetch) return;
    std::string data;
    if (readFile(copyPath(job.kind, copy.file), data) && decodeCopy(data, result.pixels) &&
        fitCover(job.width, result.pixels)) {
      result.ok = true;
      result.full = copy.full && !job.width;
      result.fitted = job.width;
      std::lock_guard<std::mutex> lock(mutex);
      finished.push_back(std::move(result));
      return;
    }
    // Damaged: made again.
    std::error_code ec;
    fs::remove(copyPath(job.kind, copy.file), ec);
    std::lock_guard<std::mutex> lock(mutex);
    auto& copies = disk[job.entry()].copies;
    copies.erase(std::remove_if(copies.begin(), copies.end(),
                                [&](const DiskCopy& c) { return c.file == copy.file; }),
                 copies.end());
  }
  if (original.empty()) {
    queueDownload(job);
    return;
  }
  std::string data;
  bool full = false;
  if (!readFile(originalPath(job.kind, original), data) ||
      !sizeOriginal(data, job, result.pixels, full)) {
    // Removed, so a later session downloads it again.
    std::error_code ec;
    fs::remove(originalPath(job.kind, original), ec);
    {
      std::lock_guard<std::mutex> lock(mutex);
      disk[job.entry()].original.clear();
    }
    fail({job});
    return;
  }
  std::string encoded;
  const bool png = result.pixels.transparent;
  const Picture& p = result.pixels;
  if (png ? encodePng(p, encoded) : encodeJpeg(p.pixels, p.width, p.height, encoded)) {
    DiskCopy made;
    made.crop = job.crop;
    made.width = result.pixels.width;
    made.full = full;
    made.file = job.id + "-" + job.version + (job.crop ? "-l-" : "-") +
                std::to_string(made.width) + (full ? "f" : "") + (png ? ".png" : ".jpg");
    if (writeFile(copyPath(job.kind, made.file), encoded)) {
      std::lock_guard<std::mutex> lock(mutex);
      DiskEntry& e = disk[job.entry()];
      if (e.version == job.version) {
        e.copies.erase(std::remove_if(e.copies.begin(), e.copies.end(),
                                      [&](const DiskCopy& c) { return c.file == made.file; }),
                       e.copies.end());
        e.copies.push_back(made);
      }
    }
  }
  if (job.prefetch) return;
  if (!fitCover(job.width, result.pixels)) {
    fail({job});
    return;
  }
  result.ok = true;
  result.full = full && !job.width;
  result.fitted = job.width;
  std::lock_guard<std::mutex> lock(mutex);
  finished.push_back(std::move(result));
}

// Adds a job to the download of its image if one is queued or under way.
// Called with the mutex held.
bool StashImages::State::joinDownload(const Job& job) {
  const std::string image = job.image(), key = job.key();
  auto add = [&](std::vector<Job>& jobs) {
    const auto same = std::find_if(jobs.begin(), jobs.end(), [&](const Job& j) { return j.key() == key; });
    if (same == jobs.end())
      jobs.push_back(job);
    else if (!job.prefetch)
      same->prefetch = false;  // prefetched, it would show nothing
  };
  const auto flying = inFlight.find(image);
  if (flying != inFlight.end()) {
    add(flying->second);
    return true;
  }
  for (std::deque<Download>* queue : {&downloads, &prefetchDownloads})
    for (auto it = queue->begin(); it != queue->end(); ++it) {
      if (it->image != image) continue;
      add(it->jobs);
      if (!job.prefetch && queue == &prefetchDownloads) {  // wanted now
        downloads.push_back(std::move(*it));
        prefetchDownloads.erase(it);
      }
      return true;
    }
  return false;
}

void StashImages::State::queueDownload(const Job& job) {
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (joinDownload(job)) return;
    (job.prefetch ? prefetchDownloads : downloads).push_back(Download{job, job.image(), {job}});
  }
  downloadWake.notify_one();
}

void StashImages::State::downloadLoop() {
  CURL* curl = curl_easy_init();
  curl_slist* headers = nullptr;
  if (!apiKey.empty()) headers = curl_slist_append(headers, ("ApiKey: " + apiKey).c_str());
  while (true) {
    Download d;
    {
      std::unique_lock<std::mutex> lock(mutex);
      while (true) {
        if (quit) break;
        if (std::chrono::steady_clock::now() < offlineUntil) {
          downloadWake.wait_until(lock, offlineUntil);
          continue;
        }
        std::deque<Download>& queue = !downloads.empty() ? downloads : prefetchDownloads;
        if (!queue.empty()) {
          d = std::move(queue.front());
          queue.pop_front();
          break;
        }
        downloadWake.wait(lock);
      }
      if (quit) break;
      inFlight[d.image] = std::move(d.jobs);
    }
    std::string body;
    long status = 0;
    CURLcode code = CURLE_FAILED_INIT;
    if (curl) {
      curl_easy_reset(curl);  // keeps the connection
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendDownload);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
      curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 4096L);
      curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, stopWhenQuitting);
      curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &quit);
      // On the server typed, whatever host Stash named.
      const StashReply reply =
          stashPerform(curl, address, address.pathOn(d.job.url), [&body] { body.clear(); });
      code = reply.code;
      status = reply.status;
    }
    const Format format = formatOf(body, d.job.kind);
    std::vector<Job> jobs;
    {
      std::lock_guard<std::mutex> lock(mutex);
      jobs = std::move(inFlight[d.image]);
      inFlight.erase(d.image);
    }
    if (quit) break;
    const std::string file = d.job.id + "-" + d.job.version + "." + extensionOf(format);
    if (code == CURLE_OK && status == 200 && format != Format::Unknown &&
        writeFile(originalPath(d.job.kind, file), body)) {
      // Takes the place of an older version and its copies.
      std::vector<fs::path> stale;
      {
        std::lock_guard<std::mutex> lock(mutex);
        DiskEntry& e = disk[d.job.entry()];
        if (e.version != d.job.version) {
          if (!e.original.empty()) stale.push_back(originalPath(d.job.kind, e.original));
          for (const DiskCopy& c : e.copies) stale.push_back(copyPath(d.job.kind, c.file));
          e.copies.clear();
        }
        e.version = d.job.version;
        e.original = file;
        for (Job& job : jobs) {
          if (job.prefetch) prefetching.push_front(std::move(job));
          else if (wantedKeys.count(job.key())) wanted.push_front(std::move(job));
        }
      }
      std::error_code ec;
      for (const fs::path& p : stale)
        if (p.filename() != file) fs::remove(p, ec);
      resizeWake.notify_all();
    } else if (code != CURLE_OK && code != CURLE_WRITE_ERROR) {
      // Unreachable: a pause, then the images are asked for again.
      std::lock_guard<std::mutex> lock(mutex);
      offlineUntil = std::chrono::steady_clock::now() + kOfflinePause;
      wantedDropped = true;
    } else {
      fail(jobs);  // an HTTP error, too big, or not an image
    }
  }
  curl_slist_free_all(headers);
  if (curl) curl_easy_cleanup(curl);
}
