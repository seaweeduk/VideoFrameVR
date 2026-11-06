#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

class DirectFrame;
class SharedImage;

// How a decoded frame's planes become the picture mpv would have drawn
// (shaders/video.glsl, yuvToRgb; DirectVideo).
struct YuvColor {
  float range[4];   // luma scale, offset; chroma scale, offset: from a plane's sampled value
  // The same without brightness, contrast and saturation: for what masks, keys
  // and stereo focus read, which those must not change.
  float neutralRange[4];
  float matrix[4];  // Kr, Kb; the chroma plane's lookup offset (x, y), in texture coordinates
  bool tvGamma = false;  // BT.1886 video shown with TV gamma (kTvGamma)
};

// The chroma sitings a Q08C frame's Y'CbCr sampler can place its chroma at
// (DirectFrame::q08c): video_export_chroma's left, centre and top-left.
constexpr int kQ08cSitings = 3;

// An image the scene samples, by stable handle: a canvas's, a video slot's or
// a photo's. It stays valid until every FrameScene that refers to it has
// finished on the GPU (its owner retires it through GpuContext).
struct Texture {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;  // sRGB, all levels
  uint32_t width = 0, height = 0, levels = 1;
  // GL-rendered images (gl_interop.h): acquired from GL around their use in
  // a frame. nullptr for Vulkan-only images, always in SHADER_READ_ONLY_OPTIMAL
  // when sampled.
  SharedImage* shared = nullptr;
  // A decoded frame sampled from its planes (direct_video.h): view is its
  // luma, the frame has its chroma and colour. nullptr for RGB images.
  const DirectFrame* yuv = nullptr;
};
