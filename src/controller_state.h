#pragma once

#include "xmath.h"

#include <cstdint>

struct HandState {
  // A controller is there for this hand: OpenXR has one of its buttons.
  bool present = false;
  bool poseValid = false;
  Vec3 aimPosition;      // pointing ray origin, local space
  Quat aimOrientation;   // the ray points along -Z of this orientation
  bool select = false;          // trigger held (ControlInput: or a button that chooses)
  bool selectPressed = false;   // ... went down this frame
  bool grip = false;            // grip button held
  bool gripPressed = false;     // grip went down this frame
  float stickX = 0, stickY = 0;
  // The buttons (controls.h's controlBit()s) held, and those that went
  // down this frame.
  uint32_t controls = 0;
  uint32_t pressed = 0;
};

struct ControllerState {
  // The app has input focus (not Steam's dashboard or other system UI).
  // Without it, or until every button and stick is let go after it comes
  // back, the hands press nothing; without it, they have no pose either.
  bool focused = false;
  // Each time it was lost: what was held before is never let go here, even
  // where no frame saw it lost (none drawn meanwhile).
  uint32_t focusLosses = 0;
  HandState hands[2];  // 0 = left, 1 = right
};
