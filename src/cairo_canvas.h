#pragma once

#include "gpu_context.h"
#include "texture.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <deque>
#include <vector>

class Uploader;
typedef struct _cairo cairo_t;
typedef struct _cairo_surface cairo_surface_t;

// A cairo drawing surface with a matching Vulkan image (B8G8R8A8_SRGB:
// cairo's byte order, premultiplied alpha, a few mip levels) that it is
// uploaded to. Uploads copy their pixels at once and wait, in order with
// the shifts, for the next frame to record them before its eye passes; the
// mip levels are then rebuilt once (docs/vulkan-renderer.md, 3.6).
class CairoCanvas {
 public:
  CairoCanvas() = default;
  CairoCanvas(const CairoCanvas&) = delete;
  CairoCanvas& operator=(const CairoCanvas&) = delete;
  // Retires the images through GpuContext.
  ~CairoCanvas();

  // Drawn in width × height units; the surface and the image have `scale`
  // pixels per unit (sharper text for the same drawing). scrolls: shiftUp()
  // is used, which needs a spare image of the same size.
  bool init(Uploader& uploader, int width, int height, float scale = 1.0f, bool scrolls = false);
  // The surface alone, without an image: uploads and shifts are dropped and
  // texture() has nothing (drawing and measuring without a GPU).
  bool init(int width, int height, float scale = 1.0f);

  cairo_t* cr() const { return cr_; }
  cairo_surface_t* surface() const { return surface_; }
  int width() const { return width_; }    // in units
  int height() const { return height_; }

  // Makes the surface transparent (only rows [firstRow, endRow) if given).
  void clear(int firstRow = 0, int endRow = -1);
  // Copies the surface (or its rows [firstRow, endRow) and columns
  // [firstColumn, endColumn), in units) to the image, as of now: later
  // drawing does not change what this upload shows.
  void upload(int firstRow = 0, int endRow = -1, int firstColumn = 0, int endColumn = -1);
  // A complete replacement, uploaded in pieces to the scrolling spare. The
  // previous texture stays visible until finishUpdate(), before frame recording.
  // False while uploads to the visible image still await frame recording.
  bool beginUpdate();
  void finishUpdate();
  // Moves the image's pixels within x0..x1, y0..y1 (units) up by dy units
  // (down if negative), on the GPU, after the uploads before it: what leaves
  // the rectangle is dropped, what it uncovers keeps its old pixels until
  // drawn and uploaded. The surface is not changed. dy should be whole
  // pixels.
  void shiftUp(int x0, int y0, int x1, int y1, double dy);
  // What the scene samples; nullptr until something has been uploaded.
  const Texture* texture() const { return uploaded_ ? &texture_ : nullptr; }
  // True once if the waiting operations grew too big (no frames were drawn
  // for long) and were dropped: the owner draws and uploads all of it again.
  bool takeLost();

  // For Uploader: the staging bytes the waiting operations need, and
  // recording them.
  VkDeviceSize pendingBytes(VkDeviceSize alignment) const;
  void record(VkCommandBuffer cmd, Uploader& uploader);

 private:
  // An upload (its pixels, rows of x1 - x0) or a shift by dy, in pixels.
  struct Op {
    bool shift = false;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, dy = 0;
    std::vector<uint8_t> pixels;
  };
  void push(Op op);

  Uploader* uploader_ = nullptr;
  GpuImage image_, spare_;
  Texture texture_;
  bool uploaded_ = false;
  bool cleared_ = false;  // the image's first frame cleared it
  bool updating_ = false;
  bool transfer_ = false;  // unfinished replacements stay in TRANSFER_DST
  bool lost_ = false;
  std::deque<Op> ops_;
  VkDeviceSize pendingPixelBytes_ = 0;
  cairo_surface_t* surface_ = nullptr;
  cairo_t* cr_ = nullptr;
  int width_ = 0, height_ = 0;
  float scale_ = 1.0f;
  int pixelWidth_ = 0, pixelHeight_ = 0;
};
