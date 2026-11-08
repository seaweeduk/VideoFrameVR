#pragma once

#include "xmath.h"

// Head translation emulation (on by default):
// surround video is treated as a sphere `distance` away, so moving the head
// shows the parallax of things at that distance. It can follow stereo focus
// instead. The offset is measured from the head position at the last recenter.

// The head's offset from the recenter position divided by the motion
// distance (local space), as stereoCorrection() and motionCorrection() take
// it; zero when off. A positive focusDistance (meters) overrides distanceCm,
// without rounding or the fixed distance's limits. Infinity means no parallax.
Vec3 emulatedMotion(bool enabled, int distanceCm, Vec3 head, Vec3 recenterPosition,
                    float focusDistance = 0);

// The motion distances the menu offers, in cm (5 to 1000 cm).
int nextMotionDistance(int cm, int direction);
int clampMotionDistance(int cm);
