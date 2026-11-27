#pragma once

#include "controller_state.h"
#include "xmath.h"

#include <cstdint>

class ControlInput;
class Menu;
class SetupIntro;
class SetupPlacement;
struct ControllerBindings;
struct JobInput;
struct MenuPanels;

// First-run setup's panel and the controllers: placing it (SetupPlacement),
// Welcome's intro and a press skipping it (SetupIntro), whether a
// controller is seen, and Your controllers' bindings and the buttons held.
// Main thread only.
class SetupInput {
 public:
  SetupInput(Menu& menu, SetupPlacement& placement, SetupIntro& intro, ControlInput& controls)
      : menu(menu), placement(placement), intro(intro), controls(controls) {}

  // Before the buttons' jobs see them: while the intro plays, any press
  // skips it and does nothing else.
  void takeIntroPresses(const ControllerState& raw, double now);

  // What else is going on this frame.
  struct Frame {
    double now = 0;
    bool headValid = false;
    Vec3 headPos;
    Quat headRot;
    int presence = -1;   // XrContext::userPresence()
    bool bring = false;  // the menu button, recenter, Tab or R: the panel in front of them
    const JobInput* jobs = nullptr;
    uint64_t controlsVersion = 0;  // of the bindings below
    const ControllerBindings* bindings = nullptr;
  };
  // Each frame while setup runs. True if the panel opened now, or a press
  // skipped the intro: that press doesn't click.
  bool update(const Frame& frame, MenuPanels& panel);

 private:
  Menu& menu;
  SetupPlacement& placement;
  SetupIntro& intro;
  ControlInput& controls;
  bool introPressed_ = false;  // this frame's press, taken for the intro
  bool introDropped_ = false;  // its layers, once it has played
  double controllerSeen_ = -10;  // glfwGetTime() a controller was last tracked
  uint64_t bindingsSeen_ = 0;    // controlsVersion setup has
};
