#pragma once

#include "controller_state.h"
#include "xmath.h"

#include <functional>

struct ScreenPlacement;

// Holding the trigger away from the menu turns the 180°/360° picture with
// the hand (sideways, up/down and roll), for fine centring; the flat
// screen's is ScreenControl's. Releasing stops it where it is shown; what
// the smoothing had not caught up is dropped.
class PictureDrag {
 public:
  // What else is going on this frame.
  struct Frame {
    bool noDrag = false;      // the flat screen shows, or is held
    int menuHand = -1;        // the hand holding the menu (MenuGrab), -1 = none
    bool onMenu[2] = {false, false};        // each hand's pointer is on the menu
    bool triggerTaken[2] = {false, false};  // a gesture has each hand's trigger
    bool keyPicking = false;  // the trigger picks the key's colour
    bool headValid = false;
    double displayTime = 0;   // the frame's, seconds
  };

  // A pulse on a controller; set once, before the frames.
  std::function<void(int hand, float amplitude, double seconds)> buzz;

  // Each frame: takes, follows or lets go of the picture.
  void update(const ControllerState& input, const Frame& frame, ScreenPlacement& screen);
  // A drag goes on from here, without catching up first (after a recenter).
  void restart() { tracking_ = false; }

 private:
  int hand_ = -1;          // -1 = not dragging
  bool tracking_ = false;  // false: restart smoothing from the hand's current pose
  Quat smoothHand_;        // the smoothed hand orientation the picture has followed
  double lastTime_ = 0;    // display time of the last update, seconds
};
