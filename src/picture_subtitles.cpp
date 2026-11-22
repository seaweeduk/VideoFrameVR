#include "picture_subtitles.h"

#include "curved_panel.h"
#include "flat_screen.h"

#include <cairo.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

// The texture's size at first; a larger picture makes a larger one.
constexpr int kFirstWidth = 2048, kFirstHeight = 512;

int roundUp(int n, int step) { return (n + step - 1) / step * step; }

}  // namespace

void PictureSubtitles::update(const SubtitlePicture& picture) {
  const bool lost = canvas_ && canvas_->takeLost();
  if (picture.version == shown_ && !lost) return;
  shown_ = picture.version;
  if (picture.parts.empty()) {
    if (!parts_.empty()) ++version_;
    parts_.clear();
    return;
  }
  if (!upload(picture)) {
    parts_.clear();
    ++version_;
    return;
  }
  parts_ = picture.parts;
  canvasWidth_ = picture.canvasWidth;
  canvasHeight_ = picture.canvasHeight;
  ++version_;
}

bool PictureSubtitles::upload(const SubtitlePicture& picture) {
  if (picture.width > kSubtitlePictureMax || picture.height > kSubtitlePictureMax || picture.canvasWidth <= 0 ||
      picture.canvasHeight <= 0)
    return false;
  if (!canvas_ || picture.width > canvas_->width() || picture.height > canvas_->height()) {
    const int width = std::max(canvas_ ? canvas_->width() : kFirstWidth, roundUp(picture.width, 256));
    const int height = std::max(canvas_ ? canvas_->height() : kFirstHeight, roundUp(picture.height, 256));
    canvas_ = std::make_unique<CairoCanvas>();
    uploadedWidth_ = uploadedHeight_ = 0;
    if (!canvas_->init(uploader_, std::min(width, kSubtitlePictureMax),
                       std::min(height, kSubtitlePictureMax))) {
      std::fprintf(stderr, "Subtitles: no texture for picture subtitles\n");
      canvas_.reset();
      return false;
    }
  }
  // The new picture, and transparent where the last one was beyond it.
  cairo_surface_t* surface = canvas_->surface();
  cairo_surface_flush(surface);
  unsigned char* data = cairo_image_surface_get_data(surface);
  const size_t stride = static_cast<size_t>(cairo_image_surface_get_stride(surface));
  const int width = std::max(uploadedWidth_, picture.width);
  const int height = std::max(uploadedHeight_, picture.height);
  for (int y = 0; y < height; ++y) {
    unsigned char* row = data + y * stride;
    if (y < picture.height) {
      std::memcpy(row, &picture.pixels[static_cast<size_t>(y) * picture.width],
                  static_cast<size_t>(picture.width) * 4);
      std::memset(row + picture.width * 4, 0, static_cast<size_t>(width - picture.width) * 4);
    } else {
      std::memset(row, 0, static_cast<size_t>(width) * 4);
    }
  }
  cairo_surface_mark_dirty(surface);
  canvas_->upload(0, height, 0, width);
  uploadedWidth_ = picture.width;
  uploadedHeight_ = picture.height;
  return true;
}

bool PictureSubtitles::shown(const SubtitlePicture::Part& p, Shown& out) const {
  // mpv may put a part partly off the canvas (a larger subtitle scale): what
  // is on it, and the texture's columns and rows for that.
  const float cw = static_cast<float>(canvasWidth_), ch = static_cast<float>(canvasHeight_);
  const float x0 = std::max(0.0f, static_cast<float>(p.x)), x1 = std::min(cw, static_cast<float>(p.x + p.w));
  const float y0 = std::max(0.0f, static_cast<float>(p.y)), y1 = std::min(ch, static_cast<float>(p.y + p.h));
  if (x1 <= x0 || y1 <= y0) return false;
  const float tw = static_cast<float>(canvas_->width()), th = static_cast<float>(canvas_->height());
  out.x0 = x0 / cw;
  out.x1 = x1 / cw;
  out.y0 = y0 / ch;
  out.y1 = y1 / ch;
  out.u0 = (x0 - p.x) / p.w * p.width / tw;
  out.u1 = (x1 - p.x) / p.w * p.width / tw;
  out.v0 = (p.top + (y0 - p.y) / p.h * p.height) / th;
  out.v1 = (p.top + (y1 - p.y) / p.h * p.height) / th;
  return true;
}

void PictureSubtitles::addTo(std::vector<OverlayDraw>& items, const ScreenPlacement& placement,
                             float eyeAspect, float opacity) const {
  if (empty() || opacity <= 0) return;
  const ScreenSurface screen(placement, eyeAspect);
  const float w = screen.width(), h = screen.height(), r = screen.radius();
  for (const SubtitlePicture::Part& p : parts_) {
    Shown s;
    if (!shown(p, s)) continue;
    // On the screen, metres from its centre (x along the bend).
    const float x0 = (s.x0 - 0.5f) * w, x1 = (s.x1 - 0.5f) * w;
    const float top = (0.5f - s.y0) * h, bottom = (0.5f - s.y1) * h;
    PanelItem item;
    item.texture = canvas_->texture();
    item.opacity = opacity;
    if (r > 0) {
      // The cylinder of the screen's bend: its axis upright, r behind the
      // screen's centre. Its left..right and top..bottom are the texture's
      // whole width and height; the part shown is u0..u1, v0..v1.
      const float across = (x1 - x0) / (s.u1 - s.u0), down = (top - bottom) / (s.v1 - s.v0);
      CurvedPanel c;
      c.axis = transformPoint(placement.pose(), {0.0f, 0.0f, r - screen.distance()});
      c.orientation = placement.rotation();
      c.radius = r;
      c.left = x0 - s.u0 * across;
      c.right = c.left + across;
      c.top = top + s.v0 * down;
      c.bottom = c.top - down;
      c.u0 = s.u0;
      c.u1 = s.u1;
      c.v0 = s.v0;
      c.v1 = s.v1;
      item.curved = c;
    } else {
      item.model = placement.pose() *
                   translation({(x0 + x1) / 2, (top + bottom) / 2, -screen.distance()}) *
                   scaling(x1 - x0, top - bottom, 1);
      item.uv[0] = s.u0;
      item.uv[1] = s.u1;
      item.uv[2] = s.v0;
      item.uv[3] = s.v1;
    }
    items.push_back(item);
  }
}

void PictureSubtitles::addTo(std::vector<OverlayDraw>& items, const Mat4& canvas) const {
  if (empty()) return;
  for (const SubtitlePicture::Part& p : parts_) {
    Shown s;
    if (!shown(p, s)) continue;
    PanelItem item;
    item.texture = canvas_->texture();
    item.model = canvas *
                 translation({(s.x0 + s.x1) / 2 - 0.5f, 0.5f - (s.y0 + s.y1) / 2, 0.0f}) *
                 scaling(s.x1 - s.x0, s.y1 - s.y0, 1);
    item.uv[0] = s.u0;
    item.uv[1] = s.u1;
    item.uv[2] = s.v0;
    item.uv[3] = s.v1;
    items.push_back(item);
  }
}
