#pragma once

#include "xmath.h"

#include <openxr/openxr.h>
#include <vulkan/vulkan.h>

#include <cstdio>

// Logs a failed OpenXR call with its result's name; true if it succeeded.
inline bool xrCheck(XrInstance instance, XrResult result, const char* what) {
  if (XR_SUCCEEDED(result)) return true;
  char name[XR_MAX_RESULT_STRING_SIZE] = "";
  if (instance != XR_NULL_HANDLE) xrResultToString(instance, result, name);
  std::fprintf(stderr, "OpenXR: %s failed: %s (%d)\n", what, name, static_cast<int>(result));
  return false;
}

// Returns false from the function it is in, through the `check` in scope.
#define XR_TRY(call) \
  if (!check((call), #call)) return false

inline Vec3 toVec3(const XrVector3f& v) { return {v.x, v.y, v.z}; }
inline Quat toQuat(const XrQuaternionf& q) { return {q.x, q.y, q.z, q.w}; }

// Logs a failed Vulkan call; true if it succeeded.
inline bool vkCheck(VkResult result, const char* what) {
  if (result == VK_SUCCESS) return true;
  std::fprintf(stderr, "Vulkan: %s failed (%d)\n", what, static_cast<int>(result));
  return false;
}

// Returns false from the function it is in.
#define VK_TRY(call) \
  if (!vkCheck((call), #call)) return false
