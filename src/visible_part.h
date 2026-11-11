#pragma once

#include "xmath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// What one eye's pixels can look up in its part of a surround video frame,
// for the colour key's mask (KeyMask): made only there.

// The eye and the draw as surround.frag gets them (EyeData, SurroundPush).
struct VisibleView {
  float tangents[4];      // left, right, down, up
  float orientation[4];   // quaternion x, y, z, w: from the eye's axes to local space
  float toPicture[3][3];  // columns: local space into the picture's axes
  float zoom = 1.0f;
  bool correct = false;   // stereo correction: viewer (eye from head centre), camera, distance
  float viewer[3] = {};
  float camera[3] = {};
  float distance = 0.0f;
  int projection = 0;     // surroundProjection(): 0 180, 1 360, 2 fisheye, 3 and 4 cubemaps
  float fisheyeFov = 0.0f;
  bool flip = false;
};

// Bounds in the eye's picture coordinates (uv 0..1, as surround.frag's uv
// after the flip and the clamp).
struct UvBounds {
  float lo[2] = {0.0f, 0.0f};
  float hi[2] = {1.0f, 1.0f};
};

// Whether the eye sees the picture's direction d (unit, its axes) or nearly:
// the view ray that lands on d, back through the stereo correction and both
// turns, within the tangents widened by 15 % of their range and 0.1.
// viewer: the correction's viewer minus its camera, inside the sphere.
inline bool inOrNearView(const VisibleView& v, bool correct, Vec3 viewer, Vec3 d) {
  // The correction looks from the viewer at the sphere's point distance * d.
  const Vec3 ray = correct ? d * v.distance - viewer : d;
  // toPicture's transpose, then the eye's turn undone (rotateByQuat with
  // the conjugate).
  const auto column = [&](int c) {
    return Vec3{v.toPicture[c][0], v.toPicture[c][1], v.toPicture[c][2]};
  };
  const Vec3 local{dot(column(0), ray), dot(column(1), ray), dot(column(2), ray)};
  const Vec3 q{-v.orientation[0], -v.orientation[1], -v.orientation[2]};
  const Vec3 t = cross(q, local) * 2.0f;
  const Vec3 eye = local + t * v.orientation[3] + cross(q, t);
  if (!(eye.z < 0.0f)) return false;
  const float at[2] = {eye.x / -eye.z * v.zoom, eye.y / -eye.z * v.zoom};
  for (int k = 0; k < 2; ++k) {
    const float lo = std::min(v.tangents[2 * k], v.tangents[2 * k + 1]);
    const float hi = std::max(v.tangents[2 * k], v.tangents[2 * k + 1]);
    const float margin = 0.15f * (hi - lo) + 0.1f;
    if (!(at[k] >= lo - margin && at[k] <= hi + margin)) return false;
  }
  return true;
}

// The bounds of every uv the eye's pixels look up, from its field's border
// (16 points per edge): the mapping is continuous and locally one-to-one
// away from a pole (180), the point straight behind the lens (fisheye) and
// a seam, so the field's picture lies within its border's. Between two
// points on an edge a coordinate leaves their range by at most an eighth of
// its second difference there: each axis is padded by the largest second
// difference of three points on one edge (a seam's jump makes it the whole
// width). False: the whole
// part (360, cubemaps, the point behind the lens in or near the view, a
// stereo correction whose sphere doesn't hold the viewer, non-finite
// values). Near a pole, or around it, x spans the whole part and y reaches
// the pole's edge. A pole or the point behind the lens counts as in or near
// the view when the border comes close to it in the picture, goes around
// it, or the view ray that lands on it (inOrNearView) is in or near the
// field: the last also finds both poles in view, whose turns cancel. With
// those found, the seam (180) cannot be inside without crossing the border.
inline bool visiblePart(const VisibleView& v, UvBounds& out) {
  out = UvBounds{};
  if (v.projection != 0 && v.projection != 2) return false;
  constexpr double kPi = 3.14159265358979323846;
  constexpr int kSteps = 16, kCount = 4 * kSteps;
  const double zoom = v.zoom;
  Vec3 viewer{v.viewer[0] - v.camera[0], v.viewer[1] - v.camera[1], v.viewer[2] - v.camera[2]};
  const bool correct = v.correct && v.distance > 0.0f;
  // Inside its sphere the correction moves directions smoothly, one to one.
  if (correct && !(length(viewer) < 0.9f * v.distance)) return false;
  double uv[kCount][2], azimuth[kCount];
  double maxBack = 0.0;
  for (int i = 0; i < kCount; ++i) {
    // Around the border, corners included: top, right, bottom, left.
    const double s = static_cast<double>(i % kSteps) / kSteps;
    const int edge = i / kSteps;
    const double tx = edge == 0 ? s : edge == 1 ? 1.0 : edge == 2 ? 1.0 - s : 0.0;
    const double ty = edge == 0 ? 0.0 : edge == 1 ? s : edge == 2 ? 1.0 : 1.0 - s;
    // surround.frag main(): the view ray through this pixel, turned by the
    // eye and into the picture's axes.
    const Vec3 ray{static_cast<float>((v.tangents[0] + (v.tangents[1] - v.tangents[0]) * tx) / zoom),
                   static_cast<float>((v.tangents[3] + (v.tangents[2] - v.tangents[3]) * ty) / zoom),
                   -1.0f};
    // common.glsl's rotateByQuat.
    const Vec3 q{v.orientation[0], v.orientation[1], v.orientation[2]};
    const Vec3 t = cross(q, ray) * 2.0f;
    const Vec3 local = ray + t * v.orientation[3] + cross(q, t);
    Vec3 dir{0.0f, 0.0f, 0.0f};
    const float l[3] = {local.x, local.y, local.z};
    for (int c = 0; c < 3; ++c)
      dir = dir + Vec3{v.toPicture[c][0], v.toPicture[c][1], v.toPicture[c][2]} * l[c];
    dir = normalize(dir);
    if (correct) {
      // The stereo correction's sphere intersection.
      const float along = dot(viewer, dir);
      const float reach = along * along - dot(viewer, viewer) + v.distance * v.distance;
      dir = normalize(viewer + dir * (std::sqrt(std::max(reach, 0.0f)) - along));
    }
    double u, w;
    if (v.projection == 0) {
      // projection.glsl's equirectUv, span PI.
      const double lon = std::atan2(dir.x, -dir.z);
      const double lat = std::asin(std::clamp(static_cast<double>(dir.y), -1.0, 1.0));
      u = 0.5 + lon / kPi;
      w = 0.5 - lat / kPi;
      azimuth[i] = lon;
    } else {
      // ... fisheyeUv.
      const double theta = std::acos(std::clamp(-static_cast<double>(dir.z), -1.0, 1.0));
      const double r = theta / v.fisheyeFov;
      const double len = std::sqrt(static_cast<double>(dir.x) * dir.x + static_cast<double>(dir.y) * dir.y);
      const double dx = len > 1e-6 ? dir.x / len : 0.0, dy = len > 1e-6 ? dir.y / len : 0.0;
      u = 0.5 + r * dx;
      w = 0.5 - r * dy;
      azimuth[i] = std::atan2(static_cast<double>(dir.y), static_cast<double>(dir.x));
      maxBack = std::max(maxBack, theta);
    }
    if (v.flip) w = 1.0 - w;
    if (!std::isfinite(u) || !std::isfinite(w)) return false;
    uv[i][0] = u;
    uv[i][1] = w;
  }
  // How often the border goes around the picture's pole (180) or the lens's
  // axis (fisheye), and the largest second differences.
  double turns = 0.0, pad[2] = {0.0, 0.0}, lo[2] = {1.0, 1.0}, hi[2] = {0.0, 0.0};
  for (int i = 0; i < kCount; ++i) {
    const int j = (i + 1) % kCount;
    double a = azimuth[j] - azimuth[i];
    a -= 2.0 * kPi * std::floor((a + kPi) / (2.0 * kPi));
    turns += a;
    // Three points on one edge (its 16 and the next edge's first): never
    // across a corner.
    const bool onEdge = i % kSteps != 0;
    const int h = (i + kCount - 1) % kCount;
    for (int k = 0; k < 2; ++k) {
      if (onEdge) pad[k] = std::max(pad[k], std::abs(uv[h][k] - 2.0 * uv[i][k] + uv[j][k]));
      const double c = std::clamp(uv[i][k], 0.0, 1.0);
      lo[k] = std::min(lo[k], c);
      hi[k] = std::max(hi[k], c);
    }
  }
  const bool around = std::abs(turns) > kPi;
  // A little more for the shader's float arithmetic.
  for (int k = 0; k < 2; ++k) {
    lo[k] = std::max(lo[k] - pad[k] - 1e-3, 0.0);
    hi[k] = std::min(hi[k] + pad[k] + 1e-3, 1.0);
  }
  if (v.projection == 2) {
    // The point straight behind the lens (+z) maps to a whole circle: near
    // or in the view, the whole part. Around the axis in front is fine
    // (continuous).
    if (maxBack > 0.75 * kPi || (around && maxBack > 0.5 * kPi) ||
        inOrNearView(v, correct, viewer, {0.0f, 0.0f, 1.0f}))
      return false;
  } else {
    // A pole in or near the view: every longitude, latitude up to the pole.
    // The top edge (v 0) is +y, -y when flipped. Both poles in view cancel
    // each other's turn around the border, so each is also tested directly.
    const double centreY = 0.5 * (lo[1] + hi[1]);
    const float up = v.flip ? -1.0f : 1.0f;
    const bool top = lo[1] < 0.08 || (around && centreY < 0.5) ||
                     inOrNearView(v, correct, viewer, {0.0f, up, 0.0f});
    const bool bottom = hi[1] > 0.92 || (around && centreY >= 0.5) ||
                        inOrNearView(v, correct, viewer, {0.0f, -up, 0.0f});
    if (top || bottom) {
      lo[0] = 0.0;
      hi[0] = 1.0;
    }
    if (top) lo[1] = 0.0;
    if (bottom) hi[1] = 1.0;
  }
  for (int k = 0; k < 2; ++k) {
    out.lo[k] = static_cast<float>(lo[k]);
    out.hi[k] = static_cast<float>(hi[k]);
  }
  return true;
}

// Texels x0..x1, y0..y1 (ends excluded) of the colour key's mask
// (KeyMask), whose part for the frame is part.x1 x part.y1 texels.
struct TexelRect {
  uint32_t x0, y0, x1, y1;
};

// The mask texels the eye shader's lookups read for uv b of an eye's part
// (EyeData::stereo): it reads at (eyeOffset + uv * eyeScale) times the
// part's size in texels (chroma.zw times the mask's size), bilinear, so a
// texel more on every side.
inline TexelRect maskTexels(const float* stereo, const UvBounds& b, float width, float height,
                            const TexelRect& part) {
  const float size[2] = {width, height};
  const uint32_t end[2] = {part.x1, part.y1};
  uint32_t from[2], to[2];
  for (int k = 0; k < 2; ++k) {
    const float f = std::floor((stereo[2 + k] + b.lo[k] * stereo[k]) * size[k]) - 1.0f;
    const float t = std::ceil((stereo[2 + k] + b.hi[k] * stereo[k]) * size[k]) + 1.0f;
    from[k] = static_cast<uint32_t>(std::clamp(f, 0.0f, static_cast<float>(end[k])));
    to[k] = static_cast<uint32_t>(std::clamp(t, 0.0f, static_cast<float>(end[k])));
  }
  return {from[0], from[1], to[0], to[1]};
}

inline TexelRect grown(const TexelRect& r, uint32_t by, const TexelRect& part) {
  return {r.x0 > by ? r.x0 - by : 0, r.y0 > by ? r.y0 - by : 0, std::min(r.x1 + by, part.x1),
          std::min(r.y1 + by, part.y1)};
}

inline bool within(const TexelRect& r, const TexelRect& in) {
  return r.x0 >= in.x0 && r.y0 >= in.y0 && r.x1 <= in.x1 && r.y1 <= in.y1;
}

// Two rectangles as one where they overlap or touch; the count left.
inline int merged(TexelRect* rects, int count) {
  if (count == 2 && rects[0].x0 <= rects[1].x1 && rects[1].x0 <= rects[0].x1 &&
      rects[0].y0 <= rects[1].y1 && rects[1].y0 <= rects[0].y1) {
    rects[0] = {std::min(rects[0].x0, rects[1].x0), std::min(rects[0].y0, rects[1].y0),
                std::max(rects[0].x1, rects[1].x1), std::max(rects[0].y1, rects[1].y1)};
    return 1;
  }
  return count;
}
