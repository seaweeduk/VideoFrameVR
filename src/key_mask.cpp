#include "key_mask.h"

#include "pipelines.h"

#include <algorithm>
#include <cmath>
#include <cstring>

KeyMask::~KeyMask() {
  if (!gpu_) return;
  VkDevice d = gpu_->device();
  const VkRenderPass passes[2] = {pass_[0], pass_[1]};
  const VkFramebuffer framebuffers[2] = {framebuffer_[0], framebuffer_[1]};
  const VkDescriptorPool pool = pool_;
  gpu_->retire([d, passes, framebuffers, pool] {
    for (VkFramebuffer f : framebuffers)
      if (f) vkDestroyFramebuffer(d, f, nullptr);
    for (VkRenderPass p : passes)
      if (p) vkDestroyRenderPass(d, p, nullptr);
    if (pool) vkDestroyDescriptorPool(d, pool, nullptr);
  });
  gpu_->retire(distances_);
  gpu_->retire(mask_);
}

bool KeyMask::initPasses(GpuContext& gpu) {
  gpu_ = &gpu;
  const VkFormat formats[2] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM};
  for (int i = 0; i < 2; ++i) {
    // A pass draws one rectangle whole (loadOp concerns that alone); the
    // eye passes (and the next pass) of earlier frames may still read what
    // it held. The rest of the image keeps what it holds (another eye's
    // rectangle, an earlier frame's spare), so it starts from the layout the
    // passes leave it in, as init() first puts it.
    VkAttachmentDescription attachment{};
    attachment.format = formats[i];
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    // After earlier reads of the image (eye passes, the reach pass); a pass
    // over another rectangle earlier in the frame reaches it through `after`.
    VkSubpassDependency before{};
    before.srcSubpass = VK_SUBPASS_EXTERNAL;
    before.dstSubpass = 0;
    before.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    before.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    before.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkSubpassDependency after{};
    after.srcSubpass = 0;
    after.dstSubpass = VK_SUBPASS_EXTERNAL;
    after.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    after.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    after.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    const VkSubpassDependency dependencies[2] = {before, after};
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 1;
    info.pAttachments = &attachment;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = dependencies;
    if (!gpu.check(vkCreateRenderPass(gpu.device(), &info, nullptr, &pass_[i]), "key mask pass"))
      return false;
  }
  return true;
}

bool KeyMask::init(const Pipelines& pipelines) {
  VkDevice d = gpu_->device();
  const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (!gpu_->createImage(kSize, kSize, 1, VK_FORMAT_R8_UNORM, usage, distances_, "key distances") ||
      !gpu_->createImage(kSize, kSize, 1, VK_FORMAT_R8G8_UNORM, usage, mask_, "key mask"))
    return false;
  // Both images in the passes' layout once, before the first frame (their
  // content is undefined: nothing reads outside what a frame made).
  VkCommandBuffer cmd = gpu_->beginOneShot();
  if (!cmd) return false;
  VkImageMemoryBarrier barriers[2] = {{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER},
                                      {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}};
  const VkImage images[2] = {distances_.image, mask_.image};
  for (int i = 0; i < 2; ++i) {
    barriers[i].srcAccessMask = 0;
    barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[i].srcQueueFamilyIndex = barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[i].image = images[i];
    barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  }
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                       0, nullptr, 0, nullptr, 2, barriers);
  if (gpu_->submitOneShot(cmd) == 0) return false;
  distanceTexture_.image = distances_.image;
  distanceTexture_.view = distances_.view;
  distanceTexture_.width = distanceTexture_.height = kSize;
  const VkImageView views[2] = {distances_.view, mask_.view};
  for (int i = 0; i < 2; ++i) {
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = pass_[i];
    fb.attachmentCount = 1;
    fb.pAttachments = &views[i];
    fb.width = fb.height = kSize;
    fb.layers = 1;
    if (!gpu_->check(vkCreateFramebuffer(d, &fb, nullptr, &framebuffer_[i]), "key mask framebuffer"))
      return false;
  }
  const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
  VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  poolInfo.maxSets = 1;
  poolInfo.poolSizeCount = 1;
  poolInfo.pPoolSizes = &size;
  if (!gpu_->check(vkCreateDescriptorPool(d, &poolInfo, nullptr, &pool_), "key mask pool"))
    return false;
  const VkDescriptorSetLayout layout = pipelines.keyMaskSetLayout();
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = pool_;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &layout;
  if (!gpu_->check(vkAllocateDescriptorSets(d, &alloc, &set_), "key mask set")) return false;
  const VkDescriptorImageInfo image{pipelines.sampler(Pipelines::kVideoBase), mask_.view,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = set_;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = &image;
  vkUpdateDescriptorSets(d, 1, &write, 0, nullptr);
  return true;
}

uint32_t KeyMask::texelsPer(uint32_t width, uint32_t height) {
  return std::max(1u, (std::max(width, height) + kSize - 1) / kSize);
}

void KeyMask::scale(uint32_t width, uint32_t height, float* scale) {
  const float per = static_cast<float>(texelsPer(width, height)) * kSize;
  scale[0] = width / per;
  scale[1] = height / per;
}

KeyMask::Rect KeyMask::needed(const float* stereo, const UvBounds& b, uint32_t width,
                              uint32_t height) {
  const uint32_t per = texelsPer(width, height);
  const Rect part{0, 0, (width + per - 1) / per, (height + per - 1) / per};
  return maskTexels(stereo, b, static_cast<float>(width) / per, static_cast<float>(height) / per,
                    part);
}

bool KeyMask::current(const void* image, uint64_t generation, const KeyMaskPush& push,
                      const Rect* needs, int count, uint32_t width, uint32_t height) {
  const uint32_t per = texelsPer(width, height);
  const Rect part{0, 0, (width + per - 1) / per, (height + per - 1) / per};
  if (count <= 0) {
    needs = &part;
    count = 1;
  }
  if (made_ && image == image_ && generation == generation_ &&
      std::memcmp(&push, &push_, sizeof push) == 0) {
    bool inside = true;
    for (int i = 0; i < count && inside; ++i) {
      bool in = false;
      for (int m = 0; m < madeCount_; ++m) in = in || within(needs[i], madeRects_[m]);
      inside = in;
    }
    if (inside) return true;
  }
  made_ = true;
  image_ = image;
  generation_ = generation;
  push_ = push;
  madeCount_ = std::min(count, 2);
  for (int i = 0; i < madeCount_; ++i)
    madeRects_[i] = needs == &part ? part : grown(needs[i], kSpare, part);
  madeCount_ = merged(madeRects_, madeCount_);
  // key_reach.frag reads distances up to the shrink's and spill removal's
  // reach away (frame texels), bilinear.
  const uint32_t reach =
      static_cast<uint32_t>(std::ceil((push.key[2] + push.key[3]) / static_cast<float>(per))) + 1;
  distanceCount_ = madeCount_;
  for (int i = 0; i < distanceCount_; ++i) distanceRects_[i] = grown(madeRects_[i], reach, part);
  distanceCount_ = merged(distanceRects_, distanceCount_);
  return false;
}

void KeyMask::begin(VkCommandBuffer cmd, int pass, uint32_t width, uint32_t height,
                    const Rect& rect) const {
  const uint32_t per = texelsPer(width, height);
  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  begin.renderPass = pass_[pass];
  begin.framebuffer = framebuffer_[pass];
  begin.renderArea = {{static_cast<int32_t>(rect.x0), static_cast<int32_t>(rect.y0)},
                      {rect.x1 - rect.x0, rect.y1 - rect.y0}};
  vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
  const VkViewport viewport{0, 0, static_cast<float>((width + per - 1) / per),
                            static_cast<float>((height + per - 1) / per), 0, 1};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &begin.renderArea);
}

void KeyMask::end(VkCommandBuffer cmd) const { vkCmdEndRenderPass(cmd); }
