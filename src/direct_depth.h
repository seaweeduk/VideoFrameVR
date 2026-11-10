#pragma once

#include "depth_estimator.h"
#include "direct_video.h"
#include "gpu_context.h"

#include <vulkan/vulkan.h>

#include <cstdint>

// Automatic focus's patches (DepthEstimator), or MaskProbe's or
// KeyPicker's, from the decoded frames mpv hands out (Frame,
// docs/vulkan-renderer.md 11), which stay out of GL: an
// import there kept a decoder buffer after its frames were gone, and with it
// room the decoder's next session needed. Drawn by Vulkan with the GL
// program's lookups, from the frame's two planes, a Q10C frame's TP10
// descriptors or a Q08C frame's one image, into an R32F image copied to host
// memory. Submitted on its
// own after the frame's (submit()), so the eye passes never wait for it.
// Main thread.
class DirectDepth : public DepthPatchDrawer {
 public:
  DirectDepth() = default;
  DirectDepth(const DirectDepth&) = delete;
  DirectDepth& operator=(const DirectDepth&) = delete;
  ~DirectDepth() override;

  // False (and nothing drawn) unless direct is available. Q10C and Q08C
  // frames need direct to sample them (acceptsQ10c(), acceptsQ08c()) by then.
  // Patches up to width x rows (with their thumbnail) fit.
  bool init(GpuContext& gpu, DirectVideo& direct, int width = DepthEstimator::kMaxPatchWidth,
            int rows = DepthEstimator::kMaxPatchRows);
  // The drawer for automatic focus to use with frame (the shown one), or
  // nullptr if there is none or it cannot be drawn.
  DepthPatchDrawer* source(DirectFrame* frame);
  // Records and submits the patch draw() queued, if any.
  void submit();

  bool draw(const DepthPatch& patch) override;
  bool ready() override;
  const float* values() override;

 private:
  GpuContext* gpu_ = nullptr;
  DirectVideo* direct_ = nullptr;
  VkRenderPass pass_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;              // the planes'
  VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;  // the planes, pushed per draw
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkPipelineLayout tp10Layout_ = VK_NULL_HANDLE;  // Q10C frames (VK_NULL_HANDLE without)
  VkPipeline tp10Pipeline_ = VK_NULL_HANDLE;
  // Q08C frames, for each chroma siting (VK_NULL_HANDLE without).
  VkPipelineLayout q08cLayout_[kQ08cSitings] = {};
  VkPipeline q08cPipeline_[kQ08cSitings] = {};
  VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
  GpuImage image_;
  VkBuffer buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory memory_ = VK_NULL_HANDLE;
  bool coherent_ = false;
  float* mapped_ = nullptr;
  DirectFrame* source_ = nullptr;  // the frame source() was last asked for
  DirectFrame* frame_ = nullptr;   // the queued draw's, pinned until submitted
  DepthPatch patch_;
  uint32_t width_ = 0, rows_ = 0;  // the image's
  uint64_t done_ = 0;  // the frame timeline value of the submitted draw (0: none)
};
