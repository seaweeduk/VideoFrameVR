#define XR_USE_GRAPHICS_API_VULKAN

#include "xr_context.h"

#include "platform.h"
#include "strings.h"
#include "xr_device.h"
#include "xr_util.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glib.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <thread>
#include <unistd.h>

namespace {

constexpr XrViewConfigurationType kViewConfig = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
constexpr float kNearZ = 0.05f;
constexpr float kFarZ = 100.0f;
// xrWaitSwapchainImage: this long at a time, this many times.
constexpr XrDuration kImageWaitTimeout = 1'000'000'000;
constexpr int kImageWaitTries = 5;

// Foveation (docs/vulkan-renderer.md, 5.1): used only if the runtime offers
// every one.
const char* const kFoveationExtensions[] = {
    XR_FB_FOVEATION_EXTENSION_NAME,           XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME,
    XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME, XR_FB_FOVEATION_VULKAN_EXTENSION_NAME,
    XR_META_FOVEATION_EYE_TRACKED_EXTENSION_NAME, XR_META_VULKAN_SWAPCHAIN_CREATE_INFO_EXTENSION_NAME};

// Launch-dashboard dismissal and environment isolation credited to FramePlayerX,
// Copyright 2026 remotesojourner, Apache-2.0. Upstream: crates/fp-xr/src/context.rs
// and session.rs at 8d95bab648c057cf53a0d56c65970e3c0a72dc59.
// https://github.com/remotesojourner/frameplayerx
// Modified for VideoFrameVR's GLib spawn and single-attempt launch handoff;
// present by 2026-11-03, attribution added 2026-11-10. See packaging/frameplayerx/.
void hideLaunchDashboard() {
  gchar** env = g_get_environ();
  for (const char* key : {"SteamAppId", "SteamGameId", "SteamOverlayGameId", "LD_PRELOAD"})
    env = g_environ_unsetenv(env, key);
  env = g_environ_setenv(env, "LD_LIBRARY_PATH", kVrcmdDir, TRUE);
  gchar command[] = "/opt/steamvr/bin/linuxarm64/vrcmd";
  gchar option[] = "--hidedashboard";
  gchar* argv[] = {command, option, nullptr};
  GError* error = nullptr;
  // Without DO_NOT_REAP_CHILD, GLib arranges child reaping without a main loop.
  const auto flags = static_cast<GSpawnFlags>(G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL);
  if (g_spawn_async(nullptr, argv, env, flags, nullptr, nullptr, nullptr, &error))
    std::printf("OpenXR: requested launch dashboard dismissal\n");
  else
    std::fprintf(stderr, "OpenXR: launch dashboard dismissal failed: %s\n", error->message);
  g_clear_error(&error);
  g_strfreev(env);
}

int scaledSize(int base, int recommended, float scale) {
  return std::max(16, static_cast<int>(std::floor((base > 0 ? base : recommended) * scale)));
}

}  // namespace

XrContext::~XrContext() {
  if (launchDashboard_.valid()) launchDashboard_.wait();
  if (making_.valid())
    for (Swapchain& sc : making_.get()) destroySwapchain(sc);
  for (auto& job : unmaking_) job.wait();
  VkDevice device = gpu_.device;
  if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
  for (XrFoveationProfileFB profile : profiles_)
    if (profile != XR_NULL_HANDLE) destroyFoveationProfile_(profile);
  for (auto& sc : swapchains_) destroySwapchain(sc);
  input_.destroy();
  if (viewSpace_ != XR_NULL_HANDLE) xrDestroySpace(viewSpace_);
  if (localSpace_ != XR_NULL_HANDLE) xrDestroySpace(localSpace_);
  if (session_ != XR_NULL_HANDLE) xrDestroySession(session_);
  if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
  if (gpu_.instance != VK_NULL_HANDLE) vkDestroyInstance(gpu_.instance, nullptr);
  if (instance_ != XR_NULL_HANDLE) xrDestroyInstance(instance_);
}

bool XrContext::check(XrResult result, const char* what) const {
  return xrCheck(instance_, result, what);
}

bool XrContext::init(const GpuRequirements& gpu) {
  uint32_t extCount = 0;
  if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr))) {
    std::fprintf(stderr,
                 "No OpenXR runtime found. Start SteamVR and set it as the OpenXR runtime "
                 "(SteamVR settings > OpenXR).\n");
    return false;
  }
  std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
  XR_TRY(xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data()));
  const bool hasVulkan = std::any_of(exts.begin(), exts.end(), [](const XrExtensionProperties& e) {
    return std::strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0;
  });
  if (!hasVulkan) {
    std::fprintf(stderr, "The active OpenXR runtime does not support %s.\n",
                 XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
    return false;
  }

  auto offered = [&](const char* name) {
    return std::any_of(exts.begin(), exts.end(), [&](const XrExtensionProperties& e) {
      return std::strcmp(e.extensionName, name) == 0;
    });
  };
  std::vector<const char*> enabledExts = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
  input_.enableExtensions(offered, enabledExts);
  presenceExtension_ = offered(XR_EXT_USER_PRESENCE_EXTENSION_NAME);
  if (presenceExtension_) enabledExts.push_back(XR_EXT_USER_PRESENCE_EXTENSION_NAME);
  refreshExtension_ = refreshDiagnostic_ && offered(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
  if (refreshExtension_) enabledExts.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
  if (gpu.foveation) {
    foveationExtensions_ = std::all_of(std::begin(kFoveationExtensions),
                                       std::end(kFoveationExtensions), offered);
    if (foveationExtensions_)
      enabledExts.insert(enabledExts.end(), std::begin(kFoveationExtensions),
                         std::end(kFoveationExtensions));
  }
  XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
  // SteamVR derives its app key from this name; keep it free of spaces.
  std::strncpy(instanceInfo.applicationInfo.applicationName, kAppName,
               XR_MAX_APPLICATION_NAME_SIZE - 1);
  std::strncpy(instanceInfo.applicationInfo.engineName, kAppName, XR_MAX_ENGINE_NAME_SIZE - 1);
  instanceInfo.applicationInfo.applicationVersion = 1;
  instanceInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  instanceInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExts.size());
  instanceInfo.enabledExtensionNames = enabledExts.data();
  // Until the headset is there (SteamVR still starting, or the Frame not
  // connected yet), SteamVR fails to create the instance or finds no
  // system: wait and try again.
  XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
  systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  bool waiting = false;
  for (;;) {
    XrResult result = xrCreateInstance(&instanceInfo, &instance_);
    if (XR_SUCCEEDED(result)) {
      result = xrGetSystem(instance_, &systemInfo, &systemId_);
      if (XR_SUCCEEDED(result)) break;
      xrDestroyInstance(instance_);
      instance_ = XR_NULL_HANDLE;
    }
    if (result != XR_ERROR_FORM_FACTOR_UNAVAILABLE && result != XR_ERROR_RUNTIME_FAILURE &&
        result != XR_ERROR_RUNTIME_UNAVAILABLE && result != XR_ERROR_INSTANCE_LOST)
      return check(result, "starting OpenXR");
    if (!waiting) {
      std::printf("Waiting for the headset: start SteamVR and connect the Steam Frame "
                  "(Ctrl+C to give up) ...\n");
      std::fflush(stdout);
      waiting = true;
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));
  }
  if (waiting) std::printf("Headset found.\n");

  XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
  if (XR_SUCCEEDED(xrGetInstanceProperties(instance_, &props))) {
    std::printf("OpenXR runtime: %s\n", props.runtimeName);
    runtimeVersion_ = props.runtimeVersion;
    const char* appId = g_getenv("SteamAppId");
    launchDashboardPending_ = kStandaloneHeadset &&
        std::strncmp(props.runtimeName, "SteamVR", 7) == 0 && appId && *appId &&
        std::strcmp(appId, "0") != 0 &&
        std::all_of(appId, appId + std::strlen(appId), [](char c) { return c >= '0' && c <= '9'; }) &&
        access(kVrcmd, X_OK) == 0;
  }
  {
    XrSystemEyeGazeInteractionPropertiesEXT gazeProps{
        XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
    XrSystemFoveationEyeTrackedPropertiesMETA foveationProps{
        XR_TYPE_SYSTEM_FOVEATION_EYE_TRACKED_PROPERTIES_META};
    XrSystemUserPresencePropertiesEXT presenceProps{XR_TYPE_SYSTEM_USER_PRESENCE_PROPERTIES_EXT};
    XrSystemProperties systemProps{XR_TYPE_SYSTEM_PROPERTIES};
    if (presenceExtension_) {
      presenceProps.next = systemProps.next;
      systemProps.next = &presenceProps;
    }
    if (input_.gazeExtension()) {
      gazeProps.next = systemProps.next;
      systemProps.next = &gazeProps;
    }
    if (foveationExtensions_) {
      foveationProps.next = systemProps.next;
      systemProps.next = &foveationProps;
    }
    if (XR_SUCCEEDED(xrGetSystemProperties(instance_, systemId_, &systemProps))) {
      systemName_ = systemProps.systemName;
      input_.setGazeSupported(gazeProps.supportsEyeGazeInteraction);
      std::printf("OpenXR system: %s\n", systemProps.systemName);
    }
    std::printf("Headset presence: %s\n",
                !presenceExtension_ ? "not offered by the OpenXR runtime"
                : presenceProps.supportsUserPresence ? "available (XR_EXT_user_presence)"
                                                     : "not supported by this headset");
    // Its extensions alone do not say the headset can do it.
    if (foveationExtensions_ && !foveationProps.supportsFoveationEyeTracked) {
      std::printf("Foveation: this headset has no eye-tracked foveation\n");
      foveationExtensions_ = false;
    }
  }
  if (gpu.foveation && foveationExtensions_) {
    auto proc = [&](const char* name, auto& fn) {
      return XR_SUCCEEDED(xrGetInstanceProcAddr(instance_, name,
                                                reinterpret_cast<PFN_xrVoidFunction*>(&fn))) &&
             fn != nullptr;
    };
    foveationExtensions_ = proc("xrCreateFoveationProfileFB", createFoveationProfile_) &&
                           proc("xrDestroyFoveationProfileFB", destroyFoveationProfile_) &&
                           proc("xrUpdateSwapchainFB", updateSwapchain_) &&
                           proc("xrGetFoveationEyeTrackedStateMETA", getFoveationState_);
  } else if (gpu.foveation) {
    std::printf("Foveation: the OpenXR runtime does not offer it\n");
  }
  input_.logSupport();

  if (!createVulkanDevice(instance_, systemId_, gpu, foveationExtensions_, gpu_)) return false;

  XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
  binding.instance = gpu_.instance;
  binding.physicalDevice = gpu_.physical;
  binding.device = gpu_.device;
  binding.queueFamilyIndex = gpu_.queueFamily;
  binding.queueIndex = 0;

  XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
  sessionInfo.next = &binding;
  sessionInfo.systemId = systemId_;
  XR_TRY(xrCreateSession(instance_, &sessionInfo, &session_));

  XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
  spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  XR_TRY(xrCreateReferenceSpace(session_, &spaceInfo, &localSpace_));
  spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  XR_TRY(xrCreateReferenceSpace(session_, &spaceInfo, &viewSpace_));

  if (!input_.create(instance_, session_, localSpace_)) return false;
  return queryViewConfiguration();
}

bool XrContext::queryViewConfiguration() {
  uint32_t viewCount = 0;
  XR_TRY(xrEnumerateViewConfigurationViews(instance_, systemId_, kViewConfig, 0, &viewCount, nullptr));
  configViews_.assign(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
  XR_TRY(xrEnumerateViewConfigurationViews(instance_, systemId_, kViewConfig, viewCount, &viewCount,
                                           configViews_.data()));

  uint32_t formatCount = 0;
  XR_TRY(xrEnumerateSwapchainFormats(session_, 0, &formatCount, nullptr));
  std::vector<int64_t> formats(formatCount);
  XR_TRY(xrEnumerateSwapchainFormats(session_, formatCount, &formatCount, formats.data()));
  auto has = [&](VkFormat f) { return std::find(formats.begin(), formats.end(), f) != formats.end(); };
  // The scene blends in linear light and the images encode to sRGB; a
  // linear format would need another colour contract with the compositor.
  swapchainFormat_ = 0;
  for (VkFormat preferred : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB}) {
    if (has(preferred)) {
      swapchainFormat_ = preferred;
      break;
    }
  }
  if (swapchainFormat_ == 0) {
    std::fprintf(stderr, "OpenXR: the runtime offers no sRGB eye image format (R8G8B8A8 or "
                         "B8G8R8A8)\n");
    return false;
  }
  imageLimit_ = gpu_.caps.maxImageDimension2D;
  queryBlendModes();

  if (viewCount != 2) {
    std::fprintf(stderr, "OpenXR: %u views, not 2\n", viewCount);
    return false;
  }
  views_.assign(viewCount, {XR_TYPE_VIEW});
  projViews_.assign(viewCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
  eyes_.resize(viewCount);
  return true;
}

int XrContext::recommendedEyeWidth() const {
  return configViews_.empty() ? 0 : static_cast<int>(configViews_[0].recommendedImageRectWidth);
}

int XrContext::recommendedEyeHeight() const {
  return configViews_.empty() ? 0 : static_cast<int>(configViews_[0].recommendedImageRectHeight);
}

// One factor for both sides of every eye, so the eye images keep their
// shape; lowered to fit the runtime's maximum image rect and the GPU's limits.
float XrContext::fittingScale(float scale, int baseWidth, int baseHeight,
                              bool& runtimeLimited) const {
  runtimeLimited = false;
  for (const auto& cv : configViews_) {
    const int recommended[2] = {static_cast<int>(cv.recommendedImageRectWidth),
                                static_cast<int>(cv.recommendedImageRectHeight)};
    const int base[2] = {baseWidth > 0 ? baseWidth : recommended[0],
                         baseHeight > 0 ? baseHeight : recommended[1]};
    const uint32_t runtimeMax[2] = {cv.maxImageRectWidth, cv.maxImageRectHeight};
    for (int k = 0; k < 2; ++k) {
      if (base[k] <= 0) continue;
      const uint32_t most = runtimeMax[k] > 0 ? std::min(runtimeMax[k], imageLimit_) : imageLimit_;
      const float fits = static_cast<float>(most) / base[k];
      if (fits < scale) {
        scale = fits;
        runtimeLimited = runtimeMax[k] > 0 && runtimeMax[k] <= imageLimit_;
      }
    }
  }
  return scale;
}

bool XrContext::createSwapchain(int width, int height, bool foveated, Swapchain& out) const {
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  // Foveated: with the runtime's density maps, and made so their offsets can
  // move the sharp area (without the flag a moved one leaves an unrendered
  // band, docs/frame-plan.md W11).
  XrSwapchainCreateInfoFoveationFB foveation{XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB};
  foveation.flags = XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB;
  XrVulkanSwapchainCreateInfoMETA vulkanInfo{XR_TYPE_VULKAN_SWAPCHAIN_CREATE_INFO_META};
  vulkanInfo.additionalCreateFlags = VK_IMAGE_CREATE_FRAGMENT_DENSITY_MAP_OFFSET_BIT_QCOM;
  if (foveated) {
    vulkanInfo.next = &foveation;
    info.next = &vulkanInfo;
  }
  // SteamVR samples the images when copying them to its compositor, so
  // SAMPLED is required even though we only write to them.
  info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
  // The desktop mirror copies the left eye from it.
  if (gpu_.caps.mirror) info.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
  info.format = swapchainFormat_;
  info.sampleCount = 1;
  info.width = static_cast<uint32_t>(width);
  info.height = static_cast<uint32_t>(height);
  info.faceCount = 1;
  info.arraySize = 1;
  info.mipCount = 1;
  out.width = out.imageWidth = width;
  out.height = out.imageHeight = height;
  XR_TRY(xrCreateSwapchain(session_, &info, &out.handle));

  uint32_t imageCount = 0;
  XR_TRY(xrEnumerateSwapchainImages(out.handle, 0, &imageCount, nullptr));
  std::vector<XrSwapchainImageVulkan2KHR> images(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
  std::vector<XrSwapchainImageFoveationVulkanFB> maps(imageCount,
                                                      {XR_TYPE_SWAPCHAIN_IMAGE_FOVEATION_VULKAN_FB});
  if (foveated)
    for (uint32_t i = 0; i < imageCount; ++i) images[i].next = &maps[i];
  XR_TRY(xrEnumerateSwapchainImages(out.handle, imageCount, &imageCount,
                                    reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));
  out.area = {{0, 0}, {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}};
  for (uint32_t i = 0; i < imageCount; ++i) {
    out.images.push_back(images[i].image);
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = images[i].image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = eyeFormat();
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    VK_TRY(vkCreateImageView(gpu_.device, &view, nullptr, &v));
    out.views.push_back(v);
    if (!foveated) continue;
    // The map must cover the image at the GPU's coarsest texel.
    const VkExtent2D texel = gpu_.caps.maxDensityTexelSize;
    if (maps[i].image == VK_NULL_HANDLE ||
        maps[i].width * texel.width < static_cast<uint32_t>(width) ||
        maps[i].height * texel.height < static_cast<uint32_t>(height)) {
      std::fprintf(stderr, "OpenXR: the runtime's density map (%ux%u) does not fit a %dx%d eye image\n",
                   maps[i].width, maps[i].height, width, height);
      return false;
    }
    out.densitySize = {maps[i].width, maps[i].height};
    view.image = maps[i].image;
    view.format = VK_FORMAT_R8G8_UNORM;
    VK_TRY(vkCreateImageView(gpu_.device, &view, nullptr, &v));
    out.densityViews.push_back(v);
  }
  return true;
}

// The images belong to the runtime: only our views go, and our density map.
void XrContext::destroySwapchain(Swapchain& sc) const {
  for (VkImageView view : sc.views) vkDestroyImageView(gpu_.device, view, nullptr);
  for (VkImageView view : sc.densityViews) vkDestroyImageView(gpu_.device, view, nullptr);
  destroyAppMap(sc.appMap);
  if (sc.handle != XR_NULL_HANDLE) xrDestroySwapchain(sc.handle);
  sc = Swapchain{};
}

void XrContext::destroyAppMap(AppMap& map) const {
  if (map.view) vkDestroyImageView(gpu_.device, map.view, nullptr);
  if (map.image) vkDestroyImage(gpu_.device, map.image, nullptr);
  if (map.memory) vkFreeMemory(gpu_.device, map.memory, nullptr);
  map = AppMap{};
}

// Destroying a swapchain can take a while (the runtime frees its images), so
// the frame loop never waits for it.
void XrContext::retireSwapchains(std::vector<Swapchain> old) {
  if (old.empty()) return;
  context_->retire([this, old]() mutable {
    std::vector<XrSwapchain> handles;
    for (Swapchain& sc : old) {
      for (VkImageView view : sc.views) vkDestroyImageView(gpu_.device, view, nullptr);
      for (VkImageView view : sc.densityViews) vkDestroyImageView(gpu_.device, view, nullptr);
      destroyAppMap(sc.appMap);
      if (sc.handle != XR_NULL_HANDLE) handles.push_back(sc.handle);
    }
    unmaking_.push_back(std::async(std::launch::async, [handles] {
      for (XrSwapchain handle : handles) xrDestroySwapchain(handle);
    }));
  });
}

// Linear and host-visible: Turnip reads a density map on the CPU when the
// pass is recorded, and the app writes it the same way. For the size in use;
// still to be drawn and put in the layout the eye pass uses it in.
bool XrContext::createAppDensityMap(const Swapchain& sc, AppMap& map) {
  const VkExtent2D texel = gpu_.caps.minDensityTexelSize;
  if (texel.width == 0 || texel.height == 0) return false;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.flags = VK_IMAGE_CREATE_FRAGMENT_DENSITY_MAP_OFFSET_BIT_QCOM;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = VK_FORMAT_R8G8_UNORM;
  const VkExtent2D eye{static_cast<uint32_t>(sc.width), static_cast<uint32_t>(sc.height)};
  const VkExtent2D extent =
      densityMapExtent(eyePassTileGrid(gpu_.caps, eye.width, eye.height), eye, texel.width);
  info.extent = {extent.width, extent.height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_LINEAR;
  info.usage = VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT;
  info.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
  VkImageFormatProperties supported{};
  if (vkGetPhysicalDeviceImageFormatProperties(gpu_.physical, info.format, info.imageType, info.tiling,
                                               info.usage, info.flags, &supported) != VK_SUCCESS ||
      supported.maxExtent.width < info.extent.width || supported.maxExtent.height < info.extent.height) {
    std::printf("Foveation: the GPU cannot have a linear density map of %ux%u\n", info.extent.width,
                info.extent.height);
    return false;
  }
  VK_TRY(vkCreateImage(gpu_.device, &info, nullptr, &map.image));
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(gpu_.device, map.image, &req);
  VkPhysicalDeviceMemoryProperties memory;
  vkGetPhysicalDeviceMemoryProperties(gpu_.physical, &memory);
  uint32_t type = context_->memoryType(
      req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (type == UINT32_MAX) type = context_->memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
  if (type == UINT32_MAX) {
    std::printf("Foveation: no host-visible memory for a density map\n");
    return false;
  }
  map.coherent = (memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = type;
  VK_TRY(vkAllocateMemory(gpu_.device, &alloc, nullptr, &map.memory));
  VK_TRY(vkBindImageMemory(gpu_.device, map.image, map.memory, 0));
  void* mapped = nullptr;
  VK_TRY(vkMapMemory(gpu_.device, map.memory, 0, VK_WHOLE_SIZE, 0, &mapped));
  const VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
  VkSubresourceLayout layout;
  vkGetImageSubresourceLayout(gpu_.device, map.image, &sub, &layout);
  map.pixels = {static_cast<uint8_t*>(mapped) + layout.offset, static_cast<size_t>(layout.rowPitch),
                {info.extent.width, info.extent.height}, texel.width};

  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = map.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = info.format;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VK_TRY(vkCreateImageView(gpu_.device, &view, nullptr, &map.view));
  map.views.assign(sc.images.size(), map.view);
  return true;
}

// New maps rather than redrawn ones: frames still on the GPU keep theirs,
// so nothing waits.
bool XrContext::drawDensityMaps(const std::function<void(int eye, const DensityMapPixels&)>& draw) {
  if (!appMaps_) return false;
  std::vector<AppMap> made(swapchains_.size());
  bool ok = true;
  for (size_t i = 0; ok && i < swapchains_.size(); ++i) {
    ok = createAppDensityMap(swapchains_[i], made[i]);
    if (!ok) break;
    draw(static_cast<int>(i), made[i].pixels);
    if (made[i].coherent) continue;
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = made[i].memory;
    range.size = VK_WHOLE_SIZE;
    ok = vkCheck(vkFlushMappedMemoryRanges(gpu_.device, 1, &range), "writing a density map");
  }
  // Into the layout of the eye pass, before the next frame (submitted ahead of it).
  VkCommandBuffer cmd = ok ? context_->beginOneShot() : VK_NULL_HANDLE;
  if (cmd) {
    std::vector<VkImageMemoryBarrier> barriers;
    for (const AppMap& map : made) {
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
      barrier.newLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
      barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = map.image;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      barriers.push_back(barrier);
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT,
                         0, 0, nullptr, 0, nullptr, static_cast<uint32_t>(barriers.size()),
                         barriers.data());
    ok = context_->submitOneShot(cmd) != 0;
  } else {
    ok = false;
  }
  if (!ok) {
    for (AppMap& map : made) destroyAppMap(map);
    return false;
  }
  for (size_t i = 0; i < swapchains_.size(); ++i) {
    AppMap old = swapchains_[i].appMap;
    swapchains_[i].appMap = made[i];
    if (old.image) context_->retire([this, old]() mutable { destroyAppMap(old); });
  }
  ++targetsVersion_;
  return true;
}

bool XrContext::createEyeImages(GpuContext& gpu, int baseWidth, int baseHeight, float videoScale,
                                float capScale) {
  context_ = &gpu;
  baseWidth_ = baseWidth;
  baseHeight_ = baseHeight;
  bool runtimeLimited = false, capLimited = false;
  activeScale_ = requestedScale_ = fittingScale(videoScale, baseWidth, baseHeight, runtimeLimited);
  capScale_ = fittingScale(capScale, baseWidth, baseHeight, capLimited);
  // Up to the cap, sizes are drawn in images of the cap's size, so that
  // switching between them never waits for new images.
  imageScale_ = holds(capScale_, activeScale_) ? capScale_ : activeScale_;

  // Foveated if all of it can be had, else as plain images.
  for (const bool foveated : {true, false}) {
    if (foveated && !(foveationExtensions_ && gpu_.caps.foveation)) continue;
    bool made = true;
    for (const auto& cv : configViews_) {
      Swapchain sc;
      made = createSwapchain(
          scaledSize(baseWidth, static_cast<int>(cv.recommendedImageRectWidth), imageScale_),
          scaledSize(baseHeight, static_cast<int>(cv.recommendedImageRectHeight), imageScale_),
          foveated, sc);
      if (!made) {
        destroySwapchain(sc);
        break;
      }
      swapchains_.push_back(sc);
    }
    if (made) {
      useScale(activeScale_);
      // Without profiles the images stay, drawn unfoveated.
      foveated_ = foveated && createFoveationProfiles();
      if (foveated && !foveated_) std::printf("Foveation: no profiles; the eye images are not foveated\n");
      if (foveated_) {
        tileGrid_ = eyePassTileGrid(gpu_.caps, static_cast<uint32_t>(swapchains_[0].width),
                                    static_cast<uint32_t>(swapchains_[0].height));
        appMaps_ = preferAppMaps_ && tileGrid_.known();
        if (appMaps_)
          appMaps_ = drawDensityMaps(
              [](int, const DensityMapPixels& map) { drawDensityMap(FoveationLevel::Off, {}, {}, 3, true, 4, map); });
        if (preferAppMaps_ && !appMaps_)
          std::printf("Foveation: %s; drawn with SteamVR's density maps\n",
                      tileGrid_.known() ? "the app's density maps could not be made"
                                        : "this GPU's bins are not known");
        // With app maps the runtime's profile only keeps its eye-tracked
        // centres coming.
        if (appMaps_) setFoveationLevel(FoveationLevel::High);
      }
      break;
    }
    for (auto& sc : swapchains_) destroySwapchain(sc);
    swapchains_.clear();
    if (!foveated) return false;
    std::printf("Foveation: the eye images could not have density maps; made without\n");
  }

  std::printf("OpenXR: %zu views, %dx%d per eye in %dx%d images, %s swapchain of %zu images, %.2f times %s%s\n",
              configViews_.size(), swapchains_.empty() ? 0 : swapchains_[0].width,
              swapchains_.empty() ? 0 : swapchains_[0].height,
              swapchains_.empty() ? 0 : swapchains_[0].imageWidth,
              swapchains_.empty() ? 0 : swapchains_[0].imageHeight,
              eyeFormat() == VK_FORMAT_R8G8B8A8_SRGB ? "RGBA sRGB" : "BGRA sRGB",
              swapchains_.empty() ? size_t{0} : swapchains_[0].images.size(),
              activeScale_, baseWidth > 0 ? "the panel" : "the recommended size",
              foveated_ ? ", foveated" : "");
  if (foveated_)
    std::printf("  density maps %ux%u (%s, %.0f px per texel)\n", densityMapSize(0).width,
                densityMapSize(0).height, appMaps_ ? "the app's" : "SteamVR's",
                appMaps_ ? static_cast<double>(swapchains_[0].appMap.pixels.texel)
                         : static_cast<double>(swapchains_[0].width) / densityMapSize(0).width);
  if (tileGrid_.known())
    std::printf("  bins %ux%u of %ux%u (%s)\n", tileGrid_.columns, tileGrid_.rows, tileGrid_.binWidth,
                tileGrid_.binHeight, tileGrid_.source);
  if (activeScale_ < videoScale - 0.005f)
    std::printf("  scale %.2f requested, limited to %.2f by the %s\n", videoScale, activeScale_,
                runtimeLimited ? "runtime's maximum image size" : "GPU's maximum texture size");
  // The eye images are drawn at 1 sample, 1 mip, all of the size in use.
  for (size_t i = 0; i < configViews_.size(); ++i) {
    const XrViewConfigurationView& cv = configViews_[i];
    std::printf("  eye %zu: recommended %ux%u (%u samples), max %ux%u; requested %dx%d, "
                "allocated %dx%d, viewport and image rect 0,0 %dx%d\n",
                i, cv.recommendedImageRectWidth, cv.recommendedImageRectHeight,
                cv.recommendedSwapchainSampleCount, cv.maxImageRectWidth, cv.maxImageRectHeight,
                scaledSize(baseWidth, static_cast<int>(cv.recommendedImageRectWidth), videoScale),
                scaledSize(baseHeight, static_cast<int>(cv.recommendedImageRectHeight), videoScale),
                swapchains_[i].imageWidth, swapchains_[i].imageHeight, swapchains_[i].width,
                swapchains_[i].height);
  }
  std::printf("  sizes up to %.2f times switch at once; larger ones get images of their own\n", capScale_);
  return true;
}

void XrContext::requestEyeScale(float scale) {
  bool runtimeLimited = false;
  requestedScale_ = fittingScale(scale, baseWidth_, baseHeight_, runtimeLimited);
}

bool XrContext::holds(float images, float size) const {
  for (const auto& cv : configViews_) {
    const int w = static_cast<int>(cv.recommendedImageRectWidth);
    const int h = static_cast<int>(cv.recommendedImageRectHeight);
    if (scaledSize(baseWidth_, w, size) > scaledSize(baseWidth_, w, images) ||
        scaledSize(baseHeight_, h, size) > scaledSize(baseHeight_, h, images))
      return false;
  }
  return true;
}

// Images hold every size up to theirs. Larger ones are made at the cap at
// least, so later switches up to it are instant; from above the cap, a size
// the cap holds goes back to the cap's images, and a size above it keeps the
// images only while they are at most 1.25 times as wide, to free their memory
// (3x images held at 1.5x: 1.5 GB against 0.4 on the desktop).
float XrContext::imageScaleFor(float request, float images) const {
  constexpr float kAboveCapSlack = 1.25f;
  const bool capHolds = holds(capScale_, request);
  if (holds(images, request) &&
      (holds(capScale_, images) || (!capHolds && holds(request * kAboveCapSlack, images))))
    return images;
  return capHolds ? capScale_ : request;
}

void XrContext::startMakingImages(float scale) {
  std::vector<std::pair<int, int>> sizes;
  for (const auto& cv : configViews_)
    sizes.emplace_back(scaledSize(baseWidth_, static_cast<int>(cv.recommendedImageRectWidth), scale),
                       scaledSize(baseHeight_, static_cast<int>(cv.recommendedImageRectHeight), scale));
  makingScale_ = scale;
  makingSince_ = std::chrono::steady_clock::now();
  std::printf("Eye resolution: making %dx%d images\n", sizes[0].first, sizes[0].second);
  // xrCreateSwapchain may not use our queue (XR_KHR_vulkan_enable2), so it
  // runs beside the frame loop.
  const XrFoveationProfileFB profile = foveated_ ? profiles_[static_cast<int>(foveationLevel_)] : XR_NULL_HANDLE;
  making_ = std::async(std::launch::async, [this, sizes, foveated = foveated_, profile] {
    std::vector<Swapchain> made;
    for (const auto& [width, height] : sizes) {
      Swapchain sc;
      if (!createSwapchain(width, height, foveated, sc) ||
          (profile != XR_NULL_HANDLE && !applyFoveationProfile(sc.handle, profile))) {
        destroySwapchain(sc);
        for (Swapchain& m : made) destroySwapchain(m);
        return std::vector<Swapchain>{};
      }
      made.push_back(sc);
    }
    return made;
  });
}

bool XrContext::useScale(float scale) {
  activeScale_ = scale;
  bool changed = false;
  for (size_t i = 0; i < swapchains_.size() && i < configViews_.size(); ++i) {
    Swapchain& sc = swapchains_[i];
    const XrViewConfigurationView& cv = configViews_[i];
    const int width =
        std::min(sc.imageWidth, scaledSize(baseWidth_, static_cast<int>(cv.recommendedImageRectWidth), scale));
    const int height =
        std::min(sc.imageHeight, scaledSize(baseHeight_, static_cast<int>(cv.recommendedImageRectHeight), scale));
    if (width == sc.width && height == sc.height) continue;
    sc.width = width;
    sc.height = height;
    sc.area = {{0, 0}, {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}};
    changed = true;
  }
  if (!changed) return false;
  if (foveated_)
    tileGrid_ = eyePassTileGrid(gpu_.caps, static_cast<uint32_t>(swapchains_[0].width),
                                static_cast<uint32_t>(swapchains_[0].height));
  ++targetsVersion_;
  return true;
}

bool XrContext::updateEyeSize() {
  unmaking_.erase(std::remove_if(unmaking_.begin(), unmaking_.end(),
                                 [](const std::future<void>& job) {
                                   return job.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                                 }),
                  unmaking_.end());
  bool changed = false;
  if (making_.valid() && making_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    std::vector<Swapchain> made = making_.get();
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - makingSince_).count();
    if (made.size() != swapchains_.size()) {
      std::fprintf(stderr, "Eye resolution: the images could not be made; staying at %dx%d\n",
                   swapchains_[0].width, swapchains_[0].height);
      retireSwapchains(std::move(made));
      requestedScale_ = activeScale_;
    } else if (imageScaleFor(requestedScale_, imageScale_) != makingScale_) {
      retireSwapchains(std::move(made));  // asked for another size meanwhile
    } else {
      // Between frames nothing is acquired: the old images go as they are.
      std::printf("Eye resolution: %dx%d images made in %.0f ms beside the frame loop\n",
                  made[0].imageWidth, made[0].imageHeight, ms);
      retireSwapchains(std::move(swapchains_));
      swapchains_ = std::move(made);
      layerHeld_ = false;
      imageScale_ = makingScale_;
      ++targetsVersion_;
      changed = true;
    }
  }
  const float images = imageScaleFor(requestedScale_, imageScale_);
  if (images != imageScale_ && !making_.valid()) startMakingImages(images);
  // The size asked for once the images hold it; until then the one in use.
  return useScale(holds(imageScale_, requestedScale_) ? requestedScale_ : activeScale_) || changed;
}

// Runtime profiles are independent of the app's named levels and Custom map.
bool XrContext::createFoveationProfiles() {
  const XrFoveationLevelFB levels[5] = {XR_FOVEATION_LEVEL_NONE_FB, XR_FOVEATION_LEVEL_MEDIUM_FB,
                                      XR_FOVEATION_LEVEL_HIGH_FB, XR_FOVEATION_LEVEL_HIGH_FB,
                                      XR_FOVEATION_LEVEL_HIGH_FB};
  for (int i = 0; i < 5; ++i) {
    XrFoveationEyeTrackedProfileCreateInfoMETA eyeTracked{
        XR_TYPE_FOVEATION_EYE_TRACKED_PROFILE_CREATE_INFO_META};
    XrFoveationLevelProfileCreateInfoFB level{XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
    level.next = &eyeTracked;
    level.level = levels[i];
    level.verticalOffset = 0;
    level.dynamic = XR_FOVEATION_DYNAMIC_DISABLED_FB;
    XrFoveationProfileCreateInfoFB info{XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB};
    info.next = &level;
    if (!check(createFoveationProfile_(session_, &info, &profiles_[i]), "xrCreateFoveationProfileFB")) {
      for (XrFoveationProfileFB& p : profiles_) {
        if (p != XR_NULL_HANDLE) destroyFoveationProfile_(p);
        p = XR_NULL_HANDLE;
      }
      return false;
    }
  }
  return true;
}

bool XrContext::applyFoveationProfile(XrSwapchain swapchain, XrFoveationProfileFB profile) const {
  XrSwapchainStateFoveationFB state{XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB};
  state.profile = profile;
  return check(updateSwapchain_(swapchain, reinterpret_cast<XrSwapchainStateBaseHeaderFB*>(&state)),
               "xrUpdateSwapchainFB");
}

bool XrContext::setFoveationLevel(FoveationLevel level) {
  if (!foveated_) return false;
  const XrFoveationProfileFB want = profiles_[static_cast<int>(level)];
  const XrFoveationProfileFB had = profiles_[static_cast<int>(foveationLevel_)];
  for (size_t i = 0; i < swapchains_.size(); ++i) {
    if (applyFoveationProfile(swapchains_[i].handle, want)) continue;
    // Never eyes with different levels: back to the one they had.
    bool restored = true;
    for (size_t k = 0; k < i; ++k) restored = applyFoveationProfile(swapchains_[k].handle, had) && restored;
    if (!restored) {
      std::fprintf(stderr, "Foveation: the eyes could not be given the same level; drawn unfoveated\n");
      for (auto& sc : swapchains_) applyFoveationProfile(sc.handle, profiles_[0]);
      foveated_ = false;
      foveationLevel_ = FoveationLevel::Off;
    }
    return false;
  }
  foveationLevel_ = level;
  return true;
}

bool XrContext::refreshFoveation() {
  if (!foveated_) return false;
  bool ok = true;
  for (auto& sc : swapchains_)
    ok = applyFoveationProfile(sc.handle, profiles_[static_cast<int>(foveationLevel_)]) && ok;
  return ok;
}

bool XrContext::foveationCentres(float centres[2][2], bool& valid) {
  valid = false;
  if (!foveated_) return false;
  XrFoveationEyeTrackedStateMETA state{XR_TYPE_FOVEATION_EYE_TRACKED_STATE_META};
  if (!check(getFoveationState_(session_, &state), "xrGetFoveationEyeTrackedStateMETA")) return false;
  for (int i = 0; i < 2; ++i) {
    centres[i][0] = state.foveationCenter[i].x;
    centres[i][1] = state.foveationCenter[i].y;
  }
  valid = (state.flags & XR_FOVEATION_EYE_TRACKED_STATE_VALID_BIT_META) != 0;
  return true;
}

void XrContext::diagnoseRefreshRate() {
  if (!refreshExtension_) {
    std::printf("%s\n", strings().refreshRateUnavailable);
    return;
  }
  PFN_xrEnumerateDisplayRefreshRatesFB enumerate = nullptr;
  PFN_xrGetDisplayRefreshRateFB current = nullptr;
  PFN_xrRequestDisplayRefreshRateFB request = nullptr;
  auto proc = [&](const char* name, auto& fn) {
    return check(xrGetInstanceProcAddr(instance_, name,
                                     reinterpret_cast<PFN_xrVoidFunction*>(&fn)), name) && fn;
  };
  if (!proc("xrEnumerateDisplayRefreshRatesFB", enumerate) ||
      !proc("xrGetDisplayRefreshRateFB", current) ||
      !proc("xrRequestDisplayRefreshRateFB", request)) return;
  uint32_t count = 0;
  if (!check(enumerate(session_, 0, &count, nullptr), "xrEnumerateDisplayRefreshRatesFB")) return;
  std::vector<float> rates(count);
  if (!check(enumerate(session_, count, &count, rates.data()), "xrEnumerateDisplayRefreshRatesFB")) return;
  std::printf("%s", strings().refreshRateOffered);
  for (uint32_t i = 0; i < count; ++i) std::printf(" %g", rates[i]);
  std::printf("\n");
  float hz = 0;
  if (check(current(session_, &hz), "xrGetDisplayRefreshRateFB"))
    std::printf(strings().refreshRateCurrent, hz);
  if (refreshRequest_ < 0) return;
  const XrResult result = request(session_, refreshRequest_);
  char name[XR_MAX_RESULT_STRING_SIZE] = "";
  xrResultToString(instance_, result, name);
  std::printf(strings().refreshRateRequested, refreshRequest_, name, static_cast<int>(result));
  if (check(current(session_, &hz), "xrGetDisplayRefreshRateFB"))
    std::printf(strings().refreshRateCurrent, hz);
}

void XrContext::pollEvents(bool& quit) {
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  while (xrPollEvent(instance_, &event) == XR_SUCCESS) {
    switch (event.type) {
      case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
        const auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
        sessionState_ = e->state;
        // FramePlayerX launch-handoff approach; attribution above hideLaunchDashboard.
        // First focus ends the launch handoff, even if another event in this
        // batch takes focus away again. Never close a dashboard opened later.
        if (e->state == XR_SESSION_STATE_FOCUSED || e->state == XR_SESSION_STATE_STOPPING ||
            e->state == XR_SESSION_STATE_EXITING || e->state == XR_SESSION_STATE_LOSS_PENDING)
          launchDashboardPending_ = false;
        layerHeld_ = false;  // shown again only within one unbroken run of frames
        switch (e->state) {
          case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
            beginInfo.primaryViewConfigurationType = kViewConfig;
            if (check(xrBeginSession(session_, &beginInfo), "xrBeginSession")) {
              running_ = true;
              if (refreshDiagnostic_) diagnoseRefreshRate();
            }
            break;
          }
          case XR_SESSION_STATE_STOPPING:
            xrEndSession(session_);
            running_ = false;
            break;
          case XR_SESSION_STATE_EXITING:
          case XR_SESSION_STATE_LOSS_PENDING:
            quit = true;
            break;
          default:
            break;
        }
        break;
      }
      case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
        const auto* e = reinterpret_cast<XrEventDataDisplayRefreshRateChangedFB*>(&event);
        if (refreshDiagnostic_)
          std::printf(strings().refreshRateChanged, e->fromDisplayRefreshRate, e->toDisplayRefreshRate);
        break;
      }
      case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
        input_.readInteractionProfiles();
        break;
      case XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT:
        userPresence_ = reinterpret_cast<XrEventDataUserPresenceChangedEXT*>(&event)->isUserPresent;
        std::printf("Headset presence: %s\n", userPresence_ ? "worn" : "taken off");
        break;
      case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
        quit = true;
        break;
      default:
        break;
    }
    event = {XR_TYPE_EVENT_DATA_BUFFER};
  }
  if (launchDashboardPending_ && sceneSubmitted_ && running_ && !failed_ && !quit &&
      sessionState_ == XR_SESSION_STATE_VISIBLE) {
    // One attempt, only after the scene is ready. Process startup stays off
    // the render thread; a failed spawn must not turn into retries.
    launchDashboardPending_ = false;
    try {
      launchDashboard_ = std::async(std::launch::async, hideLaunchDashboard);
    } catch (const std::system_error& error) {
      std::fprintf(stderr, "OpenXR: launch dashboard worker failed: %s\n", error.what());
    }
  }
}

bool XrContext::beginFrame() {
  frameBegun_ = false;
  viewsValid_ = false;
  viewStateFlags_ = 0;
  XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
  frameState_ = {XR_TYPE_FRAME_STATE};
  if (!checkFrame(xrWaitFrame(session_, &waitInfo, &frameState_), "xrWaitFrame")) return false;
  XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
  // XR_FRAME_DISCARDED (a success code): the frame before was not ended;
  // this one begins all the same.
  if (!checkFrame(xrBeginFrame(session_, &beginInfo), "xrBeginFrame")) return false;
  frameBegun_ = true;

  input_.sync(frameState_.predictedDisplayTime);

  if (!frameState_.shouldRender) return false;

  XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
  locateInfo.viewConfigurationType = kViewConfig;
  locateInfo.displayTime = frameState_.predictedDisplayTime;
  locateInfo.space = headLocked_ ? viewSpace_ : localSpace_;
  XrViewState viewState{XR_TYPE_VIEW_STATE};
  uint32_t count = static_cast<uint32_t>(views_.size());
  if (XR_FAILED(xrLocateViews(session_, &locateInfo, &viewState, count, &count, views_.data())))
    return false;
  // A short result must not leave the previous frame's other eye in use.
  if (count != views_.size()) return false;
  viewStateFlags_ = viewState.viewStateFlags;
  const XrViewStateFlags needed =
      XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
  if ((viewState.viewStateFlags & needed) != needed) return false;

  for (size_t i = 0; i < views_.size(); ++i) {
    const XrView& v = views_[i];
    eyes_[i].projection = projectionFromFov(v.fov.angleLeft, v.fov.angleRight, v.fov.angleUp,
                                            v.fov.angleDown, kNearZ, kFarZ);
    eyes_[i].view = viewFromPose(toQuat(v.pose.orientation), toVec3(v.pose.position));
    eyes_[i].orientation = toQuat(v.pose.orientation);
    eyes_[i].position = toVec3(v.pose.position);
    eyes_[i].tanLeft = std::tan(v.fov.angleLeft);
    eyes_[i].tanRight = std::tan(v.fov.angleRight);
    eyes_[i].tanUp = std::tan(v.fov.angleUp);
    eyes_[i].tanDown = std::tan(v.fov.angleDown);
  }
  logFieldOfView();
  viewsValid_ = true;
  return true;
}

const EyeFrame* XrContext::acquireEyes() {
  if (!frameBegun_ || !viewsValid_) return nullptr;
  for (size_t i = 0; i < swapchains_.size(); ++i) {
    Swapchain& sc = swapchains_[i];
    if (!sc.isAcquired) {
      XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
      if (!checkFrame(xrAcquireSwapchainImage(sc.handle, &acquireInfo, &sc.acquired),
                      "xrAcquireSwapchainImage"))
        return nullptr;
      sc.isAcquired = true;
    }
    // A timeout keeps the image acquired: it is waited for again, a few
    // times, before the runtime counts as hung.
    for (int tries = 0; !sc.isWaited; ++tries) {
      XrSwapchainImageWaitInfo imageWaitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
      imageWaitInfo.timeout = kImageWaitTimeout;
      const XrResult result = xrWaitSwapchainImage(sc.handle, &imageWaitInfo);
      if (result == XR_TIMEOUT_EXPIRED) {
        if (tries + 1 < kImageWaitTries) continue;
        std::fprintf(stderr, "OpenXR: eye %zu's image was not ready in %d s\n", i,
                     kImageWaitTries);
        failed_ = true;
        return nullptr;
      }
      if (!checkFrame(result, "xrWaitSwapchainImage")) return nullptr;
      sc.isWaited = true;
    }
    EyeImage& e = eyeFrame_.eyes[i];
    e.image = sc.images[sc.acquired];
    e.view = sc.views[sc.acquired];
    e.index = sc.acquired;
    e.width = sc.width;
    e.height = sc.height;
    e.area = sc.area;
    const std::vector<VkImageView>& maps = densityMapViews(static_cast<int>(i));
    const bool density = foveated_ && useDensityMaps_ && sc.acquired < maps.size();
    e.densityMap = density ? maps[sc.acquired] : VK_NULL_HANDLE;
    e.densityOffset = density ? sc.densityOffset : VkOffset2D{};
  }
  eyeFrame_.count = static_cast<int>(swapchains_.size());
  return &eyeFrame_;
}

void XrContext::releaseEyes() {
  for (auto& sc : swapchains_) {
    // An image acquired but never waited for cannot be released; the next
    // acquireEyes() waits for it again.
    if (!sc.isWaited) continue;
    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    checkFrame(xrReleaseSwapchainImage(sc.handle, &releaseInfo), "xrReleaseSwapchainImage");
    sc.isAcquired = sc.isWaited = false;
  }
  eyeFrame_.count = 0;
}

// Each eye's field of view and pixel density, when first known and when it changes.
void XrContext::logFieldOfView() {
  constexpr float kDeg = 180.0f / 3.14159265f;
  loggedFov_.resize(views_.size(), XrFovf{});
  for (size_t i = 0; i < views_.size(); ++i) {
    const XrFovf& f = views_[i].fov;
    const XrFovf& old = loggedFov_[i];
    const float change = std::max({std::abs(f.angleLeft - old.angleLeft),
                                   std::abs(f.angleRight - old.angleRight),
                                   std::abs(f.angleUp - old.angleUp),
                                   std::abs(f.angleDown - old.angleDown)});
    if (change * kDeg < 0.5f) continue;
    loggedFov_[i] = f;
    std::printf("OpenXR: eye %zu field of view left %.1f° right %.1f° up %.1f° down %.1f°, "
                "%.1f x %.1f px/° straight ahead\n",
                i, f.angleLeft * kDeg, f.angleRight * kDeg, f.angleUp * kDeg, f.angleDown * kDeg,
                eyePixelsPerDegree(static_cast<int>(i), false),
                eyePixelsPerDegree(static_cast<int>(i), true));
  }
}

float XrContext::eyePixelsPerDegree(int i, bool vertical) const {
  // Straight ahead, one radian spans size / (tangent range) pixels of the eye image.
  const EyeMatrices& e = eyes_[i];
  const float range = vertical ? e.tanUp - e.tanDown : e.tanRight - e.tanLeft;
  const int size = vertical ? swapchains_[i].height : swapchains_[i].width;
  return range > 0 ? size / range * 3.14159265f / 180.0f : 0.0f;
}

void XrContext::endFrame(bool drawn) {
  if (!frameBegun_) return;
  // Only images drawn, submitted and released go to the compositor.
  bool layerReady = drawn && viewsValid_;
  for (size_t i = 0; i < swapchains_.size(); ++i) {
    const Swapchain& sc = swapchains_[i];
    if (sc.isAcquired) layerReady = false;
    XrCompositionLayerProjectionView& pv = projViews_[i];
    pv.pose = views_[i].pose;
    pv.fov = views_[i].fov;
    pv.subImage.swapchain = sc.handle;
    pv.subImage.imageRect.offset = {sc.area.offset.x, sc.area.offset.y};
    pv.subImage.imageRect.extent = {static_cast<int32_t>(sc.area.extent.width),
                                    static_cast<int32_t>(sc.area.extent.height)};
    pv.subImage.imageArrayIndex = 0;
  }
  submitFrame(layerReady);
}

bool XrContext::canShowAgain() const {
  if (!frameBegun_ || !layerHeld_) return false;
  for (const Swapchain& sc : swapchains_)
    if (sc.isAcquired) return false;
  return true;
}

void XrContext::endFrameShownAgain() {
  if (!frameBegun_) return;
  // projViews_ are still the last drawn frame's.
  submitFrame(canShowAgain());
}

void XrContext::submitFrame(bool layerReady) {
  frameBegun_ = false;
  XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  layer.space = headLocked_ ? viewSpace_ : localSpace_;
  layer.viewCount = static_cast<uint32_t>(projViews_.size());
  layer.views = projViews_.data();
  // Alpha blending: colour premultiplied by alpha, as the runtime expects
  // without XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT.
  if (useAlphaBlend_) layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  const XrCompositionLayerBaseHeader* layers[] = {
      reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};

  XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
  endInfo.displayTime = frameState_.predictedDisplayTime;
  endInfo.environmentBlendMode =
      useAlphaBlend_ ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  endInfo.layerCount = layerReady ? 1 : 0;
  endInfo.layers = layerReady ? layers : nullptr;
  layerHeld_ = checkFrame(xrEndFrame(session_, &endInfo), "xrEndFrame") && layerReady;
  if (layerHeld_) sceneSubmitted_ = true;
}

bool XrContext::checkFrame(XrResult result, const char* what) {
  if (check(result, what)) return true;
  if (result == XR_ERROR_SESSION_LOST || result == XR_ERROR_INSTANCE_LOST) failed_ = true;
  return false;
}

void XrContext::queryBlendModes() {
  uint32_t count = 0;
  std::vector<XrEnvironmentBlendMode> modes;
  if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(instance_, systemId_, kViewConfig, 0, &count,
                                                    nullptr))) {
    modes.resize(count);
    if (XR_FAILED(xrEnumerateEnvironmentBlendModes(instance_, systemId_, kViewConfig, count,
                                                   &count, modes.data())))
      modes.clear();
  }
  alphaBlend_ = std::find(modes.begin(), modes.end(), XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) !=
                modes.end();
  std::printf("OpenXR: native passthrough (alpha blending) %s\n",
              alphaBlend_ ? "available" : "not offered");
}

bool XrContext::headPose(Vec3& position, Quat& orientation) const {
  if (headLocked_) {
    position = {};
    orientation = {};
    return true;
  }
  if (!frameBegun_) return false;
  XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
  const bool located = XR_SUCCEEDED(
      xrLocateSpace(viewSpace_, localSpace_, frameState_.predictedDisplayTime, &location));
  const XrSpaceLocationFlags flags = located ? location.locationFlags : 0;
  const bool stereo = views_.size() == 2;
  Vec3 headPosition;
  Quat headOrientation;
  // VIEW's origin is the stereo centroid. Prefer this frame's eyes even when
  // VIEW reports a valid but frozen position.
  if (stereo && (viewStateFlags_ & XR_VIEW_STATE_POSITION_VALID_BIT)) {
    headPosition = (toVec3(views_[0].pose.position) + toVec3(views_[1].pose.position)) * 0.5f;
  } else if (flags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
    headPosition = toVec3(location.pose.position);
  } else {
    return false;
  }
  if (flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) {
    // VIEW excludes eye cant, so keep its orientation whenever available.
    headOrientation = toQuat(location.pose.orientation);
  } else if (stereo && (viewStateFlags_ & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
    const Quat a = toQuat(views_[0].pose.orientation);
    const Quat b = toQuat(views_[1].pose.orientation);
    const float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    const float sign = d < 0 ? -1.0f : 1.0f;
    headOrientation = normalize(Quat{a.x + sign * b.x, a.y + sign * b.y,
                                     a.z + sign * b.z, a.w + sign * b.w});
  } else {
    return false;
  }
  position = headPosition;
  orientation = headOrientation;
  return true;
}
