#include "motion.h"

#include <algorithm>

namespace {

constexpr int kMinDistanceCm = 5, kMaxDistanceCm = 1000;

}  // namespace

Vec3 emulatedMotion(bool enabled, int distanceCm, Vec3 head, Vec3 recenterPosition) {
  if (!enabled) return {};
  return (head - recenterPosition) * (100.0f / clampMotionDistance(distanceCm));
}

int nextMotionDistance(int cm, int direction) {
  return clampMotionDistance(cm + direction);
}

int clampMotionDistance(int cm) { return std::clamp(cm, kMinDistanceCm, kMaxDistanceCm); }
