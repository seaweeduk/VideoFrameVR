#version 450
#extension GL_GOOGLE_include_directive : require

#include "pointer.glsl"

layout(location = 0) in vec2 vLocal;
layout(location = 0) out vec4 fragColor;

void main() {
  vec4 c = pc.draw.color;
  if ((pc.draw.mode.x & kPointerDot) != 0u) {
    // A dark rim, the outer 35% of the radius, keeps the dot clear on light panels.
    const float kRimFrom = 0.65;
    const vec3 kRimColor = vec3(0.025, 0.030, 0.037);  // charcoal, linear
    const float kRimAlpha = 0.93;
    float r = length(vLocal);
    float w = fwidth(r);
    vec4 rim = vec4(kRimColor, 1.0) * (kRimAlpha * c.a);
    c = mix(c, rim, smoothstep(kRimFrom - w, kRimFrom, r));
    c *= 1.0 - smoothstep(1.0 - w, 1.0, r);
  }
  if ((pc.draw.mode.x & kPointerBeam) != 0u) {
    // The first two corners are at the controller, the last two at the dot.
    float remaining = clamp(0.5 * (1.0 - vLocal.y), 0.0, 1.0);
    c *= pow(remaining, 1.5);
  }
  fragColor = c;
}
