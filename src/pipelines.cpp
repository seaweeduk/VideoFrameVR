#include "pipelines.h"

#include "scene_renderer.h"

#include <cstdint>
// The SPIR-V headers declare bare arrays: <cstdint> first.
#include "shaders/blur.frag.h"
#include "shaders/fullscreen.vert.h"
#include "shaders/foveation.frag.h"
#include "shaders/panel.frag.h"
#include "shaders/panel.vert.h"
#include "shaders/pointer.frag.h"
#include "shaders/pointer.vert.h"
#include "shaders/preview.frag.h"
#include "shaders/preview.vert.h"
#include "shaders/screen.frag.h"
#include "shaders/screen.vert.h"
#include "shaders/screen_sharpen.frag.h"
#include "shaders/screen_tp10.frag.h"
#include "shaders/surround.frag.h"
#include "shaders/surround_tp10.frag.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <system_error>
#include <thread>
#include <vector>

namespace {

// surround.frag's pipeline constants (projection, source, cut, sharpening)
// for its general pipelines: -1, from the draw.
constexpr int32_t kMaskFirst[] = {-1, -1, 1, 0};
constexpr int32_t kSharpened[] = {-1, -1, -1, 1};
constexpr int32_t kMaskFirstSharpened[] = {-1, -1, 1, 1};
// ... with the mask's other colours (Pipelines::SurroundCut's values).
constexpr int32_t kRedBlueFirst[] = {-1, -1, Pipelines::kPackedRedBlue, 0};
constexpr int32_t kRedBlueFirstSharpened[] = {-1, -1, Pipelines::kPackedRedBlue, 1};
// The general pipelines for a Q08C frame (source 2; screen.frag reads the
// source alone).
constexpr int32_t kYcbcrGeneral[] = {-1, 2, -1, 0};
constexpr int32_t kYcbcrMaskFirst[] = {-1, 2, 1, 0};
constexpr int32_t kYcbcrRedBlueFirst[] = {-1, 2, Pipelines::kPackedRedBlue, 0};

// A common mode's source as the shader's constant (video.glsl's kSource...).
int32_t sourceConstant(Pipelines::SurroundSource source) {
  switch (source) {
    case Pipelines::kImage: return 0;
    case Pipelines::kPlanes: return 1;
    case Pipelines::kTp10: return 1;  // its own build of the shader, which ignores it
    default: return 2;                // a Y'CbCr image, of any chroma siting
  }
}

}  // namespace

Pipelines::~Pipelines() {
  if (!gpu_) return;
  VkDevice d = gpu_->device();
  std::vector<VkPipeline> pipelines(std::begin(pipelines_), std::end(pipelines_));
  pipelines.insert(pipelines.end(), std::begin(plain_), std::end(plain_));
  pipelines.insert(pipelines.end(), std::begin(glass_), std::end(glass_));
  pipelines.push_back(blur_);
  pipelines.insert(pipelines.end(), std::begin(foveation_), std::end(foveation_));
  pipelines.insert(pipelines.end(), std::begin(tp10_), std::end(tp10_));
  pipelines.insert(pipelines.end(), std::begin(tp10Plain_), std::end(tp10Plain_));
  std::vector<VkPipelineLayout> layouts = {layout_, tp10Layout_};
  for (int s = 0; s < kQ08cSitings; ++s) {
    pipelines.insert(pipelines.end(), std::begin(q08c_[s]), std::end(q08c_[s]));
    pipelines.insert(pipelines.end(), std::begin(q08cPlain_[s]), std::end(q08cPlain_[s]));
    layouts.push_back(q08cLayout_[s]);
  }
  for (const auto& balance : surround_)
    for (const auto& pass : balance)
      for (const auto& projection : pass)
        for (const auto& cuts : projection)
          pipelines.insert(pipelines.end(), std::begin(cuts), std::end(cuts));
  std::vector<VkSampler> samplers(std::begin(samplers_), std::end(samplers_));
  VkDescriptorSetLayout setLayout = setLayout_;
  gpu_->retire([d, pipelines, samplers, layouts, setLayout] {
    for (VkPipeline p : pipelines)
      if (p) vkDestroyPipeline(d, p, nullptr);
    for (VkSampler s : samplers)
      if (s) vkDestroySampler(d, s, nullptr);
    for (VkPipelineLayout l : layouts)
      if (l) vkDestroyPipelineLayout(d, l, nullptr);
    if (setLayout) vkDestroyDescriptorSetLayout(d, setLayout, nullptr);
  });
  gpu_->retire(dummyImage_);
}

bool Pipelines::init(GpuContext& gpu, VkRenderPass eyePass, VkRenderPass plainEyePass) {
  gpu_ = &gpu;
  eyePass_ = eyePass;
  plainEyePass_ = plainEyePass;
  VkDevice d = gpu.device();
  VkDescriptorSetLayoutBinding bindings[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  setInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
  setInfo.bindingCount = 2;
  setInfo.pBindings = bindings;
  if (!gpu.check(vkCreateDescriptorSetLayout(d, &setInfo, nullptr, &setLayout_), "set layout"))
    return false;
  VkPushConstantRange push{};
  push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  push.offset = 0;
  push.size = kPushConstantBytes;
  VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layoutInfo.setLayoutCount = 1;
  layoutInfo.pSetLayouts = &setLayout_;
  layoutInfo.pushConstantRangeCount = 1;
  layoutInfo.pPushConstantRanges = &push;
  if (!gpu.check(vkCreatePipelineLayout(d, &layoutInfo, nullptr, &layout_), "pipeline layout"))
    return false;
  if (!createSamplers() || !createDummy()) return false;

  const Desc descs[kCount] = {
      {kSpirv_fullscreen_vert, sizeof(kSpirv_fullscreen_vert), kSpirv_surround_frag,
       sizeof(kSpirv_surround_frag), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false},
      {kSpirv_screen_vert, sizeof(kSpirv_screen_vert), kSpirv_screen_frag,
       sizeof(kSpirv_screen_frag), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, false},
      {kSpirv_panel_vert, sizeof(kSpirv_panel_vert), kSpirv_panel_frag, sizeof(kSpirv_panel_frag),
       VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, true},
      {kSpirv_pointer_vert, sizeof(kSpirv_pointer_vert), kSpirv_pointer_frag,
       sizeof(kSpirv_pointer_frag), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, true},
      {kSpirv_preview_vert, sizeof(kSpirv_preview_vert), kSpirv_preview_frag,
       sizeof(kSpirv_preview_frag), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, true},
      {kSpirv_fullscreen_vert, sizeof(kSpirv_fullscreen_vert), kSpirv_surround_frag,
       sizeof(kSpirv_surround_frag), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, kMaskFirst, 4},
      {kSpirv_fullscreen_vert, sizeof(kSpirv_fullscreen_vert), kSpirv_surround_frag,
       sizeof(kSpirv_surround_frag), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, kSharpened, 4},
      {kSpirv_fullscreen_vert, sizeof(kSpirv_fullscreen_vert), kSpirv_surround_frag,
       sizeof(kSpirv_surround_frag), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, kMaskFirstSharpened,
       4},
      {kSpirv_screen_vert, sizeof(kSpirv_screen_vert), kSpirv_screen_sharpen_frag,
       sizeof(kSpirv_screen_sharpen_frag), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, false},
      {kSpirv_fullscreen_vert, sizeof(kSpirv_fullscreen_vert), kSpirv_surround_frag,
       sizeof(kSpirv_surround_frag), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, kRedBlueFirst, 4},
      {kSpirv_fullscreen_vert, sizeof(kSpirv_fullscreen_vert), kSpirv_surround_frag,
       sizeof(kSpirv_surround_frag), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false,
       kRedBlueFirstSharpened, 4},
  };
  static const char* names[kCount] = {"surround",
                                      "screen",
                                      "panel",
                                      "pointer",
                                      "preview",
                                      "surround (mask first)",
                                      "surround (sharpened)",
                                      "surround (mask first, sharpened)",
                                      "screen (sharpened)",
                                      "surround (mask first, red and blue)",
                                      "surround (mask first, red and blue, sharpened)"};
  std::vector<Job> jobs;
  for (int i = 0; i < kCount; ++i) {
    descs_[i] = descs[i];
    add(jobs, descs[i], eyePass, names[i], &pipelines_[i]);
    // A pass with a density map is not compatible with one without.
    if (plainEyePass != eyePass) add(jobs, descs[i], plainEyePass, names[i], &plain_[i]);
  }
  addSurround(jobs, {kImage, kPlanes}, kSpirv_surround_frag, sizeof(kSpirv_surround_frag), layout_);
  if (gpu.caps().foveation) {
    const Desc debug{kSpirv_fullscreen_vert, sizeof(kSpirv_fullscreen_vert), kSpirv_foveation_frag,
                     sizeof(kSpirv_foveation_frag), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true};
    add(jobs, debug, eyePass, "foveation view", &foveation_[0]);
    add(jobs, debug, plainEyePass, "foveation view (off)", &foveation_[1]);
  }
  return createAll(jobs);
}

bool Pipelines::initGlass(VkRenderPass glassPass) {
  std::vector<Job> jobs;
  for (int i = 0; i < kCount; ++i)
    add(jobs, descs_[i], glassPass, "glass backdrop pipeline", &glass_[i]);
  add(jobs,
      {kSpirv_fullscreen_vert, sizeof(kSpirv_fullscreen_vert), kSpirv_blur_frag,
       sizeof(kSpirv_blur_frag), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false},
      glassPass, "glass blur pipeline", &blur_);
  return createAll(jobs);
}

bool Pipelines::initTp10(VkDescriptorSetLayout setLayout) {
  const Desc descs[kDirectCount] = {
      {kSpirv_fullscreen_vert, sizeof(kSpirv_fullscreen_vert), kSpirv_surround_tp10_frag,
       sizeof(kSpirv_surround_tp10_frag), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false},
      {kSpirv_fullscreen_vert, sizeof(kSpirv_fullscreen_vert), kSpirv_surround_tp10_frag,
       sizeof(kSpirv_surround_tp10_frag), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, kMaskFirst, 4},
      {kSpirv_screen_vert, sizeof(kSpirv_screen_vert), kSpirv_screen_tp10_frag,
       sizeof(kSpirv_screen_tp10_frag), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, false},
      {kSpirv_fullscreen_vert, sizeof(kSpirv_fullscreen_vert), kSpirv_surround_tp10_frag,
       sizeof(kSpirv_surround_tp10_frag), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, kRedBlueFirst,
       4},
  };
  static const char* names[kDirectCount] = {
      "surround (TP10)", "surround (mask first, TP10)", "screen (TP10)",
      "surround (mask first, red and blue, TP10)"};
  std::vector<Job> jobs;
  if (!addDirect(jobs, setLayout, descs, names, tp10Layout_, tp10_, tp10Plain_)) return false;
  addSurround(jobs, {kTp10}, kSpirv_surround_tp10_frag, sizeof(kSpirv_surround_tp10_frag),
              tp10Layout_);
  return createAll(jobs);
}

bool Pipelines::initQ08c(const VkDescriptorSetLayout* setLayouts) {
  const Desc descs[kDirectCount] = {
      {kSpirv_fullscreen_vert, sizeof(kSpirv_fullscreen_vert), kSpirv_surround_frag,
       sizeof(kSpirv_surround_frag), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, kYcbcrGeneral, 4},
      {kSpirv_fullscreen_vert, sizeof(kSpirv_fullscreen_vert), kSpirv_surround_frag,
       sizeof(kSpirv_surround_frag), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, kYcbcrMaskFirst, 4},
      {kSpirv_screen_vert, sizeof(kSpirv_screen_vert), kSpirv_screen_frag,
       sizeof(kSpirv_screen_frag), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, false, kYcbcrGeneral, 4},
      {kSpirv_fullscreen_vert, sizeof(kSpirv_fullscreen_vert), kSpirv_surround_frag,
       sizeof(kSpirv_surround_frag), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, kYcbcrRedBlueFirst,
       4},
  };
  static const char* names[kDirectCount] = {
      "surround (Q08C)", "surround (mask first, Q08C)", "screen (Q08C)",
      "surround (mask first, red and blue, Q08C)"};
  // All sitings in one batch.
  std::vector<Job> jobs;
  for (int s = 0; s < kQ08cSitings; ++s) {
    if (!addDirect(jobs, setLayouts[s], descs, names, q08cLayout_[s], q08c_[s], q08cPlain_[s]))
      return false;
    addSurround(jobs, {static_cast<SurroundSource>(kYcbcr + s)}, kSpirv_surround_frag,
                sizeof(kSpirv_surround_frag), q08cLayout_[s]);
  }
  return createAll(jobs);
}

bool Pipelines::addDirect(std::vector<Job>& jobs, VkDescriptorSetLayout setLayout,
                          const Desc (&descs)[kDirectCount],
                          const char* const (&names)[kDirectCount], VkPipelineLayout& layout,
                          VkPipeline (&pipelines)[kDirectCount],
                          VkPipeline (&plain)[kDirectCount]) {
  VkPushConstantRange push{};
  push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  push.size = kPushConstantBytes;
  VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layoutInfo.setLayoutCount = 1;
  layoutInfo.pSetLayouts = &setLayout;
  layoutInfo.pushConstantRangeCount = 1;
  layoutInfo.pPushConstantRanges = &push;
  if (!gpu_->check(vkCreatePipelineLayout(gpu_->device(), &layoutInfo, nullptr, &layout),
                   "decoder frames' pipeline layout"))
    return false;
  for (int i = 0; i < kDirectCount; ++i) {
    add(jobs, descs[i], eyePass_, names[i], &pipelines[i], layout);
    if (plainEyePass_ != eyePass_) add(jobs, descs[i], plainEyePass_, names[i], &plain[i], layout);
  }
  return true;
}

void Pipelines::add(std::vector<Job>& jobs, const Desc& desc, VkRenderPass pass, const char* what,
                    VkPipeline* pipeline, VkPipelineLayout layout) {
  Job job{desc, {}, pass, layout, what, pipeline};
  for (uint32_t i = 0; i < desc.constantCount; ++i) job.constants[i] = desc.constants[i];
  job.desc.constants = nullptr;  // createAll() points it at job.constants
  jobs.push_back(job);
}

void Pipelines::addSurround(std::vector<Job>& jobs, std::initializer_list<SurroundSource> sources,
                            const uint32_t* fragment, size_t fragmentBytes,
                            VkPipelineLayout layout) {
  for (SurroundSource source : sources)
    for (int balance = 0; balance < 2; ++balance)
      for (int p = 0; p < kProjectionCount; ++p)
        for (int c = 0; c < kCutCount; ++c)
          for (int plain = 0; plain < (plainEyePass_ == eyePass_ ? 1 : 2); ++plain) {
            if (c >= kPackedRedBlue && p != kFisheye) continue;
            const int32_t constants[] = {p, sourceConstant(source), c, 0, balance};
            add(jobs,
                {kSpirv_fullscreen_vert, sizeof(kSpirv_fullscreen_vert), fragment, fragmentBytes,
                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, constants, 5},
                plain ? plainEyePass_ : eyePass_, "surround (common mode)",
                &surround_[balance][plain][p][source][c], layout);
          }
}

// With nothing cached yet (after an update) a batch's compiles add up to
// seconds on the Frame. Its threads: one per core but one, which stays with
// the runtime (SteamVR shows its loading view meanwhile), at most 8: past that
// the longest single compiles (the general surround pipelines) set a batch's
// length, and each thread holds a compiler's memory.
bool Pipelines::createAll(const std::vector<Job>& jobs) {
  // Each shader's module, made once for the batch: most jobs share theirs.
  // Modules are only read while pipelines are made, on any thread.
  std::vector<std::pair<const uint32_t*, VkShaderModule>> modules;
  auto moduleFor = [&](const uint32_t* code, size_t bytes, const char* what) {
    for (const auto& m : modules)
      if (m.first == code) return m.second;
    modules.emplace_back(code, module(code, bytes, what));
    return modules.back().second;
  };
  std::vector<std::pair<VkShaderModule, VkShaderModule>> stages;  // vertex, fragment per job
  bool ok = true;
  for (const Job& job : jobs) {
    stages.emplace_back(moduleFor(job.desc.vertex, job.desc.vertexBytes, job.what),
                        VK_NULL_HANDLE);
    if (stages.back().first)
      stages.back().second = moduleFor(job.desc.fragment, job.desc.fragmentBytes, job.what);
    if (!stages.back().second) {
      ok = false;
      break;
    }
  }
  if (ok) {
    // Each job writes only its own pipeline; the pipeline cache synchronizes
    // itself (GpuContext makes it without EXTERNALLY_SYNCHRONIZED).
    std::atomic<size_t> next{0};
    auto work = [&] {
      for (size_t i; (i = next++) < jobs.size();) {
        Desc desc = jobs[i].desc;
        desc.constants = jobs[i].constants;
        *jobs[i].pipeline = create(desc, stages[i].first, stages[i].second, jobs[i].pass,
                                   jobs[i].what, jobs[i].layout);
      }
    };
    // This thread works the queue too; a worker that can't start leaves its
    // share to the others.
    const unsigned cores = std::thread::hardware_concurrency();  // 0: unknown
    const size_t threads =
        std::min<size_t>(cores ? std::clamp(cores, 2u, 9u) - 1 : 4, jobs.size());
    std::vector<std::thread> workers;
    workers.reserve(threads ? threads - 1 : 0);
    try {
      while (workers.size() + 1 < threads) workers.emplace_back(work);
    } catch (const std::system_error&) {
    }
    work();
    for (std::thread& t : workers) t.join();
  }
  for (const auto& m : modules) vkDestroyShaderModule(gpu_->device(), m.second, nullptr);
  if (!ok) return false;
  for (const Job& job : jobs)
    if (!*job.pipeline) return false;
  return true;
}

VkShaderModule Pipelines::module(const uint32_t* code, size_t bytes, const char* what) {
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = bytes;
  info.pCode = code;
  VkShaderModule m = VK_NULL_HANDLE;
  gpu_->check(vkCreateShaderModule(gpu_->device(), &info, nullptr, &m), what);
  return m;
}

VkPipeline Pipelines::create(const Desc& desc, VkRenderPass pass, const char* what,
                             VkPipelineLayout layout) {
  const VkShaderModule vertex = module(desc.vertex, desc.vertexBytes, what);
  const VkShaderModule fragment =
      vertex ? module(desc.fragment, desc.fragmentBytes, what) : VK_NULL_HANDLE;
  const VkPipeline pipeline =
      fragment ? create(desc, vertex, fragment, pass, what, layout) : VK_NULL_HANDLE;
  vkDestroyShaderModule(gpu_->device(), vertex, nullptr);
  vkDestroyShaderModule(gpu_->device(), fragment, nullptr);
  return pipeline;
}

VkPipeline Pipelines::create(const Desc& desc, VkShaderModule vertex, VkShaderModule fragment,
                             VkRenderPass pass, const char* what, VkPipelineLayout layout) {
  VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                               {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vertex;
  stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fragment;
  stages[1].pName = "main";
  VkSpecializationMapEntry entries[kMaxConstants] = {};
  for (uint32_t i = 0; i < desc.constantCount; ++i)
    entries[i] = {i, i * static_cast<uint32_t>(sizeof(int32_t)), sizeof(int32_t)};
  const VkSpecializationInfo specialization{desc.constantCount, entries,
                                            desc.constantCount * sizeof(int32_t), desc.constants};
  if (desc.constantCount) stages[1].pSpecializationInfo = &specialization;

  // Vertices come from gl_VertexIndex alone.
  VkPipelineVertexInputStateCreateInfo vertexInput{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = desc.topology;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  // Layering is drawing order; nothing is culled (the y flip turns windings).
  VkPipelineRasterizationStateCreateInfo raster{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState attachment{};
  attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  if (desc.blend) {
    attachment.blendEnable = VK_TRUE;
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.colorBlendOp = VK_BLEND_OP_ADD;
    attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.alphaBlendOp = VK_BLEND_OP_ADD;
  }
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &attachment;
  const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicInfo{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamicInfo.dynamicStateCount = 2;
  dynamicInfo.pDynamicStates = dynamic;

  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vertexInput;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamicInfo;
  info.layout = layout ? layout : layout_;
  info.flags = layout == tp10Layout_ && layout ? VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT : 0;
  info.renderPass = pass;
  info.subpass = 0;
  VkPipeline pipeline = VK_NULL_HANDLE;
  gpu_->check(vkCreateGraphicsPipelines(gpu_->device(), gpu_->pipelineCache(), 1, &info, nullptr,
                                        &pipeline),
              what);
  return pipeline;
}

bool Pipelines::createSamplers() {
  const GpuCapabilities& caps = gpu_->caps();
  auto make = [&](SamplerId id, float anisotropy, float maxLod, VkSamplerAddressMode u,
                  const char* what) {
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = u;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    const float most = std::min(anisotropy, caps.maxAnisotropy);
    info.anisotropyEnable = caps.samplerAnisotropy && most > 1.0f ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = info.anisotropyEnable ? most : 1.0f;
    info.minLod = 0.0f;
    info.maxLod = maxLod;
    return gpu_->check(vkCreateSampler(gpu_->device(), &info, nullptr, &samplers_[id]), what);
  };
  const VkSamplerAddressMode clamp = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  return make(kVideoTrilinear, 16.0f, VK_LOD_CLAMP_NONE, clamp, "video sampler") &&
         make(kVideoBase, 1.0f, 0.0f, clamp, "video base level sampler") &&
         make(kCanvas, 8.0f, VK_LOD_CLAMP_NONE, clamp, "canvas sampler") &&
         make(kSky, 1.0f, VK_LOD_CLAMP_NONE, VK_SAMPLER_ADDRESS_MODE_REPEAT, "sky sampler") &&
         make(kGlass, 1.0f, 0.0f, clamp, "glass sampler");
}

bool Pipelines::createDummy() {
  if (!gpu_->createImage(1, 1, 1, VK_FORMAT_R8G8B8A8_SRGB,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, dummyImage_,
                         "placeholder texture"))
    return false;
  VkCommandBuffer cmd = gpu_->beginOneShot();
  if (!cmd) return false;
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = 0;
  b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = dummyImage_.image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &b);
  const VkClearColorValue clear{{0.0f, 0.0f, 0.0f, 0.0f}};
  vkCmdClearColorImage(cmd, dummyImage_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                       &b.subresourceRange);
  b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &b);
  if (gpu_->submitOneShot(cmd) == 0) return false;
  dummy_.image = dummyImage_.image;
  dummy_.view = dummyImage_.view;
  dummy_.width = dummy_.height = 1;
  return true;
}

void Pipelines::bindTextures(VkCommandBuffer cmd, const Texture* texture, SamplerId sampler,
                             const Texture* glass) const {
  VkDescriptorImageInfo images[2] = {};
  images[0].sampler = samplers_[sampler];
  images[0].imageView = texture ? texture->view : dummy_.view;
  images[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  images[1].sampler = samplers_[kGlass];
  images[1].imageView = glass ? glass->view : dummy_.view;
  images[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet writes[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
  for (uint32_t i = 0; i < 2; ++i) {
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[i].pImageInfo = &images[i];
  }
  gpu_->cmdPushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, writes);
}

void Pipelines::bindQ08c(VkCommandBuffer cmd, int siting, const Texture& frame) const {
  const VkDescriptorImageInfo images[2] = {
      {VK_NULL_HANDLE, frame.view, VK_IMAGE_LAYOUT_GENERAL},  // the set layout's sampler
      {samplers_[kGlass], dummy_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
  VkWriteDescriptorSet writes[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
  for (uint32_t i = 0; i < 2; ++i) {
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[i].pImageInfo = &images[i];
  }
  gpu_->cmdPushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, q08cLayout_[siting], 0, 2,
                             writes);
}
