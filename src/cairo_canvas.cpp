#include "cairo_canvas.h"

#include "uploader.h"

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// Waiting operations beyond this many full canvases are dropped (takeLost()).
constexpr VkDeviceSize kMaxPendingCanvases = 3;

void barrier(VkCommandBuffer cmd, VkImage image, uint32_t levels, VkImageLayout from,
             VkImageLayout to, VkAccessFlags fromAccess, VkAccessFlags toAccess,
             VkPipelineStageFlags fromStage) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = fromAccess;
  b.dstAccessMask = toAccess;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
  vkCmdPipelineBarrier(cmd, fromStage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                       1, &b);
}

bool overlaps(const VkRect2D& a, int x0, int y0, int x1, int y1) {
  return a.offset.x < x1 && x0 < a.offset.x + static_cast<int>(a.extent.width) &&
         a.offset.y < y1 && y0 < a.offset.y + static_cast<int>(a.extent.height);
}

}  // namespace

CairoCanvas::~CairoCanvas() {
  if (cr_) cairo_destroy(cr_);
  if (surface_) cairo_surface_destroy(surface_);
  if (!uploader_) return;
  uploader_->forget(this);
  uploader_->gpu().retire(image_);
  if (spare_.image) uploader_->gpu().retire(spare_);
}

bool CairoCanvas::init(int width, int height, float scale) {
  width_ = width;
  height_ = height;
  scale_ = scale;
  pixelWidth_ = static_cast<int>(std::lround(width * scale));
  pixelHeight_ = static_cast<int>(std::lround(height * scale));
  surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixelWidth_, pixelHeight_);
  if (cairo_surface_status(surface_) != CAIRO_STATUS_SUCCESS) return false;
  cairo_surface_set_device_scale(surface_, scale, scale);
  cr_ = cairo_create(surface_);
  return true;
}

bool CairoCanvas::init(Uploader& uploader, int width, int height, float scale, bool scrolls) {
  uploader_ = &uploader;
  if (!init(width, height, scale)) return false;

  // Down to an eighth of the size at scale 1, a sixteenth at 2.
  const uint32_t full =
      1 + static_cast<uint32_t>(std::floor(std::log2(std::max(pixelWidth_, pixelHeight_))));
  const uint32_t levels = std::min(
      full, 4 + static_cast<uint32_t>(std::ceil(std::log2(std::max(1.0f, scale)))));
  GpuContext& gpu = uploader.gpu();
  const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (!gpu.createImage(pixelWidth_, pixelHeight_, levels, VK_FORMAT_B8G8R8A8_SRGB, usage, image_,
                       "canvas"))
    return false;
  if (scrolls && !gpu.createImage(pixelWidth_, pixelHeight_, levels, VK_FORMAT_B8G8R8A8_SRGB, usage,
                                  spare_, "canvas scroll spare"))
    return false;
  texture_.image = image_.image;
  texture_.view = image_.view;
  texture_.width = image_.width;
  texture_.height = image_.height;
  texture_.levels = image_.levels;
  return true;
}

void CairoCanvas::clear(int firstRow, int endRow) {
  if (endRow < 0) endRow = height_;
  cairo_save(cr_);
  cairo_rectangle(cr_, 0, firstRow, width_, endRow - firstRow);
  cairo_clip(cr_);
  cairo_set_operator(cr_, CAIRO_OPERATOR_CLEAR);
  cairo_paint(cr_);
  cairo_restore(cr_);
}

void CairoCanvas::upload(int firstRow, int endRow, int firstColumn, int endColumn) {
  if (!uploader_) return;
  if (endRow < 0) endRow = height_;
  if (endColumn < 0) endColumn = width_;
  // In the surface's pixels.
  Op op;
  op.y0 = std::max(0, static_cast<int>(std::floor(firstRow * scale_)));
  op.y1 = std::min(pixelHeight_, static_cast<int>(std::ceil(endRow * scale_)));
  op.x0 = std::max(0, static_cast<int>(std::floor(firstColumn * scale_)));
  op.x1 = std::min(pixelWidth_, static_cast<int>(std::ceil(endColumn * scale_)));
  if (op.y1 <= op.y0 || op.x1 <= op.x0) return;
  cairo_surface_flush(surface_);
  const int stride = cairo_image_surface_get_stride(surface_);
  const size_t rowBytes = static_cast<size_t>(op.x1 - op.x0) * 4;
  // Copied in without zeroing the bytes first: a scrolled strip can be
  // megabytes in every frame.
  const unsigned char* src = cairo_image_surface_get_data(surface_) +
                             static_cast<size_t>(op.y0) * stride + static_cast<size_t>(op.x0) * 4;
  op.pixels.reserve(static_cast<size_t>(op.y1 - op.y0) * rowBytes);
  if (rowBytes == static_cast<size_t>(stride)) {
    op.pixels.assign(src, src + static_cast<size_t>(op.y1 - op.y0) * rowBytes);
  } else {
    for (int y = op.y0; y < op.y1; ++y, src += stride)
      op.pixels.insert(op.pixels.end(), src, src + rowBytes);
  }
  if (!updating_) uploaded_ = true;
  push(std::move(op));
}

bool CairoCanvas::beginUpdate() {
  if (!updating_ && !ops_.empty()) return false;
  if (!updating_) std::swap(image_, spare_);
  updating_ = true;
  cleared_ = false;
  ops_.clear();
  pendingPixelBytes_ = 0;
  return true;
}

void CairoCanvas::finishUpdate() {
  texture_.image = image_.image;
  texture_.view = image_.view;
  updating_ = false;
  if (uploader_) uploaded_ = true;
}

void CairoCanvas::shiftUp(int x0, int y0, int x1, int y1, double dy) {
  if (!uploader_) return;
  if (!spare_.image) {
    std::fprintf(stderr, "CairoCanvas: shiftUp() on a canvas made without scrolling\n");
    return;
  }
  // In the image's pixels.
  Op op;
  op.shift = true;
  op.x0 = std::max(0, static_cast<int>(std::lround(x0 * scale_)));
  op.x1 = std::min(pixelWidth_, static_cast<int>(std::lround(x1 * scale_)));
  op.y0 = std::max(0, static_cast<int>(std::lround(y0 * scale_)));
  op.y1 = std::min(pixelHeight_, static_cast<int>(std::lround(y1 * scale_)));
  op.dy = static_cast<int>(std::lround(dy * scale_));
  if (op.x1 <= op.x0 || op.y1 - op.y0 - std::abs(op.dy) <= 0 || op.dy == 0) return;
  push(std::move(op));
}

void CairoCanvas::push(Op op) {
  if (!op.shift) {
    // What a new upload covers needs no earlier upload since the last shift;
    // a whole-canvas upload needs nothing before it.
    if (op.x0 == 0 && op.y0 == 0 && op.x1 == pixelWidth_ && op.y1 == pixelHeight_) {
      ops_.clear();
      pendingPixelBytes_ = 0;
    }
    for (size_t i = ops_.size(); i > 0 && !ops_[i - 1].shift;) {
      --i;
      const Op& old = ops_[i];
      if (old.x0 >= op.x0 && old.y0 >= op.y0 && old.x1 <= op.x1 && old.y1 <= op.y1) {
        pendingPixelBytes_ -= old.pixels.size();
        ops_.erase(ops_.begin() + static_cast<std::ptrdiff_t>(i));
      }
    }
    pendingPixelBytes_ += op.pixels.size();
  }
  ops_.push_back(std::move(op));
  if (pendingPixelBytes_ > kMaxPendingCanvases * static_cast<VkDeviceSize>(pixelWidth_) *
                               pixelHeight_ * 4) {
    ops_.clear();
    pendingPixelBytes_ = 0;
    lost_ = true;
    return;
  }
  uploader_->queue(this);
}

bool CairoCanvas::takeLost() {
  const bool lost = lost_;
  lost_ = false;
  return lost;
}

VkDeviceSize CairoCanvas::pendingBytes(VkDeviceSize alignment) const {
  VkDeviceSize bytes = 0;
  for (const Op& op : ops_)
    if (!op.shift) bytes += (op.pixels.size() + alignment - 1) / alignment * alignment;
  return bytes;
}

// Level 0 takes the operations in order, as a transfer destination, with a
// barrier wherever one touches what an earlier one wrote; then the mip
// levels are made from it again (recordMipmaps), which leaves the image
// ready for sampling.
void CairoCanvas::record(VkCommandBuffer cmd, Uploader& uploader) {
  if (ops_.empty()) return;
  const VkImage image = image_.image;
  barrier(cmd, image, 1,
          !cleared_ ? VK_IMAGE_LAYOUT_UNDEFINED
                    : transfer_ ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, transfer_ ? VK_ACCESS_TRANSFER_WRITE_BIT : 0,
          VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT);
  std::vector<VkRect2D> written;  // since the last barrier
  auto writeBarrier = [&] {
    barrier(cmd, image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    written.clear();
  };
  if (!cleared_) {
    // A new canvas starts transparent wherever nothing is uploaded.
    const VkClearColorValue clear{{0.0f, 0.0f, 0.0f, 0.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    written.push_back({{0, 0}, {image_.width, image_.height}});
    cleared_ = true;
  }

  for (Op& op : ops_) {
    const uint32_t w = static_cast<uint32_t>(op.x1 - op.x0);
    if (!op.shift) {
      Uploader::Space space;
      if (!uploader.reserve(op.pixels.size(), space)) continue;
      std::memcpy(space.data, op.pixels.data(), op.pixels.size());
      for (const VkRect2D& r : written)
        if (overlaps(r, op.x0, op.y0, op.x1, op.y1)) {
          writeBarrier();
          break;
        }
      VkBufferImageCopy copy{};
      copy.bufferOffset = space.offset;
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageOffset = {op.x0, op.y0, 0};
      copy.imageExtent = {w, static_cast<uint32_t>(op.y1 - op.y0), 1};
      vkCmdCopyBufferToImage(cmd, space.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                             &copy);
      written.push_back({{op.x0, op.y0}, {copy.imageExtent.width, copy.imageExtent.height}});
      continue;
    }
    // A copy may not overlap itself: the part that stays goes through the spare.
    const uint32_t h = static_cast<uint32_t>(op.y1 - op.y0 - std::abs(op.dy));
    const int from = op.dy > 0 ? op.y0 + op.dy : op.y0, to = op.dy > 0 ? op.y0 : op.y0 - op.dy;
    barrier(cmd, image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    // The spare's earlier contents and reads (an earlier shift) are done with.
    barrier(cmd, spare_.image, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = copy.srcSubresource;
    copy.srcOffset = {op.x0, from, 0};
    copy.dstOffset = {0, 0, 0};
    copy.extent = {w, h, 1};
    vkCmdCopyImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, spare_.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(cmd, spare_.image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(cmd, image, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    copy.srcOffset = {0, 0, 0};
    copy.dstOffset = {op.x0, to, 0};
    vkCmdCopyImage(cmd, spare_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    written.assign(1, {{op.x0, to}, {w, h}});
  }
  ops_.clear();
  pendingPixelBytes_ = 0;

  if (updating_) {
    transfer_ = true;
    return;  // Mipmaps are needed only when the complete page becomes visible.
  }

  barrier(cmd, image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT);
  recordMipmaps(cmd, image, image_.width, image_.height, image_.levels,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  transfer_ = false;
}
