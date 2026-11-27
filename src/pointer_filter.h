#pragma once

#include "xmath.h"

// Steadies where a hand points: a One Euro filter on the pointing direction,
// so tracking noise doesn't shake the dot on a panel 1.5 m away while a quick
// sweep still follows the hand. Cutoff (MenuFeel): pointMinCutoff at rest,
// plus pointBeta per rad/s of the direction's filtered speed, that speed
// itself filtered at pointSpeedCutoff. A minimum cutoff of 0 turns it off.
class PointerFilter {
 public:
  // The raw direction (unit) at `seconds`: the steadied one.
  Vec3 update(Vec3 direction, double seconds);
  // Tracking was lost: the next direction is taken as it is.
  void reset() { at_ = -1; }

 private:
  Vec3 steady_ = {0.0f, 0.0f, -1.0f};
  Vec3 speed_;      // filtered rate of change, per second
  double at_ = -1;  // time of the last direction, -1 none
};
