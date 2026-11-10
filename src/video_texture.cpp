#include "video_texture.h"

#include <algorithm>
#include <cmath>

bool VideoTexture::resize(int width, int height) {
  release();
  levels_ = 1 + static_cast<int>(std::floor(std::log2(std::max(width, height))));
  for (auto& slot : slots_) {
    slot = interop_->create(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                            static_cast<uint32_t>(levels_), "the video");
    if (!slot) {
      release();
      return false;
    }
  }
  width_ = width;
  height_ = height;
  return true;
}

void VideoTexture::release() {
  for (auto& slot : slots_)
    if (slot) interop_->retire(std::move(slot));
  for (bool& valid : mipsValid_) valid = false;
  current_ = 0;
  pending_ = -1;
  hasFrame_ = false;
  width_ = height_ = levels_ = 0;
}

SharedImage& VideoTexture::back() {
  SharedImage& image = *slots_[hasFrame_ ? 1 - current_ : current_];
  interop_->acquireForWrite(image);
  return image;
}

void VideoTexture::swap() {
  if (hasFrame_) current_ = 1 - current_;
  hasFrame_ = true;
  pending_ = -1;
  mipsValid_[current_] = false;
}

void VideoTexture::deferSwap() {
  pending_ = hasFrame_ ? 1 - current_ : current_;
  mipsValid_[pending_] = false;
}

bool VideoTexture::advance() {
  if (pending_ < 0) return false;
  current_ = pending_;
  pending_ = -1;
  hasFrame_ = true;
  return true;
}

int VideoTexture::slotOf(const SharedImage* image) const {
  for (int i = 0; i < kSlots; ++i)
    if (image && slots_[i].get() == image) return i;
  return -1;
}

bool VideoTexture::mipmapsValid(const SharedImage* image) const {
  const int slot = slotOf(image);
  return slot >= 0 && mipsValid_[slot];
}

void VideoTexture::setMipmapsValid(const SharedImage* image) {
  const int slot = slotOf(image);
  if (slot >= 0) mipsValid_[slot] = true;
}
