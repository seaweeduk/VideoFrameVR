// The screen draw's block, shared by screen.vert and screen.frag.

#include "video.glsl"

struct ScreenDraw {
  vec4 pose0;  // rows of the screen's pose in local space (recentered head position and heading)
  vec4 pose1;
  vec4 pose2;
  vec4 size;   // width, height, distance, radius of the bend (0 = flat; ScreenSurface)
  vec4 color;  // rgb linear, shown without a picture; a opacity
  vec4 adjust;  // brightness, contrast, saturation (-1..1), w sharpening strength (0 = off)
  uvec4 mode;  // y flags (video.glsl), z footprint taps per side at most (footprintSample)
  vec4 yuv;    // kYuv: the planes' range and matrix (videoTap)
  vec4 yuvMatrix;
};
DECLARE_PUSH(ScreenDraw)
uint drawFlags() { return pc.draw.mode.y; }
