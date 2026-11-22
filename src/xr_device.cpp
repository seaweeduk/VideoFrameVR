#define XR_USE_GRAPHICS_API_VULKAN

#include "xr_device.h"

#include "platform.h"
#include "xr_util.h"

#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// createVulkanDevice's steps, in order. `instance_`, `systemId_`,
// `foveationExtensions_` and `gpu` are given and `gpu_` is the device made;
// the rest is what a step finds for later ones.
struct DeviceBuilder {
  XrInstance instance_;
  XrSystemId systemId_;
  bool foveationExtensions_;
  const GpuRequirements& gpu;
  GpuDevice& gpu_;
  GpuCapabilities& caps = gpu_.caps;

  PFN_xrGetVulkanGraphicsDevice2KHR getDevice = nullptr;
  PFN_xrCreateVulkanDeviceKHR createDevice = nullptr;
  VkResult vkResult = VK_SUCCESS;
  bool mirror = gpu.mirror, surfaceMaintenance = false;
  VkPhysicalDeviceIDProperties ids{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
  VkPhysicalDevicePushDescriptorPropertiesKHR push{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
  VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
  VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  const VkPhysicalDeviceProperties& props = props2.properties;
  std::vector<VkQueueFamilyProperties> families;
  std::vector<VkExtensionProperties> offered;
  std::vector<const char*> deviceExts;
  bool maintenance = false;
  // Queried, then chained through pNext into the device's creation.
  // SteamVR syncs with its compositor through timeline semaphores on our
  // queue, and so do GL and the scene (gpu_context.h).
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
  VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT presentFences{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
  VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  VkPhysicalDeviceFragmentDensityMapFeaturesEXT fdm{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT};
  VkPhysicalDeviceFragmentDensityMapOffsetFeaturesQCOM fdmOffset{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_OFFSET_FEATURES_QCOM};
  VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptorBuffer{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
  VkPhysicalDeviceBufferDeviceAddressFeatures deviceAddress{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
  VkPhysicalDeviceDescriptorIndexingFeatures indexing{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
  VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};

  bool check(XrResult result, const char* what) const { return xrCheck(instance_, result, what); }
  bool offers(const char* name) const {
    return std::any_of(offered.begin(), offered.end(), [&](const VkExtensionProperties& e) {
      return std::strcmp(e.extensionName, name) == 0;
    });
  }
  bool makeInstance();
  bool findPhysicalDevice();
  bool chooseExtensions();
  bool chooseFeatures();
  bool chooseVideoFeatures();
  bool makeDevice();
};

bool DeviceBuilder::makeInstance() {
  // The runtime requires this call before any Vulkan objects are created.
  PFN_xrGetVulkanGraphicsRequirements2KHR getRequirements = nullptr;
  PFN_xrCreateVulkanInstanceKHR createInstance = nullptr;
  XR_TRY(xrGetInstanceProcAddr(instance_, "xrGetVulkanGraphicsRequirements2KHR",
                               reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)));
  XR_TRY(xrGetInstanceProcAddr(instance_, "xrCreateVulkanInstanceKHR",
                               reinterpret_cast<PFN_xrVoidFunction*>(&createInstance)));
  XR_TRY(xrGetInstanceProcAddr(instance_, "xrGetVulkanGraphicsDevice2KHR",
                               reinterpret_cast<PFN_xrVoidFunction*>(&getDevice)));
  XR_TRY(xrGetInstanceProcAddr(instance_, "xrCreateVulkanDeviceKHR",
                               reinterpret_cast<PFN_xrVoidFunction*>(&createDevice)));

  XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
  XR_TRY(getRequirements(instance_, systemId_, &requirements));
  // Vulkan 1.2: render pass 2 and timeline semaphores are core there.
  const XrVersion wanted = XR_MAKE_VERSION(1, 2, 0);
  auto majorMinor = [](XrVersion v) { return XR_MAKE_VERSION(XR_VERSION_MAJOR(v), XR_VERSION_MINOR(v), 0); };
  if (wanted < majorMinor(requirements.minApiVersionSupported) ||
      wanted > majorMinor(requirements.maxApiVersionSupported)) {
    std::fprintf(stderr, "The OpenXR runtime supports Vulkan %u.%u to %u.%u, not 1.2\n",
                 XR_VERSION_MAJOR(requirements.minApiVersionSupported),
                 XR_VERSION_MINOR(requirements.minApiVersionSupported),
                 XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
                 XR_VERSION_MINOR(requirements.maxApiVersionSupported));
    return false;
  }

  VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  appInfo.pApplicationName = kAppName;
  appInfo.pEngineName = kAppName;
  appInfo.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo vkInstanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  vkInstanceInfo.pApplicationInfo = &appInfo;
  // The mirror window's surface, and its present fences where offered.
  std::vector<const char*> instanceExts;
  if (mirror) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
    auto has = [&](const char* name) {
      return std::any_of(available.begin(), available.end(), [&](const VkExtensionProperties& e) {
        return std::strcmp(e.extensionName, name) == 0;
      });
    };
    for (const char* name : gpu.mirrorInstanceExtensions) {
      if (!has(name)) {
        std::printf("Mirror: off, Vulkan lacks %s\n", name);
        mirror = false;
      }
    }
    if (mirror) instanceExts = gpu.mirrorInstanceExtensions;
    surfaceMaintenance = mirror && has(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) &&
                         has(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    if (surfaceMaintenance) {
      for (const char* name : {VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
                               VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME})
        if (std::none_of(instanceExts.begin(), instanceExts.end(),
                         [&](const char* e) { return std::strcmp(e, name) == 0; }))
          instanceExts.push_back(name);
    }
  }
  vkInstanceInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExts.size());
  vkInstanceInfo.ppEnabledExtensionNames = instanceExts.data();

  XrVulkanInstanceCreateInfoKHR xrInstanceInfo{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
  xrInstanceInfo.systemId = systemId_;
  xrInstanceInfo.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
  xrInstanceInfo.vulkanCreateInfo = &vkInstanceInfo;
  XR_TRY(createInstance(instance_, &xrInstanceInfo, &gpu_.instance, &vkResult));
  VK_TRY(vkResult);
  return true;
}

bool DeviceBuilder::findPhysicalDevice() {
  XrVulkanGraphicsDeviceGetInfoKHR deviceGetInfo{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
  deviceGetInfo.systemId = systemId_;
  deviceGetInfo.vulkanInstance = gpu_.instance;
  XR_TRY(getDevice(instance_, &deviceGetInfo, &gpu_.physical));

  ids.pNext = &push;
  push.pNext = &driver;
  props2.pNext = &ids;
  vkGetPhysicalDeviceProperties2(gpu_.physical, &props2);
  std::printf("Vulkan device: %s (%04x:%08x), %s %s\n", props.deviceName, props.vendorID,
              props.deviceID, driver.driverName, driver.driverInfo);
  if (props.apiVersion < VK_API_VERSION_1_2) {
    std::fprintf(stderr, "Vulkan: the device supports Vulkan %u.%u, not 1.2\n",
                 VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));
    return false;
  }
  if (props.limits.maxPushConstantsSize < 256) {
    std::fprintf(stderr, "Vulkan: the device has %u bytes of push constants, not 256\n",
                 props.limits.maxPushConstantsSize);
    return false;
  }

  uint32_t familyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(gpu_.physical, &familyCount, nullptr);
  families.resize(familyCount);
  vkGetPhysicalDeviceQueueFamilyProperties(gpu_.physical, &familyCount, families.data());
  bool found = false;
  for (uint32_t i = 0; i < familyCount; ++i) {
    if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      gpu_.queueFamily = i;
      found = true;
      break;
    }
  }
  if (!found) {
    std::fprintf(stderr, "Vulkan: no graphics queue\n");
    return false;
  }
  return true;
}

bool DeviceBuilder::chooseExtensions() {
  uint32_t extCount = 0;
  vkEnumerateDeviceExtensionProperties(gpu_.physical, nullptr, &extCount, nullptr);
  offered.resize(extCount);
  vkEnumerateDeviceExtensionProperties(gpu_.physical, nullptr, &extCount, offered.data());
  for (const char* name : {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                           VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
                           VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME}) {
    if (!offers(name)) {
      std::fprintf(stderr, "Vulkan: the device lacks %s\n", name);
      return false;
    }
    deviceExts.push_back(name);
  }
  if (push.maxPushDescriptors < 2) {
    std::fprintf(stderr, "Vulkan: the device pushes %u descriptors, not 2\n",
                 push.maxPushDescriptors);
    return false;
  }
  caps.memoryBudget = offers(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
  if (caps.memoryBudget) deviceExts.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
  if (mirror && !offers(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
    std::printf("Mirror: off, the device lacks %s\n", VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    mirror = false;
  }
  if (mirror) deviceExts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  caps.mirror = mirror;
  // Decoded frames sampled straight from the decoder's buffers (Frame,
  // docs/vulkan-renderer.md 11): DMA-BUF images with explicit layouts.
  if (gpu.directVideo) {
    caps.directVideo = true;
    for (const char* name : {VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                             VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME})
      caps.directVideo = caps.directVideo && offers(name);
    if (caps.directVideo) {
      deviceExts.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
      deviceExts.push_back(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
      caps.foreignQueue = offers(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
      if (caps.foreignQueue) deviceExts.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
    } else {
      std::printf("Video: the device cannot sample decoder buffers; mpv draws every frame\n");
    }
  }
  return true;
}

bool DeviceBuilder::chooseFeatures() {
  maintenance =
      mirror && surfaceMaintenance && offers(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
  if (maintenance) timeline.pNext = &presentFences;
  supported.pNext = &timeline;
  // Foveation: density maps on non-subsampled images, moved by offsets
  // (VK_QCOM_fragment_density_map_offset; the Frame's headers lack the EXT
  // structures, docs/vulkan-renderer.md 2.2).
  const bool fdmOffered = gpu.foveation && foveationExtensions_ &&
                          offers(VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME) &&
                          offers(VK_QCOM_FRAGMENT_DENSITY_MAP_OFFSET_EXTENSION_NAME);
  if (fdmOffered) {
    timeline.pNext = &fdm;
    fdm.pNext = &fdmOffset;
  }
  vkGetPhysicalDeviceFeatures2(gpu_.physical, &supported);
  if (!timeline.timelineSemaphore) {
    std::fprintf(stderr, "Vulkan: the device lacks timeline semaphores\n");
    return false;
  }
  caps.foveation = fdmOffered && fdm.fragmentDensityMap && fdm.fragmentDensityMapNonSubsampledImages &&
                   fdmOffset.fragmentDensityMapOffset;
  if (caps.foveation) {
    VkPhysicalDeviceFragmentDensityMapOffsetPropertiesQCOM offsetProps{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_OFFSET_PROPERTIES_QCOM};
    VkPhysicalDeviceFragmentDensityMapPropertiesEXT fdmProps{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_PROPERTIES_EXT};
    fdmProps.pNext = &offsetProps;
    VkPhysicalDeviceProperties2 fdmProps2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    fdmProps2.pNext = &fdmProps;
    vkGetPhysicalDeviceProperties2(gpu_.physical, &fdmProps2);
    // The maps are R8G8_UNORM, which Vulkan requires to work as one with the
    // feature (Turnip 26.3 does not list it among the format's features).
    caps.densityOffsetGranularity = offsetProps.fragmentDensityOffsetGranularity;
    caps.minDensityTexelSize = fdmProps.minFragmentDensityTexelSize;
    caps.maxDensityTexelSize = fdmProps.maxFragmentDensityTexelSize;
    caps.foveation = caps.maxDensityTexelSize.width > 0 && caps.maxDensityTexelSize.height > 0;
  }
  if (gpu.foveation && foveationExtensions_ && !caps.foveation)
    std::printf("Foveation: the GPU lacks fragment density maps with offsets\n");
  timeline.pNext = nullptr;
  if (caps.foveation) {
    deviceExts.push_back(VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME);
    deviceExts.push_back(VK_QCOM_FRAGMENT_DENSITY_MAP_OFFSET_EXTENSION_NAME);
    fdm.fragmentDensityMapDynamic = VK_FALSE;
    fdm.pNext = &fdmOffset;
    fdmOffset.pNext = timeline.pNext;
    timeline.pNext = &fdm;
  }
  return true;
}

bool DeviceBuilder::chooseVideoFeatures() {
  // Q10C decoder frames sampled as the TP10 textures they are (Frame,
  // docs/vulkan-renderer.md 11): descriptors written into a descriptor
  // buffer, indexed by row band per pixel.
  if (caps.directVideo && offers(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME) &&
      offers(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) {
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    query.pNext = &descriptorBuffer;
    descriptorBuffer.pNext = &deviceAddress;
    deviceAddress.pNext = &indexing;
    vkGetPhysicalDeviceFeatures2(gpu_.physical, &query);
    caps.tp10 = descriptorBuffer.descriptorBuffer && deviceAddress.bufferDeviceAddress &&
                indexing.shaderSampledImageArrayNonUniformIndexing;
  }
  if (caps.tp10) {
    deviceExts.push_back(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
    deviceExts.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    descriptorBuffer = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
    descriptorBuffer.descriptorBuffer = VK_TRUE;
    deviceAddress = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    deviceAddress.bufferDeviceAddress = VK_TRUE;
    indexing = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    indexing.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    indexing.pNext = timeline.pNext;
    deviceAddress.pNext = &indexing;
    descriptorBuffer.pNext = &deviceAddress;
    timeline.pNext = &descriptorBuffer;
  }
  // Q08C decoder frames sampled as the NV12 images they are, through a
  // Y'CbCr conversion that only places the chroma.
  if (caps.directVideo) {
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    query.pNext = &ycbcr;
    vkGetPhysicalDeviceFeatures2(gpu_.physical, &query);
    caps.q08c = ycbcr.samplerYcbcrConversion;
  }
  if (caps.q08c) {
    ycbcr.pNext = timeline.pNext;
    timeline.pNext = &ycbcr;
  }
  return true;
}

bool DeviceBuilder::makeDevice() {
  // The mirror waits for its presentations before replacing its swapchain.
  if (maintenance && presentFences.swapchainMaintenance1) {
    caps.presentFences = true;
    deviceExts.push_back(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
    presentFences.pNext = timeline.pNext;
    timeline.pNext = &presentFences;
  }
  VkPhysicalDeviceFeatures enabled{};
  enabled.samplerAnisotropy = supported.features.samplerAnisotropy;
  caps.samplerAnisotropy = enabled.samplerAnisotropy == VK_TRUE;
  caps.maxAnisotropy = caps.samplerAnisotropy ? props.limits.maxSamplerAnisotropy : 1.0f;

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queueInfo.queueFamilyIndex = gpu_.queueFamily;
  queueInfo.queueCount = 1;
  queueInfo.pQueuePriorities = &priority;
  VkDeviceCreateInfo vkDeviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  vkDeviceInfo.pNext = &timeline;
  vkDeviceInfo.queueCreateInfoCount = 1;
  vkDeviceInfo.pQueueCreateInfos = &queueInfo;
  vkDeviceInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExts.size());
  vkDeviceInfo.ppEnabledExtensionNames = deviceExts.data();
  vkDeviceInfo.pEnabledFeatures = &enabled;

  XrVulkanDeviceCreateInfoKHR xrDeviceInfo{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
  xrDeviceInfo.systemId = systemId_;
  xrDeviceInfo.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
  xrDeviceInfo.vulkanPhysicalDevice = gpu_.physical;
  xrDeviceInfo.vulkanCreateInfo = &vkDeviceInfo;
  XR_TRY(createDevice(instance_, &xrDeviceInfo, &gpu_.device, &vkResult));
  VK_TRY(vkResult);
  vkGetDeviceQueue(gpu_.device, gpu_.queueFamily, 0, &gpu_.queue);

  caps.maxPushDescriptors = push.maxPushDescriptors;
  caps.timestampPeriod = props.limits.timestampPeriod;
  caps.timestampValidBits = families[gpu_.queueFamily].timestampValidBits;
  caps.nonCoherentAtomSize = props.limits.nonCoherentAtomSize;
  caps.bufferCopyOffsetAlignment = std::max<VkDeviceSize>(4, props.limits.optimalBufferCopyOffsetAlignment);
  caps.maxImageDimension2D = props.limits.maxImageDimension2D;
  caps.vendorId = props.vendorID;
  caps.deviceId = props.deviceID;
  caps.driverId = driver.driverID;
  std::memcpy(caps.deviceUuid, ids.deviceUUID, VK_UUID_SIZE);
  std::memcpy(caps.driverUuid, ids.driverUUID, VK_UUID_SIZE);
  std::memcpy(caps.pipelineCacheUuid, props.pipelineCacheUUID, VK_UUID_SIZE);
  std::printf("Vulkan: anisotropy %.0fx, %u push descriptors, timestamps %s, memory budget %s%s\n",
              caps.maxAnisotropy, caps.maxPushDescriptors,
              caps.timestampValidBits > 0 ? "yes" : "no", caps.memoryBudget ? "yes" : "no",
              caps.mirror ? caps.presentFences ? ", mirror with present fences" : ", mirror" : "");
  if (caps.foveation)
    std::printf("Vulkan: density maps up to %ux%u px per texel, offsets in steps of %ux%u\n",
                caps.maxDensityTexelSize.width, caps.maxDensityTexelSize.height,
                caps.densityOffsetGranularity.width, caps.densityOffsetGranularity.height);
  return true;
}

}  // namespace

bool createVulkanDevice(XrInstance instance, XrSystemId systemId, const GpuRequirements& gpu,
                        bool foveationExtensions, GpuDevice& out) {
  DeviceBuilder builder{instance, systemId, foveationExtensions, gpu, out};
  return builder.makeInstance() && builder.findPhysicalDevice() && builder.chooseExtensions() &&
         builder.chooseFeatures() && builder.chooseVideoFeatures() && builder.makeDevice();
}
