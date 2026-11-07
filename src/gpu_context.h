#pragma once

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

// What the renderer needs of the Vulkan device beyond its core set, decided
// before XrContext creates the device.
struct GpuRequirements {
  bool mirror = false;     // the desktop mirror window's swapchain (docs/vulkan-renderer.md, 6)
  bool foveation = false;  // fragment density maps with offsets (5)
  bool directVideo = false;  // decoded frames sampled from their DMA-BUFs (11)
  // The instance extensions the mirror window's surface needs
  // (glfwGetRequiredInstanceExtensions).
  std::vector<const char*> mirrorInstanceExtensions;
};

// What device setup found and enabled (XrContext::createVulkanDevice).
struct GpuCapabilities {
  bool samplerAnisotropy = false;
  float maxAnisotropy = 1.0f;  // 1 without the feature
  bool memoryBudget = false;   // VK_EXT_memory_budget
  bool mirror = false;         // requested and enabled
  bool presentFences = false;  // ... with VK_EXT_swapchain_maintenance1's present fences
  bool foveation = false;      // fragment density maps with QCOM offsets
  bool directVideo = false;    // requested, and DMA-BUF images with DRM modifiers
  bool foreignQueue = false;   // ... and VK_EXT_queue_family_foreign
  bool tp10 = false;           // ... and descriptor buffers: Q10C frames sampled as TP10
  bool q08c = false;           // ... and Y'CbCr conversions: Q08C frames sampled as NV12
  VkExtent2D densityOffsetGranularity{1, 1};
  VkExtent2D minDensityTexelSize{}, maxDensityTexelSize{};
  uint32_t maxPushDescriptors = 0;
  float timestampPeriod = 0;  // nanoseconds per tick
  uint32_t timestampValidBits = 0;
  VkDeviceSize nonCoherentAtomSize = 1;
  VkDeviceSize bufferCopyOffsetAlignment = 4;
  uint32_t maxImageDimension2D = 0;
  uint32_t vendorId = 0, deviceId = 0;
  VkDriverId driverId{};
  uint8_t deviceUuid[VK_UUID_SIZE] = {};
  uint8_t driverUuid[VK_UUID_SIZE] = {};
  uint8_t pipelineCacheUuid[VK_UUID_SIZE] = {};
};

// The device XrContext made through the runtime; GpuContext does not own it.
struct GpuDevice {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  uint32_t queueFamily = 0;
  VkQueue queue = VK_NULL_HANDLE;
  GpuCapabilities caps;
};

// A Vulkan image with memory and a view of all its levels (GpuContext::createImage).
struct GpuImage {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0, levels = 1;
  VkDeviceSize bytes = 0;
};

struct GpuBuffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapped = nullptr;  // host-visible buffers stay mapped
  bool coherent = false;
  VkDeviceSize size = 0;
};

// A semaphore wait for a submission. Timeline semaphores wait for `value`;
// binary ones ignore it.
struct SemaphoreWait {
  VkSemaphore semaphore = VK_NULL_HANDLE;
  uint64_t value = 0;
  VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
};

// Shared Vulkan state for everything that draws: the device's handles, memory
// helpers, the frame timeline and the frames in flight, deferred destruction
// and the pipeline cache. All calls on the main thread.
//
// Submission N (counting successful ones from 1) signals the frame timeline
// (frameTimeline()) to N. GL imports that semaphore too (gl_interop.h), so the
// one counter tells both APIs how far Vulkan has got.
class GpuContext {
 public:
  static constexpr int kFramesInFlight = 2;
  // Timestamps a frame can write (SceneRenderer).
  static constexpr uint32_t kTimestamps = 8;

  struct FrameSlot {
    int index = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    uint64_t completion = 0;  // the frame timeline value of its last submission
    uint32_t timestampsWritten = 0;  // by that submission
  };

  GpuContext() = default;
  GpuContext(const GpuContext&) = delete;
  GpuContext& operator=(const GpuContext&) = delete;
  // Waits for the GPU, then destroys what is still retired and its own objects.
  ~GpuContext();

  bool init(const GpuDevice& device);

  VkDevice device() const { return device_.device; }
  VkPhysicalDevice physicalDevice() const { return device_.physical; }
  VkInstance instance() const { return device_.instance; }
  VkQueue queue() const { return device_.queue; }
  uint32_t queueFamily() const { return device_.queueFamily; }
  const GpuCapabilities& caps() const { return device_.caps; }

  // A Vulkan call's result: false (and logged) on failure. Device loss stops
  // the app (lost()).
  bool check(VkResult result, const char* what);
  bool lost() const { return lost_; }

  // Memory: the first type allowed by `bits` with all of `want`, preferring
  // one without `avoid`; UINT32_MAX if none.
  uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags want,
                      VkMemoryPropertyFlags avoid = 0) const;
  // A 2D image in device-local memory with a view of all its levels (none
  // for an image only copied to and from).
  bool createImage(uint32_t width, uint32_t height, uint32_t levels, VkFormat format,
                   VkImageUsageFlags usage, GpuImage& out, const char* what);
  bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, GpuBuffer& out,
                    const char* what);
  // Destroys at once; only for objects the GPU never used (failed setup).
  void destroyNow(GpuImage& image);
  void destroyNow(GpuBuffer& buffer);
  // Destroys after the submissions that may use it, through retire().
  void retire(GpuImage& image);
  void retire(GpuBuffer& buffer);
  // Makes the host's writes to a mapped buffer visible to the GPU.
  void flush(const GpuBuffer& buffer, VkDeviceSize offset, VkDeviceSize size);

  // Device memory we allocated, live and waiting for destruction.
  VkDeviceSize liveBytes() const { return liveBytes_; }
  VkDeviceSize retiredBytes() const { return retiredBytes_; }
  void countAllocation(VkDeviceSize bytes) { liveBytes_ += bytes; }

  VkSemaphore frameTimeline() const { return frameTimeline_; }
  uint64_t submitted() const { return submitted_; }
  // The frame timeline's value now (the latest submission the GPU finished).
  uint64_t completed();

  // The slot for the next submission, its command buffer reset and begun,
  // once the GPU has finished the slot's last submission (a wait, measured
  // by the caller). Its timestamps are those of that submission, unread.
  // nullptr on failure.
  FrameSlot* beginFrame();
  // Ends the slot's command buffer and submits it, waiting for `waits` and
  // signalling the frame timeline (and `signals`, binary semaphores). The
  // new submission's value, 0 on failure (nothing was submitted).
  uint64_t submit(FrameSlot& slot, const std::vector<SemaphoreWait>& waits,
                  const std::vector<VkSemaphore>& signals = {});
  // A frame that was begun but is not submitted (the eye images could not
  // be had): its command buffer is dropped.
  void abandonFrame(FrameSlot& slot);
  // The timestamps of the slot's last submission, in ticks masked to the
  // valid bits; false if they are not all there (never waits).
  bool readTimestamps(const FrameSlot& slot, uint64_t* ticks);

  // Work outside the frames (setup, a new texture's first transition): a
  // command buffer to record into, then submitted on its own as the next
  // frame timeline value (returned, 0 on failure), its GPU work after
  // `waits`. Never waits on the CPU.
  VkCommandBuffer beginOneShot();
  uint64_t submitOneShot(VkCommandBuffer cmd, const std::vector<SemaphoreWait>& waits = {});

  // Runs `destroy` once the GPU has finished every submission made so far
  // (and the one being recorded), and GL has passed glValue on the GL
  // timeline (setGlTimeline). bytes: device memory it frees, for liveBytes().
  void retire(std::function<void()> destroy, uint64_t glValue = 0, VkDeviceSize bytes = 0);
  // The semaphore GL signals when it is done with shared images
  // (gl_interop.h); none once GL has finished everything.
  void setGlTimeline(VkSemaphore glDone) { glTimeline_ = glDone; }
  // Destroys what both timelines have passed. Never waits.
  void collect();
  // Startup and shutdown only: waits until the queue is idle.
  void drain();

  // Loaded from cacheDir() at init, saved by savePipelineCache().
  VkPipelineCache pipelineCache() const { return pipelineCache_; }
  void savePipelineCache();

  // Extension entry points.
  PFN_vkCmdPushDescriptorSetKHR cmdPushDescriptorSet = nullptr;
  PFN_vkGetMemoryFdKHR getMemoryFd = nullptr;
  PFN_vkGetSemaphoreFdKHR getSemaphoreFd = nullptr;

 private:
  struct Retired {
    std::function<void()> destroy;
    uint64_t vkValue = 0, glValue = 0;
    VkDeviceSize bytes = 0;
  };
  bool createPipelineCache();
  uint64_t currentUse() const { return recording_ ? submitted_ + 1 : submitted_; }

  GpuDevice device_;
  std::atomic<bool> lost_{false};  // set by check() from any thread (pipeline workers)
  VkPhysicalDeviceMemoryProperties memory_{};
  VkSemaphore frameTimeline_ = VK_NULL_HANDLE;
  VkSemaphore glTimeline_ = VK_NULL_HANDLE;
  uint64_t submitted_ = 0;
  uint64_t frameSubmissions_ = 0;  // of frames (beginFrame()), choosing the next slot
  bool recording_ = false;
  FrameSlot slots_[kFramesInFlight];
  VkCommandPool oneShotPool_ = VK_NULL_HANDLE;
  std::deque<Retired> retired_;
  VkDeviceSize liveBytes_ = 0, retiredBytes_ = 0;
  VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
  std::string pipelineCachePath_;
};

// Records levels 1 on of `image` made from level 0 by linear blits. Level 0
// must be in TRANSFER_SRC_OPTIMAL with its writes visible to transfers; the
// other levels' contents are discarded once `readers` (the stages that last
// sampled them) are done. Leaves every level SHADER_READ_ONLY_OPTIMAL,
// visible to fragment shaders.
void recordMipmaps(VkCommandBuffer cmd, VkImage image, uint32_t width, uint32_t height,
                   uint32_t levels, VkPipelineStageFlags readers);
