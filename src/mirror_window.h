#pragma once

#include "gpu_context.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

struct EyeImage;
struct GLFWwindow;
struct Texture;

// The desktop mirror (docs/vulkan-renderer.md, 6): a window without a client
// API (the GL context's window stays hidden) whose swapchain gets the left
// eye, menus included, scaled to fit. A frame copies into it only if an
// image is free at once, and it is presented after xrEndFrame, so the
// headset never waits for the desktop.
class MirrorWindow {
 public:
  // What its surface needs of the Vulkan instance (none without GLFW's
  // Vulkan support).
  static std::vector<const char*> instanceExtensions();

  MirrorWindow() = default;
  MirrorWindow(const MirrorWindow&) = delete;
  MirrorWindow& operator=(const MirrorWindow&) = delete;
  // Waits for the GPU and its presentations, then destroys its objects.
  ~MirrorWindow();

  // Once the device exists (with GpuCapabilities::mirror). False, with the
  // reason printed, if the window cannot show the eyes (the caller closes it).
  bool init(GpuContext& gpu, GLFWwindow* window, VkFormat eyeFormat);
  bool active() const { return surface_ != VK_NULL_HANDLE && !stopped_; }

  // Takes an image for the frame slot being recorded, if one is free now
  // (never waits). Skipped while the window is minimized or being resized.
  void acquire(const GpuContext::FrameSlot& slot);
  // Records the copy of `eye` (after its pass; it goes back to its
  // attachment layout) into the image taken, and adds the wait for it.
  void record(VkCommandBuffer cmd, const EyeImage& eye, std::vector<SemaphoreWait>& waits);
  // While set, shown instead of the eye, scaled to fit (first-run setup's
  // "Put on your headset"): a canvas's texture, not owned, unset before it goes.
  void setCover(const Texture* cover) { cover_ = cover; }
  // What the frame's submission signals for presentation.
  void addSignals(std::vector<VkSemaphore>& signals) const;
  // After the submission (false: it failed).
  void submitted(bool ok);
  // After xrEndFrame: shows what was copied this frame.
  void present();

 private:
  enum class Image { None, Acquired, Recorded, Submitted };
  bool createSwapchain(uint32_t width, uint32_t height);
  bool destroySwapchain(uint64_t timeout);
  bool createSemaphores();
  void destroySemaphores();
  void stop(const char* why);

  GpuContext* gpu_ = nullptr;
  GLFWwindow* window_ = nullptr;
  const Texture* cover_ = nullptr;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  bool stopped_ = false;
  VkSurfaceFormatKHR format_{};
  VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkExtent2D extent_{};
  uint32_t madeWidth_ = 0, madeHeight_ = 0;  // the window's size it was made for
  std::vector<VkImage> images_;
  std::vector<VkSemaphore> presentReady_;  // per image, signalled by the frame
  std::vector<VkFence> presented_;         // per image, with present fences
  std::vector<bool> fenceUsed_;
  VkSemaphore acquired_[GpuContext::kFramesInFlight] = {};  // per frame slot
  // This frame's image.
  Image state_ = Image::None;
  uint32_t index_ = 0;
  int slot_ = 0;
  // A swapchain to replace once the window's size has settled.
  bool outdated_ = false;
  int pendingWidth_ = 0, pendingHeight_ = 0;
  double pendingSince_ = 0;
};
