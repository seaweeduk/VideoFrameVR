#pragma once

#include "gpu_context.h"
#include "texture.h"
#include "visible_part.h"

#include <vulkan/vulkan.h>

#include <cstdint>

class Pipelines;

// shaders/key_mask.glsl's push constants.
struct KeyMaskPush {
  float frame[4];  // the frame's size in texels, its texels per mask texel, the key's colour (keyColour())
  float key[4];    // cut, soft edge; the shrink's reach, spill removal's past it, in texels (0: off)
  float yuv[4];    // a decoded frame's planes: YuvColor::neutralRange
  float matrix[4];  // ... YuvColor::matrix
  uint32_t mode[4];  // source (video.glsl's kSource...), flags (kTvGamma), stereo layout, projection
};
static_assert(sizeof(KeyMaskPush) == 80, "KeyMaskPush must match shaders/key_mask.glsl");

// The colour key's mask, made from each new frame before the eye passes
// read it (one bilinear lookup each), at most kSize texels on a side over
// the frame: key_distance.frag stores how far each texel is from the key's
// colour, key_reach.frag the shrink's reach and how much background is near
// (spill removal) from those. Made only in the rectangles the eyes can read
// (visiblePart(), up to one per eye's part) with kSpare texels around them,
// and again when an eye needs more; the rest of the images keeps what
// earlier frames stored, which nothing reads. Made once at startup, so its
// descriptors never change: the passes, both images and their
// framebuffers, and the eye pipelines' set 1 for it (or a Q10C frame's,
// DirectVideo::setTp10Mask).
class KeyMask {
 public:
  static constexpr uint32_t kSize = 2048;  // video.glsl's kKeyMaskSize
  // Mask texels made around what the eyes need, so that small head
  // movements while a frame shows don't make it again.
  static constexpr uint32_t kSpare = 24;
  using Rect = TexelRect;

  KeyMask() = default;
  KeyMask(const KeyMask&) = delete;
  KeyMask& operator=(const KeyMask&) = delete;
  ~KeyMask();

  // The passes, before the pipelines for them.
  bool initPasses(GpuContext& gpu);
  VkRenderPass distancePass() const { return pass_[0]; }
  VkRenderPass reachPass() const { return pass_[1]; }
  // The images, their framebuffers and the mask's set (pipelines' layout).
  bool init(const Pipelines& pipelines);
  VkImageView maskView() const { return mask_.view; }
  VkDescriptorSet set() const { return set_; }

  // The frame's texels per mask texel, and the part of the image its mask
  // fills (as surround.frag's chroma.zw).
  static uint32_t texelsPer(uint32_t width, uint32_t height);
  static void scale(uint32_t width, uint32_t height, float* scale);

  // The mask texels the eye shader's lookups read for uv b of an eye's part
  // (EyeData::stereo) of a width x height frame (maskTexels()).
  static Rect needed(const float* stereo, const UvBounds& b, uint32_t width, uint32_t height);

  // Whether the mask holds what `push` makes of this image's content
  // (`generation`: the publication of its picture, or its decoded frame) in
  // the `count` rectangles `needs` (none: the frame's whole part); if not,
  // it will once the passes over made() being recorded are (then true until
  // frameAbandoned()).
  bool current(const void* image, uint64_t generation, const KeyMaskPush& push, const Rect* needs,
               int count, uint32_t width, uint32_t height);
  void frameAbandoned() { made_ = false; }
  // The rectangles the reach pass makes (the needs with kSpare around them,
  // one where they overlap or touch), and those the distance pass makes
  // for them: grown by what the reach pass reads around a texel.
  int madeCount() const { return madeCount_; }
  const Rect& made(int i) const { return madeRects_[i]; }
  int distanceCount() const { return distanceCount_; }
  const Rect& distance(int i) const { return distanceRects_[i]; }

  // Records a pass over a rectangle of the frame's part of its image
  // (distancePass(): 0, reachPass(): 1) with `pipeline`, whose textures the
  // caller binds; the viewport is the whole part.
  void begin(VkCommandBuffer cmd, int pass, uint32_t width, uint32_t height, const Rect& rect) const;
  void end(VkCommandBuffer cmd) const;
  const Texture& distances() const { return distanceTexture_; }

 private:
  GpuContext* gpu_ = nullptr;
  VkRenderPass pass_[2] = {};
  GpuImage distances_, mask_;
  Texture distanceTexture_;
  VkFramebuffer framebuffer_[2] = {};
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSet set_ = VK_NULL_HANDLE;
  // What the mask was made from last, if made_.
  bool made_ = false;
  const void* image_ = nullptr;
  uint64_t generation_ = 0;
  KeyMaskPush push_{};
  Rect madeRects_[2] = {}, distanceRects_[2] = {};
  int madeCount_ = 0, distanceCount_ = 0;
};
