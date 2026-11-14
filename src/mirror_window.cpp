#include "mirror_window.h"

#include "texture.h"
#include "xr_context.h"

// With vulkan.h first, GLFW declares its Vulkan functions.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>

namespace {

// A window being dragged to a new size changes it every frame: its swapchain
// is made anew once the size has stayed the same this long.
constexpr double kSettleSeconds = 0.15;
// Waiting for a presentation to let go of its semaphore, which it has
// normally done long before.
constexpr uint64_t kPresentWaitNs = 1'000'000'000;

const char* presentModeName(VkPresentModeKHR mode) {
  switch (mode) {
    case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
    default: return "fifo";
  }
}

}  // namespace

std::vector<const char*> MirrorWindow::instanceExtensions() {
  if (!glfwVulkanSupported()) return {};
  uint32_t count = 0;
  const char** names = glfwGetRequiredInstanceExtensions(&count);
  if (!names) return {};
  return std::vector<const char*>(names, names + count);
}

MirrorWindow::~MirrorWindow() {
  if (!gpu_ || !surface_) return;
  gpu_->drain();
  // A stopped mirror may still own a pending presentation. At final teardown
  // wait for its actual completion, not just the graphics queue.
  if (!destroySwapchain(UINT64_MAX)) return;
  destroySemaphores();
  vkDestroySurfaceKHR(gpu_->instance(), surface_, nullptr);
}

bool MirrorWindow::init(GpuContext& gpu, GLFWwindow* window, VkFormat eyeFormat) {
  gpu_ = &gpu;
  window_ = window;
  if (!gpu.caps().mirror) return false;  // XrContext said why
  const VkResult result = glfwCreateWindowSurface(gpu.instance(), window, nullptr, &surface_);
  if (result != VK_SUCCESS) {
    std::printf("Mirror: off, no Vulkan surface for its window (%d)\n", static_cast<int>(result));
    surface_ = VK_NULL_HANDLE;
    return false;
  }
  VkPhysicalDevice physical = gpu.physicalDevice();
  const char* problem = nullptr;
  VkBool32 presents = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(physical, gpu.queueFamily(), surface_, &presents);
  if (!presents) problem = "the graphics queue cannot present to its window";

  // An sRGB format, so the copy keeps the eye's colours.
  uint32_t count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface_, &count, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface_, &count, formats.data());
  format_.format = VK_FORMAT_UNDEFINED;
  for (VkFormat wanted : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB}) {
    for (const VkSurfaceFormatKHR& f : formats)
      if (f.format == wanted && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) format_ = f;
    if (format_.format != VK_FORMAT_UNDEFINED) break;
  }
  if (!problem && format_.format == VK_FORMAT_UNDEFINED) problem = "its window offers no sRGB format";

  VkFormatProperties eye{}, mirror{};
  vkGetPhysicalDeviceFormatProperties(physical, eyeFormat, &eye);
  vkGetPhysicalDeviceFormatProperties(physical, format_.format, &mirror);
  const VkFormatFeatureFlags source =
      VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  if (!problem && ((eye.optimalTilingFeatures & source) != source ||
                   !(mirror.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)))
    problem = "the GPU cannot scale the eye image into it";

  // Not tied to the monitor's refresh: the headset sets the pace.
  vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface_, &count, nullptr);
  std::vector<VkPresentModeKHR> modes(count);
  vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface_, &count, modes.data());
  presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
  for (VkPresentModeKHR wanted : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
    if (std::find(modes.begin(), modes.end(), wanted) != modes.end()) {
      presentMode_ = wanted;
      break;
    }
  }
  if (!problem && !createSemaphores()) problem = "no semaphores";
  if (problem) {
    std::printf("Mirror: off, %s\n", problem);
    destroySemaphores();
    vkDestroySurfaceKHR(gpu.instance(), surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

bool MirrorWindow::createSemaphores() {
  VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (VkSemaphore& s : acquired_)
    if (!gpu_->check(vkCreateSemaphore(gpu_->device(), &info, nullptr, &s), "mirror semaphore"))
      return false;
  return true;
}

void MirrorWindow::destroySemaphores() {
  for (VkSemaphore& s : acquired_) {
    if (s) vkDestroySemaphore(gpu_->device(), s, nullptr);
    s = VK_NULL_HANDLE;
  }
}

// After a queue drain: the old swapchain (retired by the new one) is
// destroyed once its presentations let go of it.
bool MirrorWindow::createSwapchain(uint32_t width, uint32_t height) {
  VkDevice d = gpu_->device();
  VkSurfaceCapabilitiesKHR caps{};
  const VkResult queried =
      vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_->physicalDevice(), surface_, &caps);
  if (queried != VK_SUCCESS) {
    gpu_->check(queried, "querying the mirror's window");
    stop("its window's surface was lost");
    return false;
  }
  if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
    stop("its window cannot be copied into");
    return false;
  }
  VkExtent2D extent = caps.currentExtent;
  if (extent.width == UINT32_MAX) {
    extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
  }
  if (extent.width == 0 || extent.height == 0) return false;
  uint32_t images = std::max(caps.minImageCount + 1, 3u);
  if (caps.maxImageCount > 0) images = std::min(images, caps.maxImageCount);

  VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  info.surface = surface_;
  info.minImageCount = images;
  info.imageFormat = format_.format;
  info.imageColorSpace = format_.colorSpace;
  info.imageExtent = extent;
  info.imageArrayLayers = 1;
  info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = caps.currentTransform;
  info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  if (!(caps.supportedCompositeAlpha & info.compositeAlpha))
    info.compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(
        caps.supportedCompositeAlpha & ~(caps.supportedCompositeAlpha - 1));
  info.presentMode = presentMode_;
  info.clipped = VK_TRUE;
  info.oldSwapchain = swapchain_;
  VkSwapchainKHR created = VK_NULL_HANDLE;
  const VkResult result = vkCreateSwapchainKHR(d, &info, nullptr, &created);
  const bool createdOk = gpu_->check(result, "creating the mirror's swapchain");
  // The old one is retired whether or not the new one was made.
  if (!destroySwapchain(kPresentWaitNs)) {
    if (created) vkDestroySwapchainKHR(d, created, nullptr);
    stopped_ = true;  // keep the old presentation's resources until teardown
    return false;
  }
  if (!createdOk) {
    stop("no swapchain for its window");
    return false;
  }
  swapchain_ = created;
  extent_ = extent;
  madeWidth_ = width;
  madeHeight_ = height;
  outdated_ = false;
  vkGetSwapchainImagesKHR(d, swapchain_, &images, nullptr);
  images_.resize(images);
  vkGetSwapchainImagesKHR(d, swapchain_, &images, images_.data());
  VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  presentReady_.assign(images, VK_NULL_HANDLE);
  presented_.assign(gpu_->caps().presentFences ? images : 0, VK_NULL_HANDLE);
  fenceUsed_.assign(presented_.size(), false);
  bool ok = true;
  for (VkSemaphore& s : presentReady_)
    ok = ok && gpu_->check(vkCreateSemaphore(d, &semaphoreInfo, nullptr, &s), "mirror semaphore");
  for (VkFence& f : presented_)
    ok = ok && gpu_->check(vkCreateFence(d, &fenceInfo, nullptr, &f), "mirror fence");
  if (!ok) {
    stop("no semaphores");
    return false;
  }
  std::printf("Mirror: %ux%u, %s, %s, %u images\n", extent.width, extent.height,
              format_.format == VK_FORMAT_B8G8R8A8_SRGB ? "BGRA sRGB" : "RGBA sRGB",
              presentModeName(presentMode_), images);
  return true;
}

bool MirrorWindow::destroySwapchain(uint64_t timeout) {
  VkDevice d = gpu_->device();
  // A drained queue does not prove that presentation is done with its
  // semaphores; the present fences do, where there are some.
  std::vector<VkFence> used;
  for (size_t i = 0; i < presented_.size(); ++i)
    if (fenceUsed_[i]) used.push_back(presented_[i]);
  if (!used.empty() && !gpu_->lost()) {
    const VkResult result =
        vkWaitForFences(d, static_cast<uint32_t>(used.size()), used.data(), VK_TRUE, timeout);
    if (!gpu_->check(result, "waiting for mirror presentations") && !gpu_->lost()) return false;
  }
  for (VkFence f : presented_)
    if (f) vkDestroyFence(d, f, nullptr);
  for (VkSemaphore s : presentReady_)
    if (s) vkDestroySemaphore(d, s, nullptr);
  presented_.clear();
  fenceUsed_.clear();
  presentReady_.clear();
  images_.clear();
  if (swapchain_) vkDestroySwapchainKHR(d, swapchain_, nullptr);
  swapchain_ = VK_NULL_HANDLE;
  state_ = Image::None;
  return true;
}

// The window stays (it still takes the keyboard), without the eyes.
void MirrorWindow::stop(const char* why) {
  if (!active()) return;
  std::printf("Mirror: off, %s\n", why);
  stopped_ = true;
  state_ = Image::None;
  gpu_->drain();
  if (!destroySwapchain(kPresentWaitNs)) return;
  destroySemaphores();
  vkDestroySurfaceKHR(gpu_->instance(), surface_, nullptr);
  surface_ = VK_NULL_HANDLE;
}

void MirrorWindow::acquire(const GpuContext::FrameSlot& slot) {
  if (!active()) return;
  if (state_ != Image::None) {
    // Taken but never submitted: its semaphore cannot be used again.
    stop("an image was left over");
    return;
  }
  int width = 0, height = 0;
  glfwGetFramebufferSize(window_, &width, &height);
  if (width <= 0 || height <= 0 || glfwGetWindowAttrib(window_, GLFW_ICONIFIED)) return;
  const bool resized = static_cast<uint32_t>(width) != madeWidth_ ||
                       static_cast<uint32_t>(height) != madeHeight_;
  if (!swapchain_ || outdated_ || resized) {
    const double now = glfwGetTime();
    if (width != pendingWidth_ || height != pendingHeight_) {
      pendingWidth_ = width;
      pendingHeight_ = height;
      pendingSince_ = now;
    }
    if (!swapchain_ || now - pendingSince_ >= kSettleSeconds) {
      // A user action, not a per-frame cost: the GPU finishes first.
      gpu_->drain();
      if (!createSwapchain(static_cast<uint32_t>(width), static_cast<uint32_t>(height))) return;
    } else if (outdated_) {
      return;
    }
  }
  const VkResult result =
      vkAcquireNextImageKHR(gpu_->device(), swapchain_, 0, acquired_[slot.index], VK_NULL_HANDLE,
                            &index_);
  switch (result) {
    // Suboptimal still shows: a new size is seen above, anything else waits.
    case VK_SUCCESS:
    case VK_SUBOPTIMAL_KHR:
      state_ = Image::Acquired;
      slot_ = slot.index;
      break;
    case VK_NOT_READY:
    case VK_TIMEOUT: break;
    case VK_ERROR_OUT_OF_DATE_KHR: outdated_ = true; break;
    case VK_ERROR_SURFACE_LOST_KHR: stop("its window's surface was lost"); break;
    default: gpu_->check(result, "acquiring a mirror image"); break;
  }
}

void MirrorWindow::record(VkCommandBuffer cmd, const EyeImage& eye,
                          std::vector<SemaphoreWait>& waits) {
  if (state_ != Image::Acquired) return;
  waits.push_back({acquired_[slot_], 0, VK_PIPELINE_STAGE_TRANSFER_BIT});
  VkImage target = images_[index_];
  const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                     VkAccessFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = range;
    return b;
  };
  // The eye's pass is done with it (or the cover's upload, its mip levels
  // made); the window's image is free once its acquire semaphore (waited
  // for at the transfer stage) is signalled.
  const VkImage source = cover_ ? cover_->image : eye.image;
  const VkImageLayout sourceLayout =
      cover_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  const VkImageMemoryBarrier before[2] = {
      barrier(source, sourceLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              cover_ ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
              VK_ACCESS_TRANSFER_READ_BIT),
      barrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
              VK_ACCESS_TRANSFER_WRITE_BIT)};
  vkCmdPipelineBarrier(cmd,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, before);

  // Scaled to fit, the rest of the window black.
  const VkRect2D area = cover_ ? VkRect2D{{0, 0}, {cover_->width, cover_->height}} : eye.area;
  const float scale = std::min(static_cast<float>(extent_.width) / area.extent.width,
                               static_cast<float>(extent_.height) / area.extent.height);
  const int32_t w = std::max(1, static_cast<int32_t>(area.extent.width * scale));
  const int32_t h = std::max(1, static_cast<int32_t>(area.extent.height * scale));
  const int32_t x = (static_cast<int32_t>(extent_.width) - w) / 2;
  const int32_t y = (static_cast<int32_t>(extent_.height) - h) / 2;
  if (w < static_cast<int32_t>(extent_.width) || h < static_cast<int32_t>(extent_.height)) {
    const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
    vkCmdClearColorImage(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    const VkImageMemoryBarrier cleared =
        barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &cleared);
  }
  VkImageBlit blit{};
  blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.srcOffsets[0] = {area.offset.x, area.offset.y, 0};
  blit.srcOffsets[1] = {area.offset.x + static_cast<int32_t>(area.extent.width),
                        area.offset.y + static_cast<int32_t>(area.extent.height), 1};
  blit.dstSubresource = blit.srcSubresource;
  blit.dstOffsets[0] = {x, y, 0};
  blit.dstOffsets[1] = {x + w, y + h, 1};
  vkCmdBlitImage(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

  // The eye goes back to the layout OpenXR expects at release, ordered
  // before the runtime's own use of it as an attachment; the cover to
  // where it is sampled and uploaded to.
  const VkImageMemoryBarrier sourceBack =
      cover_ ? barrier(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sourceLayout, 0,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT)
             : barrier(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sourceLayout, 0,
                       VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       cover_ ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT
                              : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &sourceBack);
  const VkImageMemoryBarrier shown =
      barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
              VK_ACCESS_TRANSFER_WRITE_BIT, 0);
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &shown);
  state_ = Image::Recorded;
}

void MirrorWindow::addSignals(std::vector<VkSemaphore>& signals) const {
  if (state_ == Image::Recorded) signals.push_back(presentReady_[index_]);
}

void MirrorWindow::submitted(bool ok) {
  if (state_ != Image::Recorded) return;
  if (ok) {
    state_ = Image::Submitted;
  } else {
    state_ = Image::None;
    // Its acquire semaphore stays signalled with nothing to wait for it.
    stop("its frame was not submitted");
  }
}

void MirrorWindow::present() {
  if (state_ != Image::Submitted) {
    if (state_ != Image::None) stop("an image was left over");
    return;
  }
  state_ = Image::None;
  VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  info.waitSemaphoreCount = 1;
  info.pWaitSemaphores = &presentReady_[index_];
  info.swapchainCount = 1;
  info.pSwapchains = &swapchain_;
  info.pImageIndices = &index_;
  VkSwapchainPresentFenceInfoEXT fence{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
  if (!presented_.empty()) {
    VkDevice d = gpu_->device();
    // This image was acquired again, so its last presentation has long
    // signalled its fence.
    if (fenceUsed_[index_]) {
      if (!gpu_->check(vkWaitForFences(d, 1, &presented_[index_], VK_TRUE, kPresentWaitNs),
                       "reusing a mirror presentation fence")) {
        stop("a presentation did not finish");
        return;
      }
      vkResetFences(d, 1, &presented_[index_]);
    }
    fence.swapchainCount = 1;
    fence.pFences = &presented_[index_];
    info.pNext = &fence;
    fenceUsed_[index_] = true;
  }
  const VkResult result = vkQueuePresentKHR(gpu_->queue(), &info);
  switch (result) {
    case VK_SUCCESS:
    case VK_SUBOPTIMAL_KHR: break;
    case VK_ERROR_OUT_OF_DATE_KHR: outdated_ = true; break;
    case VK_ERROR_SURFACE_LOST_KHR: stop("its window's surface was lost"); break;
    default: gpu_->check(result, "presenting the mirror"); break;
  }
}
