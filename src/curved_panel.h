#pragma once

#include "xmath.h"

// Part of an upright cylinder around the viewer, facing its axis: a menu
// panel. A texture is drawn on it with u across (left to right) and v down.
struct CurvedPanel {
  Vec3 axis;         // the cylinder's axis at eye level
  Mat4 orientation;  // its Y axis is the cylinder's, -Z straight ahead
  float radius = 1.0f;
  float left = -0.5f, right = 0.5f;  // arc length (m) at u = 0 and 1, + = to the right
  float top = 0.5f, bottom = -0.5f;  // height (m) of the texture's v = 0 and 1
  float u0 = 0.0f, u1 = 1.0f;        // the columns of the texture that are shown
  float v0 = 0.0f, v1 = 1.0f;        // ... and its rows
  float pixelScale = 1.0f;  // texture pixels per pixel of what is drawn on it (hover rectangles)

  Mat4 model() const { return translation(axis) * orientation; }
  // Intersects a ray with the shown part; u/v in 0..1 of the whole texture.
  bool hit(Vec3 origin, Vec3 dir, float& u, float& v, float& distance) const;
};
