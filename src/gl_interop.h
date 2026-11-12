#pragma once

#include "gl_util.h"
#include "gpu_context.h"
#include "texture.h"

#include <cstdint>
#include <memory>
#include <vector>

// An image GL renders (or uploads) into and the scene samples: Vulkan's
// memory, imported into GL as a texture with a framebuffer on level 0
// (docs/vulkan-renderer.md, 3.1). GlInterop alone changes its state.
class SharedImage {
 public:
  const Texture& texture() const { return texture_; }
  GLuint glTexture() const { return glTexture_; }
  GLuint framebuffer() const { return framebuffer_; }  // level 0
  // GL holds it since its last acquireForGl(), until the next publish().
  bool inGl() const { return inGl_; }
  // The glDone value of the publication that brought its picture: new
  // content when it changes (not when GL only read it, or made mipmaps).
  uint64_t content() const { return contentAt_; }

 private:
  friend class GlInterop;
  Texture texture_;
  VkDeviceMemory memory_ = VK_NULL_HANDLE;
  VkDeviceSize bytes_ = 0;
  GLuint memoryObject_ = 0, glTexture_ = 0, framebuffer_ = 0;
  bool inGl_ = false;
  bool keepInGl_ = false;  // not handed to Vulkan at the next publish()
  // Who had it last: a Vulkan frame (released at frame timeline value
  // lastVulkanUse_) or GL (published at glDone value publishedAt_).
  bool vulkanLast_ = true;
  uint64_t lastVulkanUse_ = 0, publishedAt_ = 0, lastGlUse_ = 0;
  uint64_t contentAt_ = 0;
  bool written_ = false;  // a new picture since its last publication
  bool inFrame_ = false;  // acquired by the frame being recorded
};

// Sharing GL-rendered images with the Vulkan scene: GL's external memory and
// semaphore entry points, the two timelines (GpuContext's frame timeline,
// which GL waits on, and glDone, which GL signals and Vulkan waits on), and
// the ownership of each SharedImage between the two (3.3).
//
// Per loop iteration, on the main thread with the GL context current:
// acquireForGl() before GL touches an image, one publish() once all GL work
// is issued, then the frame: useInFrame() while recording, recordAcquires()
// before the eye passes and recordReleases() after them, frameWaits() for the
// submission and frameSubmitted() or frameAbandoned() after it.
class GlInterop {
 public:
  GlInterop() = default;
  GlInterop(const GlInterop&) = delete;
  GlInterop& operator=(const GlInterop&) = delete;
  // Waits for GL (shutdown), then gives the timeline to GpuContext's
  // retirement.
  ~GlInterop();

  // After GpuContext::init, with the GL context current: the GL entry points,
  // the GL and Vulkan device and driver match, the timelines.
  bool init(GpuContext& gpu);

  // An image of `levels` mip levels (R8G8B8A8_SRGB, mutable to UNORM),
  // released to GL after a first layout transition on the GPU. nullptr if it
  // could not be made (logged with `what`).
  std::unique_ptr<SharedImage> create(uint32_t width, uint32_t height, uint32_t levels,
                                      const char* what);
  // Destroys it once neither API uses it any more.
  void retire(std::unique_ptr<SharedImage> image);
  // The largest side create() can make.
  uint32_t maxSize() const { return maxSize_; }

  // Before GL reads or writes `image`: GL waits (on the GPU) for its last
  // use by Vulkan. Nothing if GL holds it already.
  void acquireForGl(SharedImage& image);
  // The same before GL writes a new picture into it (its content() changes
  // at the next publication).
  void acquireForWrite(SharedImage& image) {
    acquireForGl(image);
    image.written_ = true;
  }
  // An image GL is not done with (a photo half uploaded) stays with GL at
  // publish(), out of the scene, until this is called with false.
  void keepInGl(SharedImage& image, bool keep) { image.keepInGl_ = keep; }
  // Hands every image GL acquired to Vulkan in one glDone signal, then
  // flushes GL. Nothing to signal if GL touched none.
  void publish();

  // The frame being recorded samples `image` (not one GL holds).
  bool useInFrame(SharedImage& image);
  void recordAcquires(VkCommandBuffer cmd);
  void recordReleases(VkCommandBuffer cmd);
  // What the frame's submission waits for: at most one value per timeline.
  void frameWaits(std::vector<SemaphoreWait>& waits) const;
  // The frame was submitted as frame timeline value `value`, or was not.
  void frameSubmitted(uint64_t value);
  void frameAbandoned();

 private:
  bool loadGl();
  bool importSemaphore(VkSemaphore semaphore, GLuint& out, const char* what);
  void destroyGl(SharedImage& image);

  GpuContext* gpu_ = nullptr;
  VkSemaphore glDone_ = VK_NULL_HANDLE;
  GLuint glDoneGl_ = 0, vkFrameGl_ = 0;
  uint64_t glDoneValue_ = 0;  // signalled last
  bool signalNeeded_ = false;  // GL work since the last signal that something waits for
  std::vector<SharedImage*> inGl_;     // acquired by GL in this window
  std::vector<SharedImage*> inFrame_;  // used by the frame being recorded
  VkImageUsageFlags usage_ = 0;
  uint32_t maxSize_ = 0;
  uint32_t live_ = 0;
};
