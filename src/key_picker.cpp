#include "key_picker.h"

#include "scene_renderer.h"  // stereoRect, surroundProjection
#include "vr_space.h"         // kPi

#include <algorithm>
#include <cmath>

namespace {

const char* kVertex = R"(#version 330 core
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2) * 2.0 - 1.0;
  gl_Position = vec4(p, 0.0, 1.0);
}
)";

// From the sRGB picture mpv drew (row 0 its top): the mean of the 5 x 5
// texels around uUv within uPart, in display values as the frame came
// (before TV gamma, which uTv undoes as surround.frag's videoTap()), and in
// alpha the largest difference between them in any channel, as the scene's
// colour key compares them (linearToSrgb() of the picture).
const char* kFragment = R"(#version 330 core
uniform sampler2D uTex;
uniform vec2 uUv;
uniform vec4 uPart;  // offset, size
uniform bool uTv;
out vec4 fragColor;
vec3 display(vec3 c) {
  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}
void main() {
  vec2 size = vec2(textureSize(uTex, 0));
  ivec2 lo = ivec2(uPart.xy * size), hi = ivec2((uPart.xy + uPart.zw) * size) - 1;
  ivec2 centre = ivec2(floor(uUv * size));
  vec3 sum = vec3(0.0), least = vec3(1.0), most = vec3(0.0);
  for (int j = -2; j <= 2; ++j)
    for (int i = -2; i <= 2; ++i) {
      vec3 c = clamp(texelFetch(uTex, clamp(centre + ivec2(i, j), lo, hi), 0).rgb, 0.0, 1.0);
      vec3 t = display(c);
      sum += uTv ? pow(c, vec3(1.0 / 2.4)) : t;
      least = min(least, t);
      most = max(most, t);
    }
  vec3 spread = most - least;
  fragColor = vec4(sum / 25.0, max(max(spread.r, spread.g), spread.b));
}
)";

// The largest difference in a picked patch (display values, any channel)
// that still makes one colour: a flat fill's texels are the same, a real
// background's differ by a few levels of noise and compression; across an
// edge of the subject they differ far more.
constexpr float kMaxSpread = 0.12f;

// projection.glsl's eacUv(): where a direction falls in the equi-angular cubemap.
void eacUv(Vec3 dir, float uv[2]) {
  const float x = dir.x, y = -dir.y, z = -dir.z;  // v360 axes: x right, y down, z forward
  const float phi = std::atan2(x, z);
  const float theta = std::asin(std::clamp(y, -1.0f, 1.0f));
  const float q = kPi / 4;
  int face;  // grid position: 0 left, 1 front, 2 right, 3 down, 4 back, 5 up
  float phiNorm;
  if (phi >= -q && phi < q) {
    face = 1;
    phiNorm = phi;
  } else if (phi >= -3 * q && phi < -q) {
    face = 0;
    phiNorm = phi + kPi / 2;
  } else if (phi >= q && phi < 3 * q) {
    face = 2;
    phiNorm = phi - kPi / 2;
  } else {
    face = 4;
    phiNorm = phi + (phi > 0 ? -kPi : kPi);
  }
  const float threshold = std::atan(std::cos(phiNorm));
  if (theta > threshold) face = 3;
  else if (theta < -threshold) face = 5;
  float f[2];
  switch (face) {
    case 0: f[0] = -z / x, f[1] = -y / x; break;
    case 1: f[0] = x / z, f[1] = y / z; break;
    case 2: f[0] = -z / x, f[1] = y / x; break;
    case 3: f[0] = x / y, f[1] = -z / y; break;
    case 4: f[0] = x / z, f[1] = -y / z; break;
    default: f[0] = -x / y, f[1] = -z / y; break;
  }
  if (face == 4) {
    const float t = f[0];
    f[0] = -f[1];
    f[1] = t;
  } else if (face == 3 || face == 5) {
    const float t = f[0];
    f[0] = f[1];
    f[1] = -t;
  }
  for (float& v : f) v = 2 / kPi * std::atan(v) + 0.5f;
  uv[0] = (face % 3 + f[0]) / 3;
  uv[1] = (face / 3 + f[1]) / 2;
}

}  // namespace

KeyPicker::~KeyPicker() {
  if (fence_) glDeleteSync(fence_);
  if (pbo_) glDeleteBuffers(1, &pbo_);
  if (fbo_) glDeleteFramebuffers(1, &fbo_);
  if (target_) glDeleteTextures(1, &target_);
  if (vao_) glDeleteVertexArrays(1, &vao_);
  if (program_) glDeleteProgram(program_);
}

bool KeyPicker::init() {
  program_ = compileProgram(kVertex, kFragment);
  if (!program_) return false;
  uTex_ = glGetUniformLocation(program_, "uTex");
  uUv_ = glGetUniformLocation(program_, "uUv");
  uPart_ = glGetUniformLocation(program_, "uPart");
  uTv_ = glGetUniformLocation(program_, "uTv");
  glGenVertexArrays(1, &vao_);
  glGenTextures(1, &target_);
  glBindTexture(GL_TEXTURE_2D, target_);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, 1, 1);
  glBindTexture(GL_TEXTURE_2D, 0);
  glGenFramebuffers(1, &fbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_, 0);
  const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (!complete) return false;
  glGenBuffers(1, &pbo_);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_);
  glBufferData(GL_PIXEL_PACK_BUFFER, 4 * sizeof(float), nullptr, GL_STREAM_READ);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  glReady_ = true;
  return true;
}

bool KeyPicker::pointOf(const VideoFormat& format, float frameAspect, bool flip, Vec3 direction,
                        float uv[2], float part[4]) {
  if (format.projection == Projection::Flat) return false;
  float stereo[4];
  stereoRect(format.stereo, 0, stereo);
  float fisheyeFov = 0;
  const int projection =
      surroundProjection(format, frameAspect * stereo[0] / stereo[1], fisheyeFov);
  // As surroundUv() in surround.frag (projection.glsl's pictureUv()).
  const Vec3 d = normalize(direction);
  float u = 0, v = 0;
  if (projection >= 3) {
    float s[2];
    eacUv(d, s);
    u = projection == 4 ? s[1] : s[0];
    v = projection == 4 ? 1 - s[0] : s[1];
  } else if (projection == 2) {
    const float r = std::acos(std::clamp(-d.z, -1.0f, 1.0f)) / fisheyeFov;
    if (r > 0.5f) return false;
    const float len = std::sqrt(d.x * d.x + d.y * d.y);
    u = 0.5f + (len > 1e-6f ? r * d.x / len : 0);
    v = 0.5f - (len > 1e-6f ? r * d.y / len : 0);
  } else {
    const float span = projection == 0 ? kPi : 2 * kPi;
    const float lon = std::atan2(d.x, -d.z);
    if (std::abs(lon) > 0.5f * span) return false;
    u = 0.5f + lon / span;
    v = 0.5f - std::asin(std::clamp(d.y, -1.0f, 1.0f)) / kPi;
  }
  if (flip) v = 1 - v;
  const float at[2] = {std::clamp(u, 0.0f, 1.0f), std::clamp(v, 0.0f, 1.0f)};
  part[0] = stereo[2];
  part[1] = stereo[3];
  part[2] = stereo[0];
  part[3] = stereo[1];
  for (int i = 0; i < 2; ++i) uv[i] = part[i] + at[i] * part[2 + i];
  if (projection >= 3) {
    // Only its cube face, as surround.frag keeps its taps.
    const float grid[2] = {projection == 3 ? 3.0f : 2.0f, projection == 3 ? 2.0f : 3.0f};
    for (int i = 0; i < 2; ++i) {
      const float cell = std::min(std::floor(at[i] * grid[i]), grid[i] - 1);
      part[i] += cell / grid[i] * part[2 + i];
      part[2 + i] /= grid[i];
    }
  }
  return true;
}

KeyPicker::Read KeyPicker::read(const float uv[2], const float part[4], GLuint texture,
                                bool tvGamma, const std::function<bool()>& prepare,
                                DepthPatchDrawer* drawer) {
  if (busy()) return Read::Later;
  if (drawer) {
    DepthPatch patch;
    patch.width = 4;
    patch.height = 1;
    patch.keyPick = true;
    for (int i = 0; i < 2; ++i) patch.pickUv[i] = uv[i];
    for (int i = 0; i < 4; ++i) patch.pickPart[i] = part[i];
    if (!drawer->draw(patch)) return Read::Later;
    drawer_ = drawer;
    return Read::Started;
  }
  if (!texture || !prepare) return Read::Later;
  if (!glReady_) return Read::Unavailable;
  if (!prepare()) return Read::Later;
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glDisable(GL_FRAMEBUFFER_SRGB);
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glViewport(0, 0, 1, 1);
  glUseProgram(program_);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);
  glBindSampler(0, 0);
  glBindVertexArray(vao_);
  glUniform1i(uTex_, 0);
  glUniform2f(uUv_, uv[0], uv[1]);
  glUniform4f(uPart_, part[0], part[1], part[2], part[3]);
  glUniform1i(uTv_, tvGamma);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindVertexArray(0);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, nullptr);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  fence_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);  // flushed with the frame
  return Read::Started;
}

KeyPicker::Result KeyPicker::result(KeyColour& colour) {
  float read[4];  // red, green, blue, spread
  if (drawer_) {
    if (!drawer_->ready()) return Result::None;
    const float* values = drawer_->values();
    drawer_ = nullptr;
    if (!values) return Result::Failed;
    for (int c = 0; c < 4; ++c) read[c] = values[c];
  } else if (fence_) {
    const GLenum state = glClientWaitSync(fence_, 0, 0);
    if (state != GL_ALREADY_SIGNALED && state != GL_CONDITION_SATISFIED) return Result::None;
    glDeleteSync(fence_);
    fence_ = nullptr;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_);
    const void* data = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, 4 * sizeof(float), GL_MAP_READ_BIT);
    if (data) {
      for (int c = 0; c < 4; ++c) read[c] = static_cast<const float*>(data)[c];
      glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (!data) return Result::Failed;
  } else {
    return Result::None;
  }
  for (float v : read)
    if (!std::isfinite(v)) return Result::Failed;
  if (read[3] > kMaxSpread) return Result::Uneven;
  auto byte = [](float v) {
    return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255));
  };
  colour = {byte(read[0]), byte(read[1]), byte(read[2])};
  return Result::Picked;
}

void KeyPicker::cancel() {
  if (fence_) glDeleteSync(fence_);
  fence_ = nullptr;
  drawer_ = nullptr;
}
