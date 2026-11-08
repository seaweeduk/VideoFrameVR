#include "mask_probe.h"

#include <cstdio>

namespace {

constexpr double kInterval = 0.5;  // seconds between probes
constexpr int kEnough = 4;         // taps with the blue in each eye, as VideoCheck's
constexpr int kFrames = 2;         // probes with it that find the layer
// An eye's mask in the frame (side by side), as SceneRenderer reads it.
constexpr float kAlphaScale[2] = {0.2f, 0.4f};

const char* kVertex = R"(#version 330 core
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2) * 2.0 - 1.0;
  gl_Position = vec4(p, 0.0, 1.0);
}
)";

// As depth_patch.frag's mode 2, from the linear RGB picture mpv drew: 1
// where the eye's mask has VideoCheck's blue, else 0.
const char* kFragment = R"(#version 330 core
uniform sampler2D uTex;
uniform vec4 uRect;  // viewport x, y, width, height
uniform vec2 uAlphaCenter;
uniform vec2 uAlphaScale;
out vec4 fragColor;
void main() {
  vec2 m = (gl_FragCoord.xy - uRect.xy) / uRect.zw - 0.5;
  vec3 c = textureLod(uTex, fract(uAlphaCenter + m * uAlphaScale), 0.0).rgb;
  fragColor = vec4(length(m) <= 0.5 && c.b >= 0.318 && c.r <= 0.0331 && c.g <= 0.0331 ? 1.0 : 0.0);
}
)";

}  // namespace

MaskProbe::~MaskProbe() {
  if (fence_) glDeleteSync(fence_);
  if (pbo_) glDeleteBuffers(1, &pbo_);
  if (fbo_) glDeleteFramebuffers(1, &fbo_);
  if (target_) glDeleteTextures(1, &target_);
  if (vao_) glDeleteVertexArrays(1, &vao_);
  if (sampler_) glDeleteSamplers(1, &sampler_);
  if (program_) glDeleteProgram(program_);
}

bool MaskProbe::init() {
  program_ = compileProgram(kVertex, kFragment);
  if (!program_) return false;
  uTex_ = glGetUniformLocation(program_, "uTex");
  uRect_ = glGetUniformLocation(program_, "uRect");
  uAlphaCenter_ = glGetUniformLocation(program_, "uAlphaCenter");
  glUseProgram(program_);
  glUniform2f(glGetUniformLocation(program_, "uAlphaScale"), kAlphaScale[0], kAlphaScale[1]);
  glUseProgram(0);
  glGenVertexArrays(1, &vao_);
  // One texel of the full-size level: its mipmaps may be out of date.
  glGenSamplers(1, &sampler_);
  glSamplerParameteri(sampler_, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glSamplerParameteri(sampler_, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glSamplerParameteri(sampler_, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glSamplerParameteri(sampler_, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glGenTextures(1, &target_);
  glBindTexture(GL_TEXTURE_2D, target_);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, kGrid, 2 * kGrid);
  glBindTexture(GL_TEXTURE_2D, 0);
  glGenFramebuffers(1, &fbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_, 0);
  const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (!complete) return false;
  glGenBuffers(1, &pbo_);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_);
  glBufferData(GL_PIXEL_PACK_BUFFER, kGrid * 2 * kGrid, nullptr, GL_STREAM_READ);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  return true;
}

bool MaskProbe::tally(int left, int right) {
  if (stale_) {
    stale_ = false;
    return false;
  }
  if (left < kEnough || right < kEnough || ++found_ != kFrames) return false;
  std::printf("Mask layer check: found while playing\n");
  return true;
}

bool MaskProbe::count(const float* values) {
  int hits[2] = {};
  for (int eye = 0; eye < 2; ++eye)
    for (int i = eye * kGrid * kGrid; i < (eye + 1) * kGrid * kGrid; ++i) hits[eye] += values[i] > 0.5f;
  return tally(hits[0], hits[1]);
}

bool MaskProbe::count(const uint8_t* values) {
  int hits[2] = {};
  for (int eye = 0; eye < 2; ++eye)
    for (int i = eye * kGrid * kGrid; i < (eye + 1) * kGrid * kGrid; ++i) hits[eye] += values[i] > 127;
  return tally(hits[0], hits[1]);
}

bool MaskProbe::update(double now, const std::string& video, bool wanted, GLuint texture,
                       const std::function<bool()>& prepare, DepthPatchDrawer* drawer) {
  if (video != video_) {
    video_ = video;
    found_ = 0;
    next_ = 0;
    stale_ = fence_ || drawer_;
  }
  bool found = false;
  if (drawer_ && drawer_->ready()) {
    const float* values = drawer_->values();
    drawer_ = nullptr;
    if (values) found = count(values);
    else stale_ = false;
  } else if (fence_) {
    const GLenum state = glClientWaitSync(fence_, 0, 0);
    if (state == GL_ALREADY_SIGNALED || state == GL_CONDITION_SATISFIED) {
      glDeleteSync(fence_);
      fence_ = nullptr;
      glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_);
      if (const void* data =
              glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, kGrid * 2 * kGrid, GL_MAP_READ_BIT)) {
        found = count(static_cast<const uint8_t*>(data));
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
      } else {
        stale_ = false;
      }
      glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
  }
  if (found || !wanted || drawer_ || fence_ || now < next_) return found;
  next_ = now + kInterval;

  if (drawer) {
    DepthPatch patch;
    patch.width = patch.height = kGrid;
    patch.sbs = true;
    patch.alphaScale[0] = kAlphaScale[0];
    patch.alphaScale[1] = kAlphaScale[1];
    patch.maskProbe = true;
    if (drawer->draw(patch)) drawer_ = drawer;
    return false;
  }
  if (!texture || !prepare || !prepare()) return false;
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glDisable(GL_FRAMEBUFFER_SRGB);
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glUseProgram(program_);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);
  glBindSampler(0, sampler_);
  glBindVertexArray(vao_);
  glUniform1i(uTex_, 0);
  // Rows 0.. the left eye's mask, then the right eye's.
  for (int eye = 0; eye < 2; ++eye) {
    glViewport(0, eye * kGrid, kGrid, kGrid);
    glUniform4f(uRect_, 0.0f, static_cast<float>(eye * kGrid), kGrid, kGrid);
    glUniform2f(uAlphaCenter_, eye == 0 ? 0.5f : 0.0f, 0.0f);  // as SceneRenderer
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }
  glBindVertexArray(0);
  glBindSampler(0, 0);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glReadPixels(0, 0, kGrid, 2 * kGrid, GL_RED, GL_UNSIGNED_BYTE, nullptr);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  fence_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);  // flushed with the frame
  return false;
}
