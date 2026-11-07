#include "direct_depth.h"

#include <cstdint>
// The SPIR-V headers declare bare arrays: <cstdint> first.
#include "shaders/depth_patch.frag.h"
#include "shaders/depth_patch_tp10.frag.h"
#include "shaders/depth_patch_ycbcr.frag.h"

#include <cstdio>
#include <iterator>
#include <vector>

namespace {

constexpr VkFormat kFormat = VK_FORMAT_R32_SFLOAT;
constexpr uint32_t kWidth = DepthEstimator::kMaxPatchWidth, kRows = DepthEstimator::kMaxPatchRows;

// depth_patch.frag's push constants.
struct Push {
  float rect[4];
  float yuvRange[4];
  float yuvMatrix[4];
  float start[2];
  float eyeScale[2];
  float eyeOffset[2];
  float alphaCenter[2];
  float alphaScale[2];
  float step;
  float fisheyeFov;
  int32_t mode, projection, flip, alphaPacked;
  float greenKey;
};

}  // namespace

DirectDepth::~DirectDepth() {
  if (!gpu_) return;
  if (frame_) direct_->unpin(*frame_, 0);
  VkDevice d = gpu_->device();
  VkRenderPass pass = pass_;
  VkSampler sampler = sampler_;
  VkDescriptorSetLayout setLayout = setLayout_;
  std::vector<VkPipelineLayout> layouts{layout_, tp10Layout_};
  std::vector<VkPipeline> pipelines{pipeline_, tp10Pipeline_};
  layouts.insert(layouts.end(), std::begin(q08cLayout_), std::end(q08cLayout_));
  pipelines.insert(pipelines.end(), std::begin(q08cPipeline_), std::end(q08cPipeline_));
  VkFramebuffer framebuffer = framebuffer_;
  VkBuffer buffer = buffer_;
  VkDeviceMemory memory = memory_;
  gpu_->retire([d, pass, sampler, setLayout, layouts, pipelines, framebuffer, buffer, memory] {
    for (VkPipeline p : pipelines)
      if (p) vkDestroyPipeline(d, p, nullptr);
    for (VkPipelineLayout l : layouts)
      if (l) vkDestroyPipelineLayout(d, l, nullptr);
    if (setLayout) vkDestroyDescriptorSetLayout(d, setLayout, nullptr);
    if (sampler) vkDestroySampler(d, sampler, nullptr);
    if (framebuffer) vkDestroyFramebuffer(d, framebuffer, nullptr);
    if (pass) vkDestroyRenderPass(d, pass, nullptr);
    if (buffer) vkDestroyBuffer(d, buffer, nullptr);
    if (memory) vkFreeMemory(d, memory, nullptr);
  });
  gpu_->retire(image_);
}

bool DirectDepth::init(GpuContext& gpu, DirectVideo& direct) {
  if (!direct.available()) return false;
  gpu_ = &gpu;
  direct_ = &direct;
  VkDevice d = gpu.device();
  if (!gpu.createImage(kWidth, kRows, 1, kFormat,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, image_,
                       "focus patch image"))
    return false;

  VkAttachmentDescription attachment{};
  attachment.format = kFormat;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color;
  // The copy after the pass reads what it wrote.
  VkSubpassDependency after{};
  after.srcSubpass = 0;
  after.dstSubpass = VK_SUBPASS_EXTERNAL;
  after.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  after.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
  after.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  after.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  // The last copy is done reading before the pass writes again.
  VkSubpassDependency before{};
  before.srcSubpass = VK_SUBPASS_EXTERNAL;
  before.dstSubpass = 0;
  before.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
  before.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  before.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  const VkSubpassDependency dependencies[2] = {before, after};
  VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  passInfo.attachmentCount = 1;
  passInfo.pAttachments = &attachment;
  passInfo.subpassCount = 1;
  passInfo.pSubpasses = &subpass;
  passInfo.dependencyCount = 2;
  passInfo.pDependencies = dependencies;
  if (!gpu.check(vkCreateRenderPass(d, &passInfo, nullptr, &pass_), "focus patch pass")) return false;
  VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fbInfo.renderPass = pass_;
  fbInfo.attachmentCount = 1;
  fbInfo.pAttachments = &image_.view;
  fbInfo.width = kWidth;
  fbInfo.height = kRows;
  fbInfo.layers = 1;
  if (!gpu.check(vkCreateFramebuffer(d, &fbInfo, nullptr, &framebuffer_), "focus patch framebuffer"))
    return false;

  // The planes as the eye passes sample them (DirectVideo), pushed per draw:
  // the full-size level, bilinear (the shader takes several taps).
  VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
  samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW =
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (!gpu.check(vkCreateSampler(d, &samplerInfo, nullptr, &sampler_), "focus patch sampler"))
    return false;
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
  if (!gpu.check(vkCreateDescriptorSetLayout(d, &setInfo, nullptr, &setLayout_), "focus patch set layout"))
    return false;
  VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
  auto makeLayout = [&](VkDescriptorSetLayout set, VkPipelineLayout& layout) {
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &set;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    return gpu.check(vkCreatePipelineLayout(d, &layoutInfo, nullptr, &layout), "focus patch layout");
  };
  if (!makeLayout(setLayout_, layout_)) return false;
  pipeline_ = fullscreenPipeline(gpu, layout_, pass_, kSpirv_depth_patch_frag,
                                 sizeof(kSpirv_depth_patch_frag), 0, VK_COLOR_COMPONENT_R_BIT,
                                 "focus patch pipeline");
  if (!pipeline_) return false;
  if (direct.acceptsQ10c()) {
    if (makeLayout(direct.tp10SetLayout(), tp10Layout_))
      tp10Pipeline_ = fullscreenPipeline(gpu, tp10Layout_, pass_, kSpirv_depth_patch_tp10_frag,
                                         sizeof(kSpirv_depth_patch_tp10_frag),
                                         VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
                                         VK_COLOR_COMPONENT_R_BIT, "focus patch pipeline");
    if (!tp10Pipeline_) std::printf("Focus: no automatic focus on Main 10 video\n");
  }
  if (direct.acceptsQ08c()) {
    bool all = true;
    for (int s = 0; s < kQ08cSitings; ++s) {
      if (makeLayout(direct.q08cSetLayouts()[s], q08cLayout_[s]))
        q08cPipeline_[s] = fullscreenPipeline(gpu, q08cLayout_[s], pass_, kSpirv_depth_patch_ycbcr_frag,
                                              sizeof(kSpirv_depth_patch_ycbcr_frag), 0,
                                              VK_COLOR_COMPONENT_R_BIT, "focus patch pipeline");
      all = all && q08cPipeline_[s];
    }
    if (!all) std::printf("Focus: no automatic focus on some 8-bit video\n");
  }

  // Read back by the CPU: cached memory if there is such.
  VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bufferInfo.size = sizeof(float) * kWidth * kRows;
  bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (!gpu.check(vkCreateBuffer(d, &bufferInfo, nullptr, &buffer_), "focus patch buffer")) return false;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(d, buffer_, &req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = gpu.memoryType(
      req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
  if (alloc.memoryTypeIndex == UINT32_MAX)
    alloc.memoryTypeIndex = gpu.memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
  if (alloc.memoryTypeIndex == UINT32_MAX ||
      !gpu.check(vkAllocateMemory(d, &alloc, nullptr, &memory_), "focus patch memory") ||
      !gpu.check(vkBindBufferMemory(d, buffer_, memory_, 0), "focus patch memory") ||
      !gpu.check(vkMapMemory(d, memory_, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&mapped_)),
                 "mapping the focus patch"))
    return false;
  VkPhysicalDeviceMemoryProperties props;
  vkGetPhysicalDeviceMemoryProperties(gpu.physicalDevice(), &props);
  coherent_ = (props.memoryTypes[alloc.memoryTypeIndex].propertyFlags &
               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
  gpu.countAllocation(req.size);
  return true;
}

DepthPatchDrawer* DirectDepth::source(DirectFrame* frame) {
  source_ = gpu_ && frame && (!frame->tp10() || tp10Pipeline_) &&
                    (frame->q08c() < 0 || q08cPipeline_[frame->q08c()])
                ? frame
                : nullptr;
  return source_ ? this : nullptr;
}

bool DirectDepth::draw(const DepthPatch& patch) {
  if (!source_ || frame_ || patch.width > static_cast<int>(kWidth) ||
      2 * patch.height + DepthEstimator::kThumbH > static_cast<int>(kRows))
    return false;
  frame_ = source_;
  direct_->pin(*frame_);
  patch_ = patch;
  done_ = 0;
  return true;
}

void DirectDepth::submit() {
  if (!frame_) return;
  DirectFrame& frame = *frame_;
  frame_ = nullptr;
  VkCommandBuffer cmd = gpu_->beginOneShot();
  if (!cmd) {
    direct_->unpin(frame, 0);
    done_ = UINT64_MAX;  // never: the measurement waits for the next one
    return;
  }
  direct_->recordBarriers(cmd, frame, true);
  const int w = patch_.width, h = patch_.height;
  const uint32_t rows = static_cast<uint32_t>(2 * h + DepthEstimator::kThumbH);
  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  begin.renderPass = pass_;
  begin.framebuffer = framebuffer_;
  begin.renderArea = {{0, 0}, {static_cast<uint32_t>(w), rows}};
  vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
  const int q08c = frame.q08c();
  const VkPipelineLayout layout = frame.tp10() ? tp10Layout_ : q08c >= 0 ? q08cLayout_[q08c] : layout_;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    frame.tp10() ? tp10Pipeline_ : q08c >= 0 ? q08cPipeline_[q08c] : pipeline_);
  if (frame.tp10()) {
    direct_->bindTp10(cmd, layout, frame);
  } else if (q08c >= 0) {
    // The one image, through the immutable sampler of its siting's set layout.
    const VkDescriptorImageInfo image{VK_NULL_HANDLE, frame.texture().view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    gpu_->cmdPushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &write);
  } else {
    VkDescriptorImageInfo images[2] = {};
    VkWriteDescriptorSet writes[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    for (uint32_t i = 0; i < 2; ++i) {
      images[i].sampler = sampler_;
      images[i].imageView = (i == 0 ? frame.texture() : frame.chroma()).view;
      images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[i].pImageInfo = &images[i];
    }
    gpu_->cmdPushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, writes);
  }
  // As DepthEstimator::request's draws: both eyes over the same angles,
  // rows 0.. left, then right, then the thumbnail of the left eye.
  Push p{};
  for (int i = 0; i < 4; ++i) {
    p.yuvRange[i] = frame.color().range[i];
    p.yuvMatrix[i] = frame.color().matrix[i];
  }
  p.start[0] = patch_.start[0];
  p.start[1] = patch_.start[1];
  p.eyeScale[0] = patch_.sbs ? 0.5f : 1.0f;
  p.eyeScale[1] = patch_.sbs ? 1.0f : 0.5f;
  p.alphaScale[0] = patch_.alphaScale[0];
  p.alphaScale[1] = patch_.alphaScale[1];
  p.step = patch_.step;
  p.fisheyeFov = patch_.fisheyeFov;
  p.projection = patch_.projection;
  p.flip = patch_.flip;
  p.alphaPacked = patch_.alphaPacked;
  p.greenKey = patch_.greenKey;
  for (int part = 0; part < 3; ++part) {
    const bool thumb = part == 2;
    const VkRect2D area{{0, part * h},
                        {static_cast<uint32_t>(thumb ? DepthEstimator::kThumbW : w),
                         static_cast<uint32_t>(thumb ? DepthEstimator::kThumbH : h)}};
    const VkViewport viewport{0.0f, static_cast<float>(area.offset.y), static_cast<float>(area.extent.width),
                              static_cast<float>(area.extent.height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &area);
    p.rect[0] = 0.0f;
    p.rect[1] = static_cast<float>(area.offset.y);
    p.rect[2] = static_cast<float>(area.extent.width);
    p.rect[3] = static_cast<float>(area.extent.height);
    const int eye = thumb ? 0 : part;
    p.eyeOffset[0] = thumb || !patch_.sbs ? 0.0f : 0.5f * eye;
    p.eyeOffset[1] = thumb || patch_.sbs ? 0.0f : 0.5f * eye;
    p.alphaCenter[0] = eye == 0 ? 0.5f : 0.0f;  // as SceneRenderer
    p.alphaCenter[1] = 0.0f;
    p.mode = thumb ? 1 : 0;
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
    vkCmdDraw(cmd, 3, 1, 0, 0);
  }
  vkCmdEndRenderPass(cmd);
  VkBufferImageCopy copy{};
  copy.bufferRowLength = static_cast<uint32_t>(w);
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {static_cast<uint32_t>(w), rows, 1};
  vkCmdCopyImageToBuffer(cmd, image_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer_, 1, &copy);
  VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  host.buffer = buffer_;
  host.size = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,
                       1, &host, 0, nullptr);
  direct_->recordBarriers(cmd, frame, false);
  // The frame's last submission gave its images back to the converter's side
  // first: the acquire above is ordered after that release.
  const uint64_t value = gpu_->submitOneShot(cmd, {direct_->useWait(frame)});
  direct_->unpin(frame, value);
  done_ = value ? value : UINT64_MAX;
}

bool DirectDepth::ready() {
  if (!done_ || done_ == UINT64_MAX || gpu_->completed() < done_) {
    // A draw that could not be submitted: no values, the next request goes on.
    if (done_ == UINT64_MAX) {
      done_ = 0;
      return true;
    }
    return false;
  }
  return true;
}

const float* DirectDepth::values() {
  if (!done_) return nullptr;
  done_ = 0;
  if (!coherent_) {
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = memory_;
    range.size = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(gpu_->device(), 1, &range);
  }
  return mapped_;
}
