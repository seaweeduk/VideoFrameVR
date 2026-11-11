#pragma once

#include <string>

struct GLFWwindow;
struct mpv_handle;
struct mpv_render_context;

// The hidden GL context and, for VA-API, its DRM render device. Outlives all
// mpv render contexts and GL objects; used only on the GL-context thread.
class MpvGlContext {
 public:
  MpvGlContext() = default;
  MpvGlContext(const MpvGlContext&) = delete;
  MpvGlContext& operator=(const MpvGlContext&) = delete;
  ~MpvGlContext();

  bool init(bool egl, bool vaapi);
  GLFWwindow* window() const { return window_; }
  // Before mpv_initialize: copy-back decoding uses the same device as GL.
  bool configure(mpv_handle* mpv) const;
  // After mpv_initialize, with this GL context current.
  bool createRenderContext(mpv_handle* mpv, mpv_render_context** render) const;

 private:
  GLFWwindow* window_ = nullptr;
  int renderFd_ = -1;
  std::string renderNode_;
};
