#include "notices.h"

#include "menu_geometry.h"

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <utility>

bool NoticeCanvas::init(Uploader& uploader) {
  return canvas_.init(uploader, Menu::kNoticeMaxW + 2 * Menu::kNoticeShadow,
                      Menu::kNoticeH[lines_ > 1] + 2 * Menu::kNoticeShadow,
                      menu_geometry::kRasterScale);
}

void NoticeCanvas::set(Menu& menu, NoticeIcon icon, const std::string& text) {
  const bool lost = canvas_.takeLost();
  if (drawn_ && !lost && !ringShown_ && icon == icon_ && text == text_) return;
  icon_ = icon;
  text_ = text;
  ringShown_ = false;
  std::string shown = text;
  if (lines_ < 2) std::replace(shown.begin(), shown.end(), '\n', ' ');
  redraw(lost, [&](cairo_t* cr, double box[4]) { menu.drawNotice(cr, width(), height(), icon, shown, box); });
  pillHeight_ = Menu::kNoticeH[shown.find('\n') != std::string::npos];
}

void NoticeCanvas::set(Menu& menu, const RingNotice& ring) {
  const bool lost = canvas_.takeLost();
  if (drawn_ && !lost && ringShown_ && ring == ring_) return;
  ring_ = ring;
  ringShown_ = true;
  redraw(lost, [&](cairo_t* cr, double box[4]) { menu.drawRingNotice(cr, width(), height(), ring, box); });
  pillHeight_ = Menu::kNoticeH[0];
}

template <class Draw>
void NoticeCanvas::redraw(bool lost, Draw draw) {
  cairo_t* cr = canvas_.cr();
  if (drawn_) {  // the old pill away
    cairo_save(cr);
    cairo_rectangle(cr, box_[0], box_[1], box_[2], box_[3]);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_fill(cr);
    cairo_restore(cr);
  }
  double box[4];
  draw(cr, box);
  // Uploaded where the old and the new are; all of it if what waited to be
  // uploaded was dropped.
  double x0 = box[0], y0 = box[1], x1 = box[0] + box[2], y1 = box[1] + box[3];
  if (drawn_ && !lost) {
    x0 = std::min(x0, box_[0]), y0 = std::min(y0, box_[1]);
    x1 = std::max(x1, box_[0] + box_[2]), y1 = std::max(y1, box_[1] + box_[3]);
  } else if (lost) {
    x0 = y0 = 0, x1 = width(), y1 = height();
  }
  canvas_.upload(static_cast<int>(std::floor(y0)), static_cast<int>(std::ceil(y1)),
                 static_cast<int>(std::floor(x0)), static_cast<int>(std::ceil(x1)));
  std::copy(box, box + 4, box_);
  drawn_ = true;
}

PanelItem NoticeCanvas::item(const Mat4& place, float scale) const {
  PanelItem item;
  item.texture = texture();
  item.model = place * scaling(width() * scale, height() * scale, 1);
  return item;
}

bool Notices::init(Uploader& uploader) {
  for (NoticeCanvas& slot : slots_)
    if (!slot.init(uploader)) return false;
  return true;
}

void Notices::show(Menu& menu, Slot slot, NoticeIcon icon, const std::string& text) {
  slots_[slot].set(menu, icon, text);
  shown_[slot] = any_ = true;
}

void Notices::show(Menu& menu, Slot slot, const RingNotice& ring) {
  slots_[slot].set(menu, ring);
  shown_[slot] = any_ = true;
}

void Notices::addTo(std::vector<OverlayDraw>& overlays, Vec3 headPos, Quat headRot, bool overHint) {
  if (!any_) return;
  any_ = false;
  using namespace notice_place;
  const Mat4 gaze = rotation(headRot);
  PanelItem items[kSlots];
  int count = 0;
  // The next pill's bottom edge, up from the hint's pill.
  float bottom = kLow + Menu::kNoticeH[0] / 2.0f * kScale + kGap;
  for (int i = 0; i < kSlots; ++i) {
    if (!std::exchange(shown_[i], false) || !slots_[i].texture()) continue;
    const float half = static_cast<float>(slots_[i].pillHeight() / 2 * kScale);
    if (count == 0 && !overHint) bottom = kLow - half;  // the first at the place itself
    const float centre = bottom + half;
    bottom = centre + half + kGap;
    items[count++] =
        slots_[i].item(translation(headPos + transformDir(gaze, {0.0f, centre, -kNear})) * gaze, kScale);
  }
  // The highest first: each pill drawn over the shadow of the one above it.
  for (int i = count; i-- > 0;) overlays.push_back(items[i]);
}
