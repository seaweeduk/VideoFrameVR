#include "gl_interop.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

// GL_EXT_memory_object(_fd), GL_EXT_semaphore(_fd) and GL_NV_timeline_semaphore.
struct GlFunctions {
  PFNGLCREATEMEMORYOBJECTSEXTPROC createMemoryObjects = nullptr;
  PFNGLDELETEMEMORYOBJECTSEXTPROC deleteMemoryObjects = nullptr;
  PFNGLMEMORYOBJECTPARAMETERIVEXTPROC memoryObjectParameteriv = nullptr;
  PFNGLIMPORTMEMORYFDEXTPROC importMemoryFd = nullptr;
  PFNGLTEXTURESTORAGEMEM2DEXTPROC textureStorageMem2D = nullptr;
  PFNGLDELETESEMAPHORESEXTPROC deleteSemaphores = nullptr;
  PFNGLIMPORTSEMAPHOREFDEXTPROC importSemaphoreFd = nullptr;
  PFNGLWAITSEMAPHOREEXTPROC waitSemaphore = nullptr;
  PFNGLSIGNALSEMAPHOREEXTPROC signalSemaphore = nullptr;
  PFNGLSEMAPHOREPARAMETERUI64VEXTPROC semaphoreParameterui64v = nullptr;
  PFNGLGETUNSIGNEDBYTEVEXTPROC getUnsignedBytev = nullptr;
  PFNGLGETUNSIGNEDBYTEI_VEXTPROC getUnsignedBytei_v = nullptr;
  PFNGLCREATESEMAPHORESNVPROC createSemaphores = nullptr;
  PFNGLSEMAPHOREPARAMETERIVNVPROC semaphoreParameteriv = nullptr;
};
GlFunctions gl;

template <typename F>
bool load(F& f, const char* name) {
  f = reinterpret_cast<F>(glfwGetProcAddress(name));
  if (!f) std::fprintf(stderr, "OpenGL: %s is missing\n", name);
  return f != nullptr;
}

bool hasGlExtension(const char* name) {
  GLint count = 0;
  glGetIntegerv(GL_NUM_EXTENSIONS, &count);
  for (GLint i = 0; i < count; ++i) {
    const auto* e = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));
    if (e && std::strcmp(e, name) == 0) return true;
  }
  return false;
}

void clearGlErrors() {
  while (glGetError() != GL_NO_ERROR) {
  }
}

constexpr VkFormat kFormats[2] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM};

}  // namespace

GlInterop::~GlInterop() {
  if (!gpu_) return;
  // Shutdown: whatever GL still does with shared memory finishes first.
  glFinish();
  if (glDoneGl_) gl.deleteSemaphores(1, &glDoneGl_);
  if (vkFrameGl_) gl.deleteSemaphores(1, &vkFrameGl_);
  gpu_->setGlTimeline(VK_NULL_HANDLE);
  VkDevice d = gpu_->device();
  VkSemaphore glDone = glDone_;
  if (glDone) gpu_->retire([d, glDone] { vkDestroySemaphore(d, glDone, nullptr); });
}

bool GlInterop::loadGl() {
  for (const char* ext : {"GL_EXT_memory_object", "GL_EXT_memory_object_fd", "GL_EXT_semaphore",
                          "GL_EXT_semaphore_fd", "GL_NV_timeline_semaphore"}) {
    if (!hasGlExtension(ext)) {
      std::fprintf(stderr, "OpenGL: %s is missing (%s, %s)\n", ext,
                   reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                   reinterpret_cast<const char*>(glGetString(GL_VERSION)));
      if (gpu_->caps().vendorId == 0x1002)
        std::fprintf(stderr, "AMD requires Mesa 26.1+ radeonsi with timeline semaphore support\n");
      return false;
    }
  }
  return load(gl.createMemoryObjects, "glCreateMemoryObjectsEXT") &&
         load(gl.deleteMemoryObjects, "glDeleteMemoryObjectsEXT") &&
         load(gl.memoryObjectParameteriv, "glMemoryObjectParameterivEXT") &&
         load(gl.importMemoryFd, "glImportMemoryFdEXT") &&
         load(gl.textureStorageMem2D, "glTextureStorageMem2DEXT") &&
         load(gl.deleteSemaphores, "glDeleteSemaphoresEXT") &&
         load(gl.importSemaphoreFd, "glImportSemaphoreFdEXT") &&
         load(gl.waitSemaphore, "glWaitSemaphoreEXT") &&
         load(gl.signalSemaphore, "glSignalSemaphoreEXT") &&
         load(gl.semaphoreParameterui64v, "glSemaphoreParameterui64vEXT") &&
         load(gl.getUnsignedBytev, "glGetUnsignedBytevEXT") &&
         load(gl.getUnsignedBytei_v, "glGetUnsignedBytei_vEXT") &&
         load(gl.createSemaphores, "glCreateSemaphoresNV") &&
         load(gl.semaphoreParameteriv, "glSemaphoreParameterivNV");
}

bool GlInterop::init(GpuContext& gpu) {
  gpu_ = &gpu;
  if (!loadGl()) return false;
  const GpuCapabilities& caps = gpu.caps();

  // Opaque file descriptors only work between the same device and driver.
  GLint devices = 0;
  glGetIntegerv(GL_NUM_DEVICE_UUIDS_EXT, &devices);
  bool sameDevice = false;
  for (GLint i = 0; i < devices && !sameDevice; ++i) {
    GLubyte uuid[GL_UUID_SIZE_EXT] = {};
    gl.getUnsignedBytei_v(GL_DEVICE_UUID_EXT, static_cast<GLuint>(i), uuid);
    sameDevice = std::memcmp(uuid, caps.deviceUuid, VK_UUID_SIZE) == 0;
  }
  GLubyte driver[GL_UUID_SIZE_EXT] = {};
  gl.getUnsignedBytev(GL_DRIVER_UUID_EXT, driver);
  const bool sameDriver = std::memcmp(driver, caps.driverUuid, VK_UUID_SIZE) == 0;
  if (!sameDevice || !sameDriver) {
    std::fprintf(stderr, "OpenGL (%s, %s) does not run on the %s Vulkan uses\n",
                 reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                 reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                 sameDevice ? "driver" : "GPU");
    return false;
  }

  // Timeline semaphores shared as opaque file descriptors.
  VkSemaphoreTypeCreateInfo timelineType{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
  timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  VkPhysicalDeviceExternalSemaphoreInfo semaphoreInfo{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
  semaphoreInfo.pNext = &timelineType;
  semaphoreInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkExternalSemaphoreProperties semaphoreProps{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
  vkGetPhysicalDeviceExternalSemaphoreProperties(gpu.physicalDevice(), &semaphoreInfo,
                                                 &semaphoreProps);
  if (!(semaphoreProps.externalSemaphoreFeatures &
        VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT)) {
    std::fprintf(stderr, "Vulkan: timeline semaphores cannot be shared with OpenGL\n");
    return false;
  }

  // Every usage the format supports (EXT_external_objects, issue 13).
  VkFormatProperties formatProps{};
  vkGetPhysicalDeviceFormatProperties(gpu.physicalDevice(), kFormats[0], &formatProps);
  const VkFormatFeatureFlags features = formatProps.optimalTilingFeatures;
  usage_ = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  if (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) usage_ |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
  if (features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) usage_ |= VK_IMAGE_USAGE_STORAGE_BIT;

  VkImageFormatListCreateInfo formatList{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
  formatList.viewFormatCount = 2;
  formatList.pViewFormats = kFormats;
  VkPhysicalDeviceExternalImageFormatInfo externalInfo{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
  externalInfo.pNext = &formatList;
  externalInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkPhysicalDeviceImageFormatInfo2 imageInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
  imageInfo.pNext = &externalInfo;
  imageInfo.format = kFormats[0];
  imageInfo.type = VK_IMAGE_TYPE_2D;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.usage = usage_;
  imageInfo.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  VkExternalImageFormatProperties externalProps{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
  VkImageFormatProperties2 imageProps{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
  imageProps.pNext = &externalProps;
  if (!gpu.check(vkGetPhysicalDeviceImageFormatProperties2(gpu.physicalDevice(), &imageInfo,
                                                           &imageProps),
                 "querying shared images") ||
      !(externalProps.externalMemoryProperties.externalMemoryFeatures &
        VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
    std::fprintf(stderr, "Vulkan: images cannot be shared with OpenGL\n");
    return false;
  }
  GLint glMax = 0;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &glMax);
  maxSize_ = std::min({imageProps.imageFormatProperties.maxExtent.width,
                       imageProps.imageFormatProperties.maxExtent.height,
                       static_cast<uint32_t>(std::max(glMax, 1))});

  // glDone: made here, signalled by GL. The frame timeline: GpuContext's.
  VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
  exportInfo.pNext = &timelineType;
  exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkSemaphoreCreateInfo createInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  createInfo.pNext = &exportInfo;
  if (!gpu.check(vkCreateSemaphore(gpu.device(), &createInfo, nullptr, &glDone_), "GL timeline"))
    return false;
  if (!importSemaphore(glDone_, glDoneGl_, "GL timeline") ||
      !importSemaphore(gpu.frameTimeline(), vkFrameGl_, "frame timeline"))
    return false;
  gpu.setGlTimeline(glDone_);
  std::printf("GL/Vulkan sharing: %s, images up to %u\n",
              reinterpret_cast<const char*>(glGetString(GL_RENDERER)), maxSize_);
  return true;
}

bool GlInterop::importSemaphore(VkSemaphore semaphore, GLuint& out, const char* what) {
  VkSemaphoreGetFdInfoKHR fdInfo{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
  fdInfo.semaphore = semaphore;
  fdInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
  int fd = -1;
  if (!gpu_->check(gpu_->getSemaphoreFd(gpu_->device(), &fdInfo, &fd), what)) return false;
  clearGlErrors();
  gl.createSemaphores(1, &out);
  const GLint type = GL_SEMAPHORE_TYPE_TIMELINE_NV;
  gl.semaphoreParameteriv(out, GL_SEMAPHORE_TYPE_NV, &type);
  gl.importSemaphoreFd(out, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);  // GL owns the fd now
  const GLenum error = glGetError();
  if (error != GL_NO_ERROR) {
    std::fprintf(stderr, "OpenGL: importing the %s failed (0x%x)\n", what, error);
    close(fd);
    gl.deleteSemaphores(1, &out);
    out = 0;
    return false;
  }
  return true;
}

std::unique_ptr<SharedImage> GlInterop::create(uint32_t width, uint32_t height, uint32_t levels,
                                               const char* what) {
  GpuContext& gpu = *gpu_;
  VkDevice d = gpu.device();
  auto image = std::make_unique<SharedImage>();
  Texture& t = image->texture_;
  t.width = width;
  t.height = height;
  t.levels = levels;
  t.shared = image.get();
  auto fail = [&](const char* step) {
    std::fprintf(stderr, "Shared image for %s (%ux%u, %u levels): %s failed\n", what, width,
                 height, levels, step);
    destroyGl(*image);
    if (t.view) vkDestroyImageView(d, t.view, nullptr);
    if (t.image) vkDestroyImage(d, t.image, nullptr);
    if (image->memory_) vkFreeMemory(d, image->memory_, nullptr);
    return nullptr;
  };
  if (width == 0 || height == 0 || width > maxSize_ || height > maxSize_) return fail("its size");

  VkImageFormatListCreateInfo formatList{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
  formatList.viewFormatCount = 2;
  formatList.pViewFormats = kFormats;
  VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  external.pNext = &formatList;
  external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.pNext = &external;
  info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = kFormats[0];
  info.extent = {width, height, 1};
  info.mipLevels = levels;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage_;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!gpu.check(vkCreateImage(d, &info, nullptr, &t.image), "creating a shared image"))
    return fail("vkCreateImage");

  VkMemoryRequirements reqs;
  vkGetImageMemoryRequirements(d, t.image, &reqs);
  VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dedicated.image = t.image;
  VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
  exportInfo.pNext = &dedicated;
  exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.pNext = &exportInfo;
  alloc.allocationSize = reqs.size;
  alloc.memoryTypeIndex = gpu.memoryType(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (alloc.memoryTypeIndex == UINT32_MAX ||
      !gpu.check(vkAllocateMemory(d, &alloc, nullptr, &image->memory_), "allocating a shared image"))
    return fail("allocating its memory");
  if (!gpu.check(vkBindImageMemory(d, t.image, image->memory_, 0), "binding a shared image"))
    return fail("binding its memory");
  image->bytes_ = reqs.size;

  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = t.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = kFormats[0];
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
  if (!gpu.check(vkCreateImageView(d, &view, nullptr, &t.view), "a shared image's view"))
    return fail("its view");

  VkMemoryGetFdInfoKHR fdInfo{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
  fdInfo.memory = image->memory_;
  fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  int fd = -1;
  if (!gpu.check(gpu.getMemoryFd(d, &fdInfo, &fd), "exporting a shared image"))
    return fail("exporting its memory");

  // GL: dedicated, optimally tiled, exactly as allocated.
  clearGlErrors();
  gl.createMemoryObjects(1, &image->memoryObject_);
  const GLint isDedicated = GL_TRUE;
  gl.memoryObjectParameteriv(image->memoryObject_, GL_DEDICATED_MEMORY_OBJECT_EXT, &isDedicated);
  gl.importMemoryFd(image->memoryObject_, reqs.size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);
  if (glGetError() != GL_NO_ERROR) {
    close(fd);  // not taken over
    return fail("importing it into OpenGL");
  }
  glCreateTextures(GL_TEXTURE_2D, 1, &image->glTexture_);
  glTextureParameteri(image->glTexture_, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
  gl.textureStorageMem2D(image->glTexture_, static_cast<GLsizei>(levels), GL_SRGB8_ALPHA8,
                         static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                         image->memoryObject_, 0);
  glTextureParameteri(image->glTexture_, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTextureParameteri(image->glTexture_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTextureParameteri(image->glTexture_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTextureParameteri(image->glTexture_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glCreateFramebuffers(1, &image->framebuffer_);
  glNamedFramebufferTexture(image->framebuffer_, GL_COLOR_ATTACHMENT0, image->glTexture_, 0);
  const GLenum status = glCheckNamedFramebufferStatus(image->framebuffer_, GL_FRAMEBUFFER);
  const GLenum error = glGetError();
  if (status != GL_FRAMEBUFFER_COMPLETE || error != GL_NO_ERROR) {
    std::fprintf(stderr, "OpenGL: shared texture error 0x%x, framebuffer 0x%x\n", error, status);
    return fail("its OpenGL texture");
  }

  // Its first layout, then over to GL, which waits for this submission.
  VkCommandBuffer cmd = gpu.beginOneShot();
  if (!cmd) return fail("its first transition");
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = 0;
  barrier.dstAccessMask = 0;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcQueueFamilyIndex = gpu.queueFamily();
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
  barrier.image = t.image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &barrier);
  const uint64_t value = gpu.submitOneShot(cmd);
  if (value == 0) return fail("its first transition");
  image->lastVulkanUse_ = value;
  image->vulkanLast_ = true;
  gpu.countAllocation(image->bytes_);
  ++live_;
  return image;
}

void GlInterop::destroyGl(SharedImage& image) {
  if (image.framebuffer_) glDeleteFramebuffers(1, &image.framebuffer_);
  if (image.glTexture_) glDeleteTextures(1, &image.glTexture_);
  if (image.memoryObject_) gl.deleteMemoryObjects(1, &image.memoryObject_);
  image.framebuffer_ = image.glTexture_ = image.memoryObject_ = 0;
}

void GlInterop::retire(std::unique_ptr<SharedImage> image) {
  if (!image) return;
  uint64_t glValue = image->lastGlUse_;
  if (image->inGl_) {
    // GL's work on it so far ends at the next signal.
    inGl_.erase(std::remove(inGl_.begin(), inGl_.end(), image.get()), inGl_.end());
    signalNeeded_ = true;
    glValue = glDoneValue_ + 1;
  }
  inFrame_.erase(std::remove(inFrame_.begin(), inFrame_.end(), image.get()), inFrame_.end());
  // GL lets go of deleted objects once its commands are done; the memory
  // stays until both timelines are past it.
  destroyGl(*image);
  VkDevice d = gpu_->device();
  const Texture t = image->texture_;
  VkDeviceMemory memory = image->memory_;
  --live_;
  gpu_->retire([d, t, memory] {
    vkDestroyImageView(d, t.view, nullptr);
    vkDestroyImage(d, t.image, nullptr);
    vkFreeMemory(d, memory, nullptr);
  }, glValue, image->bytes_);
}

void GlInterop::acquireForGl(SharedImage& image) {
  if (image.inGl_) return;
  // After Vulkan's last release of it, or (never sampled since) GL's own
  // last publication.
  const GLuint semaphore = image.vulkanLast_ ? vkFrameGl_ : glDoneGl_;
  const GLuint64 value = image.vulkanLast_ ? image.lastVulkanUse_ : image.publishedAt_;
  const GLenum layout = GL_LAYOUT_SHADER_READ_ONLY_EXT;
  gl.semaphoreParameterui64v(semaphore, GL_TIMELINE_SEMAPHORE_VALUE_NV, &value);
  gl.waitSemaphore(semaphore, 0, nullptr, 1, &image.glTexture_, &layout);
  image.inGl_ = true;
  inGl_.push_back(&image);
  signalNeeded_ = true;
}

void GlInterop::publish() {
  if (!signalNeeded_) return;
  std::vector<GLuint> textures;
  std::vector<GLenum> layouts;
  for (SharedImage* image : inGl_) {
    if (image->keepInGl_) continue;
    textures.push_back(image->glTexture_);
    layouts.push_back(GL_LAYOUT_SHADER_READ_ONLY_EXT);
  }
  const GLuint64 value = ++glDoneValue_;
  gl.semaphoreParameterui64v(glDoneGl_, GL_TIMELINE_SEMAPHORE_VALUE_NV, &value);
  gl.signalSemaphore(glDoneGl_, 0, nullptr, static_cast<GLuint>(textures.size()), textures.data(),
                     layouts.data());
  glFlush();
  std::vector<SharedImage*> kept;
  for (SharedImage* image : inGl_) {
    image->lastGlUse_ = value;
    if (image->keepInGl_) {
      kept.push_back(image);
      continue;
    }
    image->inGl_ = false;
    image->vulkanLast_ = false;
    image->publishedAt_ = value;
  }
  inGl_ = kept;
  signalNeeded_ = !inGl_.empty();
}

bool GlInterop::useInFrame(SharedImage& image) {
  if (image.inGl_) return false;
  if (!image.inFrame_) {
    image.inFrame_ = true;
    inFrame_.push_back(&image);
  }
  return true;
}

void GlInterop::recordAcquires(VkCommandBuffer cmd) {
  if (inFrame_.empty()) return;
  std::vector<VkImageMemoryBarrier> barriers;
  for (SharedImage* image : inFrame_) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    b.dstQueueFamilyIndex = gpu_->queueFamily();
    b.image = image->texture_.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, image->texture_.levels, 0, 1};
    barriers.push_back(b);
  }
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       0, 0, nullptr, 0, nullptr, static_cast<uint32_t>(barriers.size()),
                       barriers.data());
}

void GlInterop::recordReleases(VkCommandBuffer cmd) {
  if (inFrame_.empty()) return;
  std::vector<VkImageMemoryBarrier> barriers;
  for (SharedImage* image : inFrame_) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = gpu_->queueFamily();
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    b.image = image->texture_.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, image->texture_.levels, 0, 1};
    barriers.push_back(b);
  }
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       0, 0, nullptr, 0, nullptr, static_cast<uint32_t>(barriers.size()),
                       barriers.data());
}

void GlInterop::frameWaits(std::vector<SemaphoreWait>& waits) const {
  uint64_t vk = 0, gl = 0;
  for (const SharedImage* image : inFrame_) {
    if (image->vulkanLast_) vk = std::max(vk, image->lastVulkanUse_);
    else gl = std::max(gl, image->publishedAt_);
  }
  if (vk > 0) waits.push_back({gpu_->frameTimeline(), vk, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT});
  if (gl > 0) waits.push_back({glDone_, gl, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT});
}

void GlInterop::frameSubmitted(uint64_t value) {
  for (SharedImage* image : inFrame_) {
    image->inFrame_ = false;
    image->vulkanLast_ = true;
    image->lastVulkanUse_ = value;
  }
  inFrame_.clear();
}

void GlInterop::frameAbandoned() {
  for (SharedImage* image : inFrame_) image->inFrame_ = false;
  inFrame_.clear();
}
