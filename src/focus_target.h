#pragma once

#include "autofocus.h"
#include "gaze_correction.h"
#include "xmath.h"
#include "xr_context.h"

// What the automatic focus is aimed by, as the menu tells it.
enum class TargetStatus {
  Head,           // head direction chosen
  Eyes,           // eye tracking chosen and following the eyes
  EyesHolding,    // ... gaze lost a moment ago (a blink): the depth is held
  EyesFallback,   // ... gaze lost for longer: head direction until it is back
  EyesInactive,   // ... the gaze input is not delivering (permission, tracker off)
  EyesUnsupported,  // ... the runtime or headset has no eye tracking: head direction
};

// Picks the focus target each frame: the head's forward direction, or the
// eye gaze converted into head axes. Through a short gaze loss the last gaze
// direction is kept and no depth is measured, so a blink changes nothing;
// after that the target glides to the head direction, and back to the gaze
// once it has been tracked steadily again. The gaze is turned by `fit`
// (what the menu's clicks say the eye tracker is off by, gaze_correction.h)
// and smoothed with a One Euro filter; only the target is smoothed, never
// the rendering pose. The chosen mode itself is never changed here.
class FocusTargetSelector {
 public:
  FocusTarget update(bool eyeTracking, const GazeState& gaze, Quat head, double now,
                     const GazeFit& fit);
  TargetStatus status() const { return status_; }
  // Where the eyes look, head axes, as the target follows it (also when aimed
  // by the head), held through a blink; for the gaze dot.
  bool gazeKnown() const { return gazeKnown_; }
  Vec3 gaze() const { return gaze_; }
  // This frame's gaze as the tracker gives it, head axes, if it has one.
  bool trackerGazeGood() const { return trackerGood_; }
  Vec3 trackerGaze() const { return trackerGaze_; }

 private:
  void smooth(Vec3 g, double time);

  enum class Phase { Head, Following, Holding, Fallback, Reacquiring };
  Phase phase_ = Phase::Head;
  TargetStatus status_ = TargetStatus::Head;
  Vec3 gaze_ = {0.0f, 0.0f, -1.0f};    // smoothed gaze, head axes
  Vec3 raw_, velocity_;                // the last sample, and the filtered rate (per second)
  double sampleAt_ = -1;               // the last sample's time
  double goodAt_ = -1;                 // when the gaze was last good
  bool gazeKnown_ = false;
  bool trackerGood_ = false;
  Vec3 trackerGaze_ = {0.0f, 0.0f, -1.0f};
  Vec3 target_ = {0.0f, 0.0f, -1.0f};  // what was handed out last
  Vec3 blendFrom_ = {0.0f, 0.0f, -1.0f};
  double since_ = 0;  // when the current phase began
  double last_ = -1;
};

// One line for the menu.
const char* targetStatusText(TargetStatus status);
