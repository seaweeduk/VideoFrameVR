// The pointer draw's block, shared by pointer.vert and pointer.frag.

#include "common.glsl"

const uint kPointerDot = 1u;
const uint kPointerBeam = 2u;

struct PointerDraw {
  vec4 corners[4];  // xyz, local space
  vec4 color;       // premultiplied, linear
  uvec4 mode;       // x: pointer flags; no flags fills the whole quad
};
DECLARE_PUSH(PointerDraw)
