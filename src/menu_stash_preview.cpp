#include "menu_style.h"

#include "menu_stash_covers.h"
#include "stash.h"

namespace {

constexpr double kHoverDelay = 0.4;

}  // namespace

void Menu::stopPreview() {
  if (preview_.path.empty()) return;
  if (stash_) {
    if (stash_->preview().frame()) browserChanged(preview_.y, preview_.y + preview_.h);
    stash_->preview().cancel();
  }
  preview_ = {};
}

void Menu::updatePreview() {
  const double now = steadySeconds();
  if (!stash_ || hovered_ < 0 || hovered_ >= static_cast<int>(buttons_.size()) ||
      pointerPanel_ != kBrowser || dialog_ != Dialog::None || dropdown_ != Dropdown::None ||
      listDrag_.pressed || listDragging() || draggingScroll_ || ratingDrag_.active ||
      now - listMovedAt_ < 0.15) {
    stopPreview();
    return;
  }
  const Button& b = buttons_[hovered_];
  // Stars and other overlaid controls have their own hit targets and no
  // thumbPath. Text below/beside the picture is not a preview target either.
  if (b.thumbPath.empty() || (!b.card && !b.listRow)) {
    stopPreview();
    return;
  }
  double x, y, w, h;  // where its row or card draws the picture
  listPicture(b, x, y, w, h);
  if (pointerX_ < x || pointerX_ > x + w || pointerY_ < y || pointerY_ > y + h) {
    stopPreview();
    return;
  }
  if (preview_.path != b.thumbPath || preview_.x != x || preview_.y != y ||
      preview_.w != w || preview_.h != h) {
    stopPreview();
    preview_ = {b.thumbPath, x, y, w, h, now, false};
  }
  if (preview_.requested || now - preview_.since < kHoverDelay) return;
  preview_.requested = true;  // no repeated requests after a failure during this hover
  const StashScene* scene = stashCovers_->scene(preview_.path);
  if (!scene) return;
  VideoFormat format = guessVideoFormat(scene->fileName(), scene->width, scene->height);
  VideoSettings saved;
  if (library_ && library_->settings(preview_.path, saved) && saved.hasFormat) format = saved.format;
  stash_->preview().request(
      scene->id, scene->code, format, static_cast<int>(std::ceil(preview_.w * menu_geometry::kRasterScale)));
}

cairo_surface_t* Menu::previewFrame(const std::string& path, double x, double y) const {
  // A scene may occur in several Home shelves. Only this tile animates.
  return stash_ && preview_.path == path && preview_.x == x && preview_.y == y
             ? stash_->preview().frame()
             : nullptr;
}

void Menu::drawPreview() {
  const int x0 = static_cast<int>(std::floor(preview_.x));
  const int x1 = static_cast<int>(std::ceil(preview_.x + preview_.w));
  const int y0 = std::max(static_cast<int>(std::floor(preview_.y)),
                          static_cast<int>(std::ceil(listTop_ - 2)));
  const int y1 = std::min(static_cast<int>(std::ceil(preview_.y + preview_.h)),
                          static_cast<int>(std::floor(listBottom_)));
  if (y1 <= y0) return;
  CairoCanvas& canvas = canvases_[kBrowser];
  cairo_t* cr = canvas.cr();
  cairo_save(cr);
  cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
  cairo_clip(cr);
  // Replace the background under the rounded picture's antialiased edge,
  // then paint the tile and its existing badges/stars in their normal order.
  // No layout, headings, rail or whole-panel upload for a new video frame.
  paintBrowserFrame(cr);
  drawButtons(cr, kBrowser);
  cairo_restore(cr);
  canvas.upload(y0, y1, x0, x1);
  lookStale_ = true;
}
