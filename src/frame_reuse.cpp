#include "frame_reuse.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {

// How far the head may have turned since the frame was drawn: 0.15° is about
// five pixels of a 3780 pixel eye image, and that much of the view's edge is
// all reprojection cannot fill. Looking around is faster than this from one
// refresh to the next (18°/s at 120 Hz), so those frames are all drawn.
constexpr float kMaxTurn = 0.15f * 3.14159265f / 180.0f;
// How far anything in the picture may be from where a drawn frame would
// show it, for what reprojection does not follow (the eyes' places, the
// stereo correction, a zoomed view): 0.02° is under a pixel of a 3780 pixel
// eye image, and half an 8K video's.
constexpr float kMaxShift = 0.02f * 3.14159265f / 180.0f;
// The gaze may have moved by this fraction of the sharp area's size.
constexpr int kGazeFraction = 8;

bool same(Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
bool same(const float* a, const float* b, int count) { return std::equal(a, a + count, b); }

bool same(const ScreenPlacement& a, const ScreenPlacement& b) {
  return same(a.anchorPosition, b.anchorPosition) && same(a.anchorRotation.m, b.anchorRotation.m, 16) &&
         a.turn == b.turn && a.size == b.size && a.zoom == b.zoom;
}

bool same(const SeeThrough& a, const SeeThrough& b) {
  return a.packedAlpha == b.packedAlpha && a.maskColours == b.maskColours && a.greenKey == b.greenKey &&
         a.colourKey == b.colourKey && same(a.keyColour, b.keyColour, 3) && a.keyCut == b.keyCut &&
         a.keySoft == b.keySoft && a.picking == b.picking;
}

// How much more than the head a surround picture's parts turn in the view,
// at most: 1 unless it is zoomed (surround.frag scales the view's tangents
// by 1 / zoom, so the picture is not fixed in the room then).
float magnification(const VideoDraw& v) {
  if (v.format.projection == Projection::Flat || !(v.screen.zoom > 0)) return 1;
  return std::max(v.screen.zoom, 1 / v.screen.zoom);
}

// How far (radians, at most) a changed stereo correction moves the picture
// in an eye; more than kMaxShift if it cannot be told. The correction
// follows the eyes' positions and the focus distance, which never hold
// still. The shader (surround.frag) shows the picture on a sphere of the
// focus distance around the camera, seen from the viewer's place: only that
// place in the sphere's radii counts. Moving it by d radii moves what is
// seen on the sphere by at most d / (1 - r), r radii from the centre (the
// nearest of the sphere is 1 - r away), times the zoom.
float shift(const EyeCorrection& a, const EyeCorrection& b, float magnified) {
  constexpr float kUnknown = 2 * kMaxShift;
  if (a.on != b.on) return kUnknown;
  if (!a.on) return 0;
  if (!(a.distance > 0 && b.distance > 0)) return kUnknown;
  const Vec3 from = (a.viewer - a.camera) * (1.0f / a.distance);
  const Vec3 to = (b.viewer - b.camera) * (1.0f / b.distance);
  const float off = std::max(length(from), length(to));
  return off < 0.95f ? length(to - from) * magnified / (1.0f - off) : kUnknown;
}

// The nearest the flat screen can be to an eye (meters): the screen's plane,
// or the upright cylinder a curved one is part of (ScreenSurface).
float screenDistance(const ScreenPlacement& s, Vec3 eye) {
  const Vec3 o = transformDir(transposedRotation(s.rotation()), eye - s.anchorPosition);
  if (s.size.curve <= 1e-3f) return std::abs(o.z + s.size.distance);
  const float radius = s.size.distance / s.size.curve;
  return std::abs(radius - std::hypot(o.x, o.z - (radius - s.size.distance)));
}

// Every field of VideoDraw but the correction: a new one is compared here too.
bool same(const VideoDraw& a, const VideoDraw& b) {
  return a.texture == b.texture && a.format.projection == b.format.projection &&
         a.format.stereo == b.format.stereo && a.format.packing == b.format.packing &&
         a.frameAspect == b.frameAspect && a.flip == b.flip && same(a.screen, b.screen) &&
         same(a.adjust, b.adjust, 3) && a.tvGamma == b.tvGamma && a.photo == b.photo &&
         same(a.balance, b.balance, 3) && a.opacity == b.opacity && a.mipmaps == b.mipmaps &&
         a.antiShimmer == b.antiShimmer && a.look == b.look && a.sharpen == b.sharpen &&
         same(a.seeThrough, b.seeThrough) && a.key == b.key;
}

// The images are shown again with the fields of view they were drawn for,
// so another field of view only moves the view's edges, as a turn does. An
// edge may differ by what a turn of kMaxTurn moves it: a tangent changes by
// at most twice the angle, out to 45° off the axis.
bool sameView(const EyeMatrices& a, const EyeMatrices& b) {
  const float limit = 2 * kMaxTurn;
  return std::abs(a.tanLeft - b.tanLeft) <= limit && std::abs(a.tanRight - b.tanRight) <= limit &&
         std::abs(a.tanUp - b.tanUp) <= limit && std::abs(a.tanDown - b.tanDown) <= limit;
}

// The angle between two orientations (radians), for small ones.
float turn(const EyeMatrices& a, const EyeMatrices& b) {
  // Two unit quaternions an angle t apart differ by 2 sin(t / 4) in length
  // (q and -q are one orientation), which for a small t is t / 2. The cosine
  // from their dot product cannot tell such angles apart in single precision.
  const Quat& p = a.orientation;
  const Quat& q = b.orientation;
  const float s = p.x * q.x + p.y * q.y + p.z * q.z + p.w * q.w < 0 ? -1.0f : 1.0f;
  const float dx = p.x - s * q.x, dy = p.y - s * q.y, dz = p.z - s * q.z, dw = p.w - s * q.w;
  return 2 * std::sqrt(dx * dx + dy * dy + dz * dz + dw * dw);
}

// Nothing but the picture: no overlays and no foveation view. Photos are
// left out: their image is uploaded in parts, without a version to tell one
// state from the next.
bool plain(const FrameScene& scene) {
  return scene.overlays.empty() && !scene.showFoveation && !(scene.video && scene.video->photo);
}

}  // namespace

const char* FrameReuse::reasonName(Reason reason) {
  static const char* const names[kReasons] = {"same", "off",  "overlays", "picture", "scene", "focus",
                                              "view", "turn", "move",     "gaze",    "age"};
  return names[reason];
}

FrameReuse::Reason FrameReuse::compare(const Frame& f) const {
  if (!enabled_ || !valid_ || !f.scene || !f.mapsKnown) return Off;
  const FrameScene& scene = *f.scene;
  if (!plain(scene)) return Overlays;
  if (f.picture != picture_) return Picture;
  if (scene.clear != clear_ || scene.sky != sky_ || scene.video.has_value() != video_.has_value() ||
      f.alphaBlend != alphaBlend_ || f.eyeCount != eyeCount_ || f.targets != targets_ || f.maps != maps_)
    return Scene;
  if (scene.video && !same(*scene.video, *video_)) return Scene;
  // What reprojection does not follow has kMaxShift between it, per eye: the
  // stereo correction's change, and a zoomed picture turning in the view by
  // more than the head does.
  const float magnified = scene.video ? magnification(*scene.video) : 1;
  float shifted[2] = {0, 0};
  for (int i = 0; scene.video && i < 2; ++i) {
    shifted[i] = shift(scene.video->correction.eyes[i], video_->correction.eyes[i], magnified);
    if (shifted[i] > kMaxShift) return Focus;
  }
  const bool flat = scene.video && scene.video->format.projection == Projection::Flat;
  for (int i = 0; i < eyeCount_; ++i) {
    if (!sameView(f.eyes[i], eyes_[i])) return View;
    const float turned = turn(f.eyes[i], eyes_[i]);
    if (turned > kMaxTurn || shifted[i] + (magnified - 1) * turned > kMaxShift) return Turn;
    // Only a flat screen is somewhere in the room (a surround picture's
    // stereo correction is compared above): the eye may have moved by what
    // moves the nearest of the screen within kMaxShift.
    if (flat && length(f.eyes[i].position - eyes_[i].position) >
                    kMaxShift * std::min(screenDistance(scene.video->screen, f.eyes[i].position),
                                         screenDistance(scene.video->screen, eyes_[i].position)))
      return Move;
    if (std::abs(f.mapOffset[i].x - mapOffset_[i].x) * kGazeFraction > static_cast<int>(f.sharp[i].width) ||
        std::abs(f.mapOffset[i].y - mapOffset_[i].y) * kGazeFraction > static_cast<int>(f.sharp[i].height))
      return Gaze;
  }
  const double age = f.displayTime - displayTime_;
  return age > 0 && age <= kMaxAge ? Same : Age;
}

void FrameReuse::drawn(const Frame& f) {
  // A frame with something over the picture is not one to show again.
  valid_ = enabled_ && f.scene && f.eyeCount >= 1 && f.eyeCount <= 2 && plain(*f.scene);
  if (!valid_) return;
  clear_ = f.scene->clear;
  sky_ = f.scene->sky;
  video_ = f.scene->video;
  picture_ = f.picture;
  alphaBlend_ = f.alphaBlend;
  displayTime_ = f.displayTime;
  eyeCount_ = f.eyeCount;
  for (int i = 0; i < eyeCount_; ++i) {
    eyes_[i] = f.eyes[i];
    mapOffset_[i] = f.mapOffset[i];
  }
  targets_ = f.targets;
  maps_ = f.maps;
}
