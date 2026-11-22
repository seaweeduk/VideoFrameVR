#pragma once

#include "value_ranges.h"
#include "xmath.h"

#include <array>
#include <vector>

// The flat projection's virtual screen: where it is, how big and how bent.

// Screen distance, height and curve, with their defaults and limits.
// The width follows from the picture's shape.
struct ScreenSize {
  static constexpr float kMinDistance = value_range::kMinScreenDistance,
                         kMaxDistance = value_range::kMaxScreenDistance;  // meters
  static constexpr float kMinHeight = value_range::kMinScreenHeight,
                         kMaxHeight = value_range::kMaxScreenHeight;  // meters
  static constexpr float kMaxCurve = value_range::kMaxScreenCurve;

  float distance = 3.0f;  // meters from the viewer to the screen's centre
  float height = 3.0f;    // meters
  // 0 = flat; 1 = bent around the viewer, so every column is equally far.
  // In between, the bend's radius is distance / curve.
  float curve = 1.0f;

  ScreenSize clamped() const;
  bool operator==(const ScreenSize& o) const {
    return distance == o.distance && height == o.height && curve == o.curve;
  }
  bool operator!=(const ScreenSize& o) const { return !(*this == o); }
};

// Where the screen sits: in front of the anchor (the viewer at the last
// recenter or move), facing it.
struct ScreenPlacement {
  Vec3 anchorPosition;  // head position at the last recenter
  // Head heading at the last recenter.
  Mat4 anchorRotation;
  float turn = 0.0f;  // extra turn about the anchor's up axis (stick turning)
  ScreenSize size;
  float zoom = 1.0f;  // 180°/360° video: magnification of the view

  // Where "front" and "up" of the video are.
  Mat4 rotation() const { return anchorRotation * rotationY(turn); }
  // The anchor's frame: the screen's centre is at (0, 0, -distance) in it.
  Mat4 pose() const { return translation(anchorPosition) * rotation(); }
};

// The screen's surface as drawn for a picture `aspect` times as wide as
// high: the shape kScreenVertex in renderer.cpp gives it, for drawing it,
// pointing at it and placing things on it. Places on it are x meters right
// of its centre along the surface (around the bend when curved) and y
// meters up.
class ScreenSurface {
 public:
  // A curved screen wraps at most this far around either side of its
  // centre (radians); a larger one is drawn smaller, keeping its shape.
  static constexpr float kMaxHalfArc = 2.6f;

  ScreenSurface(const ScreenPlacement& placement, float aspect);

  float width() const { return width_; }
  float height() const { return height_; }
  float distance() const { return distance_; }
  float radius() const { return radius_; }  // of the bend; 0 = flat
  const Mat4& pose() const { return pose_; }

  // The world point at x, y.
  Vec3 point(float x, float y) const;
  // Where a ray from `origin` along `dir` meets the surface, extended past
  // its edges (x within kMaxHalfArc when curved); false if it doesn't, in
  // front of the origin.
  bool hit(Vec3 origin, Vec3 dir, float& x, float& y) const;

 private:
  Mat4 pose_, inverse_;
  float width_ = 0, height_ = 0, distance_ = 0, radius_ = 0;
};

// A corner of the screen, by the sides it is on: +1 = right or top, -1 =
// left or bottom; 0, 0 = none.
struct ScreenCorner {
  int sx = 0, sy = 0;
  bool valid() const { return sx != 0; }
};

// The corner whose handle a place on the surface is on or near enough to
// take hold of; none if it is on no corner.
ScreenCorner cornerAt(const ScreenSurface& surface, float x, float y);

// A flat quad given by its corners in triangle strip order.
using Quad = std::array<Vec3, 4>;

// The handle that shows on a corner while it can be dragged: an L along the
// two edges, just outside the picture.
std::vector<Quad> cornerHandle(const ScreenSurface& surface, ScreenCorner corner);
