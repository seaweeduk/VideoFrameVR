#pragma once

#include "gpu_context.h"
#include "texture.h"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>

// The scene's pipelines, made once at startup against the eye render pass,
// whatever the settings (docs/vulkan-renderer.md, 2.5), with their one
// descriptor set layout (two combined image samplers, pushed per draw: the
// draw's texture and the glass backdrop), pipeline layout (that set and 256
// bytes of push constants) and samplers (2.6).
class Pipelines {
 public:
  // kSurround... : surround.frag's general pipelines (the common modes have
  // their own, surround()); kSurroundMask with the packed mask looked up
  // first, for packed alpha passthrough; the ...Sharpen ones: the video
  // shaders with sharpening, for a strength above 0 (videoPipeline()).
  enum Id {
    kSurround,
    kScreen,
    kPanel,
    kPointer,
    kPreview,
    kSurroundMask,
    kSurroundSharpen,
    kSurroundMaskSharpen,
    kScreenSharpen,
    kCount
  };
  // The video's pipeline: the surround or the flat screen, with the mask
  // first or not (surround only), sharpened or not.
  static Id videoPipeline(bool screen, bool maskFirst, bool sharpen) {
    if (screen) return sharpen ? kScreenSharpen : kScreen;
    if (maskFirst) return sharpen ? kSurroundMaskSharpen : kSurroundMask;
    return sharpen ? kSurroundSharpen : kSurround;
  }
  enum SamplerId {
    kVideoTrilinear,  // 16x anisotropy
    kVideoBase,       // the full-size level alone, bilinear (the shaders take several taps)
    kCanvas,          // 8x anisotropy
    kSky,             // repeats around
    kGlass,           // linear, clamped
    kSamplerCount
  };
  // surround.frag's common modes (its pipeline constants): a pipeline each,
  // whose per-pixel path has only that mode's code.
  enum SurroundProjection { kEquirect, kFisheye, kCubemap, kProjectionCount };  // 180 or 360; both grids
  enum SurroundSource { kImage, kPlanes, kTp10, kSourceCount };  // RGBA, YUV planes, a Q10C frame
  // No packed mask or green screen; the packed mask, with transparent output
  // only; a green screen without shrink.
  enum SurroundCut { kUncut, kPackedMask, kGreenScreen, kCutCount };
  // A pipeline's shaders (embedded SPIR-V) and fixed state.
  struct Desc {
    const uint32_t* vertex;
    size_t vertexBytes;
    const uint32_t* fragment;
    size_t fragmentBytes;
    VkPrimitiveTopology topology;
    bool blend;  // premultiplied alpha, colour and alpha alike
    const int32_t* constants = nullptr;  // the fragment shader's specialization constants 0..3
    uint32_t constantCount = 0;
  };

  Pipelines() = default;
  Pipelines(const Pipelines&) = delete;
  Pipelines& operator=(const Pipelines&) = delete;
  // Retires everything through GpuContext.
  ~Pipelines();

  // eyePass and plainEyePass: the eye render passes with a density map and
  // without (EyeTargets); the same pass on eye images without maps.
  bool init(GpuContext& gpu, VkRenderPass eyePass, VkRenderPass plainEyePass);
  // The desktop's glass backdrop (glass_backdrop.h): the scene's pipelines
  // again for its float images, and its blur.
  bool initGlass(VkRenderPass glassPass);
  // The video's pipelines for Q10C decoder frames (Frame, DirectVideo): the
  // unsharpened surround (its common modes too), its mask-first version and
  // the screen, built for TP10 row bands, with their own layout (setLayout, a
  // descriptor buffer's, and the same push constants).
  bool initTp10(VkDescriptorSetLayout setLayout);
  VkPipeline tp10(bool screen, bool maskFirst, bool plain) const {
    const int i = screen ? 2 : maskFirst ? 1 : 0;
    return plain && tp10Plain_[i] ? tp10Plain_[i] : tp10_[i];
  }
  VkPipelineLayout tp10Layout() const { return tp10Layout_; }
  VkPipeline surround(SurroundProjection projection, SurroundSource source, SurroundCut cut,
                      bool plain) const {
    const VkPipeline p = surround_[1][projection][source][cut];
    return plain && p ? p : surround_[0][projection][source][cut];
  }
  // A pipeline of the scene's layout (or another one) for another render
  // pass (the sky, glass); the caller retires it.
  VkPipeline create(const Desc& desc, VkRenderPass pass, const char* what,
                    VkPipelineLayout layout = VK_NULL_HANDLE);

  // For the eye pass with its density map, or the plain one.
  VkPipeline get(Id id, bool plain = false) const { return plain && plain_[id] ? plain_[id] : pipelines_[id]; }
  VkPipeline glass(Id id) const { return glass_[id]; }
  VkPipeline blur() const { return blur_; }
  VkPipeline foveation(bool plain) const { return foveation_[plain ? 1 : 0]; }
  VkPipelineLayout layout() const { return layout_; }
  VkSampler sampler(SamplerId id) const { return samplers_[id]; }
  // A 1x1 transparent image for bindings a draw does not use.
  const Texture& dummy() const { return dummy_; }

  // Pushes a draw's textures: binding 0 (the dummy if none) and the glass
  // backdrop at binding 1 (the dummy if none).
  void bindTextures(VkCommandBuffer cmd, const Texture* texture, SamplerId sampler,
                    const Texture* glass = nullptr) const;

 private:
  bool createSamplers();
  // The common modes' surround pipelines of one source for the eye passes.
  bool createSurround(SurroundSource source, const uint32_t* fragment, size_t fragmentBytes,
                      VkPipelineLayout layout);
  bool createDummy();

  GpuContext* gpu_ = nullptr;
  VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  Desc descs_[kCount] = {};
  VkPipeline pipelines_[kCount] = {};
  VkPipeline plain_[kCount] = {};  // for the plain eye pass, where it is another
  VkPipeline glass_[kCount] = {};
  VkPipeline blur_ = VK_NULL_HANDLE;
  VkPipeline foveation_[2] = {};
  VkRenderPass eyePass_ = VK_NULL_HANDLE, plainEyePass_ = VK_NULL_HANDLE;
  VkPipelineLayout tp10Layout_ = VK_NULL_HANDLE;
  VkPipeline tp10_[3] = {}, tp10Plain_[3] = {};
  VkPipeline surround_[2][kProjectionCount][kSourceCount][kCutCount] = {};  // eye pass, plain one
  VkSampler samplers_[kSamplerCount] = {};
  GpuImage dummyImage_;
  Texture dummy_;
};
