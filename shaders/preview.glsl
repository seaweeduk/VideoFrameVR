// The seek preview's draw block, shared by preview.vert and preview.frag.

#include "common.glsl"

struct PreviewDraw {
  vec4 model0;   // rows of the model matrix: the unit quad (-0.5..0.5) to local space
  vec4 model1;
  vec4 model2;
  vec4 picture;  // the eye's part of the frame it shows: scale xy, offset xy
  vec4 view;     // curved: x the radius, y the half angle (radians), around
                 // the model's Y axis; z the fisheye's field of view (radians);
                 // w its opacity (fading out toward 0)
  uvec4 mode;    // x projection (projection.glsl, surround), y flags (below)
};
DECLARE_PUSH(PreviewDraw)

// PreviewFlag in scene_renderer.h.
const uint kPreviewFlip = 1u;      // the picture is shown upside down
const uint kPreviewSurround = 2u;  // the middle 16:9 of the front 180°, as a 180° video stores it
