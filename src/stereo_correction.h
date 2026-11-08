#pragma once

#include "video_format.h"
#include "xmath.h"

// Depth-dependent stereo correction (see docs/autofocus.md, "Implementation").
//
// Each eye image of a calibrated stereo video is a picture of the directions
// seen from its own camera, the two cameras `baseline` apart along the
// video's x axis. Each eye uses a sphere of radius `distance` around its
// source camera. The shader subtracts that camera from the viewer before
// intersecting the sphere, then samples the resulting camera-relative
// direction. These separate depth proxies do not describe one common scene
// sphere. Head translation is left to the motion emulation (motion.h);
// without it the rig centre moves with the head.

// Stereo video in a projection this geometry is implemented for: 180°
// equirectangular and fisheye side-by-side or top-bottom. Not mono, flat,
// 360° (other camera models) or cubemaps.
bool stereoCorrectionSupported(const VideoFormat& format);

// What the surround shader needs for one eye, in the video's axes (meters).
struct EyeCorrection {
  bool on = false;
  Vec3 viewer;   // eye offset from the head centre, plus distance * motion
  Vec3 camera;   // its camera's offset from the rig centre
  float distance = 0;
};

struct StereoCorrection {
  EyeCorrection eyes[2];  // 0 = left
};

// Combined origin/head offset in sphere radii: unchanged through 0.6, then
// eased toward 0.8. Shared by sphere lookups and the autofocus marker inverse.
Vec3 limitedMotion(Vec3 motion);

// For the current eye positions (local space, from OpenXR) and the picture's
// rotation(): the per-eye lookups using source-camera-centred spheres of
// radius `distance`. viewerIpd (meters) replaces the eyes' separation, in
// their direction; 0 = the headset's own.
// motion: head translation emulation (motion.h), the head's offset divided
// by the motion distance (local space); the eyes then look from that far
// off each sphere's centre, in units of its radius, plus the viewer-minus-
// source eye offset divided by distance.
// size: the scene shown this many times larger (shownSize()), as if the
// cameras, spheres and motion distance were: the eye offsets are divided by
// it. The caller divides the head's part of motion by the chosen size.
StereoCorrection stereoCorrection(float distance, float baseline, Vec3 leftEye, Vec3 rightEye,
                                  const Mat4& pictureRotation, float viewerIpd = 0,
                                  Vec3 motion = {}, float size = 1);
// The scene size that may be shown at this focus distance, for the size
// chosen (1 = as filmed) and the viewer IPD in effect. Exact only at the focus
// distance. Larger narrows the eyes' lookups, which turns what lies behind
// the focus outward: a far background by at most 1°, and looking ahead never
// more than 1° past parallel. Smaller never brings the focus nearer than
// 30 cm. Neither limits what size 1 shows. reach: the nearest the sphere
// can be from the eyes, in parts of its radius (1 = from its centre).
float shownSize(float size, float distance, float baseline, float ipd, float reach = 1);
// Head translation emulation alone, for any surround video without stereo
// correction: both eyes look from `motion` inside a unit sphere.
StereoCorrection motionCorrection(Vec3 motion, const Mat4& pictureRotation);
// Where the ray from `origin` (inside the unit sphere) along `direction`
// meets it, and how far that is (in radii).
Vec3 throughUnitSphere(Vec3 origin, Vec3 direction, float* along = nullptr);

// Epipolar angles of a direction in video axes: alpha from the baseline (+x),
// beta around it (0 = ahead, positive up). A scene point has the same beta
// seen from either camera; only alpha differs, by its disparity.
void epipolarAngles(Vec3 direction, float& alpha, float& beta);
Vec3 epipolarDirection(float alpha, float beta);
// Disparity (alpha seen from the right camera minus from the left, radians)
// of a point `distance` meters from the rig centre at angle alpha.
float disparityAt(float alpha, float distance, float baseline);
// The inverse: the distance of the point seen at alpha -/+ disparity / 2 from
// the two cameras; infinite for a disparity of 0 or less.
float distanceFromDisparity(float alpha, float disparity, float baseline);
