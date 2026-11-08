#include "stereo_correction.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

// Keep ordinary motion exact, then ease through the last quarter of the
// allowed radius. The slope is 1 at the knee and stays positive beyond it,
// unlike a hard clamp that stops forward motion at strong saved origins.
constexpr double kMotionKnee = 0.6;
constexpr double kMaxMotion = 0.8;
constexpr float kMinDistance = 0.1f;  // meters: the smallest focus radius
// A larger scene: how far past parallel a far background may turn, in
// radians, while the eyes converge on the focus.
constexpr float kMaxDivergence = 1.0f * 3.14159265f / 180.0f;
constexpr float kNearest = 0.3f;  // meters: a smaller scene's nearest focus

}  // namespace

Vec3 limitedMotion(Vec3 m) {
  // Double avoids overflowing the norm for large finite float inputs.
  const double len = std::sqrt(static_cast<double>(m.x) * m.x +
                               static_cast<double>(m.y) * m.y +
                               static_cast<double>(m.z) * m.z);
  if (len <= kMotionKnee) return m;
  constexpr double room = kMaxMotion - kMotionKnee;
  const double eased = kMaxMotion - room * room / (room + len - kMotionKnee);
  // Leave two float ulps so rounding the components cannot reach the limit.
  const double ceiling = std::nextafter(std::nextafter(0.8f, 0.0f), 0.0f);
  const double scale = std::min(eased, ceiling) / len;
  return {static_cast<float>(m.x * scale), static_cast<float>(m.y * scale),
          static_cast<float>(m.z * scale)};
}

bool stereoCorrectionSupported(const VideoFormat& format) {
  if (format.stereo != StereoLayout::SideBySide && format.stereo != StereoLayout::TopBottom)
    return false;
  return format.projection == Projection::Equirect180 ||
         format.projection == Projection::Fisheye180 ||
         format.projection == Projection::Fisheye190 ||
         format.projection == Projection::Fisheye200;
}

StereoCorrection stereoCorrection(float distance, float baseline, Vec3 leftEye, Vec3 rightEye,
                                  const Mat4& pictureRotation, float viewerIpd, Vec3 motion,
                                  float size) {
  StereoCorrection c;
  const Mat4 toVideo = transposedRotation(pictureRotation);
  const Vec3 center = (leftEye + rightEye) * 0.5f;
  const Vec3 eyes[2] = {leftEye, rightEye};
  // Keep a nonzero minimum focus radius; eye offsets also affect whether
  // the viewer is inside its source-camera-centred sphere.
  distance = std::max(distance, kMinDistance);
  const Vec3 moved = transformDir(toVideo, limitedMotion(motion)) * distance;
  for (int i = 0; i < 2; ++i) {
    EyeCorrection& e = c.eyes[i];
    e.on = true;
    Vec3 offset = eyes[i] - center;
    const float half = length(offset);
    if (viewerIpd > 0 && half > 1e-5f) offset = offset * (0.5f * viewerIpd / half);
    if (size != 1) offset = offset * (1 / size);
    e.viewer = transformDir(toVideo, offset) + moved;
    e.camera = {(i == 0 ? -0.5f : 0.5f) * baseline, 0.0f, 0.0f};
    e.distance = distance;
  }
  return c;
}

// The eyes' lookups of a far background differ by (ipd / size - baseline)
// over the distance to the sphere looking ahead, and the size turns them
// outward by at most ipd * (1 - 1 / size) over it in any direction. The
// focus is shown size times as far.
float shownSize(float size, float distance, float baseline, float ipd, float reach) {
  distance = std::max(distance, kMinDistance);
  float shown = std::max(size, std::min(1.0f, kNearest / distance));
  const float turn = kMaxDivergence * distance * reach;
  const float narrowest = std::max(ipd - turn, std::min(ipd, baseline - turn));
  if (narrowest > 0) shown = std::min(shown, ipd / narrowest);
  return shown;
}

StereoCorrection motionCorrection(Vec3 motion, const Mat4& pictureRotation) {
  StereoCorrection c;
  const Vec3 viewer = transformDir(transposedRotation(pictureRotation), limitedMotion(motion));
  for (EyeCorrection& e : c.eyes) {
    e.on = true;
    e.viewer = viewer;
    e.camera = {};
    e.distance = 1.0f;
  }
  return c;
}

Vec3 throughUnitSphere(Vec3 origin, Vec3 direction, float* along) {
  origin = limitedMotion(origin);
  direction = normalize(direction);
  const float b = dot(origin, direction);
  const float t = std::sqrt(std::max(b * b - dot(origin, origin) + 1.0f, 0.0f)) - b;
  if (along) *along = t;
  return origin + direction * t;
}

void epipolarAngles(Vec3 d, float& alpha, float& beta) {
  alpha = std::acos(std::clamp(d.x, -1.0f, 1.0f));
  beta = std::atan2(d.y, -d.z);
}

Vec3 epipolarDirection(float alpha, float beta) {
  const float s = std::sin(alpha);
  return {std::cos(alpha), s * std::sin(beta), -s * std::cos(beta)};
}

// In the epipolar plane: the cameras at x = -/+ baseline / 2, the point at
// angle alpha and `distance` from the centre between them.
float disparityAt(float alpha, float distance, float baseline) {
  const float x = distance * std::cos(alpha), y = distance * std::sin(alpha);
  return std::atan2(y, x - 0.5f * baseline) - std::atan2(y, x + 0.5f * baseline);
}

float distanceFromDisparity(float alpha, float disparity, float baseline) {
  if (disparity <= 0) return std::numeric_limits<float>::infinity();
  const float left = alpha - 0.5f * disparity, right = alpha + 0.5f * disparity;
  // Law of sines in the triangle of both cameras and the point.
  const float fromLeft = baseline * std::sin(right) / std::sin(disparity);
  const float x = -0.5f * baseline + fromLeft * std::cos(left), y = fromLeft * std::sin(left);
  return std::sqrt(x * x + y * y);
}
