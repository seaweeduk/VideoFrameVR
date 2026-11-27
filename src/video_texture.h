#pragma once

#include "gl_interop.h"

#include <memory>

// The video's two shared images (docs/vulkan-renderer.md, 3.2): mpv renders
// a new frame into the back one, which then becomes the one shown, so a new
// frame never overwrites the image a frame in flight samples. Both have the
// video's size and a full mip chain (automatic focus reads small levels).
class VideoTexture {
 public:
  static constexpr int kSlots = 2;

  VideoTexture() = default;
  VideoTexture(const VideoTexture&) = delete;
  VideoTexture& operator=(const VideoTexture&) = delete;
  ~VideoTexture() { release(); }

  void init(GlInterop& interop) { interop_ = &interop; }
  enum class Resize { Ready, Failed, OutOfDeviceMemory };
  // New slots of this size, the old ones retired. No slots on failure.
  Resize resize(int width, int height);
  void release();
  bool valid() const { return slots_[0] != nullptr; }
  int width() const { return width_; }
  int height() const { return height_; }
  int levels() const { return levels_; }

  // Where the next frame goes, acquired for GL.
  SharedImage& back();
  // Its framebuffer, not acquired: for a render call that draws nothing.
  GLuint backFramebuffer() const { return slots_[hasFrame_ ? 1 - current_ : current_]->framebuffer(); }
  // A frame was rendered into back(). swap(): it is the one shown from now
  // on. deferSwap(): only from the next advance() on, so the frames drawn
  // until then need not wait for its GPU work.
  void swap();
  void deferSwap();
  // A deferred frame becomes the one shown. True if there was one.
  bool advance();
  // The frame shown; nullptr before the first one.
  SharedImage* current() { return hasFrame_ ? slots_[current_].get() : nullptr; }
  // The latest frame rendered: the deferred one, else current().
  SharedImage* newest() { return pending_ >= 0 ? slots_[pending_].get() : current(); }
  // Whether an image's mipmaps match its picture.
  bool mipmapsValid(const SharedImage* image) const;
  void setMipmapsValid(const SharedImage* image);

 private:
  int slotOf(const SharedImage* image) const;

  GlInterop* interop_ = nullptr;
  std::unique_ptr<SharedImage> slots_[kSlots];
  bool mipsValid_[kSlots] = {};
  int current_ = 0;
  int pending_ = -1;  // the slot of a deferred frame
  bool hasFrame_ = false;
  int width_ = 0, height_ = 0, levels_ = 0;
};
