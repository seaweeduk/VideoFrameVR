#include "motion.h"

#include "value_ranges.h"

#include <algorithm>

Vec3 emulatedMotion(bool enabled, int distanceCm, Vec3 head, Vec3 recenterPosition) {
  if (!enabled) return {};
  return (head - recenterPosition) * (100.0f / clampMotionDistance(distanceCm));
}

int nextMotionDistance(int cm, int direction) {
  return clampMotionDistance(cm + direction);
}

int clampMotionDistance(int cm) {
  return std::clamp(cm, value_range::kMotionDistance.least, value_range::kMotionDistance.most);
}
