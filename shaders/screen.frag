#version 450
#extension GL_GOOGLE_include_directive : require
#ifdef TP10
#extension GL_EXT_nonuniform_qualifier : require
#endif

#include "screen.glsl"

#ifdef TP10
#define tex tp10Bands[0]  // a Q10C frame's luma (video.glsl), always kYuv
#else
layout(set = 0, binding = 0) uniform sampler2D tex;
#endif
layout(set = 0, binding = 1) uniform sampler2D chromaTex;  // kYuv
// What tex holds (video.glsl's kSource...); -1: an image or planes, from the
// draw. A Y'CbCr image (2) is sampled from its only level.
layout(constant_id = 1) const int kSource = -1;
int source() {
  if (kSource >= 0) return kSource;
  return (pc.draw.mode.y & kYuv) != 0u ? kSourcePlanes : kSourceImage;
}

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

// The picture at uv (0..1 in this eye's part), kept m inside it, sampled
// with the gradients dx, dy: through the mipmaps, or without them over the
// pixel's footprint.
vec3 picture(vec2 uv, vec2 m, vec2 dx, vec2 dy, EyeData e, uint flags) {
  if (kSource == kSourceYcbcr || baseLevel(flags))
    return footprintSample(tex, chromaTex, source(), pc.draw.yuv, pc.draw.yuvMatrix, uv, dx, dy,
                           e.stereo, m, 1.0 - m, int(pc.draw.mode.z));
  vec2 eyeUv = e.stereo.zw + clamp(uv, m, 1.0 - m) * e.stereo.xy;
  return textureGrad(tex, eyeUv, dx * e.stereo.xy, dy * e.stereo.xy).rgb;
}

void main() {
  EyeData e = currentEye();
  uint flags = pc.draw.mode.y;
  vec3 c = pc.draw.color.rgb;
  if ((flags & kTexture) != 0u) {
    // Half a texel inside this eye's part, so the other eye never bleeds in.
    vec2 m = 0.5 / (videoSize(tex, chromaTex) * e.stereo.xy);
    vec2 dx = dFdx(vUV), dy = dFdy(vUV);
    c = applyLook(adjustPicture(picture(vUV, m, dx, dy, e, flags), pc.draw.adjust.xyz), e, flags,
                  gl_FragCoord.xy);
#ifdef SHARPEN
    // Only in the pipeline for it, as in surround.frag.
    float strength = pc.draw.adjust.w;
#else
    const float strength = 0.0;
#endif
    if (strength > 0.0) {
      // The picture one eye pixel to each side, as those pixels sample it
      // (it stops at the picture's edges).
      vec2 steps[4] = vec2[](-dFdxFine(vUV), dFdxFine(vUV), -dFdyFine(vUV), dFdyFine(vUV));
      vec2 pixels[4] = vec2[](vec2(-1.0, 0.0), vec2(1.0, 0.0), vec2(0.0, -1.0), vec2(0.0, 1.0));
      vec3 n[4];
      for (int i = 0; i < 4; ++i) {
        n[i] = applyLook(adjustPicture(picture(vUV + steps[i], m, dx, dy, e, flags),
                                       pc.draw.adjust.xyz),
                         e, flags, gl_FragCoord.xy + pixels[i]);
        n[i] = clamp(n[i], 0.0, 1.0);
      }
      c = sharpen(clamp(c, 0.0, 1.0), n[0], n[1], n[2], n[3], strength);
    }
  }
  fragColor = vec4(clamp(c, 0.0, 1.0), 1.0) * pc.draw.color.a;  // premultiplied by the opacity
}
