#pragma once

#include "controller_state.h"
#include "xmath.h"

#include <functional>

struct MenuPanels;

// Holding the grip on the menu moves it with the hand; the trigger off it
// holds the flat screen (ScreenControl).
class MenuGrab {
 public:
  // What else is going on this frame.
  struct Frame {
    bool onMenu[2] = {false, false};  // each hand's pointer is on the menu
    bool screenHeld = false;          // the flat screen is held (ScreenControl)
    bool headValid = false;
    Vec3 headPos;
    Vec3 up;  // the room's up
  };

  // A pulse on a controller; set once, before the frames.
  std::function<void(int hand, float amplitude, double seconds)> buzz;

  // Each frame: takes, follows or lets go of the menu.
  void update(const ControllerState& input, const Frame& frame, MenuPanels& panel);

  int hand() const { return hand_; }  // -1 = not held

 private:
  int hand_ = -1;
  Mat4 startInverse_;  // inverse of the hand pose at the grab
  Vec3 startPoint_;    // the menu's front at the grab
};
