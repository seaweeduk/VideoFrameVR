#include "frame_scaler.h"

extern "C" {
#include <libavutil/display.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <cmath>
#include <cstring>
#include <utility>

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

int frameRotation(const AVFrame* frame) {
  const AVFrameSideData* side = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX);
  if (!side) return 0;
  const double angle = av_display_rotation_get(reinterpret_cast<const int32_t*>(side->data));
  if (std::isnan(angle)) return 0;
  const int clockwise = ((static_cast<int>(std::lround(-angle)) % 360) + 360) % 360;
  return clockwise == 90 || clockwise == 180 || clockwise == 270 ? clockwise : 0;
}

void rotatePixels(std::vector<uint32_t>& pixels, int& width, int& height, int degrees) {
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
