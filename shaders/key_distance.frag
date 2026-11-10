#version 450
#extension GL_GOOGLE_include_directive : require
#ifdef TP10
#extension GL_EXT_nonuniform_qualifier : require
#endif

// The colour key's first pass over a new frame (KeyMask): how far the frame
// is from the key's colour, as the eye passes compare it, sampled at the
// middle of each mask texel. Stored as the square root of that distance
// (fine near 0, where black keys cut), up to 1.

#include "video.glsl"
#include "key_mask.glsl"

// What tex holds: video.glsl's kSource..., from the push constants. A Q10C
// frame's build (TP10) reads its bands.
#ifdef TP10
#define tex tp10Bands[0]
#else
layout(set = 0, binding = 0) uniform sampler2D tex;
#endif
layout(set = 0, binding = 1) uniform sampler2D chromaTex;

layout(location = 0) out vec4 fragColor;

void main() {
#ifdef TP10
  const int source = kSourcePlanes;
#else
  int source = int(u.mode.x);
#endif
  vec2 p = min(gl_FragCoord.xy * u.frame.z / u.frame.xy, 1.0);
  vec3 e;
  if (source == kSourceImage) {
    e = linearToSrgb(clamp(textureLod(tex, p, 0.0).rgb, 0.0, 1.0));
  } else {
    // A decoded frame's display values straight from its planes, unless TV
    // gamma converts them as mpv would.
    e = videoDisplay(tex, chromaTex, p, source, u.yuv, u.matrix);
    if ((u.mode.y & kTvGamma) != 0u) e = linearToSrgb(pow(e, vec3(2.4)));
  }
  fragColor = vec4(sqrt(min(sqrt(keyDistance2(e, keyColour(u.frame.w))), 1.0)));
}
