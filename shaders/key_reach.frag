#version 450
#extension GL_GOOGLE_include_directive : require

// The colour key's second pass (KeyMask), from the first one's distances:
// x the square root of the least distance among eight points the shrink's
// reach away (1 without shrink), so the cut moves that far into the
// subject; y how much of the background shows among four diagonal points
// spill removal's reach beyond that (0 without spill removal). The points
// stay on this eye's part, or a cubemap's face, and wrap around 360 video.

#include "video.glsl"
#include "key_mask.glsl"

layout(set = 0, binding = 0) uniform sampler2D distances;

layout(location = 0) out vec4 fragColor;

vec2 offset, scale, at, lo, hi;

// The distance o texels of the frame away.
float distanceAt(vec2 o) {
  vec2 p = at + o / (u.frame.xy * scale);
  if (u.mode.w == 1u) p.x = fract(p.x);
  p = offset + clamp(p, lo, hi) * scale;
  float d = textureLod(distances, p * u.frame.xy / (u.frame.z * kKeyMaskSize), 0.0).r;
  return d * d;
}

void main() {
  vec2 p = min(gl_FragCoord.xy * u.frame.z / u.frame.xy, 1.0);
  // This eye's part of the frame (EyeData::stereo), and where p is in it.
  scale = vec2(1.0);
  offset = vec2(0.0);
  if (u.mode.z == 1u) {
    scale.x = 0.5;
    offset.x = p.x >= 0.5 ? 0.5 : 0.0;
  } else if (u.mode.z == 2u) {
    scale.y = 0.5;
    offset.y = p.y >= 0.5 ? 0.5 : 0.0;
  }
  at = (p - offset) / scale;
  // Half a mask texel within the part, or the cubemap's face.
  vec2 margin = 0.5 * u.frame.z / (u.frame.xy * scale);
  lo = margin;
  hi = 1.0 - margin;
  if (u.mode.w >= 3u) {
    vec2 grid = u.mode.w == 3u ? vec2(3.0, 2.0) : vec2(2.0, 3.0);
    vec2 cell = min(floor(at * grid), grid - 1.0);
    lo = cell / grid + margin;
    hi = (cell + 1.0) / grid - margin;
  }

  float nearest = 1.0;
  if (u.key.z > 0.0) {
    // Eight directions, so the cut stays round.
    vec2 d = vec2(1.0, 0.0);
    for (int i = 0; i < 8; ++i) {
      nearest = min(nearest, distanceAt(u.key.z * d));
      d = vec2(d.x - d.y, d.x + d.y) * 0.70710678;
    }
  }
  float edge = 0.0;
  if (u.key.w > 0.0) {
    vec2 d = vec2(0.70710678);
    float kept = 0.0;
    for (int i = 0; i < 4; ++i) {
      kept += colourAlpha(distanceAt((u.key.z + u.key.w) * d), u.key);
      d = vec2(-d.y, d.x);
    }
    edge = 1.0 - 0.25 * kept;
  }
  fragColor = vec4(sqrt(nearest), edge, 0.0, 0.0);
}
