#include "gpu_context.h"

#include "settings.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {

// A slot's or one-shot submission's completion: far longer than any frame,
// so only a hung GPU reaches it.
constexpr uint64_t kWaitTimeoutNs = 10'000'000'000ull;
constexpr size_t kMaxPipelineCacheBytes = 64u << 20;

}  // namespace

GpuContext::~GpuContext() {
  if (device_.device == VK_NULL_HANDLE) return;
  drain();
  while (!retired_.empty()) {
    retired_.front().destroy();
    retired_.pop_front();
  }
  VkDevice d = device_.device;
  for (FrameSlot& s : slots_) {
    if (s.queries) vkDestroyQueryPool(d, s.queries, nullptr);
    if (s.pool) vkDestroyCommandPool(d, s.pool, nullptr);
  }
  if (oneShotPool_) vkDestroyCommandPool(d, oneShotPool_, nullptr);
  if (pipelineCache_) vkDestroyPipelineCache(d, pipelineCache_, nullptr);
  if (frameTimeline_) vkDestroySemaphore(d, frameTimeline_, nullptr);
}

bool GpuContext::check(VkResult result, const char* what) {
  if (result == VK_SUCCESS) return true;
  std::fprintf(stderr, "Vulkan: %s failed (%d)\n", what, static_cast<int>(result));
  if (result == VK_ERROR_DEVICE_LOST) lost_ = true;
  return false;
}

bool GpuContext::init(const GpuDevice& device) {
  device_ = device;
  VkDevice d = device_.device;
  vkGetPhysicalDeviceMemoryProperties(device_.physical, &memory_);
  cmdPushDescriptorSet = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
      vkGetDeviceProcAddr(d, "vkCmdPushDescriptorSetKHR"));
  getMemoryFd =
      reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(d, "vkGetMemoryFdKHR"));
  getSemaphoreFd =
      reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(d, "vkGetSemaphoreFdKHR"));
  if (!cmdPushDescriptorSet || !getMemoryFd || !getSemaphoreFd) {
    std::fprintf(stderr, "Vulkan: the device lacks push descriptors or external memory/semaphore "
                         "file descriptors\n");
    return false;
  }

  // The frame timeline, exported so GL can wait on it (gl_interop.cpp).
  VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
  type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  type.initialValue = 0;
  VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
  exportInfo.pNext = &type;
  exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  semaphoreInfo.pNext = &exportInfo;
  if (!check(vkCreateSemaphore(d, &semaphoreInfo, nullptr, &frameTimeline_), "frame timeline"))
    return false;

  for (int i = 0; i < kFramesInFlight; ++i) {
    FrameSlot& s = slots_[i];
    s.index = i;
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = device_.queueFamily;
    if (!check(vkCreateCommandPool(d, &poolInfo, nullptr, &s.pool), "frame command pool"))
      return false;
    VkCommandBufferAllocateInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdInfo.commandPool = s.pool;
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    if (!check(vkAllocateCommandBuffers(d, &cmdInfo, &s.cmd), "frame command buffer"))
      return false;
    if (device_.caps.timestampValidBits > 0) {
      VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
      queryInfo.queryCount = kTimestamps;
      if (!check(vkCreateQueryPool(d, &queryInfo, nullptr, &s.queries), "timestamp queries"))
        return false;
    }
  }
  VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  poolInfo.queueFamilyIndex = device_.queueFamily;
  if (!check(vkCreateCommandPool(d, &poolInfo, nullptr, &oneShotPool_), "one-shot command pool"))
    return false;
  return createPipelineCache();
}

uint32_t GpuContext::memoryType(uint32_t bits, VkMemoryPropertyFlags want,
                                VkMemoryPropertyFlags avoid) const {
  uint32_t fallback = UINT32_MAX;
  for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags flags = memory_.memoryTypes[i].propertyFlags;
    if (!(bits & (1u << i)) || (flags & want) != want) continue;
    if (!(flags & avoid)) return i;
    if (fallback == UINT32_MAX) fallback = i;
  }
  return fallback;
}

bool GpuContext::createImage(uint32_t width, uint32_t height, uint32_t levels, VkFormat format,
                             VkImageUsageFlags usage, GpuImage& out, const char* what) {
  out = GpuImage{};
  out.format = format;
  out.width = width;
  out.height = height;
  out.levels = levels;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = levels;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!check(vkCreateImage(device(), &info, nullptr, &out.image), what)) return false;
  VkMemoryRequirements reqs;
  vkGetImageMemoryRequirements(device(), out.image, &reqs);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = reqs.size;
  alloc.memoryTypeIndex = memoryType(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (alloc.memoryTypeIndex == UINT32_MAX ||
      !check(vkAllocateMemory(device(), &alloc, nullptr, &out.memory), what)) {
    std::fprintf(stderr, "Vulkan: no memory for %s (%ux%u, %.1f MiB)\n", what, width, height,
                 reqs.size / 1048576.0);
    destroyNow(out);
    return false;
  }
  out.bytes = reqs.size;
  liveBytes_ += out.bytes;
  if (!check(vkBindImageMemory(device(), out.image, out.memory, 0), what)) {
    destroyNow(out);
    return false;
  }
  if (!(usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))) return true;
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = out.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
  if (!check(vkCreateImageView(device(), &view, nullptr, &out.view), what)) {
    destroyNow(out);
    return false;
  }
  return true;
}

bool GpuContext::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                              GpuBuffer& out, const char* what) {
  out = GpuBuffer{};
  out.size = size;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (!check(vkCreateBuffer(device(), &info, nullptr, &out.buffer), what)) return false;
  VkMemoryRequirements reqs;
  vkGetBufferMemoryRequirements(device(), out.buffer, &reqs);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = reqs.size;
  // Host-visible: coherent if there is such memory, else flushed (flush()).
  alloc.memoryTypeIndex =
      hostVisible ? memoryType(reqs.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                  : memoryType(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (hostVisible && alloc.memoryTypeIndex == UINT32_MAX)
    alloc.memoryTypeIndex = memoryType(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
  if (alloc.memoryTypeIndex == UINT32_MAX ||
      !check(vkAllocateMemory(device(), &alloc, nullptr, &out.memory), what)) {
    std::fprintf(stderr, "Vulkan: no memory for %s (%.1f MiB)\n", what, size / 1048576.0);
    destroyNow(out);
    return false;
  }
  out.coherent = (memory_.memoryTypes[alloc.memoryTypeIndex].propertyFlags &
                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
  out.size = reqs.size;
  liveBytes_ += out.size;
  if (!check(vkBindBufferMemory(device(), out.buffer, out.memory, 0), what) ||
      (hostVisible && !check(vkMapMemory(device(), out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped),
                             what))) {
    destroyNow(out);
    return false;
  }
  return true;
}

void GpuContext::destroyNow(GpuImage& image) {
  VkDevice d = device();
  if (image.view) vkDestroyImageView(d, image.view, nullptr);
  if (image.image) vkDestroyImage(d, image.image, nullptr);
  if (image.memory) vkFreeMemory(d, image.memory, nullptr);
  liveBytes_ -= image.bytes;
  image = GpuImage{};
}

void GpuContext::destroyNow(GpuBuffer& buffer) {
  VkDevice d = device();
  if (buffer.buffer) vkDestroyBuffer(d, buffer.buffer, nullptr);
  if (buffer.memory) vkFreeMemory(d, buffer.memory, nullptr);  // unmaps too
  liveBytes_ -= buffer.size;
  buffer = GpuBuffer{};
}

void GpuContext::retire(GpuImage& image) {
  if (!image.image) return;
  GpuImage old = image;
  image = GpuImage{};
  retire([this, old]() mutable {
    liveBytes_ += old.bytes;  // destroyNow() takes them off again
    destroyNow(old);
  }, 0, old.bytes);
}

void GpuContext::retire(GpuBuffer& buffer) {
  if (!buffer.buffer) return;
  GpuBuffer old = buffer;
  buffer = GpuBuffer{};
  retire([this, old]() mutable {
    liveBytes_ += old.size;
    destroyNow(old);
  }, 0, old.size);
}

void GpuContext::flush(const GpuBuffer& buffer, VkDeviceSize offset, VkDeviceSize size) {
  if (buffer.coherent || size == 0) return;
  const VkDeviceSize atom = std::max<VkDeviceSize>(1, device_.caps.nonCoherentAtomSize);
  const VkDeviceSize start = offset / atom * atom;
  VkDeviceSize end = (offset + size + atom - 1) / atom * atom;
  end = std::min(end, buffer.size);
  VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
  range.memory = buffer.memory;
  range.offset = start;
  range.size = end == buffer.size ? VK_WHOLE_SIZE : end - start;
  check(vkFlushMappedMemoryRanges(device(), 1, &range), "flushing a staging buffer");
}

uint64_t GpuContext::completed() {
  uint64_t value = 0;
  check(vkGetSemaphoreCounterValue(device(), frameTimeline_, &value), "reading the frame timeline");
  return value;
}

GpuContext::FrameSlot* GpuContext::beginFrame() {
  if (lost_) return nullptr;
  // Slots alternate with the frames actually submitted.
  FrameSlot& slot = slots_[frameSubmissions_ % kFramesInFlight];
  if (slot.completion > 0) {
    VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wait.semaphoreCount = 1;
    wait.pSemaphores = &frameTimeline_;
    wait.pValues = &slot.completion;
    const VkResult result = vkWaitSemaphores(device(), &wait, kWaitTimeoutNs);
    if (result == VK_TIMEOUT) {
      std::fprintf(stderr, "Vulkan: the GPU has not finished a frame in 10 s\n");
      lost_ = true;
      return nullptr;
    }
    if (!check(result, "waiting for a frame slot")) return nullptr;
  }
  if (!check(vkResetCommandPool(device(), slot.pool, 0), "resetting a frame's commands"))
    return nullptr;
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (!check(vkBeginCommandBuffer(slot.cmd, &begin), "beginning a frame")) return nullptr;
  recording_ = true;
  return &slot;
}

uint64_t GpuContext::submit(FrameSlot& slot, const std::vector<SemaphoreWait>& waits,
                            const std::vector<VkSemaphore>& signals) {
  recording_ = false;
  if (!check(vkEndCommandBuffer(slot.cmd), "ending a frame")) return 0;
  std::vector<VkSemaphore> waitSemaphores;
  std::vector<uint64_t> waitValues;
  std::vector<VkPipelineStageFlags> waitStages;
  for (const SemaphoreWait& w : waits) {
    waitSemaphores.push_back(w.semaphore);
    waitValues.push_back(w.value);
    waitStages.push_back(w.stage);
  }
  std::vector<VkSemaphore> signalSemaphores = {frameTimeline_};
  std::vector<uint64_t> signalValues = {submitted_ + 1};
  for (VkSemaphore s : signals) {
    signalSemaphores.push_back(s);
    signalValues.push_back(0);
  }
  VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
  timeline.waitSemaphoreValueCount = static_cast<uint32_t>(waitValues.size());
  timeline.pWaitSemaphoreValues = waitValues.data();
  timeline.signalSemaphoreValueCount = static_cast<uint32_t>(signalValues.size());
  timeline.pSignalSemaphoreValues = signalValues.data();
  VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  info.pNext = &timeline;
  info.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
  info.pWaitSemaphores = waitSemaphores.data();
  info.pWaitDstStageMask = waitStages.data();
  info.commandBufferCount = 1;
  info.pCommandBuffers = &slot.cmd;
  info.signalSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size());
  info.pSignalSemaphores = signalSemaphores.data();
  if (!check(vkQueueSubmit(device_.queue, 1, &info, VK_NULL_HANDLE), "submitting a frame")) {
    // Nothing waits for a value that was never submitted; the app stops.
    lost_ = true;
    return 0;
  }
  slot.completion = ++submitted_;
  ++frameSubmissions_;
  return submitted_;
}

void GpuContext::abandonFrame(FrameSlot& slot) {
  recording_ = false;
  vkEndCommandBuffer(slot.cmd);
  slot.timestampsWritten = 0;
}

bool GpuContext::readTimestamps(const FrameSlot& slot, uint64_t* ticks) {
  if (!slot.queries || slot.timestampsWritten == 0) return false;
  uint64_t results[kTimestamps * 2] = {};
  const VkResult result = vkGetQueryPoolResults(
      device(), slot.queries, 0, slot.timestampsWritten, sizeof(results), results,
      2 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
  if (result != VK_SUCCESS && result != VK_NOT_READY) {
    check(result, "reading timestamps");
    return false;
  }
  const uint32_t bits = device_.caps.timestampValidBits;
  const uint64_t mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
  for (uint32_t i = 0; i < slot.timestampsWritten; ++i) {
    if (results[2 * i + 1] == 0) return false;
    ticks[i] = results[2 * i] & mask;
  }
  return true;
}

VkCommandBuffer GpuContext::beginOneShot() {
  VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  info.commandPool = oneShotPool_;
  info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  info.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (!check(vkAllocateCommandBuffers(device(), &info, &cmd), "one-shot command buffer"))
    return VK_NULL_HANDLE;
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (!check(vkBeginCommandBuffer(cmd, &begin), "beginning one-shot commands")) {
    vkFreeCommandBuffers(device(), oneShotPool_, 1, &cmd);
    return VK_NULL_HANDLE;
  }
  return cmd;
}

uint64_t GpuContext::submitOneShot(VkCommandBuffer cmd, const std::vector<SemaphoreWait>& waits) {
  if (!cmd) return 0;
  // A frame being recorded counts on the next value: one-shot work started
  // meanwhile (a texture made while recording) waits for the frame instead.
  if (recording_) {
    std::fprintf(stderr, "Vulkan: one-shot work submitted while a frame is recorded\n");
    vkFreeCommandBuffers(device(), oneShotPool_, 1, &cmd);
    return 0;
  }
  VkDevice d = device();
  VkCommandPool pool = oneShotPool_;
  if (!check(vkEndCommandBuffer(cmd), "ending one-shot commands")) {
    vkFreeCommandBuffers(d, pool, 1, &cmd);
    return 0;
  }
  const uint64_t value = submitted_ + 1;
  std::vector<VkSemaphore> waitSemaphores;
  std::vector<uint64_t> waitValues;
  std::vector<VkPipelineStageFlags> waitStages;
  for (const SemaphoreWait& w : waits) {
    waitSemaphores.push_back(w.semaphore);
    waitValues.push_back(w.value);
    waitStages.push_back(w.stage);
  }
  VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
  timeline.waitSemaphoreValueCount = static_cast<uint32_t>(waitValues.size());
  timeline.pWaitSemaphoreValues = waitValues.data();
  timeline.signalSemaphoreValueCount = 1;
  timeline.pSignalSemaphoreValues = &value;
  VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  info.pNext = &timeline;
  info.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
  info.pWaitSemaphores = waitSemaphores.data();
  info.pWaitDstStageMask = waitStages.data();
  info.commandBufferCount = 1;
  info.pCommandBuffers = &cmd;
  info.signalSemaphoreCount = 1;
  info.pSignalSemaphores = &frameTimeline_;
  if (!check(vkQueueSubmit(device_.queue, 1, &info, VK_NULL_HANDLE), "submitting one-shot work")) {
    lost_ = true;
    vkFreeCommandBuffers(d, pool, 1, &cmd);
    return 0;
  }
  submitted_ = value;
  retire([d, pool, cmd]() mutable { vkFreeCommandBuffers(d, pool, 1, &cmd); });
  return value;
}

void GpuContext::retire(std::function<void()> destroy, uint64_t glValue, VkDeviceSize bytes) {
  retired_.push_back({std::move(destroy), currentUse(), glValue, bytes});
  liveBytes_ -= bytes;
  retiredBytes_ += bytes;
}

void GpuContext::collect() {
  if (retired_.empty() || lost_) return;
  const uint64_t vk = completed();
  // Without the GL timeline (before GL interop, or after it waited for GL
  // at shutdown) GL holds nothing.
  uint64_t gl = UINT64_MAX;
  if (glTimeline_) check(vkGetSemaphoreCounterValue(device(), glTimeline_, &gl), "GL timeline");
  // In order: later entries never need less than earlier ones on the Vulkan
  // timeline; one still waiting for GL holds back only itself.
  for (auto it = retired_.begin(); it != retired_.end();) {
    if (it->vkValue > vk) break;
    if (it->glValue > gl) {
      ++it;
      continue;
    }
    retiredBytes_ -= it->bytes;
    it->destroy();
    it = retired_.erase(it);
  }
}

void GpuContext::drain() {
  if (device_.queue) vkQueueWaitIdle(device_.queue);
}

bool GpuContext::createPipelineCache() {
  pipelineCachePath_ = (cacheDir() / "vulkan-pipelines.bin").string();
  std::vector<char> data;
  {
    std::ifstream in(pipelineCachePath_, std::ios::binary);
    if (in) {
      in.seekg(0, std::ios::end);
      const std::streamoff size = in.tellg();
      if (size > 0 && static_cast<size_t>(size) <= kMaxPipelineCacheBytes) {
        data.resize(static_cast<size_t>(size));
        in.seekg(0);
        if (!in.read(data.data(), size)) data.clear();
      }
    }
  }
  // Only a cache from this very driver and device is offered to it.
  VkPipelineCacheHeaderVersionOne header{};
  bool usable = data.size() >= sizeof(header);
  if (usable) {
    std::memcpy(&header, data.data(), sizeof(header));
    const GpuCapabilities& caps = device_.caps;
    usable = header.headerSize >= sizeof(header) &&
             header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
             header.vendorID == caps.vendorId && header.deviceID == caps.deviceId &&
             std::memcmp(header.pipelineCacheUUID, caps.pipelineCacheUuid, VK_UUID_SIZE) == 0;
  }
  if (!usable) data.clear();
  VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  info.initialDataSize = data.size();
  info.pInitialData = data.empty() ? nullptr : data.data();
  if (vkCreatePipelineCache(device(), &info, nullptr, &pipelineCache_) != VK_SUCCESS) {
    info.initialDataSize = 0;
    info.pInitialData = nullptr;
    if (!check(vkCreatePipelineCache(device(), &info, nullptr, &pipelineCache_), "pipeline cache"))
      return false;
  }
  std::printf("Vulkan pipeline cache: %s\n", data.empty() ? "new" : "loaded");
  return true;
}

void GpuContext::savePipelineCache() {
  if (!pipelineCache_ || lost_) return;
  size_t size = 0;
  if (vkGetPipelineCacheData(device(), pipelineCache_, &size, nullptr) != VK_SUCCESS || size == 0 ||
      size > kMaxPipelineCacheBytes)
    return;
  std::vector<char> data(size);
  if (vkGetPipelineCacheData(device(), pipelineCache_, &size, data.data()) != VK_SUCCESS) return;
  std::error_code ec;
  std::filesystem::create_directories(cacheDir(), ec);
  const std::string temporary = pipelineCachePath_ + ".tmp-" + std::to_string(getpid());
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out.write(data.data(), static_cast<std::streamsize>(size))) {
      out.close();
      std::filesystem::remove(temporary, ec);
      return;
    }
  }
  std::filesystem::rename(temporary, pipelineCachePath_, ec);
  if (ec) std::filesystem::remove(temporary, ec);
}

void recordMipmaps(VkCommandBuffer cmd, VkImage image, uint32_t width, uint32_t height,
                   uint32_t levels, VkPipelineStageFlags readers) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  if (levels > 1) {
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 1, levels - 1, 0, 1};
    vkCmdPipelineBarrier(cmd, readers | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  }
  int32_t w = static_cast<int32_t>(width), h = static_cast<int32_t>(height);
  for (uint32_t level = 1; level < levels; ++level) {
    const int32_t nw = std::max(1, w / 2), nh = std::max(1, h / 2);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
    blit.srcOffsets[1] = {w, h, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
    blit.dstOffsets[1] = {nw, nh, 1};
    vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    // This level is the next one's source.
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
    w = nw;
    h = nh;
  }
  b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
  b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &b);
}
