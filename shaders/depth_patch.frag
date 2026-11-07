#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

// DepthEstimator's patch (depth_estimator.cpp, kFragment) from a decoded
// frame mpv handed out, which stays out of GL (DirectDepth): the same
// lookups and results, from its two planes, (TP10) a Q10C frame's TP10
// descriptors, or (YCBCR) a Q08C frame's one image (video.glsl).

#include "video.glsl"

uint drawFlags() { return 0u; }

#ifdef TP10
#define lumaAt(p) tp10Luma(chromaTex, p)
#define pictureSize() videoSize(tp10Bands[0], chromaTex)
#else
layout(set = 0, binding = 0) uniform sampler2D lumaTex;
#define pictureSize() vec2(textureSize(lumaTex, 0))
#endif
#ifdef YCBCR
// Both planes through the Y'CbCr sampler (video.glsl's videoTap): g = Y, b = Cb, r = Cr.
#define lumaAt(p) textureLod(lumaTex, p, 0.0).g
#define chromaAt(p) textureLod(lumaTex, p, 0.0).br
#else
layout(set = 0, binding = 1) uniform sampler2D chromaTex;
#ifndef TP10
#define lumaAt(p) textureLod(lumaTex, p, 0.0).r
#endif
#define chromaAt(p) textureLod(chromaTex, p + u.yuvMatrix.zw, 0.0).rg
#endif

// DirectDepth::Push.
layout(push_constant) uniform Push {
  vec4 rect;  // viewport x, y, width, height
  vec4 yuvRange;
  vec4 yuvMatrix;
  vec2 start;  // alpha, beta at the first pixel's corner
  vec2 eyeScale;
  vec2 eyeOffset;
  vec2 alphaCenter;
  vec2 alphaScale;
  float step;
  float fisheyeFov;
  int mode;        // 0: an eye's patch, 1: the whole eye scaled down
  int projection;  // 0 = 180, 2 = fisheye
  int flip;
  int alphaPacked;
  float greenKey;
} u;

layout(location = 0) out vec4 fragColor;

const int kMaxTaps = 8;

vec3 yuvTexel(vec2 p) {
  float y = lumaAt(p) * u.yuvRange.x + u.yuvRange.y;
  vec2 c = chromaAt(p) * u.yuvRange.z + u.yuvRange.w;
  float kr = u.yuvMatrix.x, kb = u.yuvMatrix.y, kg = 1.0 - kr - kb;
  vec3 e = clamp(vec3(y + 2.0 * (1.0 - kr) * c.y,
                      y - (2.0 * kb * (1.0 - kb) * c.x + 2.0 * kr * (1.0 - kr) * c.y) / kg,
                      y + 2.0 * (1.0 - kb) * c.x), 0.0, 1.0);
  return mix(e / 12.92, pow((e + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), e));
}

vec3 picture(vec2 p, vec2 dx, vec2 dy) {
  vec2 size = pictureSize();
  int nx = clamp(int(ceil(length(dx * size))), 1, kMaxTaps);
  int ny = clamp(int(ceil(length(dy * size))), 1, kMaxTaps);
  vec3 sum = vec3(0.0);
  for (int j = 0; j < ny; ++j)
    for (int i = 0; i < nx; ++i)
      sum += yuvTexel(p + ((float(i) + 0.5) / float(nx) - 0.5) * dx +
                      ((float(j) + 0.5) / float(ny) - 0.5) * dy);
  return sum / float(nx * ny);
}

void main() {
  vec2 p = gl_FragCoord.xy - u.rect.xy;
  vec2 uv;
  bool inside = true;
  if (u.mode == 1) {
    uv = vec2(p.x / u.rect.z, 1.0 - p.y / u.rect.w);
  } else {
    float alpha = u.start.x + p.x * u.step, beta = u.start.y + p.y * u.step;
    vec3 dir = vec3(cos(alpha), sin(alpha) * sin(beta), -sin(alpha) * cos(beta));
    if (u.projection == 2) {
      float theta = acos(clamp(-dir.z, -1.0, 1.0));
      float r = theta / u.fisheyeFov;
      vec2 d = dir.xy;
      float len = length(d);
      d = len > 1e-6 ? d / len : vec2(0.0);
      uv = vec2(0.5 + r * d.x, 0.5 - r * d.y);
      inside = r <= 0.49;
    } else {
      float lon = atan(dir.x, -dir.z);
      float lat = asin(clamp(dir.y, -1.0, 1.0));
      uv = vec2(0.5 + lon / PI, 0.5 - lat / PI);
      inside = abs(lon) <= 0.49 * PI;
    }
  }
  if (u.flip != 0) uv.y = 1.0 - uv.y;
  vec2 dx = dFdx(uv), dy = dFdy(uv);
  vec3 c = picture(u.eyeOffset + clamp(uv, 0.0005, 0.9995) * u.eyeScale, dx * u.eyeScale,
                   dy * u.eyeScale);
  float l = pow(max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 0.0), 1.0 / 2.2);
  bool clear = false;
  if (u.mode == 0 && u.alphaPacked != 0) {
    vec2 m = uv - 0.5;
    // Red below 0.5 in display values.
    clear = picture(fract(u.alphaCenter + m * u.alphaScale), dx * u.alphaScale, dy * u.alphaScale).r <
            0.214;
  } else if (u.mode == 0 && u.greenKey > 0.0) {
    vec3 e = pow(max(c, 0.0), vec3(1.0 / 2.2));
    clear = (e.g - max(e.r, e.b)) / max(e.g, 0.04) > u.greenKey;
  }
  fragColor = vec4(!inside ? -1.0 : clear ? -0.5 : l);
}
