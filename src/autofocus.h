#pragma once

#include "depth_estimator.h"
#include "focus_controller.h"
#include "focus_settings.h"
#include "stereo_correction.h"
#include "video_format.h"
#include "xmath.h"

#include <cstdint>
#include <functional>
#include <string>

// Where the focus looks: a ray in head axes (-z = straight ahead).
struct FocusTarget {
  Vec3 direction = {0.0f, 0.0f, -1.0f};
  bool usable = true;  // false: keep the depth as it is, measure nothing
  bool eyes = false;   // aimed by the eyes
};

// What the focus needs to know each frame.
struct FocusInput {
  FocusSettings settings;
  VideoFormat format;
  std::string video;   // what is playing; a change starts over
  bool shown = false;  // a picture is drawn, readable or not
  GLuint texture = 0;  // its current frame (0 = none yet)
  DepthPatchDrawer* drawer = nullptr;  // DepthRequest::drawer: a frame that stays out of GL
  std::function<bool()> prepareRead;  // DepthRequest::prepareRead
  bool flip = false;
  double position = 0;  // seconds into the video
  bool paused = false;
  bool headValid = false;
  Quat head;              // head orientation, local space
  Vec3 leftEye, rightEye;  // eye positions, local space
  float viewerIpd = 0;     // meters; 0 = the eyes' own separation
  Vec3 motion;             // head translation emulation: offset / motion distance, local space
  Mat4 picture;           // ScreenPlacement::rotation()
  float zoom = 1.0f;
  FocusTarget target;
  SeeThrough seeThrough;  // passthrough: what shows the room is not measured
  double now = 0;  // seconds
};

struct FocusOutput {
  StereoCorrection correction;  // for the renderer; none if off or not possible
  float distance = 0;           // meters corrected at, 0 = none
  float markerDistance = 0;     // meters from the head to the measured spot as shown
  bool automatic = false;
  Vec3 marker;         // the measured direction (local space), for the diagnostics
  std::string status;  // one line for the menu
  std::string distanceText;  // corrected distance, centred on the diagnostic marker
};

// Stereo focus for the running video: no correction, a manual distance, or
// automatic depth where the viewer looks. Automatic mode measures the frame
// around the target (DepthEstimator, off the render thread's critical path),
// three probes in turn, and lets FocusController pick and glide to the depth.
// It measures whenever the last measurement is done, but only 15 times a
// second once a whole probe cycle has measured what it measured before with
// the target held still. A new video, layout or camera baseline, a seek or a
// scene cut clears the probes. Measurements are tagged with the content they
// were taken from; ones from before that are dropped. Call update() once per
// rendered frame on the GL thread, after the video frame was rendered.
class Autofocus {
 public:
  // timing: print the measuring rate every two seconds (--timing).
  bool init(bool timing = false);
  FocusOutput update(const FocusInput& in);

 private:
  void startOver(bool newContent, const char* why);
  void report(double now, bool settled);
  void describe(const FocusInput& in, bool automatic, float distance);

  DepthEstimator estimator_;
  FocusController controller_;
  bool ready_ = false;
  std::string contentKey_;
  uint64_t generation_ = 1;
  int requestPhase_ = 0;  // the probe of the measurement under way
  int requestAccuracy_ = 8;
  Vec3 requestAim_;       // its target in video axes, before the probe's spread
  double requestAt_ = -1;
  // What each probe (-1, 0, +1) last measured that differed from its
  // measurement before, and how many measurements in a row repeated their
  // probe's (up to kSettledRepeats).
  struct Outcome {
    bool known = false;
    DepthStatus status = DepthStatus::Edge;
    float angle = 0;
  };
  Outcome outcomes_[3];
  int repeats_ = 0;
  bool timing_ = false;
  double reportAt_ = -1;
  int frames_ = 0, settledFrames_ = 0, measurements_ = 0;
  // How far the last centre measurement moved off a see-through target (DepthEstimate).
  float reachAlpha_ = 0, reachBeta_ = 0;
  double lastPosition_ = -1, lastNow_ = 0;
  double statusAt_ = -1;
  std::string status_, distanceText_;
};
