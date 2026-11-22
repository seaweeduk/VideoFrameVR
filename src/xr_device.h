#pragma once

#include "gpu_context.h"

#include <openxr/openxr.h>

// Makes the Vulkan instance and device through the OpenXR runtime
// (XR_KHR_vulkan_enable2) into `out`, with what `gpu` asks for that the
// device offers, and fills in its capabilities. `foveationExtensions`: the
// runtime's foveation extensions are enabled. False, with the reason logged,
// where it falls short; what was made by then is in `out` for its owner to
// destroy.
bool createVulkanDevice(XrInstance instance, XrSystemId systemId, const GpuRequirements& gpu,
                        bool foveationExtensions, GpuDevice& out);
