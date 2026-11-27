#pragma once

#include "controller_state.h"
#include "value_adjust.h"
#include "vr_input.h"

// A value row pointed (its field, − or +): either stick sideways steps it,
// as − and + do, and holding repeats. Until both sticks are let go the
// stick is the value's: it doesn't seek.
class ValueStick {
 public:
  // What else is going on this frame.
  struct Frame {
    double now = 0;
    bool menuOpen = false;
    bool typing = false;        // the keyboard is up: the sticks move its cursor
    bool seeking = false;       // a stick seek is going on (StickSeek::active())
    bool pointedValue = false;  // a value row is pointed at (Menu::pointedValue())
  };

  // Each frame: -1 or 1 to step the pointed value down or up, else 0.
  int update(const ControllerState& input, const Frame& frame);

  bool held() const { return held_; }  // the sticks are the value's

 private:
  HoldRepeat less_{adjust::kStickDelay, adjust::kStickRepeat};
  HoldRepeat more_{adjust::kStickDelay, adjust::kStickRepeat};
  bool held_ = false;
};
