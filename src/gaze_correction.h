#pragma once

#include "xmath.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// A direction in head axes as angles in degrees, yaw to the right and pitch
// up, or a difference between two of them.
struct GazeOffset {
  float yaw = 0, pitch = 0;
};
GazeOffset gazeAngles(Vec3 direction);

// What to add to a gaze's yaw and its pitch, each as: degrees straight
// ahead, more per degree of the gaze's yaw, more per degree of its pitch.
struct GazeFit {
  float yaw[3] = {}, pitch[3] = {};
};
Vec3 applyGazeFit(Vec3 direction, const GazeFit& fit);

// Learns how far the eye tracker is off from clicks in the menu (ours): at a
// quick click on a button, with the eyes and the pointer at rest just before
// it, the eyes are taken to be on the pointer's dot, and the gap between that
// and the gaze as the tracker's error there. SteamVR corrects
// its tracker the same way, but only from clicks on its own dashboard, so
// nothing follows the headset moving on the face while the app runs. This
// works on what its correction leaves.
//
// The error changes across the view, so the yaw and the pitch correction are
// each a plane over the gaze's yaw and pitch, fitted to a sitting's clicks.
// Measured, the error is a turn and a stretch. The stretch stays (the
// tracker understates how far the eyes are turned sideways by about a
// tenth). The turn changes at every launch, as SteamVR refits its own
// correction from the dashboard clicks a launch takes: its roll and its
// sideways offset either way, its offset up and down around a steady degree
// or two. So the stretch and the offset straight ahead are remembered, as
// the medians of the last sittings', and the tilts across are not. A sitting
// starts from those: a few clicks move the offset, and the plane tilts only
// as clicks spread over the view. A click counts for at most
// kClip degrees more than the fit so far says, so one that was not looked at
// cannot pull it far.
//
// Yaw and pitch are fitted apart, each from every click. A sitting lasts
// until the headset comes off or the dashboard opens.
// Another runtime version forgets what is remembered.
class GazeCorrection {
 public:
  struct Input {
    double now = 0;
    bool gazeGood = false;  // a tracked sample this frame
    Vec3 gaze;              // ... as the tracker gives it, head axes
    bool pointing = false;  // the pointer is on the menu
    Vec3 dot;               // ... its dot, head axes, seen from between the eyes
    bool pressed = false;   // the trigger went down on a button (not a slider or a rating)
    bool held = false;      // the trigger is down
    bool dragged = false;   // ... and the menu took it as a drag (a list, a rating): no click
  };
  void update(const Input& in);
  // The headset came off or the app lost input focus.
  void endSitting();
  // What to add to the tracker's gaze now; changes are eased in.
  GazeFit fit() const;

  // The remembered sittings, for settings.conf. `runtime`: the OpenXR
  // runtime's version. A different learning-data or runtime version is ignored.
  void load(const std::string& saved, uint64_t runtime);
  std::string saved() const;
  // Whether saved() changed since this was last asked.
  bool takeChanged();

 private:
  struct Click {
    float yaw, pitch;  // the gaze
    float off;         // the dot's angle minus the gaze's
  };
  // Yaw or pitch.
  struct Part {
    std::vector<Click> clicks;  // this sitting's, newest last
    std::vector<float> votes;   // earlier sittings' stretch (the tilt along its own axis)
    std::vector<float> ahead;   // ... and their offset straight ahead
    float base[3] = {};         // what a sitting starts from: their median, as a plane
    float plane[3] = {};        // this sitting's, on top of it
    float shown[3] = {};        // base + plane, eased
    float at(float yaw, float pitch) const;
  };
  struct Rest {
    double at = -1;
    bool ok = false;
    GazeOffset gaze, dot;
  };

  bool rested(double now);
  void learn();
  static void settle(Part& part);
  static void remember(Part& part, int own);

  Part parts_[2];  // yaw, pitch
  std::array<Rest, 32> history_;
  size_t next_ = 0;
  bool pending_ = false;  // a press that may yet be a quick click
  double pressedAt_ = 0;
  GazeOffset pressGaze_, pressDot_;
  double last_ = -1;
  uint64_t runtime_ = 0;
  bool changed_ = false;
};
