#version 450
#extension GL_GOOGLE_include_directive : require

#include "pointer.glsl"

layout(location = 0) in vec2 vLocal;
layout(location = 0) out vec4 fragColor;

void main() {
  vec4 c = pc.draw.color;
  if ((pc.draw.mode.x & kPointerDot) != 0u) {
    float r = length(vLocal);
    c *= 1.0 - smoothstep(1.0 - fwidth(r), 1.0, r);
  }
  if ((pc.draw.mode.x & kPointerBeam) != 0u) {
    // The first two corners are at the controller, the last two at the dot.
    float remaining = clamp(0.5 * (1.0 - vLocal.y), 0.0, 1.0);
    c *= pow(remaining, 1.5);
  }
  fragColor = c;
}
