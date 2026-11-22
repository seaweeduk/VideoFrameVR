#pragma once

#include "xmath.h"

struct EyeMatrices {
  Mat4 projection;
  Mat4 view;
  // The same view as raw orientation + field-of-view tangents, for shaders
  // that compute a view ray per pixel.
  Quat orientation;
  Vec3 position;
  float tanLeft = 0, tanRight = 0, tanUp = 0, tanDown = 0;
};

// Eye gaze from XR_EXT_eye_gaze_interaction (Steam Frame), updated by beginFrame().
struct GazeState {
  bool available = false;  // the runtime offers it and the system supports it
  bool active = false;     // the input is bound and delivering (permission, tracker on)
  bool valid = false;      // a pose this frame
  bool tracked = false;    // ... measured, not estimated
  Vec3 origin;             // local space
  Quat orientation;        // the gaze looks along -Z of this orientation
  bool ageKnown = false;
  double ageSeconds = 0;   // sample time before the frame's display time (negative: predicted)
  double sampleSeconds = 0;  // the sample time itself (XrTime in seconds)
};
