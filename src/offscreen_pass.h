#pragma once

#include "gpu_context.h"

#include <cstddef>
#include <cstdint>

// Offscreen passes outside the eye passes: one colour attachment, all of it
// drawn (loadOp DONT_CARE) and stored, from initialLayout to finalLayout.
// Its writes wait for earlier work in `waitFor`, and are made visible to
// `readAccess` in `readBy` after it.
bool createOffscreenPass(GpuContext& gpu, VkFormat format, VkImageLayout initialLayout,
                         VkImageLayout finalLayout, VkPipelineStageFlags waitFor,
                         VkPipelineStageFlags readBy, VkAccessFlags readAccess, VkRenderPass& pass,
                         const char* what);

// A framebuffer of pass over one view, width x height.
bool createOffscreenFramebuffer(GpuContext& gpu, VkRenderPass pass, VkImageView view, uint32_t width,
                                uint32_t height, VkFramebuffer& framebuffer, const char* what);

// A pipeline drawing fragment over all of pass's one colour attachment
// (fullscreen.vert) through writeMask; viewport and scissor dynamic.
VkPipeline fullscreenPipeline(GpuContext& gpu, VkPipelineLayout layout, VkRenderPass pass,
                              const uint32_t* fragment, size_t fragmentBytes,
                              VkPipelineCreateFlags flags, VkColorComponentFlags writeMask,
                              const char* what);
