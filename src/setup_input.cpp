#include "setup_input.h"

#include "controller_guide.h"
#include "controls.h"
#include "menu.h"
#include "setup_intro.h"
#include "setup_placement.h"

#include <cmath>

void SetupInput::takeIntroPresses(const ControllerState& raw, double now) {
  introPressed_ = false;
  if (menu.setupRunning() && intro.playing(now))
    for (int h = 0; h < 2; ++h)
      for (int c = 0; c < kControls; ++c)
        if (raw.hands[h].pressed & (1u << c)) {
          controls.consume(h, static_cast<Control>(c));
          introPressed_ = true;
        }
}

bool SetupInput::update(const Frame& frame, MenuPanels& panel) {
  const double now = frame.now;
  const JobInput& jobs = *frame.jobs;
  const ControllerState& input = jobs.state;
  // Its panel takes B, Tab and recenter (setup_placement.h).
  SetupPlacement::Frame s;
  s.now = now;
  s.headValid = frame.headValid;
  s.headPos = frame.headPos;
  s.headRot = frame.headRot;
  s.presence = frame.presence;
  s.bring = frame.bring;
  s.pressed = s.bring;
  for (const uint8_t hands : jobs.pressed) s.pressed |= hands != 0;
  for (const HandState& h : input.hands) {
    s.pressed |= h.selectPressed || h.gripPressed;
    if (h.poseValid) controllerSeen_ = now;
  }
  // Welcome's intro plays from when the panel is placed; a press skips
  // it, and does nothing else (consumed in takeIntroPresses()).
  const bool skipped = intro.playing(now) && (s.pressed || introPressed_);
  if (skipped) {
    intro.skip(now);
    s.pressed = s.bring = false;
  }
  const bool waited = placement.waiting();
  placement.update(s, panel);
  const bool opened = (waited && !placement.waiting()) || skipped;
  if (!placement.waiting() && menu.setupIntroDue()) {
    intro.start(now);
    menu.startSetupIntro();
  }
  if (intro.started() && !intro.playing(now) && !introDropped_) {
    menu.dropSetupIntro();
    introDropped_ = true;
  }
  menu.setSetupController(now - controllerSeen_ < 1.0);
  // Your controllers: the bindings when they changed, and what is held.
  if (frame.controlsVersion != bindingsSeen_) {
    bindingsSeen_ = frame.controlsVersion;
    menu.setSetupControls(*frame.bindings);
  }
  uint32_t held = 0;
  for (int h = 0; h < 2; ++h) {
    const HandState& g = input.hands[h];
    const bool sideways = std::abs(g.stickX) > 0.5f && std::abs(g.stickX) >= std::abs(g.stickY);
    const bool upDown = std::abs(g.stickY) > 0.5f && std::abs(g.stickY) > std::abs(g.stickX);
    const bool on[] = {g.select, g.grip, jobs.isHeld(Job::Menu, h),
                       jobs.isHeld(Job::PlayPause, h), jobs.isHeld(Job::Recenter, h),
                       sideways, upDown};
    for (int c = 0; c <= kStickUpDown; ++c)
      if (on[c]) held |= heldBit(h, c);
  }
  menu.setSetupHeld(held);
  return opened;
}
