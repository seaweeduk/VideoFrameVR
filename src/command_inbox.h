#pragma once

#include "menu_command.h"

#include <cstddef>
#include <utility>
#include <vector>

// Commands from what acts between frames (the keyboard, the control
// socket), run in the order posted at one point of the frame. What acts
// inside a phase (the menu, the buttons, the gestures) runs its commands
// there instead. Main thread only.
class CommandInbox {
 public:
  void post(MenuCommand c) { queue_.push_back(std::move(c)); }
  // The next command; false once none is left. Its storage is kept.
  bool take(MenuCommand& c) {
    if (next_ == queue_.size()) {
      queue_.clear();
      next_ = 0;
      return false;
    }
    c = std::move(queue_[next_++]);
    return true;
  }

 private:
  std::vector<MenuCommand> queue_;
  size_t next_ = 0;
};
