#version 450
#extension GL_GOOGLE_include_directive : require

// The seek preview's frame: one eye of a flat video, or of a surround one
// the view straight ahead, as the headset would show it without stereo
// correction or passthrough.

#include "preview.glsl"
#include "projection.glsl"

layout(set = 0, binding = 0) uniform sampler2D tex;  // one level, sRGB

layout(location = 0) in vec2 vPos;
layout(location = 0) out vec4 fragColor;

void main() {
  uint flags = pc.draw.mode.y;
  vec2 uv = vPos;
  bool inside = true;
  if ((flags & kPreviewSurround) != 0u) {
    vec4 v = pc.draw.view;
    vec3 dir = normalize(vec3(mix(-v.x, v.x, vPos.x), mix(v.y, -v.y, vPos.y), -1.0));
    uv = pictureUv(dir, int(pc.draw.mode.x), v.z, inside);
  }
  if ((flags & kPreviewFlip) != 0u) uv.y = 1.0 - uv.y;
  // Half a texel inside this eye's part, so the other eye never bleeds in.
  vec4 p = pc.draw.picture;
  vec2 m = 0.5 / (vec2(textureSize(tex, 0)) * p.xy);
  vec3 c = inside ? textureLod(tex, p.zw + clamp(uv, m, 1.0 - m) * p.xy, 0.0).rgb : vec3(0.0);
  fragColor = vec4(clamp(c, 0.0, 1.0), 1.0) * pc.draw.view.w;  // premultiplied
}
