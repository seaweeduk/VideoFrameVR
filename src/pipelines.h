#pragma once

#include "gpu_context.h"
#include "texture.h"
#include "video_format.h"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

// The scene's pipelines, made once at startup against the eye render pass,
// whatever the settings (docs/vulkan-renderer.md, 2.5), with their one
// descriptor set layout (two combined image samplers, pushed per draw: the
// draw's texture and the glass backdrop), pipeline layout (that set and 256
// bytes of push constants) and samplers (2.6). The colour key's pipelines
// also read its mask (KeyMask) as set 1, through layouts of their own.
class Pipelines {
 public:
  // kSurround... : surround.frag's general pipelines (the common modes have
  // their own, surround()); kSurroundMask with the packed mask looked up
  // first, for packed alpha passthrough (...RedBlue: its blue second layer
  // too, MaskColours); kSurroundColourKey with a flat background's colour
  // key, the only ones with its code but keyed(); the ...Sharpen ones: the
  // video shaders with sharpening, for a strength above 0 (videoPipeline()).
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
    kSurroundMaskRedBlue,
    kSurroundMaskRedBlueSharpen,
    kSurroundColourKey,
    kSurroundColourKeySharpen,
    kCount
  };
  // The video's pipeline: the surround or the flat screen, with the mask
  // first (read with these colours) or the colour key or neither (surround
  // only), sharpened or not.
  static Id videoPipeline(bool screen, bool maskFirst, MaskColours colours, bool colourKey,
                          bool sharpen) {
    if (screen) return sharpen ? kScreenSharpen : kScreen;
    if (colourKey) return sharpen ? kSurroundColourKeySharpen : kSurroundColourKey;
    if (maskFirst && colours == MaskColours::RedBlue)
      return sharpen ? kSurroundMaskRedBlueSharpen : kSurroundMaskRedBlue;
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
  // RGBA, YUV planes, a Q10C frame, a Q08C one (kYcbcr + its chroma siting:
  // each siting's sampler is in its own set layout).
  enum SurroundSource { kImage, kPlanes, kTp10, kYcbcr, kSourceCount = kYcbcr + kQ08cSitings };
  // No packed mask or green screen; the packed mask, with transparent output
  // only; a green screen without shrink; the packed mask read in its other
  // colours (MaskColours), fisheye only (where a packed mask comes); a green
  // screen with shrink, but for cubemaps and the tone look. The shader's
  // values (4 is the colour key's build).
  enum SurroundCut {
    kUncut,
    kPackedMask,
    kGreenScreen,
    kPackedRedBlue,
    kGreenScreenShrink = 5,
    kCutCount
  };
  // The picture as it is; the colour balance; the tone look, then the colour
  // balance.
  enum SurroundFinish { kAsItIs, kBalanced, kLookBalanced, kFinishCount };
  static constexpr uint32_t kMaxConstants = 7;
  // A pipeline's shaders (embedded SPIR-V) and fixed state.
  struct Desc {
    const uint32_t* vertex;
    size_t vertexBytes;
    const uint32_t* fragment;
    size_t fragmentBytes;
    VkPrimitiveTopology topology;
    bool blend;  // premultiplied alpha, colour and alpha alike
    const int32_t* constants = nullptr;  // the fragment shader's specialization constants 0.., at
                                         // most kMaxConstants
    uint32_t constantCount = 0;
  };

  Pipelines() = default;
  Pipelines(const Pipelines&) = delete;
  Pipelines& operator=(const Pipelines&) = delete;
  // Retires everything through GpuContext.
  ~Pipelines();

  // eyePass and plainEyePass: the eye render passes with a density map and
  // without (EyeTargets); the same pass on eye images without maps.
  // distancePass, reachPass: the colour key's mask passes (KeyMask).
  bool init(GpuContext& gpu, VkRenderPass eyePass, VkRenderPass plainEyePass,
            VkRenderPass distancePass, VkRenderPass reachPass);
  // The desktop's glass backdrop (glass_backdrop.h): the scene's pipelines
  // again for its float images, and its blur.
  bool initGlass(VkRenderPass glassPass);
  // The video's pipelines for Q10C decoder frames (Frame, DirectVideo): the
  // unsharpened surround (its common modes too), its mask-first versions
  // (one for each MaskColours), its colour key and the screen, built for
  // TP10 row bands, with their own layout (setLayout, a descriptor buffer's,
  // and the same push constants; maskSetLayout the colour key's mask's set
  // in the same buffer, DirectVideo::bindTp10).
  bool initTp10(VkDescriptorSetLayout setLayout, VkDescriptorSetLayout maskSetLayout);
  VkPipeline tp10(bool screen, bool maskFirst, MaskColours colours, bool colourKey,
                  bool plain) const {
    const int i = directPipeline(screen, maskFirst, colours, colourKey);
    return plain && tp10Plain_[i] ? tp10Plain_[i] : tp10_[i];
  }
  VkPipelineLayout tp10Layout() const { return tp10Layout_; }
  VkPipelineLayout tp10KeyLayout() const { return tp10KeyLayout_; }
  // The same for Q08C decoder frames (DirectVideo), sampled through a Y'CbCr
  // sampler: a set for each chroma siting, whose immutable sampler is in its
  // set layout (setLayouts, kQ08cSitings push sets'; binding 1 unused).
  bool initQ08c(const VkDescriptorSetLayout* setLayouts);
  VkPipeline q08c(int siting, bool screen, bool maskFirst, MaskColours colours, bool colourKey,
                  bool plain) const {
    const int i = directPipeline(screen, maskFirst, colours, colourKey);
    return plain && q08cPlain_[siting][i] ? q08cPlain_[siting][i] : q08c_[siting][i];
  }
  // Pushes a Q08C frame's image for the pipelines of its siting.
  void bindQ08c(VkCommandBuffer cmd, int siting, const Texture& frame) const;
  // Binds the colour key's mask (KeyMask::set) as set 1 for a colour-keyed
  // pipeline of the scene's layout, or of a Q08C siting's (-1: the scene's).
  void bindKeyMask(VkCommandBuffer cmd, int q08cSiting, VkDescriptorSet set) const;
  // A common mode's pipeline, with this finish.
  VkPipeline surround(SurroundProjection projection, SurroundSource source, SurroundCut cut,
                      bool plain, SurroundFinish finish) const {
    const VkPipeline p = surround_[finish][1][projection][source][cut];
    return plain && p ? p : surround_[finish][0][projection][source][cut];
  }
  // The colour key's common pipeline for the eye pass with its density map
  // (180, 360 or fisheye, the colour balance always applied, without or with
  // the tone look: none when plain needs another); keyFirst: for a mask that
  // holds a shrink's reach, read before the picture.
  VkPipeline keyed(SurroundSource source, bool plain, bool look, bool keyFirst) const {
    return plain && plainEyePass_ != eyePass_ ? VK_NULL_HANDLE : keyed_[keyFirst][look][source];
  }
  // The colour key's mask passes (KeyMask): the distances from a frame of
  // this source (its own layout: the scene's, tp10Layout() or a Q08C
  // siting's), and the mask from those (the scene's layout).
  VkPipeline keyDistance(SurroundSource source) const { return keyDistance_[source]; }
  VkPipeline keyReach() const { return keyReach_; }
  VkDescriptorSetLayout keyMaskSetLayout() const { return keyMaskSetLayout_; }
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

  // Pushes a draw's textures: binding 0 (the dummy if none) and the glass
  // backdrop at binding 1 (the dummy if none).
  void bindTextures(VkCommandBuffer cmd, const Texture* texture, SamplerId sampler,
                    const Texture* glass = nullptr) const;

 private:
  bool createSamplers();
  VkShaderModule module(const uint32_t* code, size_t bytes, const char* what);
  // create() with these modules for desc's code (which it doesn't read).
  VkPipeline create(const Desc& desc, VkShaderModule vertex, VkShaderModule fragment,
                    VkRenderPass pass, const char* what, VkPipelineLayout layout);
  // One pipeline of a batch (createAll), with its own copy of desc's
  // constants; layout VK_NULL_HANDLE for the scene's.
  struct Job {
    Desc desc;
    int32_t constants[kMaxConstants];
    VkRenderPass pass;
    VkPipelineLayout layout;
    const char* what;
    VkPipeline* pipeline;
  };
  static void add(std::vector<Job>& jobs, const Desc& desc, VkRenderPass pass, const char* what,
                  VkPipeline* pipeline, VkPipelineLayout layout = VK_NULL_HANDLE);
  // Adds the common modes' surround pipelines of these sources (all of one
  // shader and layout) for the eye passes, each with every finish.
  void addSurround(std::vector<Job>& jobs, std::initializer_list<SurroundSource> sources,
                   const uint32_t* fragment, size_t fragmentBytes, VkPipelineLayout layout);
  // Creates the jobs' pipelines on several threads at once; false if any
  // failed (those made are kept, for the destructor).
  bool createAll(const std::vector<Job>& jobs);
  bool createDummy();
  // The layout of the scene's pipeline id: the colour key's reads its mask.
  VkPipelineLayout layoutOf(Id id) const {
    return id == kSurroundColourKey || id == kSurroundColourKeySharpen ? keyLayout_ : layout_;
  }
  // A decoder frame's general pipelines (tp10(), q08c()): the surround, its
  // mask-first version, the screen, then the mask-first versions for the
  // mask's other colours, and the surround with the colour key.
  enum {
    kDirectSurround,
    kDirectMask,
    kDirectScreen,
    kDirectMaskRedBlue,
    kDirectColourKey,
    kDirectCount
  };
  static int directPipeline(bool screen, bool maskFirst, MaskColours colours, bool colourKey) {
    if (screen) return kDirectScreen;
    if (colourKey) return kDirectColourKey;
    if (!maskFirst) return kDirectSurround;
    return colours == MaskColours::RedBlue ? kDirectMaskRedBlue : kDirectMask;
  }
  // Makes decoder frames' own layout (setLayout and the same push constants)
  // and adds their general pipelines (kDirect...).
  // The colour key's has keyLayout (with maskSetLayout as set 1).
  bool addDirect(std::vector<Job>& jobs, VkDescriptorSetLayout setLayout,
                 VkDescriptorSetLayout maskSetLayout, const Desc (&descs)[kDirectCount],
                 const char* const (&names)[kDirectCount], VkPipelineLayout& layout,
                 VkPipelineLayout& keyLayout, VkPipeline (&pipelines)[kDirectCount],
                 VkPipeline (&plain)[kDirectCount]);
  // A layout of these set layouts and the scene's push constants.
  bool createLayout(std::initializer_list<VkDescriptorSetLayout> sets, VkPipelineLayout& layout,
                    const char* what);
  // Adds the colour key's common pipelines and distance pass for a source
  // (keyed(), keyDistance()).
  void addKeyed(std::vector<Job>& jobs, SurroundSource source, const uint32_t* fragment,
                size_t fragmentBytes, const uint32_t* distance, size_t distanceBytes,
                VkPipelineLayout keyLayout, VkPipelineLayout layout);

  GpuContext* gpu_ = nullptr;
  VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  // The colour key's mask's set, and the layout with it as set 1.
  VkDescriptorSetLayout keyMaskSetLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout keyLayout_ = VK_NULL_HANDLE;
  Desc descs_[kCount] = {};
  VkPipeline pipelines_[kCount] = {};
  VkPipeline plain_[kCount] = {};  // for the plain eye pass, where it is another
  VkPipeline glass_[kCount] = {};
  VkPipeline blur_ = VK_NULL_HANDLE;
  VkPipeline foveation_[2] = {};
  VkRenderPass eyePass_ = VK_NULL_HANDLE, plainEyePass_ = VK_NULL_HANDLE;
  VkRenderPass distancePass_ = VK_NULL_HANDLE;
  VkPipelineLayout tp10Layout_ = VK_NULL_HANDLE, tp10KeyLayout_ = VK_NULL_HANDLE;
  VkPipeline tp10_[kDirectCount] = {}, tp10Plain_[kDirectCount] = {};
  VkPipelineLayout q08cLayout_[kQ08cSitings] = {}, q08cKeyLayout_[kQ08cSitings] = {};
  VkPipeline q08c_[kQ08cSitings][kDirectCount] = {}, q08cPlain_[kQ08cSitings][kDirectCount] = {};
  // Per finish; for the eye pass and the plain one.
  VkPipeline surround_[kFinishCount][2][kProjectionCount][kSourceCount][kCutCount] = {};
  // Mask read after or before the picture; without and with the tone look.
  VkPipeline keyed_[2][2][kSourceCount] = {};
  VkPipeline keyDistance_[kSourceCount] = {}, keyReach_ = VK_NULL_HANDLE;
  VkSampler samplers_[kSamplerCount] = {};
  GpuImage dummyImage_;
  Texture dummy_;
};
