#include "mpv_gl_context.h"

#include "platform.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_EGL
#include <GLFW/glfw3native.h>
#include <EGL/eglext.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <sstream>

namespace {

void* getProcAddress(void*, const char* name) {
  return reinterpret_cast<void*>(glfwGetProcAddress(name));
}

bool hasExtension(const char* extensions, const char* name) {
  std::istringstream list(extensions ? extensions : "");
  for (std::string extension; list >> extension;)
    if (extension == name) return true;
  return false;
}

}  // namespace

MpvGlContext::~MpvGlContext() {
  if (renderFd_ >= 0) close(renderFd_);
  if (window_) glfwDestroyWindow(window_);
}

bool MpvGlContext::init(bool egl, bool vaapi) {
  glfwDefaultWindowHints();
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_SRGB_CAPABLE, GLFW_TRUE);
  if (egl) glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
  // Only carries GL; the desktop mirror has a separate Vulkan window.
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  window_ = glfwCreateWindow(1280, 720, kAppTitle, nullptr, nullptr);
  if (!window_) return false;
  glfwMakeContextCurrent(window_);
  glfwSwapInterval(0);
  if (!vaapi) return true;

  const auto queryDisplay = reinterpret_cast<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(
      eglGetProcAddress("eglQueryDisplayAttribEXT"));
  const auto queryDevice = reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(
      eglGetProcAddress("eglQueryDeviceStringEXT"));
  EGLAttrib device = 0;
  if (!queryDisplay || !queryDevice ||
      !queryDisplay(glfwGetEGLDisplay(), EGL_DEVICE_EXT, &device)) {
    std::fprintf(stderr, "VA-API: cannot identify the EGL device; try --hwdec=no\n");
    return false;
  }
  const auto eglDevice = reinterpret_cast<EGLDeviceEXT>(device);
  if (!hasExtension(queryDevice(eglDevice, EGL_EXTENSIONS), "EGL_EXT_device_drm_render_node")) {
    std::fprintf(stderr, "VA-API: EGL cannot identify its DRM render node; try --hwdec=no\n");
    return false;
  }
  const char* node = queryDevice(eglDevice, EGL_DRM_RENDER_NODE_FILE_EXT);
  if (!node) {
    std::fprintf(stderr, "VA-API: EGL has no DRM render node; try --hwdec=no\n");
    return false;
  }
  renderNode_ = node;
  renderFd_ = open(node, O_RDWR | O_CLOEXEC);
  if (renderFd_ < 0) {
    std::perror("VA-API: opening the DRM render node");
    return false;
  }
  std::printf("VA-API device: %s\n", node);
  return true;
}

bool MpvGlContext::configure(mpv_handle* mpv) const {
  if (renderFd_ < 0) return true;
  const int result = mpv_set_option_string(mpv, "vaapi-device", renderNode_.c_str());
  if (result < 0) {
    std::fprintf(stderr, "mpv: setting the VA-API device failed: %s\n", mpv_error_string(result));
    return false;
  }
  return true;
}

bool MpvGlContext::createRenderContext(mpv_handle* mpv, mpv_render_context** render) const {
  mpv_opengl_init_params glInit{getProcAddress, nullptr};
  mpv_opengl_drm_params_v2 drm{};
  drm.fd = -1;
  drm.render_fd = renderFd_;
  mpv_render_param params[] = {
      {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
      {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
      {renderFd_ >= 0 ? MPV_RENDER_PARAM_DRM_DISPLAY_V2 : MPV_RENDER_PARAM_INVALID, &drm},
      {MPV_RENDER_PARAM_INVALID, nullptr},
  };
  return mpv_render_context_create(render, mpv, params) >= 0;
}
