// The seek preview's draw block, shared by preview.vert and preview.frag.

#include "common.glsl"

struct PreviewDraw {
  vec4 model0;   // rows of the model matrix: the unit quad (-0.5..0.5) to local space
  vec4 model1;
  vec4 model2;
  vec4 picture;  // the eye's part of the frame it shows: scale xy, offset xy
  vec4 view;     // surround: x, y the tangents of the view's half width and height,
                 // z the fisheye's field of view (radians); w its opacity (fading out toward 0)
  uvec4 mode;    // x projection (projection.glsl), y flags (below)
};
DECLARE_PUSH(PreviewDraw)

// PreviewFlag in scene_renderer.h.
const uint kPreviewFlip = 1u;      // the picture is shown upside down
const uint kPreviewSurround = 2u;  // the view straight ahead into a surround picture
