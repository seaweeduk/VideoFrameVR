// The colour key's mask passes (key_distance.frag, key_reach.frag; KeyMask):
// their push constants, KeyMaskPush in key_mask.h.

layout(push_constant) uniform Push {
  vec4 frame;   // xy the frame's size in texels, z its texels per mask texel, w the key's colour (keyColour())
  vec4 key;     // x, y: colourAlpha()'s cut and soft edge; z the shrink's reach, w spill
                // removal's past it, in the frame's texels (0: off)
  vec4 yuv;     // a decoded frame's planes: their range, as the frame came (videoTap)
  vec4 matrix;  // ... Kr, Kb, the chroma lookup's offset
  uvec4 mode;   // x the source (video.glsl's kSource...), y flags (kTvGamma), z the stereo
                // layout (0 none, 1 side by side, 2 top and bottom), w surround.frag's projection
} u;

uint drawFlags() { return u.mode.y; }
