#include "frame_scaler.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <cstring>

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
