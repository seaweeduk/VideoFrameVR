#include "autofocus.h"
#include "strings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr float kDegree = 3.14159265f / 180.0f;
// Playback that jumped this much further than the clock, or back: a seek
// (a stall while a stream buffers is not one).
constexpr double kSeekJump = 0.75;
constexpr double kSeekBack = 0.25;
// The whole eye picture changed this much (mean, 0..1) since the last
// measurement: a scene cut. Consecutive frames of a real clip differ by
// about 0.01, different scenes of it by 0.13 or more.
constexpr float kSceneCut = 0.06f;
constexpr double kStatusInterval = 0.25;  // seconds between status text updates
// The side probes' angle from the target: 0.003 / sensitivity radians,
// scaled by 4 to suit this estimator's 2.9° window.
constexpr float kSpread = 0.003f, kSpreadScale = 4.0f;
// Aimed by the eyes, the match window is wider (ours): the gaze is less exact
// than the head direction, about 2° after correction.
constexpr float kEyeWindow = 4.3f * kDegree;
// With passthrough, a gaze that lands on see-through room measures the
// nearest video within this angle instead (ours): about the tracker's
// remaining error.
constexpr float kEyeReach = 3.0f * kDegree;
// Measuring rate (ours). A still gaze on a still part of the picture learns
// nothing new from 60 measurements a second: once a whole probe cycle
// (+1, 0, -1, 0) has repeated, a measurement starts only every
// kSettledInterval. A repeat has its probe's last status and, if Ok, an angle
// within two candidate steps of the first of its run of repeats, so a depth
// that drifts slowly still ends the run (the angles are candidates: the half
// step only absorbs rounding; a step is 0.008 / accuracy radians, see
// candidateAngle()).
constexpr int kSettledRepeats = 4;
constexpr float kRepeatSteps = 2.5f, kCandidateStep = 0.008f;
constexpr double kSettledInterval = 1.0 / 15;
// Looking elsewhere (the target turned this far since the last measurement
// began), a measurement that does not repeat, a seek or a scene cut bring the
// full rate back at once.
constexpr float kSettledTurn = 0.5f * kDegree;
constexpr double kReportInterval = 2.0;  // seconds between --timing lines

Quat axisAngle(Vec3 axis, float angle) {
  const float s = std::sin(0.5f * angle);
  return {axis.x * s, axis.y * s, axis.z * s, std::cos(0.5f * angle)};
}

const char* reasonText(FocusReason r) {
  const auto& t = strings().focusStatus;
  switch (r) {
    case FocusReason::None: return "";
    case FocusReason::Blank: return t.reasons[0];
    case FocusReason::Ambiguous: return t.reasons[1];
    case FocusReason::Mismatch: return t.reasons[2];
    case FocusReason::Edge: return t.reasons[3];
    case FocusReason::Clear: return t.reasons[4];
    case FocusReason::Disagree: return t.reasons[5];
    case FocusReason::OutOfRange: return t.reasons[6];
  }
  return "";
}

std::string meters(float m) {
  char buf[32];
  if (!std::isfinite(m)) return strings().focusStatus.far;
  std::snprintf(buf, sizeof(buf), m < 10 ? "%.2f m" : "%.0f m", m);
  return buf;
}

}  // namespace

bool Autofocus::init(bool timing) {
  timing_ = timing;
  ready_ = estimator_.init();
  if (!ready_) std::fprintf(stderr, "Autofocus: depth estimator unavailable\n");
  return ready_;
}

void Autofocus::startOver(bool newContent, const char* why) {
  ++generation_;
  if (newContent) estimator_.forgetVideo();
  controller_.clearProbes();
  reachAlpha_ = reachBeta_ = 0;
  for (Outcome& o : outcomes_) o.known = false;
  repeats_ = 0;
  statusAt_ = -1;
  if (!newContent) std::printf("Autofocus: new probes (%s)\n", why);
}

FocusOutput Autofocus::update(const FocusInput& in) {
  FocusOutput out;
  const FocusSettings& s = in.settings;
  const bool supported = stereoCorrectionSupported(in.format);
  const FocusMode mode = supported ? s.mode : FocusMode::Off;
  const bool automatic = mode == FocusMode::Automatic && ready_;

  // The analysed direction: the target with its offset, as seen (marker) and
  // through the zoom into the video's axes.
  auto offset = [&](float yaw) {
    return transformDir(rotation(axisAngle({0, 1, 0}, -yaw) *
                                 axisAngle({1, 0, 0}, s.pitchDeg * kDegree)),
                        normalize(in.target.direction));
  };
  // With emulated head translation the head looks from off the centre: the
  // measured direction is where its ray meets the (unit) sphere.
  const Mat4 toVideoAxes = transposedRotation(in.picture);
  const Vec3 origin = transformDir(toVideoAxes, in.motion);
  auto toVideo = [&](Vec3 d) {
    const Vec3 zoomed = d.z < 0 ? normalize(Vec3{d.x / in.zoom, d.y / in.zoom, d.z}) : d;
    const Vec3 ray = transformDir(toVideoAxes, transformDir(rotation(in.head), zoomed));
    return normalize(throughUnitSphere(origin, ray));
  };
  out.marker = transformDir(rotation(in.head), offset(s.yawDeg * kDegree));

  float distance = 0;
  if (mode == FocusMode::Manual) {
    distance = s.distanceCm / 100.0f;
  } else if (automatic) {
    char key[64];
    std::snprintf(key, sizeof(key), "|%d|%d|%d", static_cast<int>(in.format.projection),
                  static_cast<int>(in.format.stereo), s.baselineMm);
    const std::string contentKey = in.video + key;
    const bool loading = in.texture == 0 && !in.drawer;
    if (contentKey != contentKey_) {
      contentKey_ = contentKey;
      startOver(true, "new video, layout or baseline");
    } else if (!loading && lastPosition_ >= 0) {
      const double moved = in.position - lastPosition_;
      if (moved < -kSeekBack || moved > in.now - lastNow_ + kSeekJump) startOver(false, "seek");
    }
    lastPosition_ = loading ? -1 : in.position;
    lastNow_ = in.now;

    DepthEstimate e;
    if (estimator_.poll(e) && e.tag == generation_ && !loading) {
      if (e.sceneChange > kSceneCut) startOver(false, "scene cut");
      Outcome& last = outcomes_[requestPhase_ + 1];
      const bool repeat =
          last.known && e.status == last.status &&
          (e.status != DepthStatus::Ok ||
           std::abs(e.angle - last.angle) <= kRepeatSteps * kCandidateStep / requestAccuracy_);
      repeats_ = repeat ? std::min(repeats_ + 1, kSettledRepeats) : 0;
      if (!repeat) last = {true, e.status, e.angle};
      controller_.submit(e, requestPhase_);
      if (requestPhase_ == 0) reachAlpha_ = e.reachAlpha, reachBeta_ = e.reachBeta;
    }
    // The target in the video's axes; the length of the difference of unit
    // vectors is the angle between them, for small angles.
    const Vec3 aim = toVideo(offset(s.yawDeg * kDegree));
    const Vec3 turn = aim - requestAim_;
    if (dot(turn, turn) > kSettledTurn * kSettledTurn) repeats_ = 0;
    const bool settled = repeats_ >= kSettledRepeats;
    // Measuring as often as the last measurement allows (one in flight), or
    // every kSettledInterval while settled.
    if (!loading && in.headValid && in.target.usable &&
        (!settled || in.now - requestAt_ >= kSettledInterval)) {
      const int phase = controller_.phase();
      const float spread = kSpreadScale * kSpread * 100.0f / s.sensitivity;
      DepthRequest q;
      q.texture = in.texture;
      q.drawer = in.drawer;
      q.prepareRead = in.prepareRead;
      q.format = in.format;
      q.flip = in.flip;
      q.direction = toVideo(offset(s.yawDeg * kDegree + phase * spread));
      q.baseline = s.baselineMm / 1000.0f;
      q.accuracy = s.accuracy;
      if (in.target.eyes) q.window = kEyeWindow, q.reach = kEyeReach;
      q.seeThrough = in.seeThrough;
      q.tag = generation_;
      if (estimator_.request(q)) {
        requestPhase_ = phase;
        requestAccuracy_ = s.accuracy;
        requestAim_ = aim;
        requestAt_ = in.now;
        ++measurements_;
      }
    }
    if (timing_) report(in.now, settled);
    distance = controller_.advance(s, in.now, in.target.eyes);
    out.automatic = true;
    if (!in.target.eyes) reachAlpha_ = reachBeta_ = 0;
    // The marker shows where the centre probe measured: moved off the target
    // as far as its last measurement reached, back through the motion origin
    // and the zoom.
    if (reachAlpha_ != 0 || reachBeta_ != 0) {
      float alpha = 0, beta = 0;
      epipolarAngles(aim, alpha, beta);
      const Vec3 reached = epipolarDirection(alpha + reachAlpha_, beta + reachBeta_);
      const Mat4 head = rotation(in.head);
      Vec3 d = transformDir(transposedRotation(head),
                            transformDir(in.picture, normalize(reached - limitedMotion(origin))));
      if (d.z < 0) d = normalize(Vec3{d.x * in.zoom, d.y * in.zoom, d.z});
      out.marker = transformDir(head, d);
    }
  } else {
    reportAt_ = -1;
    frames_ = settledFrames_ = measurements_ = 0;
  }

  float along = 1.0f;
  throughUnitSphere(origin, transformDir(toVideoAxes, out.marker), &along);
  if (distance > 0 && in.shown)
    out.correction = stereoCorrection(distance, s.baselineMm / 1000.0f, in.leftEye, in.rightEye,
                                      in.picture, in.viewerIpd, in.motion);
  else if (in.shown && length(in.motion) > 0 && in.format.projection != Projection::Flat)
    out.correction = motionCorrection(in.motion, in.picture);
  out.distance = distance;
  out.markerDistance = distance * along;
  if (statusAt_ < 0 || in.now - statusAt_ >= kStatusInterval) {
    statusAt_ = in.now;
    describe(in, automatic, distance);
  }
  out.status = status_;
  out.distanceText = distanceText_;
  return out;
}

void Autofocus::report(double now, bool settled) {
  if (reportAt_ < 0) reportAt_ = now;
  ++frames_;
  if (settled) ++settledFrames_;
  if (now - reportAt_ < kReportInterval) return;
  std::printf("[focus] %.1f measurements/s, settled %.0f%% of frames\n",
              measurements_ / (now - reportAt_), 100.0 * settledFrames_ / frames_);
  reportAt_ = now;
  frames_ = settledFrames_ = measurements_ = 0;
}

void Autofocus::describe(const FocusInput& in, bool automatic, float distance) {
  const FocusSettings& s = in.settings;
  const auto& t = strings().focusStatus;
  char buf[512];
  distanceText_ = distance > 0 ? meters(distance) : "";
  if (s.mode == FocusMode::Automatic && !automatic) {
    status_ = t.unavailable;
    return;
  }
  if (distance <= 0) {
    status_ = t.off;
    return;
  }
  const float ipd = 1000.0f * (in.viewerIpd > 0 ? in.viewerIpd : length(in.rightEye - in.leftEye));
  if (!automatic) {
    std::snprintf(buf, sizeof(buf), t.manual, meters(distance).c_str(), ipd);
    status_ = buf;
    return;
  }
  const FocusController::State& st = controller_.state();
  std::string why = reasonText(st.holding ? FocusReason::OutOfRange : st.reason);
  if (reachAlpha_ != 0 || reachBeta_ != 0) {
    std::snprintf(buf, sizeof(buf), t.nearest,
                  std::hypot(reachAlpha_, reachBeta_) / kDegree);
    why = why.empty() ? buf : why + ", " + buf;
  }
  if (!st.locked) {
    std::snprintf(buf, sizeof(buf), t.measuring, meters(distance).c_str(),
                  (why.empty() ? "" : "; " + why).c_str());
    status_ = buf;
  } else if (st.holding) {
    std::snprintf(buf, sizeof(buf), t.holding, meters(distance).c_str());
    status_ = buf;
  } else {
    char confidence[32];
    std::snprintf(confidence, sizeof(confidence), "%.0f%%", 100.0f * st.confidence);
    std::snprintf(buf, sizeof(buf), t.following, meters(distance).c_str(),
                  (st.measured > 0 ? meters(st.measured) : t.nothing).c_str(), confidence,
                  (why.empty() ? "" : ", " + why).c_str());
    status_ = buf;
  }
}
