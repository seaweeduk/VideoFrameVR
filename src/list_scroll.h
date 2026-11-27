#pragma once

#include "controller_state.h"

// Lists glide with the stick up/down: the further it is tilted, the faster
// (up to ~14 rows a second), with a small dead zone around the middle. Kept
// pushed well over for more than a second, a long list speeds up smoothly
// to 4 times that by 3 s. Easing off loses the speed-up within half a
// second; letting go or reversing loses it at once.
class ListScroll {
 public:
  // What else is going on this frame.
  struct Frame {
    float dt = 0;            // seconds since the last frame
    bool valueStick = false;  // the sticks step a pointed value (ValueStick)
    int menuHand = -1;       // the hand holding the menu (MenuGrab), -1 = none
    int screenHand = -1;     // the hand holding the flat screen (ScreenControl), -1 = none
  };

  // Each frame a list is open. True if the stick scrolls it, by `rows`.
  bool update(const ControllerState& input, const Frame& frame, double& rows);
  // No list: the speed-up is lost.
  void reset();

 private:
  double pushSeconds_ = 0;  // a list scrolled with the stick pushed well over, for its speed-up
  float pushSign_ = 0;      // ... and which way
};
