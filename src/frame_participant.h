#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

// Something whose images the scene samples while another side owns them:
// GL's shared images (GlInterop) and the decoder's frames (DirectVideo).
// Per frame, each is told what the frame samples (its own useInFrame()),
// then recordAcquires() before the eye passes and recordReleases() after
// them, its frameWaits() for the submission, and frameSubmitted() or
// frameAbandoned() after it. SceneRenderer makes the shared calls on each
// in turn, GlInterop first.
class FrameParticipant {
 public:
  virtual void recordAcquires(VkCommandBuffer cmd) = 0;
  virtual void recordReleases(VkCommandBuffer cmd) = 0;
  virtual void frameSubmitted(uint64_t value) = 0;
  virtual void frameAbandoned() = 0;

 protected:
  ~FrameParticipant() = default;
};
