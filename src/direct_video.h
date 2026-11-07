#pragma once

#include "gpu_context.h"
#include "texture.h"
#include "video_export.h"

#include <sys/types.h>

#include <cstdint>
#include <memory>
#include <vector>

struct DirectBuffer;
class DirectVideo;

// A decoded frame mpv handed out instead of drawing it (Frame,
// docs/vulkan-renderer.md 11): the converter's linear buffer or the
// decoder's own, sampled as two planes (a Q08C one as one image). texture()
// is its luma, with yuv pointing back here.
class DirectFrame {
 public:
  const Texture& texture() const { return luma_; }
  const Texture& chroma() const { return chroma_; }
  const YuvColor& color() const { return color_; }
  uint64_t id() const { return id_; }  // mpv's
  // A Q10C decoder buffer: sampled through its TP10 descriptors
  // (DirectVideo::bindTp10) by the pipelines for it, not as texture().
  bool tp10() const { return tp10_; }
  // A Q08C decoder buffer: texture() is the whole NV12 image, sampled through
  // the Y'CbCr conversion of chroma siting q08c() (DirectVideo::q08cSetLayouts)
  // by the pipelines for it; -1 for other frames.
  int q08c() const { return q08c_; }

 private:
  friend class DirectVideo;
  DirectBuffer* buffer_ = nullptr;
  uint64_t id_ = 0;
  int fenceFd_ = -1;  // the conversion's completion, until seen signalled
  int matrix_ = 0, fullRange_ = 0, siting_ = 0;
  Texture luma_, chroma_;
  YuvColor color_;
  bool tp10_ = false;
  int q08c_ = -1;
  bool held_ = false;     // the player's shown or next frame
  int pinned_ = 0;        // pin(): kept until unpinned
  bool inFrame_ = false;  // used by the frame being recorded
  uint64_t lastUse_ = 0;  // frame timeline value of the last submission sampling it
};

// A pipeline drawing fragment over all of pass's one colour attachment
// (fullscreen.vert) through writeMask; viewport and scissor dynamic.
VkPipeline fullscreenPipeline(GpuContext& gpu, VkPipelineLayout layout, VkRenderPass pass,
                              const uint32_t* fragment, size_t fragmentBytes,
                              VkPipelineCreateFlags flags, VkColorComponentFlags writeMask,
                              const char* what);

// The app's side of mpv's frame export: imports each converter buffer once
// into Vulkan, tracks which frames are still in use and tells mpv which it
// can have back. Its scene calls follow GlInterop's (gl_interop.h):
// useInFrame() while recording, recordAcquires() before the eye passes,
// recordReleases() after them, frameWaits() for the submission and
// frameSubmitted() or frameAbandoned() after it. Nothing is imported into
// GL: its imports could keep a decoder buffer after the frames were gone
// (DirectDepth). Main thread.
class DirectVideo {
 public:
  DirectVideo();
  DirectVideo(const DirectVideo&) = delete;
  DirectVideo& operator=(const DirectVideo&) = delete;
  ~DirectVideo();

  // After GpuContext::init. False (and logged) if the device cannot sample
  // decoder buffers; then mpv keeps drawing every frame.
  bool init(GpuContext& gpu);
  bool available() const { return gpu_ != nullptr; }

  // mpv's picture adjustments (-100..100, VideoPlayer::setPicture), which
  // mpv leaves to the app for the frames it hands out.
  void setPicture(int brightness, int contrast, int saturation);
  // TV gamma for BT.1886 video: the eye shaders decode with its 2.4 power,
  // as mpv converts it when it draws.
  void setTvGamma(bool tvGamma);
  // Q10C decoder buffers sampled as TP10 (GpuCapabilities::tp10): the set
  // layout of their descriptors (in a descriptor buffer; VK_NULL_HANDLE
  // without), for the pipelines for them; then enableTp10() once those
  // exist, with the sampler the descriptors take (bilinear, one level).
  VkDescriptorSetLayout tp10SetLayout() const { return tp10SetLayout_; }
  void enableTp10(VkSampler sampler);
  // Whether mpv can hand out Q10C frames as the decoder wrote them.
  bool acceptsQ10c() const { return tp10Sampler_ != VK_NULL_HANDLE; }
  // Q08C decoder buffers sampled as NV12 (GpuCapabilities::q08c): the push
  // set layouts of each chroma siting's Y'CbCr sampler (immutable in it;
  // nullptr unless all were made), for the pipelines for them; then
  // enableQ08c() once those exist.
  const VkDescriptorSetLayout* q08cSetLayouts() const { return q08cLayouts_ ? q08cSetLayout_ : nullptr; }
  void enableQ08c();
  // Whether mpv can hand out Q08C frames as the decoder wrote them.
  bool acceptsQ08c() const { return q08c_; }
  // Binds a tp10() frame's descriptors as set 0 of layout (Pipelines::tp10Layout).
  void bindTp10(VkCommandBuffer cmd, VkPipelineLayout layout, const DirectFrame& frame) const;
  // Work outside the frames sampling a frame (DirectDepth): it is kept from
  // pin() until unpin() with the submission's frame timeline value, which
  // takes its planes over and back as recordAcquires() and recordReleases(),
  // after useWait(): the last submission sampling it, which gave them back.
  void pin(DirectFrame& frame) { ++frame.pinned_; }
  SemaphoreWait useWait(const DirectFrame& frame) const {
    return {gpu_->frameTimeline(), frame.lastUse_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
  }
  void unpin(DirectFrame& frame, uint64_t lastUse);
  void recordBarriers(VkCommandBuffer cmd, DirectFrame& frame, bool acquire);
  // Whether this frame's colour is one the shaders reproduce as mpv would
  // draw it (BT.709 primaries, SDR, a known matrix).
  static bool supported(const video_export& e);
  // A frame mpv handed out (exported == 1), imported; nullptr if it cannot
  // be (it goes back to mpv with the next release()).
  DirectFrame* accept(video_export& e);
  // The player shows the frame or will next; others go back to mpv once
  // the GPU is done with them.
  void hold(DirectFrame* frame, bool held);
  // Fills e's release list with frames done with, and forgets them.
  void release(video_export& e);
  // Imports of buffers no frame uses are dropped (a new size, stop); release()
  // drops those of a closed decoder session too.
  void dropUnused();
  // Whether frame came from a decoder session that has since closed
  // (video_export::session is now session).
  bool closedSession(const DirectFrame& frame, uint32_t session) const;
  bool sessionReleased(uint32_t session) const;
  // Only between scene recordings: cancel a still not yet submitted. GPU
  // uses that were submitted retain their normal timeline protection.
  void cancelPendingStill();
  // The frame the player shows from a closed session (a seek's, until the
  // new session's first frame), copied into images of the app's own by the
  // next frame before its eye passes: shown instead, it lets the frame and
  // with it the old session's buffers go back at once, which the new
  // session may need the room of (Iris maps them in a 3.5 GiB window), and
  // the old session's decoder instance closes (an 8K Main 10 one's left open
  // makes the next one fail to start). A Q10C frame is drawn into 16-bit
  // planes (still.frag), a Q08C one's planes copied. nullptr if it cannot
  // be (a copy still waiting, no pipelines for Q10C ones): the frame stays.
  DirectFrame* still(DirectFrame& frame);
  // Frees the still's images (stop, another file), unless something samples it.
  void dropStill();
  // Whether frames not shown are still to go back to mpv (release()).
  bool returning() const;

  void useInFrame(const DirectFrame& frame);
  void recordAcquires(VkCommandBuffer cmd);
  void recordReleases(VkCommandBuffer cmd);
  bool frameWaits(std::vector<SemaphoreWait>& waits);
  void frameSubmitted(uint64_t value);
  void frameAbandoned();

 private:
  struct Semaphore {
    VkSemaphore semaphore = VK_NULL_HANDLE;
    uint64_t freeAfter = 0;  // the frame timeline value of its last wait
  };
  DirectBuffer* findBuffer(const video_export& e);
  bool importPlane(DirectBuffer& b, int plane, int fd);
  bool initTp10();
  bool initStill();
  void retireStillFramebuffers();
  bool importTp10(DirectBuffer& b, int fd);
  bool initQ08c();
  bool q08cSupported();
  bool importQ08c(DirectBuffer& b, int fd);
  void destroyBuffer(std::unique_ptr<DirectBuffer>& b);
  void computeColor(DirectFrame& f) const;
  static void placeChroma(DirectFrame& f);
  void recordStillCopy(VkCommandBuffer cmd);
  void recordStillDraw(VkCommandBuffer cmd);
  void barriers(VkCommandBuffer cmd, bool acquire);
  void addBarriers(std::vector<VkImageMemoryBarrier>& list, DirectFrame& frame, bool acquire);
  bool fenceDone(DirectFrame& f);
  VkSemaphore takeSemaphore();

  GpuContext* gpu_ = nullptr;
  PFN_vkGetMemoryFdPropertiesKHR getFdProperties_ = nullptr;
  PFN_vkImportSemaphoreFdKHR importSemaphoreFd_ = nullptr;
  bool semaphoreImport_ = false;  // sync_file payloads into binary semaphores
  uint32_t foreign_ = VK_QUEUE_FAMILY_EXTERNAL;

  // Q10C frames: their descriptors, a set's worth for each buffer (slot).
  VkDescriptorSetLayout tp10SetLayout_ = VK_NULL_HANDLE;
  VkSampler tp10Sampler_ = VK_NULL_HANDLE;
  VkBuffer tp10Buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory tp10Memory_ = VK_NULL_HANDLE;
  uint8_t* tp10Map_ = nullptr;
  VkDeviceAddress tp10Address_ = 0;
  VkDeviceSize tp10SetSize_ = 0, tp10Binding_[2] = {};
  size_t tp10DescriptorSize_ = 0;
  std::vector<bool> tp10Slots_;
  PFN_vkGetDescriptorEXT getDescriptor_ = nullptr;
  PFN_vkCmdBindDescriptorBuffersEXT bindDescriptorBuffers_ = nullptr;
  PFN_vkCmdSetDescriptorBufferOffsetsEXT setDescriptorBufferOffsets_ = nullptr;
  // Q08C frames: a Y'CbCr conversion for each chroma siting, with its sampler.
  VkSamplerYcbcrConversion q08cConversion_[kQ08cSitings] = {};
  VkSampler q08cSampler_[kQ08cSitings] = {};
  VkDescriptorSetLayout q08cSetLayout_[kQ08cSitings] = {};
  VkExtent3D q08cMaxExtent_{};  // the largest image importQ08c() can make
  bool q08cLayouts_ = false;    // initQ08c() made them all
  bool q08c_ = false;           // enableQ08c()

  int brightness_ = 0, contrast_ = 0, saturation_ = 0;
  bool tvGamma_ = false;
  std::vector<std::unique_ptr<DirectBuffer>> buffers_;
  std::vector<std::unique_ptr<DirectFrame>> frames_;
  std::vector<uint64_t> rejected_;  // ids handed out but not imported
  std::vector<DirectFrame*> inFrame_;
  std::vector<Semaphore> semaphores_;   // idle once the GPU passed freeAfter
  std::vector<size_t> frameSemaphores_;  // waited on by the frame being recorded
  // still(): its planes, the frame shown from them (never mpv's), and the
  // frame they are copied from, pinned until the copy is submitted.
  GpuImage stillPlanes_[2];
  std::unique_ptr<DirectFrame> still_;
  DirectFrame* stillSource_ = nullptr;
  // A Q10C frame's still: a pass, pipeline and framebuffer for each plane.
  VkRenderPass stillPass_[2] = {};
  VkPipelineLayout stillLayout_ = VK_NULL_HANDLE;
  VkPipeline stillPipeline_[2] = {};
  VkFramebuffer stillFramebuffer_[2] = {};
};
