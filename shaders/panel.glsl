// The panel draw's block, shared by panel.vert and panel.frag.

#include "common.glsl"

struct PanelDraw {
  vec4 model0;  // rows of the model matrix: the unit quad (-0.5..0.5) to local space
  vec4 model1;
  vec4 model2;
  vec4 uv;      // the texture's columns u0, u1 and rows v0, v1 (v = 0 at the top) it shows
  vec4 arc;     // curved: angle at the left and right edge, height at the top and bottom
  vec4 hover;   // the hovered button in layout pixels: x0, y0, x1, y1
  vec4 shape;   // x radius of the cylinder, y texture pixels per layout pixel,
                // z the hover's corner radius, w the held fade (faded), or the
                // left edge of a pointed row's empty stars (with stars)
  float opacity;  // the colour (premultiplied) is scaled by it: fading out
  uint hoverFill;  // laid over the hover's inside: sRGB red, green, blue, alpha (low byte first)
  uint flags;   // below
  float lodBias;  // added to the mip level chosen for the panel's texture
  vec4 hoverClip;  // the hover is cut off outside this, as the list it is in (layout pixels)
};
DECLARE_PUSH(PanelDraw)

// PanelFlag in scene_renderer.h.
const uint kCurved = 1u;       // bent around a cylinder's axis (the model's Y axis), 65 columns
const uint kHoverPressed = 1u << 1;  // sunk: the shadow is inside its top edge
const uint kHoverRing = 1u << 2;     // the fill is only a ring inside its edge
// Bits 3..7: how far a row's stars show (of 31), as the hover eases in or
// out; 8..15: its lit top edge (alpha); 16..23: its shadow (strength);
// 24..31: how far its text and icons turn white (on red), of 255.
