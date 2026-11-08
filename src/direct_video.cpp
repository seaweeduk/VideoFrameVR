#include "direct_video.h"

#include <cstdint>
// The SPIR-V headers declare bare arrays: <cstdint> first.
#include "shaders/fullscreen.vert.h"
#include "shaders/still_tp10.frag.h"

#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr uint32_t kFourccP010 = 0x30313050;  // DRM_FORMAT_P010
constexpr uint32_t kFourccNv12 = 0x3231564e;  // DRM_FORMAT_NV12
constexpr uint64_t kModifierQcomCompressed = 0x0500000000000001ull;  // Q10C with P010, Q08C with NV12
// Q10C frames: TP10 descriptors read wrongly past 2^24 samples. Read the
// planes in bands of rows, at most this many for luma (video.glsl's tp10Bands).
constexpr unsigned kTp10MaxSamples = 1u << 24;
constexpr unsigned kTp10Bands = 8;
constexpr VkFormat kTp10Carrier = VK_FORMAT_A2B10G10R10_UNORM_PACK32;  // 4 bytes, 3 samples
// A Q08C buffer's image: sampled, and copied from by still().
constexpr VkFormat kQ08cFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
constexpr VkImageUsageFlags kQ08cUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
// Imports kept for buffers no frame uses, for when mpv hands them out again
// (a closed decoder session's go at once).
constexpr double kKeepUnused = 5.0;  // seconds
constexpr size_t kMaxBuffers = 32;  // a decoder's own buffers: H.264 has 20
static_assert(kQ08cSitings == VIDEO_EXPORT_CHROMA_OTHER, "a Y'CbCr conversion per siting named");
// still.frag's push constants.
struct StillPush {
  float size[2];
  int32_t chroma;
};

double now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool signalled(int fd, int timeout = 0) {
  pollfd p{fd, POLLIN, 0};
  int result;
  do {
    result = poll(&p, 1, timeout);
  } while (result < 0 && errno == EINTR);
  return result > 0 && (p.revents & POLLIN) && !(p.revents & (POLLERR | POLLNVAL));
}

// a6xx/a7xx texture descriptor fields (Mesa's A6XX_TEX_MEMOBJ), as the
// converter (packaging/frame/mpv/dmabuf_ubwc.c) changes them.
uint32_t texGet(const uint32_t* d, unsigned word, unsigned shift, unsigned bits) {
  return (d[word] >> shift) & ((1u << bits) - 1);
}

void texSet(uint32_t* d, unsigned word, unsigned shift, unsigned bits, uint32_t v) {
  const uint32_t mask = ((1u << bits) - 1) << shift;
  d[word] = (d[word] & ~mask) | ((v << shift) & mask);
}

// Moves an address (a low word, then 17 bits of the next) on by add.
void texOffset(uint32_t* d, unsigned word, uint64_t add) {
  const uint64_t a = (d[word] | static_cast<uint64_t>(d[word + 1] & 0x1ffff) << 32) + add;
  d[word] = static_cast<uint32_t>(a);
  d[word + 1] = (d[word + 1] & ~0x1ffffu) | (static_cast<uint32_t>(a >> 32) & 0x1ffff);
}

// Turns the descriptor d of a plane's carrier (texels x rows) into the TP10
// descriptor of rows row0.. (a multiple of 64, a metadata tile) of a
// picture w samples wide. False if the carrier's is not what this expects.
bool tp10Band(uint32_t* d, unsigned texels, unsigned rows, bool chroma, unsigned row0,
              unsigned bandRows, unsigned w, unsigned pitch) {
  if (texGet(d, 0, 22, 8) != 0x36 /* FMT6_10_10_10_2_UNORM */ ||
      texGet(d, 0, 0, 2) != 3 /* TILE6_3 */ || texGet(d, 1, 0, 15) != texels ||
      texGet(d, 1, 15, 15) != rows || !texGet(d, 3, 28, 1) /* FLAG */ ||
      !texGet(d, 10, 0, 7) /* FLAG_BUFFER_PITCH */ || row0 % 64)
    return false;
  texSet(d, 0, 22, 8, chroma ? 0x9f : 0x9e);  // FMT6_TP10_UV, FMT6_TP10_Y
  texSet(d, 1, 0, 15, chroma ? w / 2 : w);
  texSet(d, 1, 15, 15, bandRows);
  unsigned logh = 0;  // FLAG_BUFFER_LOGH: the band's 4-row blocks, rounded up
  while ((1u << logh) < (bandRows + 3) / 4) logh++;
  texSet(d, 10, 12, 4, logh);
  texOffset(d, 7, static_cast<uint64_t>(row0) / 4 * texGet(d, 10, 0, 7) * 64);  // flag buffer
  texOffset(d, 4, static_cast<uint64_t>(row0) * pitch);
  return true;
}

}  // namespace

// A converter or decoder buffer, imported once: its two planes as images,
// a Q10C one's as TP10 carriers with their descriptors in slot of the
// descriptor buffer, or a Q08C one as one image (image[0], view[0]).
struct DirectBuffer {
  dev_t dev = 0;
  ino_t ino = 0;
  uint32_t format = 0;
  uint64_t modifier = 0;
  bool tp10 = false;
  int q08c = -1;  // a Q08C buffer: the chroma siting its view's Y'CbCr conversion is for
  int slot = -1;
  int width = 0, height = 0;
  uint32_t offset[2] = {}, pitch[2] = {};
  uint64_t size = 0;
  uint32_t session = 0;  // video_export::session when imported
  VkImage image[2] = {};
  VkDeviceMemory memory[2] = {};
  VkImageView view[2] = {};
  bool laidOut[2] = {};  // out of UNDEFINED
  int users = 0;  // frames
  double lastSeen = 0;
};

VkPipeline fullscreenPipeline(GpuContext& gpu, VkPipelineLayout layout, VkRenderPass pass,
                              const uint32_t* fragment, size_t fragmentBytes,
                              VkPipelineCreateFlags flags, VkColorComponentFlags writeMask,
                              const char* what) {
  VkDevice d = gpu.device();
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkShaderModule modules[2] = {};
  const uint32_t* code[2] = {kSpirv_fullscreen_vert, fragment};
  const size_t bytes[2] = {sizeof(kSpirv_fullscreen_vert), fragmentBytes};
  bool ok = true;
  for (int i = 0; i < 2 && ok; ++i) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes[i];
    info.pCode = code[i];
    ok = gpu.check(vkCreateShaderModule(d, &info, nullptr, &modules[i]), what);
  }
  if (ok) {
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = modules[0];
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = modules[1];
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = writeMask;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicInfo{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicInfo.dynamicStateCount = 2;
    dynamicInfo.pDynamicStates = dynamic;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.flags = flags;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamicInfo;
    info.layout = layout;
    info.renderPass = pass;
    ok = gpu.check(vkCreateGraphicsPipelines(d, gpu.pipelineCache(), 1, &info, nullptr, &pipeline),
                   what);
  }
  for (VkShaderModule m : modules)
    if (m) vkDestroyShaderModule(d, m, nullptr);
  return ok ? pipeline : VK_NULL_HANDLE;
}

DirectVideo::DirectVideo() = default;

DirectVideo::~DirectVideo() {
  if (!gpu_) return;
  for (auto& f : frames_)
    if (f->fenceFd_ >= 0) close(f->fenceFd_);
  frames_.clear();
  for (auto& b : buffers_) destroyBuffer(b);
  for (GpuImage& plane : stillPlanes_) gpu_->retire(plane);
  retireStillFramebuffers();
  VkDevice d = gpu_->device();
  std::vector<VkSemaphore> semaphores;
  for (const Semaphore& s : semaphores_) semaphores.push_back(s.semaphore);
  std::vector<VkDescriptorSetLayout> layouts(std::begin(q08cSetLayout_), std::end(q08cSetLayout_));
  layouts.push_back(tp10SetLayout_);
  std::vector<VkSampler> samplers(std::begin(q08cSampler_), std::end(q08cSampler_));
  std::vector<VkSamplerYcbcrConversion> conversions(std::begin(q08cConversion_), std::end(q08cConversion_));
  VkBuffer buffer = tp10Buffer_;
  VkDeviceMemory memory = tp10Memory_;
  VkPipelineLayout stillLayout = stillLayout_;
  std::vector<VkPipeline> pipelines(std::begin(stillPipeline_), std::end(stillPipeline_));
  std::vector<VkRenderPass> passes(std::begin(stillPass_), std::end(stillPass_));
  gpu_->retire([d, semaphores, layouts, samplers, conversions, buffer, memory, stillLayout, pipelines,
                passes] {
    for (VkPipeline p : pipelines)
      if (p) vkDestroyPipeline(d, p, nullptr);
    for (VkRenderPass p : passes)
      if (p) vkDestroyRenderPass(d, p, nullptr);
    if (stillLayout) vkDestroyPipelineLayout(d, stillLayout, nullptr);
    for (VkSemaphore s : semaphores) vkDestroySemaphore(d, s, nullptr);
    for (VkDescriptorSetLayout l : layouts)
      if (l) vkDestroyDescriptorSetLayout(d, l, nullptr);
    for (VkSampler s : samplers)
      if (s) vkDestroySampler(d, s, nullptr);
    for (VkSamplerYcbcrConversion c : conversions)
      if (c) vkDestroySamplerYcbcrConversion(d, c, nullptr);
    if (buffer) vkDestroyBuffer(d, buffer, nullptr);
    if (memory) vkFreeMemory(d, memory, nullptr);
  });
}

bool DirectVideo::init(GpuContext& gpu) {
  if (!gpu.caps().directVideo) return false;
  getFdProperties_ = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
      vkGetDeviceProcAddr(gpu.device(), "vkGetMemoryFdPropertiesKHR"));
  importSemaphoreFd_ = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
      vkGetDeviceProcAddr(gpu.device(), "vkImportSemaphoreFdKHR"));
  if (!getFdProperties_) {
    std::printf("Video: no vkGetMemoryFdPropertiesKHR; mpv draws every frame\n");
    return false;
  }
  VkPhysicalDeviceExternalSemaphoreInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
  info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
  VkExternalSemaphoreProperties props{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
  vkGetPhysicalDeviceExternalSemaphoreProperties(gpu.physicalDevice(), &info, &props);
  semaphoreImport_ = importSemaphoreFd_ &&
                     (props.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT);
  foreign_ = gpu.caps().foreignQueue ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
  gpu_ = &gpu;
  if (gpu.caps().tp10 && !initTp10())
    std::printf("Video: Main 10 frames cannot be sampled as the decoder writes them\n");
  if (gpu.caps().q08c && !initQ08c())
    std::printf("Video: 8-bit frames cannot be sampled as the decoder writes them\n");
  std::printf("Video: decoded frames go straight to the eyes where they can (%s)\n",
              semaphoreImport_ ? "GPU waits" : "CPU waits");
  return true;
}

// The descriptor buffer of Q10C frames: a set (kTp10Bands luma bands, then
// the chroma) for each buffer imported.
bool DirectVideo::initTp10() {
  VkDevice d = gpu_->device();
  getDescriptor_ = reinterpret_cast<PFN_vkGetDescriptorEXT>(vkGetDeviceProcAddr(d, "vkGetDescriptorEXT"));
  bindDescriptorBuffers_ = reinterpret_cast<PFN_vkCmdBindDescriptorBuffersEXT>(
      vkGetDeviceProcAddr(d, "vkCmdBindDescriptorBuffersEXT"));
  setDescriptorBufferOffsets_ = reinterpret_cast<PFN_vkCmdSetDescriptorBufferOffsetsEXT>(
      vkGetDeviceProcAddr(d, "vkCmdSetDescriptorBufferOffsetsEXT"));
  auto layoutSize = reinterpret_cast<PFN_vkGetDescriptorSetLayoutSizeEXT>(
      vkGetDeviceProcAddr(d, "vkGetDescriptorSetLayoutSizeEXT"));
  auto bindingOffset = reinterpret_cast<PFN_vkGetDescriptorSetLayoutBindingOffsetEXT>(
      vkGetDeviceProcAddr(d, "vkGetDescriptorSetLayoutBindingOffsetEXT"));
  if (!getDescriptor_ || !bindDescriptorBuffers_ || !setDescriptorBufferOffsets_ || !layoutSize ||
      !bindingOffset)
    return false;
  VkPhysicalDeviceDescriptorBufferPropertiesEXT props{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};
  VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  props2.pNext = &props;
  vkGetPhysicalDeviceProperties2(gpu_->physicalDevice(), &props2);
  tp10DescriptorSize_ = props.combinedImageSamplerDescriptorSize;
  if (tp10DescriptorSize_ < 16 * sizeof(uint32_t)) return false;
  VkDescriptorSetLayoutBinding bindings[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[i].descriptorCount = i == 0 ? kTp10Bands : 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  setInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
  setInfo.bindingCount = 2;
  setInfo.pBindings = bindings;
  if (!gpu_->check(vkCreateDescriptorSetLayout(d, &setInfo, nullptr, &tp10SetLayout_),
                   "TP10 set layout"))
    return false;
  layoutSize(d, tp10SetLayout_, &tp10SetSize_);
  for (uint32_t i = 0; i < 2; ++i) bindingOffset(d, tp10SetLayout_, i, &tp10Binding_[i]);
  const VkDeviceSize align = std::max<VkDeviceSize>(props.descriptorBufferOffsetAlignment, 1);
  tp10SetSize_ = (tp10SetSize_ + align - 1) / align * align;
  VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bufferInfo.size = tp10SetSize_ * kMaxBuffers;
  bufferInfo.usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                     VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  if (!gpu_->check(vkCreateBuffer(d, &bufferInfo, nullptr, &tp10Buffer_), "TP10 descriptor buffer"))
    return false;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(d, tp10Buffer_, &req);
  VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.pNext = &flags;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = gpu_->memoryType(
      req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (alloc.memoryTypeIndex == UINT32_MAX ||
      !gpu_->check(vkAllocateMemory(d, &alloc, nullptr, &tp10Memory_), "TP10 descriptor memory") ||
      !gpu_->check(vkBindBufferMemory(d, tp10Buffer_, tp10Memory_, 0), "TP10 descriptor memory") ||
      !gpu_->check(vkMapMemory(d, tp10Memory_, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&tp10Map_)),
                   "mapping TP10 descriptors"))
    return false;
  VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  address.buffer = tp10Buffer_;
  tp10Address_ = vkGetBufferDeviceAddress(d, &address);
  tp10Slots_.assign(kMaxBuffers, false);
  return tp10Address_ != 0;
}

void DirectVideo::enableTp10(VkSampler sampler) {
  if (!tp10Map_ || !sampler) return;
  tp10Sampler_ = sampler;
  std::printf("Video: Main 10 frames go to the eyes as the decoder wrote them (TP10)\n");
  if (!initStill()) std::printf("Video: a Main 10 frame cannot be copied; seeks keep it until the next\n");
}

// Q08C frames, each one NV12 image in the decoder's compressed layout, as
// the converter imports them (dmabuf_ubwc.c). A Y'CbCr conversion only
// places the chroma (identity model, full range: the colour stays the
// shaders'), where the frame's siting has it: one for each siting, with its
// immutable sampler (bilinear, one level) and push set layout. Only if the
// GPU can import that image and sample it so; else mpv converts them.
bool DirectVideo::initQ08c() {
  if (!q08cSupported()) return false;
  VkDevice d = gpu_->device();
  for (int i = 0; i < kQ08cSitings; ++i) {
    VkSamplerYcbcrConversionCreateInfo conversion{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO};
    conversion.format = kQ08cFormat;
    conversion.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
    conversion.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    conversion.xChromaOffset =
        i == VIDEO_EXPORT_CHROMA_CENTER ? VK_CHROMA_LOCATION_MIDPOINT : VK_CHROMA_LOCATION_COSITED_EVEN;
    conversion.yChromaOffset =
        i == VIDEO_EXPORT_CHROMA_TOP_LEFT ? VK_CHROMA_LOCATION_COSITED_EVEN : VK_CHROMA_LOCATION_MIDPOINT;
    conversion.chromaFilter = VK_FILTER_LINEAR;
    if (!gpu_->check(vkCreateSamplerYcbcrConversion(d, &conversion, nullptr, &q08cConversion_[i]),
                     "Q08C conversion"))
      return false;
    VkSamplerYcbcrConversionInfo use{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
    use.conversion = q08cConversion_[i];
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.pNext = &use;
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!gpu_->check(vkCreateSampler(d, &sampler, nullptr, &q08cSampler_[i]), "Q08C sampler")) return false;
    // Binding 1: the scene layout's second texture, which the video shaders
    // declare but don't read for these frames (Pipelines::bindQ08c).
    VkDescriptorSetLayoutBinding bindings[2] = {};
    for (uint32_t b = 0; b < 2; ++b) {
      bindings[b].binding = b;
      bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      bindings[b].descriptorCount = 1;
      bindings[b].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    bindings[0].pImmutableSamplers = &q08cSampler_[i];
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    setInfo.bindingCount = 2;
    setInfo.pBindings = bindings;
    if (!gpu_->check(vkCreateDescriptorSetLayout(d, &setInfo, nullptr, &q08cSetLayout_[i]),
                     "Q08C set layout"))
      return false;
  }
  q08cLayouts_ = true;
  return true;
}

// The format's features for the decoder's modifier (both chroma placements,
// the sampler's and the conversion's linear filters, the still's copy), and the import of
// such an image as importQ08c() makes it, up to the largest size it allows.
bool DirectVideo::q08cSupported() {
  VkPhysicalDevice physical = gpu_->physicalDevice();
  VkDrmFormatModifierPropertiesListEXT list{VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
  VkFormatProperties2 format{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
  format.pNext = &list;
  vkGetPhysicalDeviceFormatProperties2(physical, kQ08cFormat, &format);
  std::vector<VkDrmFormatModifierPropertiesEXT> modifiers(list.drmFormatModifierCount);
  list.pDrmFormatModifierProperties = modifiers.data();
  vkGetPhysicalDeviceFormatProperties2(physical, kQ08cFormat, &format);
  const VkFormatFeatureFlags need =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
      VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT | VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT |
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT;
  if (std::none_of(modifiers.begin(), modifiers.begin() + list.drmFormatModifierCount, [&](const auto& m) {
        return m.drmFormatModifier == kModifierQcomCompressed && m.drmFormatModifierPlaneCount == 2 &&
               (m.drmFormatModifierTilingFeatures & need) == need;
      }))
    return false;
  VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
  modifier.drmFormatModifier = kModifierQcomCompressed;
  modifier.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
  external.pNext = &modifier;
  external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
  info.pNext = &external;
  info.format = kQ08cFormat;
  info.type = VK_IMAGE_TYPE_2D;
  info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  info.usage = kQ08cUsage;
  VkExternalImageFormatProperties importable{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
  VkImageFormatProperties2 image{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
  image.pNext = &importable;
  if (vkGetPhysicalDeviceImageFormatProperties2(physical, &info, &image) != VK_SUCCESS ||
      !(importable.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
    return false;
  q08cMaxExtent_ = image.imageFormatProperties.maxExtent;
  return true;
}

void DirectVideo::enableQ08c() {
  q08c_ = true;
  std::printf("Video: 8-bit frames go to the eyes as the decoder wrote them (Q08C)\n");
}

// still()'s draw of a Q10C frame's planes: one pass for each, each leaving
// its plane ready for the eye passes, after the last frames sampling it.
bool DirectVideo::initStill() {
  VkDevice d = gpu_->device();
  VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(StillPush)};
  VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layoutInfo.setLayoutCount = 1;
  layoutInfo.pSetLayouts = &tp10SetLayout_;
  layoutInfo.pushConstantRangeCount = 1;
  layoutInfo.pPushConstantRanges = &push;
  if (!gpu_->check(vkCreatePipelineLayout(d, &layoutInfo, nullptr, &stillLayout_), "still layout"))
    return false;
  for (int i = 0; i < 2; ++i) {
    VkAttachmentDescription attachment{};
    attachment.format = i == 0 ? VK_FORMAT_R16_UNORM : VK_FORMAT_R16G16_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    VkSubpassDependency before{};
    before.srcSubpass = VK_SUBPASS_EXTERNAL;
    before.dstSubpass = 0;
    before.srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    before.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    before.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkSubpassDependency after{};
    after.srcSubpass = 0;
    after.dstSubpass = VK_SUBPASS_EXTERNAL;
    after.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    after.dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    after.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    const VkSubpassDependency dependencies[2] = {before, after};
    VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    passInfo.attachmentCount = 1;
    passInfo.pAttachments = &attachment;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    passInfo.dependencyCount = 2;
    passInfo.pDependencies = dependencies;
    if (!gpu_->check(vkCreateRenderPass(d, &passInfo, nullptr, &stillPass_[i]), "still pass"))
      return false;
    stillPipeline_[i] = fullscreenPipeline(
        *gpu_, stillLayout_, stillPass_[i], kSpirv_still_tp10_frag, sizeof(kSpirv_still_tp10_frag),
        VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        i == 0 ? VK_COLOR_COMPONENT_R_BIT : VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT,
        "still pipeline");
    if (!stillPipeline_[i]) return false;
  }
  return true;
}

void DirectVideo::retireStillFramebuffers() {
  VkDevice d = gpu_->device();
  for (VkFramebuffer& f : stillFramebuffer_) {
    if (f) gpu_->retire([d, f] { vkDestroyFramebuffer(d, f, nullptr); });
    f = VK_NULL_HANDLE;
  }
}

void DirectVideo::bindTp10(VkCommandBuffer cmd, VkPipelineLayout layout, const DirectFrame& frame) const {
  VkDescriptorBufferBindingInfoEXT binding{VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT};
  binding.address = tp10Address_;
  binding.usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                  VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT;
  bindDescriptorBuffers_(cmd, 1, &binding);
  const uint32_t index = 0;
  const VkDeviceSize offset = tp10SetSize_ * static_cast<VkDeviceSize>(frame.buffer_->slot);
  setDescriptorBufferOffsets_(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &index, &offset);
}

void DirectVideo::setPicture(int brightness, int contrast, int saturation) {
  brightness_ = brightness;
  contrast_ = contrast;
  saturation_ = saturation;
  for (auto& f : frames_) computeColor(*f);
}

void DirectVideo::setTvGamma(bool tvGamma) {
  tvGamma_ = tvGamma;
  for (auto& f : frames_) computeColor(*f);
}

bool DirectVideo::supported(const video_export& e) {
  return e.bt709_primaries && e.sdr && e.matrix != VIDEO_EXPORT_MATRIX_OTHER &&
         (e.drm_format == kFourccP010 || e.drm_format == kFourccNv12) && e.width > 0 &&
         e.height > 0 && (e.modifier == 0 || e.modifier == kModifierQcomCompressed);
}

// mpv's conversion (csputils.c) for these frames: limited or full range in
// its 8-bit-shifted convention, its contrast scaling luma and chroma, its
// saturation chroma, its brightness added to all (as to luma here, whose
// coefficient is 1 in each). P010 keeps its 10 bits at the top of 16; TP10
// samples as 10-bit values.
void DirectVideo::computeColor(DirectFrame& f) const {
  const DirectBuffer& b = *f.buffer_;
  const bool p010 = b.format == kFourccP010;
  const double s = p010 ? 4.0 : 1.0;  // 8-bit units to the frame's
  // a sampled value to the frame's
  const double toValue = b.tp10 ? 1023.0 : p010 ? 65535.0 / 64.0 : 255.0;
  const double contrast = (contrast_ + 100) / 100.0, saturation = (saturation_ + 100) / 100.0;
  double ymin = 16 * s, yrange = 219 * s, crange = 2 * (240 - 128) * s;
  if (f.fullRange_) {
    ymin = 0;
    yrange = 255 * s;
    crange = 2 * (255 - 128) * s;
  }
  const double ymul = contrast / yrange, cmul = contrast * saturation / crange;
  YuvColor& c = f.color_;
  c.range[0] = static_cast<float>(ymul * toValue);
  c.range[1] = static_cast<float>(-ymul * ymin + brightness_ / 100.0);
  c.range[2] = static_cast<float>(cmul * toValue);
  c.range[3] = static_cast<float>(-cmul * 128 * s);
  // Computed as range is, so the two are the same at the settings' 0.
  const double ymul0 = 1.0 / yrange, cmul0 = 1.0 / crange;
  c.neutralRange[0] = static_cast<float>(ymul0 * toValue);
  c.neutralRange[1] = static_cast<float>(-ymul0 * ymin);
  c.neutralRange[2] = static_cast<float>(cmul0 * toValue);
  c.neutralRange[3] = static_cast<float>(-cmul0 * 128 * s);
  float kr = 0.2126f, kb = 0.0722f;
  if (f.matrix_ == VIDEO_EXPORT_MATRIX_BT601) kr = 0.299f, kb = 0.114f;
  if (f.matrix_ == VIDEO_EXPORT_MATRIX_BT2020) kr = 0.2627f, kb = 0.0593f;
  c.matrix[0] = kr;
  c.matrix[1] = kb;
  c.tvGamma = tvGamma_;
  placeChroma(f);
}

// The chroma plane's texel centres sit between each two luma ones (what
// plain sampling assumes) or on the left (and top) one; a Q08C frame's
// Y'CbCr sampler places them itself.
void DirectVideo::placeChroma(DirectFrame& f) {
  const bool left = f.siting_ == VIDEO_EXPORT_CHROMA_LEFT || f.siting_ == VIDEO_EXPORT_CHROMA_TOP_LEFT;
  const bool top = f.siting_ == VIDEO_EXPORT_CHROMA_TOP_LEFT;
  f.color_.matrix[2] = left && f.q08c_ < 0 ? 0.25f / f.chroma_.width : 0.0f;
  f.color_.matrix[3] = top && f.q08c_ < 0 ? 0.25f / f.chroma_.height : 0.0f;
}

DirectBuffer* DirectVideo::findBuffer(const video_export& e) {
  struct stat st;
  if (fstat(e.fd, &st) != 0) return nullptr;
  // A Q08C frame's siting: its Y'CbCr conversion (others are centred, as plain sampling has them).
  const bool q08c = e.modifier == kModifierQcomCompressed && e.drm_format == kFourccNv12;
  const int siting = !q08c ? -1
                     : e.chroma == VIDEO_EXPORT_CHROMA_LEFT || e.chroma == VIDEO_EXPORT_CHROMA_TOP_LEFT
                         ? e.chroma
                         : VIDEO_EXPORT_CHROMA_CENTER;
  for (auto& b : buffers_)
    if (b->dev == st.st_dev && b->ino == st.st_ino && b->format == e.drm_format &&
        b->modifier == e.modifier && b->q08c == siting &&
        b->width == e.width && b->height == e.height && b->size == e.buffer_size &&
        std::equal(b->offset, b->offset + 2, e.offset) && std::equal(b->pitch, b->pitch + 2, e.pitch))
      return b.get();
  // A new one, in place of the oldest unused if there are too many.
  if (buffers_.size() >= kMaxBuffers) {
    auto oldest = buffers_.end();
    for (auto it = buffers_.begin(); it != buffers_.end(); ++it)
      if (!(*it)->users && (oldest == buffers_.end() || (*it)->lastSeen < (*oldest)->lastSeen))
        oldest = it;
    if (oldest == buffers_.end()) return nullptr;
    destroyBuffer(*oldest);
    buffers_.erase(oldest);
  }
  auto b = std::make_unique<DirectBuffer>();
  b->dev = st.st_dev;
  b->ino = st.st_ino;
  b->format = e.drm_format;
  b->modifier = e.modifier;
  b->tp10 = e.modifier == kModifierQcomCompressed && !q08c;
  b->q08c = siting;
  b->width = e.width;
  b->height = e.height;
  b->size = e.buffer_size;
  std::copy(e.offset, e.offset + 2, b->offset);
  std::copy(e.pitch, e.pitch + 2, b->pitch);
  b->session = __atomic_load_n(&e.session, __ATOMIC_ACQUIRE);
  bool ok = b->tp10   ? importTp10(*b, e.fd)
            : q08c     ? importQ08c(*b, e.fd)
                       : importPlane(*b, 0, e.fd) && importPlane(*b, 1, e.fd);
  if (!ok) {
    destroyBuffer(b);
    return nullptr;
  }
  buffers_.push_back(std::move(b));
  return buffers_.back().get();
}

// One plane as a linear image over the buffer's DMA-BUF (its own import).
bool DirectVideo::importPlane(DirectBuffer& b, int plane, int fd) {
  VkDevice d = gpu_->device();
  const bool p010 = b.format == kFourccP010;
  const VkFormat format = plane == 0 ? (p010 ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM)
                                     : (p010 ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R8G8_UNORM);
  const uint32_t w = plane == 0 ? b.width : (b.width + 1) / 2;
  const uint32_t h = plane == 0 ? b.height : (b.height + 1) / 2;
  VkSubresourceLayout layout{};
  layout.offset = b.offset[plane];
  layout.rowPitch = b.pitch[plane];
  VkImageDrmFormatModifierExplicitCreateInfoEXT modifier{
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
  modifier.drmFormatModifier = 0;  // DRM_FORMAT_MOD_LINEAR
  modifier.drmFormatModifierPlaneCount = 1;
  modifier.pPlaneLayouts = &layout;
  VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  external.pNext = &modifier;
  external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.pNext = &external;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {w, h, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // still()'s copy
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!gpu_->check(vkCreateImage(d, &info, nullptr, &b.image[plane]), "creating a decoded frame's image"))
    return false;
  VkMemoryDedicatedRequirements dedicatedReq{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
  VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
  req.pNext = &dedicatedReq;
  VkImageMemoryRequirementsInfo2 reqInfo{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
  reqInfo.image = b.image[plane];
  vkGetImageMemoryRequirements2(d, &reqInfo, &req);
  VkMemoryFdPropertiesKHR fdProps{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
  if (!gpu_->check(getFdProperties_(d, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdProps),
                   "reading a decoded frame's memory properties"))
    return false;
  const uint32_t type =
      gpu_->memoryType(req.memoryRequirements.memoryTypeBits & fdProps.memoryTypeBits, 0);
  if (type == UINT32_MAX || req.memoryRequirements.size > b.size) {
    std::fprintf(stderr, "Video: cannot import a decoded frame's plane (%llu of %llu bytes)\n",
                 static_cast<unsigned long long>(req.memoryRequirements.size),
                 static_cast<unsigned long long>(b.size));
    return false;
  }
  const int own = dup(fd);
  if (own < 0) return false;
  VkImportMemoryFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
  import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  import.fd = own;
  VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dedicated.image = b.image[plane];
  if (dedicatedReq.prefersDedicatedAllocation || dedicatedReq.requiresDedicatedAllocation)
    import.pNext = &dedicated;
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.pNext = &import;
  alloc.allocationSize = b.size;
  alloc.memoryTypeIndex = type;
  if (!gpu_->check(vkAllocateMemory(d, &alloc, nullptr, &b.memory[plane]),
                   "importing a decoded frame")) {
    close(own);
    return false;
  }
  if (!gpu_->check(vkBindImageMemory(d, b.image[plane], b.memory[plane], 0),
                   "binding a decoded frame's memory"))
    return false;
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = b.image[plane];
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return gpu_->check(vkCreateImageView(d, &view, nullptr, &b.view[plane]),
                     "creating a decoded frame's view");
}

// A Q10C buffer, as the converter reads it (dmabuf_ubwc.c): each plane a
// 32-bit carrier image over the decoder's compressed layout, whose
// descriptor becomes TP10 ones (three samples a texel) for bands of rows:
// the luma's overlapping by half (video.glsl, tp10Luma), the chroma whole.
bool DirectVideo::importTp10(DirectBuffer& b, int fd) {
  VkDevice d = gpu_->device();
  const unsigned w = static_cast<unsigned>(b.width), h = static_cast<unsigned>(b.height);
  const unsigned full = kTp10MaxSamples / w / 64 * 64, step = full / 128 * 64;
  const unsigned bands = h <= full ? 1 : step ? 1 + (h - full + step - 1) / step : 0;
  const auto free = std::find(tp10Slots_.begin(), tp10Slots_.end(), false);
  if (!tp10Sampler_ || w % 2 || h % 2 || !bands || bands > kTp10Bands || h / 2 > full ||
      b.offset[0] != 0 || b.pitch[0] != b.pitch[1] || free == tp10Slots_.end()) {
    std::fprintf(stderr, "Video: cannot sample a %ux%u Main 10 frame as the decoder wrote it\n", w, h);
    return false;
  }
  // The decoder lays out its coded height, a few rows more than h: the
  // carrier whose size reaches the chroma plane.
  const unsigned texels = b.pitch[0] / 4;
  unsigned rows = (h + 15) & ~15u;
  VkMemoryRequirements req[2] = {};
  for (int i = 0; i < 2;) {
    VkSubresourceLayout layout{};
    layout.rowPitch = b.pitch[i];
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
    modifier.drmFormatModifier = kModifierQcomCompressed;
    modifier.drmFormatModifierPlaneCount = 1;
    modifier.pPlaneLayouts = &layout;
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.pNext = &modifier;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.pNext = &external;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = kTp10Carrier;
    info.extent = {texels, i ? rows / 2 : rows, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!gpu_->check(vkCreateImage(d, &info, nullptr, &b.image[i]), "creating a Main 10 frame's image"))
      return false;
    vkGetImageMemoryRequirements(d, b.image[i], &req[i]);
    if (i == 0 && req[0].size < b.offset[1] && rows < h + 64) {
      vkDestroyImage(d, b.image[0], nullptr);
      b.image[0] = VK_NULL_HANDLE;
      rows += 16;
      continue;
    }
    ++i;
  }
  VkMemoryFdPropertiesKHR fdProps{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
  if (req[0].size != b.offset[1] || b.offset[1] % req[1].alignment ||
      b.offset[1] + req[1].size > b.size ||
      getFdProperties_(d, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdProps) != VK_SUCCESS) {
    std::fprintf(stderr, "Video: a Main 10 frame's layout is not the GPU's (%llu + %llu bytes, chroma at %u)\n",
                 static_cast<unsigned long long>(req[0].size),
                 static_cast<unsigned long long>(req[1].size), b.offset[1]);
    return false;
  }
  const uint32_t type =
      gpu_->memoryType(req[0].memoryTypeBits & req[1].memoryTypeBits & fdProps.memoryTypeBits, 0);
  const int own = type == UINT32_MAX ? -1 : dup(fd);
  if (own < 0) return false;
  VkImportMemoryFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
  import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  import.fd = own;
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.pNext = &import;
  alloc.allocationSize = b.size;
  alloc.memoryTypeIndex = type;
  if (!gpu_->check(vkAllocateMemory(d, &alloc, nullptr, &b.memory[0]), "importing a Main 10 frame")) {
    close(own);
    return false;
  }
  for (int i = 0; i < 2; ++i) {
    if (!gpu_->check(vkBindImageMemory(d, b.image[i], b.memory[0], b.offset[i]),
                     "binding a Main 10 frame's memory"))
      return false;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = b.image[i];
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = kTp10Carrier;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!gpu_->check(vkCreateImageView(d, &view, nullptr, &b.view[i]), "creating a Main 10 frame's view"))
      return false;
  }
  // Band k of the luma: rows k * step on, a full band's or to the bottom;
  // the slots past the last repeat the first.
  std::vector<uint32_t> descriptor(tp10DescriptorSize_ / sizeof(uint32_t));
  uint8_t* set = tp10Map_ + tp10SetSize_ * static_cast<VkDeviceSize>(free - tp10Slots_.begin());
  for (unsigned k = 0; k <= kTp10Bands; ++k) {
    const bool chroma = k == kTp10Bands;
    const unsigned band = chroma || k >= bands ? 0 : k;
    const unsigned row0 = band * step;
    const unsigned bandRows = chroma ? h / 2 : std::min(full, h - row0);
    VkDescriptorImageInfo image{tp10Sampler_, b.view[chroma ? 1 : 0], VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorGetInfoEXT get{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    get.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    get.data.pCombinedImageSampler = &image;
    getDescriptor_(d, &get, tp10DescriptorSize_, descriptor.data());
    if (!tp10Band(descriptor.data(), texels, chroma ? rows / 2 : rows, chroma, row0, bandRows, w,
                  b.pitch[0])) {
      std::fprintf(stderr, "Video: unexpected texture descriptor for a Main 10 frame\n");
      return false;
    }
    std::memcpy(set + (chroma ? tp10Binding_[1] : tp10Binding_[0] + k * tp10DescriptorSize_),
                descriptor.data(), tp10DescriptorSize_);
  }
  *free = true;
  b.slot = static_cast<int>(free - tp10Slots_.begin());
  return true;
}

// A Q08C buffer as one NV12 image in the compressed layout, its planes (each
// with its compression metadata first) where the decoder put them, viewed
// through the Y'CbCr conversion of its siting.
bool DirectVideo::importQ08c(DirectBuffer& b, int fd) {
  if (!q08c_ || b.width > static_cast<int>(q08cMaxExtent_.width) ||
      b.height > static_cast<int>(q08cMaxExtent_.height))
    return false;
  VkDevice d = gpu_->device();
  VkSubresourceLayout layouts[2] = {};
  for (int i = 0; i < 2; ++i) {
    layouts[i].offset = b.offset[i];
    layouts[i].rowPitch = b.pitch[i];
  }
  VkImageDrmFormatModifierExplicitCreateInfoEXT modifier{
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
  modifier.drmFormatModifier = kModifierQcomCompressed;
  modifier.drmFormatModifierPlaneCount = 2;
  modifier.pPlaneLayouts = layouts;
  VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  external.pNext = &modifier;
  external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.pNext = &external;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = kQ08cFormat;
  info.extent = {static_cast<uint32_t>(b.width), static_cast<uint32_t>(b.height), 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  info.usage = kQ08cUsage;
  if (!gpu_->check(vkCreateImage(d, &info, nullptr, &b.image[0]), "creating an 8-bit frame's image"))
    return false;
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(d, b.image[0], &req);
  VkMemoryFdPropertiesKHR fdProps{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
  if (req.size > b.size ||
      getFdProperties_(d, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdProps) != VK_SUCCESS) {
    std::fprintf(stderr, "Video: cannot import an 8-bit frame (%llu of %llu bytes)\n",
                 static_cast<unsigned long long>(req.size), static_cast<unsigned long long>(b.size));
    return false;
  }
  const uint32_t type = gpu_->memoryType(req.memoryTypeBits & fdProps.memoryTypeBits, 0);
  const int own = type == UINT32_MAX ? -1 : dup(fd);
  if (own < 0) return false;
  VkImportMemoryFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
  import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  import.fd = own;
  VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dedicated.image = b.image[0];
  import.pNext = &dedicated;
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.pNext = &import;
  alloc.allocationSize = b.size;
  alloc.memoryTypeIndex = type;
  if (!gpu_->check(vkAllocateMemory(d, &alloc, nullptr, &b.memory[0]), "importing an 8-bit frame")) {
    close(own);
    return false;
  }
  if (!gpu_->check(vkBindImageMemory(d, b.image[0], b.memory[0], 0), "binding an 8-bit frame's memory"))
    return false;
  VkSamplerYcbcrConversionInfo use{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
  use.conversion = q08cConversion_[b.q08c];
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.pNext = &use;
  view.image = b.image[0];
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = kQ08cFormat;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return gpu_->check(vkCreateImageView(d, &view, nullptr, &b.view[0]), "creating an 8-bit frame's view");
}

void DirectVideo::destroyBuffer(std::unique_ptr<DirectBuffer>& b) {
  if (!b) return;
  if (b->slot >= 0) tp10Slots_[static_cast<size_t>(b->slot)] = false;
  VkDevice d = gpu_->device();
  DirectBuffer* raw = b.release();
  auto destroy = [d, raw] {
    for (int i = 0; i < 2; ++i) {
      if (raw->view[i]) vkDestroyImageView(d, raw->view[i], nullptr);
      if (raw->image[i]) vkDestroyImage(d, raw->image[i], nullptr);
      if (raw->memory[i]) vkFreeMemory(d, raw->memory[i], nullptr);
    }
    delete raw;
  };
  // Frames are released once the GPU is done with them: a buffer no frame
  // uses can go at once, which a decoder's next session may be waiting for.
  if (raw->users) gpu_->retire(destroy);
  else destroy();
}

DirectFrame* DirectVideo::accept(video_export& e) {
  const int fence = e.fence_fd;
  e.fence_fd = -1;
  DirectBuffer* b = gpu_ && supported(e) ? findBuffer(e) : nullptr;
  if (!b) {
    if (fence >= 0) close(fence);
    rejected_.push_back(e.id);
    return nullptr;
  }
  auto f = std::make_unique<DirectFrame>();
  f->buffer_ = b;
  ++b->users;
  b->lastSeen = now();
  f->id_ = e.id;
  f->fenceFd_ = fence;
  f->matrix_ = e.matrix;
  f->fullRange_ = e.full_range;
  f->siting_ = e.chroma;
  f->tp10_ = b->tp10;
  f->q08c_ = b->q08c;
  for (int i = 0; i < 2; ++i) {
    Texture& t = i == 0 ? f->luma_ : f->chroma_;
    t.image = b->image[b->q08c >= 0 ? 0 : i];
    t.view = b->view[b->q08c >= 0 ? 0 : i];
    t.width = i == 0 ? b->width : (b->width + 1) / 2;
    t.height = i == 0 ? b->height : (b->height + 1) / 2;
    t.yuv = f.get();
  }
  computeColor(*f);
  frames_.push_back(std::move(f));
  return frames_.back().get();
}

void DirectVideo::hold(DirectFrame* frame, bool held) {
  if (frame) frame->held_ = held;
}

void DirectVideo::release(video_export& e) { release(e, gpu_ ? gpu_->completed() : 0); }

void DirectVideo::release(video_export& e, uint64_t completed) {
  e.release_count = 0;
  auto room = [&] { return e.release_count < VIDEO_EXPORT_MAX_RELEASE; };
  while (!rejected_.empty() && room()) {
    e.release[e.release_count++] = rejected_.back();
    rejected_.pop_back();
  }
  if (!gpu_) return;
  for (auto it = frames_.begin(); it != frames_.end() && room();) {
    DirectFrame& f = **it;
    const bool done = !f.held_ && !f.inFrame_ && !f.pinned_ && f.lastUse_ <= completed;
    if (!done) {
      ++it;
      continue;
    }
    // IDs belong to the render backend, including after converter replacement.
    e.release[e.release_count++] = f.id_;
    if (f.fenceFd_ >= 0) close(f.fenceFd_);
    --f.buffer_->users;
    f.buffer_->lastSeen = now();
    it = frames_.erase(it);
  }
  // Imports of buffers mpv has not handed out for a while (another size,
  // its converter gone) keep their memory alive: dropped. A closed decoder
  // session's at once: they hold room its next session needs, and a frame
  // still shown when it closed (a seek's) only goes back afterwards.
  const double t = now();
  const uint32_t session = __atomic_load_n(&e.session, __ATOMIC_ACQUIRE);
  for (auto it = buffers_.begin(); it != buffers_.end();) {
    if (!(*it)->users && ((*it)->session != session || t - (*it)->lastSeen > kKeepUnused)) {
      destroyBuffer(*it);
      it = buffers_.erase(it);
    } else {
      ++it;
    }
  }
}

bool DirectVideo::returnable(uint64_t& completed) {
  completed = 0;
  uint64_t oldest = UINT64_MAX;
  if (gpu_)
    for (auto& f : frames_)
      if (!f->held_ && !f->inFrame_ && !f->pinned_) oldest = std::min(oldest, f->lastUse_);
  if (oldest != UINT64_MAX) completed = gpu_->completed();
  return !rejected_.empty() || (oldest != UINT64_MAX && oldest <= completed);
}

bool DirectVideo::returning() const {
  return !rejected_.empty() ||
         std::any_of(frames_.begin(), frames_.end(), [](auto& f) { return !f->held_; });
}

void DirectVideo::dropUnused() {
  for (auto it = buffers_.begin(); it != buffers_.end();) {
    if (!(*it)->users) {
      destroyBuffer(*it);
      it = buffers_.erase(it);
    } else {
      ++it;
    }
  }
}

bool DirectVideo::closedSession(const DirectFrame& frame, uint32_t session) const {
  return frame.buffer_ && frame.buffer_->session != session;
}

bool DirectVideo::sessionReleased(uint32_t session) const {
  return rejected_.empty() && std::none_of(buffers_.begin(), buffers_.end(),
      [session](const auto& b) { return b->session != session; });
}

void DirectVideo::cancelPendingStill() {
  if (!stillSource_ || stillSource_->inFrame_) return;
  unpin(*stillSource_, stillSource_->lastUse_);
  stillSource_ = nullptr;
}

DirectFrame* DirectVideo::still(DirectFrame& frame) {
  if (!gpu_ || !frame.buffer_ || (frame.tp10_ && !stillPipeline_[1]) || stillSource_) return nullptr;
  const DirectBuffer& b = *frame.buffer_;
  const bool p010 = b.format == kFourccP010;  // Q10C ones too
  for (int i = 0; i < 2; ++i) {
    const VkFormat format = i == 0 ? (p010 ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM)
                                   : (p010 ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R8G8_UNORM);
    const uint32_t w = i == 0 ? b.width : (b.width + 1) / 2;
    const uint32_t h = i == 0 ? b.height : (b.height + 1) / 2;
    GpuImage& plane = stillPlanes_[i];
    if (plane.image && plane.format == format && plane.width == w && plane.height == h) continue;
    if (still_ && still_->pinned_) return nullptr;  // sampled outside the frames (DirectDepth)
    gpu_->retire(plane);
    retireStillFramebuffers();
    if (!gpu_->createImage(w, h, 1, format,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                           plane, "a still of a decoded frame"))
      return nullptr;
  }
  for (int i = 0; i < 2 && frame.tp10_; ++i) {
    if (stillFramebuffer_[i]) continue;
    VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    info.renderPass = stillPass_[i];
    info.attachmentCount = 1;
    info.pAttachments = &stillPlanes_[i].view;
    info.width = stillPlanes_[i].width;
    info.height = stillPlanes_[i].height;
    info.layers = 1;
    if (!gpu_->check(vkCreateFramebuffer(gpu_->device(), &info, nullptr, &stillFramebuffer_[i]),
                     "still framebuffer"))
      return nullptr;
  }
  if (!still_) still_ = std::make_unique<DirectFrame>();
  DirectFrame& s = *still_;
  s.matrix_ = frame.matrix_;
  s.fullRange_ = frame.fullRange_;
  s.siting_ = frame.siting_;
  s.color_ = frame.color_;
  s.held_ = false;
  for (int i = 0; i < 2; ++i) {
    Texture& t = i == 0 ? s.luma_ : s.chroma_;
    t.image = stillPlanes_[i].image;
    t.view = stillPlanes_[i].view;
    t.width = static_cast<int>(stillPlanes_[i].width);
    t.height = static_cast<int>(stillPlanes_[i].height);
    t.yuv = &s;
  }
  placeChroma(s);  // two planes, whatever the source
  stillSource_ = &frame;
  pin(frame);
  return &s;
}

void DirectVideo::dropStill() {
  if (stillSource_ || (still_ && still_->pinned_)) return;
  for (GpuImage& plane : stillPlanes_) gpu_->retire(plane);
  retireStillFramebuffers();
}

// A Q10C source's planes drawn into the still's, which the passes leave
// ready for the eye passes to sample.
void DirectVideo::recordStillDraw(VkCommandBuffer cmd) {
  for (int i = 0; i < 2; ++i) {
    const GpuImage& plane = stillPlanes_[i];
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = stillPass_[i];
    begin.framebuffer = stillFramebuffer_[i];
    begin.renderArea.extent = {plane.width, plane.height};
    vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0, 0, static_cast<float>(plane.width), static_cast<float>(plane.height),
                              0, 1};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &begin.renderArea);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, stillPipeline_[i]);
    bindTp10(cmd, stillLayout_, *stillSource_);
    const StillPush push{{static_cast<float>(plane.width), static_cast<float>(plane.height)}, i};
    vkCmdPushConstants(cmd, stillLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
  }
}

// After the acquires: the source's planes into the still's, both left as the
// eye passes and the release barriers expect them. A Q08C source is one
// image, copied from plane by plane in the general layout it stays in.
void DirectVideo::recordStillCopy(VkCommandBuffer cmd) {
  if (!stillSource_ || !stillSource_->inFrame_) return;
  if (stillSource_->tp10_) return recordStillDraw(cmd);
  DirectBuffer& b = *stillSource_->buffer_;
  const bool q08c = b.q08c >= 0;
  std::vector<VkImageMemoryBarrier> before, after;
  // image from layout `from` to `copy` for the copy's `access`, then to `to`.
  auto add = [&](VkImage image, VkImageLayout from, VkImageLayout copy, VkImageLayout to,
                 VkAccessFlags access) {
    VkImageMemoryBarrier m{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    m.srcQueueFamilyIndex = m.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    m.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    m.image = image;
    m.oldLayout = from;
    m.newLayout = copy;
    m.dstAccessMask = access;
    before.push_back(m);
    m.oldLayout = copy;
    m.newLayout = to;
    m.srcAccessMask = access;
    m.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    after.push_back(m);
  };
  const VkImageLayout shown = q08c ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  const VkImageLayout read = q08c ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  for (int i = 0; i < (q08c ? 1 : 2); ++i) add(b.image[i], shown, read, shown, VK_ACCESS_TRANSFER_READ_BIT);
  for (const GpuImage& plane : stillPlanes_)
    add(plane.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT);
  // Earlier submissions may still sample the still's last contents.
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                       nullptr, 0, nullptr, static_cast<uint32_t>(before.size()), before.data());
  for (int i = 0; i < 2; ++i) {
    VkImageCopy region{};
    region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    if (q08c) region.srcSubresource.aspectMask = i == 0 ? VK_IMAGE_ASPECT_PLANE_0_BIT : VK_IMAGE_ASPECT_PLANE_1_BIT;
    region.extent = {stillPlanes_[i].width, stillPlanes_[i].height, 1};
    vkCmdCopyImage(cmd, b.image[q08c ? 0 : i], read, stillPlanes_[i].image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  }
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                       nullptr, 0, nullptr, static_cast<uint32_t>(after.size()), after.data());
}

void DirectVideo::useInFrame(const DirectFrame& frame) {
  for (auto& f : frames_)
    if (f.get() == &frame && !f->inFrame_) {
      f->inFrame_ = true;
      inFrame_.push_back(f.get());
    }
}

// Each submission takes the planes from the converter's side and gives them
// back, as GL's shared images are handed over per frame. Linear images have
// no compression state, so these barriers cost nothing; the decoder's
// compressed ones stay in the general layout, as the converter reads them
// (a Q08C buffer is one image).
void DirectVideo::addBarriers(std::vector<VkImageMemoryBarrier>& list, DirectFrame& f, bool acquire) {
  if (!f.buffer_) return;  // a still: the app's own images, always ready to sample
  DirectBuffer& b = *f.buffer_;
  const bool compressed = b.tp10 || b.q08c >= 0;
  for (int i = 0; i < (b.q08c >= 0 ? 1 : 2); ++i) {
    VkImageMemoryBarrier m{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    m.image = b.image[i];
    m.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    m.oldLayout = m.newLayout =
        compressed ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (acquire) {
      if (!b.laidOut[i] && !compressed) m.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      b.laidOut[i] = true;
      m.srcQueueFamilyIndex = foreign_;
      m.dstQueueFamilyIndex = gpu_->queueFamily();
      m.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    } else {
      m.srcQueueFamilyIndex = gpu_->queueFamily();
      m.dstQueueFamilyIndex = foreign_;
      m.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    list.push_back(m);
  }
}

void DirectVideo::barriers(VkCommandBuffer cmd, bool acquire) {
  std::vector<VkImageMemoryBarrier> list;
  for (DirectFrame* f : inFrame_) addBarriers(list, *f, acquire);
  if (list.empty()) return;
  // External ownership transfers have no defined pipeline stage on Vulkan 1.2.
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       0, 0, nullptr, 0, nullptr, static_cast<uint32_t>(list.size()), list.data());
}

void DirectVideo::recordBarriers(VkCommandBuffer cmd, DirectFrame& frame, bool acquire) {
  std::vector<VkImageMemoryBarrier> list;
  addBarriers(list, frame, acquire);
  if (list.empty()) return;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       0, 0, nullptr, 0, nullptr, static_cast<uint32_t>(list.size()), list.data());
}

void DirectVideo::unpin(DirectFrame& frame, uint64_t lastUse) {
  if (frame.pinned_ > 0) --frame.pinned_;
  frame.lastUse_ = std::max(frame.lastUse_, lastUse);
}

void DirectVideo::recordAcquires(VkCommandBuffer cmd) {
  barriers(cmd, true);
  recordStillCopy(cmd);
}
void DirectVideo::recordReleases(VkCommandBuffer cmd) { barriers(cmd, false); }

bool DirectVideo::fenceDone(DirectFrame& f) {
  if (f.fenceFd_ >= 0 && signalled(f.fenceFd_)) {
    close(f.fenceFd_);
    f.fenceFd_ = -1;
  }
  return f.fenceFd_ < 0;
}

VkSemaphore DirectVideo::takeSemaphore() {
  const uint64_t completed = gpu_->completed();
  for (size_t i = 0; i < semaphores_.size(); ++i)
    if (semaphores_[i].freeAfter <= completed &&
        std::find(frameSemaphores_.begin(), frameSemaphores_.end(), i) == frameSemaphores_.end()) {
      frameSemaphores_.push_back(i);
      return semaphores_[i].semaphore;
    }
  VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  Semaphore s;
  if (!gpu_->check(vkCreateSemaphore(gpu_->device(), &info, nullptr, &s.semaphore),
                   "creating a decoded frame's semaphore"))
    return VK_NULL_HANDLE;
  semaphores_.push_back(s);
  frameSemaphores_.push_back(semaphores_.size() - 1);
  return s.semaphore;
}

// A conversion still running when the frame is submitted: its sync_file,
// imported as a temporary payload, holds the submission back on the GPU
// (each submission imports its own copy, so none relies on another's wait).
bool DirectVideo::frameWaits(std::vector<SemaphoreWait>& waits) {
  // A still to copy: its source is taken over and given back like a frame
  // the eyes sample.
  if (stillSource_ && !stillSource_->inFrame_) {
    stillSource_->inFrame_ = true;
    inFrame_.push_back(stillSource_);
  }
  uint64_t lastUse = 0;
  for (DirectFrame* f : inFrame_) {
    lastUse = std::max(lastUse, f->lastUse_);
    if (fenceDone(*f)) continue;
    const int fd = semaphoreImport_ ? dup(f->fenceFd_) : -1;
    VkSemaphore semaphore = fd >= 0 ? takeSemaphore() : VK_NULL_HANDLE;
    if (semaphore) {
      VkImportSemaphoreFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
      import.semaphore = semaphore;
      import.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
      import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
      import.fd = fd;
      if (importSemaphoreFd_(gpu_->device(), &import) == VK_SUCCESS) {
        waits.push_back({semaphore, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT});
        continue;
      }
      frameSemaphores_.pop_back();
    }
    if (fd >= 0) close(fd);
    // No GPU wait: do not sample the frame unless conversion completed.
    if (!signalled(f->fenceFd_, 100)) return false;
  }
  if (lastUse) {
    // GL interop may already wait on this timeline. One wait covers both.
    auto wait = std::find_if(waits.begin(), waits.end(), [&](const SemaphoreWait& w) {
      return w.semaphore == gpu_->frameTimeline();
    });
    if (wait != waits.end()) {
      wait->value = std::max(wait->value, lastUse);
      wait->stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    } else {
      waits.push_back({gpu_->frameTimeline(), lastUse, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT});
    }
  }
  return true;
}

void DirectVideo::frameSubmitted(uint64_t value) {
  for (DirectFrame* f : inFrame_) {
    f->inFrame_ = false;
    f->lastUse_ = value;
    if (f == stillSource_) {  // copied: it can go back once the GPU is done
      unpin(*f, value);
      stillSource_ = nullptr;
    }
  }
  inFrame_.clear();
  for (size_t i : frameSemaphores_) semaphores_[i].freeAfter = value;
  frameSemaphores_.clear();
}

void DirectVideo::frameAbandoned() {
  for (DirectFrame* f : inFrame_) f->inFrame_ = false;
  inFrame_.clear();
  // A payload imported but never waited on stays in the semaphore: made anew.
  VkDevice d = gpu_ ? gpu_->device() : VK_NULL_HANDLE;
  std::sort(frameSemaphores_.rbegin(), frameSemaphores_.rend());
  for (size_t i : frameSemaphores_) {
    VkSemaphore s = semaphores_[i].semaphore;
    gpu_->retire([d, s] { vkDestroySemaphore(d, s, nullptr); });
    semaphores_.erase(semaphores_.begin() + static_cast<long>(i));
  }
  frameSemaphores_.clear();
}
