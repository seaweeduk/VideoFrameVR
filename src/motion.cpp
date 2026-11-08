#include "motion.h"

#include <algorithm>
#include <iterator>

namespace {

constexpr int kSteps[] = {5, 10, 25, 50, 75, 100, 150, 200, 300, 500, 750, 1000};

}  // namespace

Vec3 emulatedMotion(bool enabled, int distanceCm, Vec3 head, Vec3 recenterPosition,
                    float focusDistance) {
  if (!enabled) return {};
  if (focusDistance > 0) return (head - recenterPosition) * (1.0f / focusDistance);
  return (head - recenterPosition) * (100.0f / clampMotionDistance(distanceCm));
}

int nextMotionDistance(int cm, int direction) {
  if (direction > 0) {
    for (int s : kSteps)
      if (s > cm) return s;
    return kSteps[std::size(kSteps) - 1];
  }
  for (auto it = std::rbegin(kSteps); it != std::rend(kSteps); ++it)
    if (*it < cm) return *it;
  return kSteps[0];
}

int clampMotionDistance(int cm) { return std::clamp(cm, kSteps[0], kSteps[std::size(kSteps) - 1]); }
