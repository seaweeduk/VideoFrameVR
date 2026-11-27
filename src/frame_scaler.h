#pragma once

#include <cstdint>
#include <vector>

struct SwsContext;
struct AVFrame;

// A swscale context (`cached` reused when it fits, as sws_getCachedContext)
// converting the left `srcWidth` columns of a decoded frame to `width` ×
// `height` pixels of `dstFormat`, with the frame's own range and matrix.
// FFmpeg's deprecated full-range formats (yuvj*, e.g. from JPEGs) are given
// as their plain formats with full range, which swscale otherwise warns
// about for every image. Null on failure.
SwsContext* frameScaler(SwsContext* cached, const AVFrame* frame, int srcWidth, int width,
                        int height, int dstFormat, int flags);

// Scales a decoded frame with `sws` into tightly packed 32-bit pixels
// (width × height), safely: swscale may write past a row's end.
bool scaleInto(SwsContext* sws, const AVFrame* frame, int width, int height,
               std::vector<uint32_t>& pixels);

// How far clockwise a decoded frame is to be turned to stand upright (its
// display matrix: a camera's orientation): 0, 90, 180 or 270 degrees.
int frameRotation(const AVFrame* frame);

// Turns a picture of 32-bit pixels clockwise by 90, 180 or 270 degrees.
void rotatePixels(std::vector<uint32_t>& pixels, int& width, int& height, int degrees);
