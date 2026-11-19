#include "menu_style.h"

#include "menu_list_parts.h"

// Star ratings: five stars are one control, and where the
// pointer is along all of it picks the rating in half stars. Pointed at, the
// stars show in white what a press would choose; holding the trigger shows it
// in gold, letting go keeps it, and moving off the stars restores the kept
// one. Choosing the saved rating again clears it: pointed or held there, the
// stars show empty. Grip held and a stick click clear it too (the reset gesture).

namespace {

// Filled stars: a warm gold, as rating stars usually are, so they stand
// apart from the blue of what is chosen; white for what a press would choose.
constexpr Rgba kStarGold = {1.0, 0.76, 0.18, 1};
constexpr Rgba kStarWhite = {0.957, 0.957, 0.965, 1};
constexpr Rgba kStarEmpty = {0.59, 0.59, 0.62, 1};  // an empty star's outline, dimmer than text

// The pointer goes this far past a half star's edge before the value pointed
// at changes, so the laser's jitter doesn't flicker between two.
constexpr double kDeadBand = 4;
// Pointing at a list's stars shows nothing until it has stopped scrolling
// this long: each change would draw all of the moving list again.
constexpr double kPointAfterScroll = 0.2;

}  // namespace

Menu::Button Menu::ratingButton(const std::string& path, float rating, int panel, double x,
                                double y, double starW, double h) const {
  Button b;
  b.panel = panel;
  b.x = x;
  b.y = y;
  b.w = 5 * starW;
  b.h = h;
  b.bare = true;
  b.radius = std::min(h / 2, 16.0);
  b.ratingPath = path;
  b.rating = rating;
  b.tooltip = text().tipRating;
  b.action = [] { return MenuCommand{}; };  // clickable; click() starts the rating
  return b;
}

// On the strip along the bottom of a tile's picture, at its left.
Menu::Button Menu::cardRating(const std::string& path, float rating, const Button& card) const {
  const double th = (card.w - 16) * 9 / 16;
  Button b = ratingButton(path, rating, card.panel, card.x + 8 + kCardStarsX,
                          card.y + 8 + th - kCardStarsUp, kCardStarW, kCardStarsH);
  b.card = true;
  return b;
}

Menu::RatingDrag Menu::ratingControl(const Button& control) const {
  RatingDrag d;
  d.path = control.ratingPath;
  d.video = control.ratingVideo;
  d.panel = control.panel;
  d.x = control.x;
  d.y = control.y;
  d.w = control.w;
  d.h = control.h;
  d.card = control.card;
  d.scroll = scrollOffset_ + scrollFraction_;
  d.saved = control.rating;
  return d;
}

// ceil(10 · x / width) · 0.5 across the stars, `kept` while the
// pointer is still within kDeadBand of its half star.
float Menu::ratingAtPointer(const RatingDrag& control, float kept) const {
  const double half = control.w / 10, x = pointerX_ - control.x;
  if (kept > 0) {
    const double k = kept * 2;
    if (x >= (k - 1) * half - kDeadBand && x <= k * half + kDeadBand) return kept;
  }
  return static_cast<float>(std::clamp(std::ceil(x / half), 1.0, 10.0) * 0.5);
}

// The press chooses what pointing showed.
void Menu::startRating(const Button& control) {
  RatingDrag& d = ratingDrag_;
  d = ratingControl(control);
  d.active = true;
  RatingDrag& pointed = ratingHover_;
  const bool here = pointed.active && pointed.is(d);
  d.rating = ratingAtPointer(d, here ? pointed.rating : 0);
  if (pointed.active && !here) ratingChanged(pointed);
  pointed.active = false;
  ratingChanged(d);
}

// While the trigger is held on stars: they follow the pointer; let go, the
// rating is kept, or cleared if it was the saved one, unless the pointer
// left them. A list scrolled meanwhile (by the stick) ends it with the
// rating as it was: other stars are under the pointer now.
MenuCommand Menu::dragRating(bool held) {
  RatingDrag& d = ratingDrag_;
  if (d.panel == kBrowser && scrollOffset_ + scrollFraction_ != d.scroll) {
    d.active = false;
    browserChanged();
    return {};
  }
  const bool on = pointerPanel_ == d.panel && pointerX_ >= d.x && pointerX_ <= d.x + d.w &&
                  pointerY_ >= d.y && pointerY_ <= d.y + d.h;
  if (!on || !held) {
    d.active = false;
    ratingChanged(d);
    if (on) {
      rate(d.path, d.video, d.rating == d.saved ? 0 : d.rating);
      ratingHover_ = d;  // still pointed at, where it was let go
      ratingHover_.active = true;
    }
    return {};
  }
  const float rating = ratingAtPointer(d, d.rating);
  if (rating != d.rating) {
    d.rating = rating;
    ratingChanged(d);
  }
  return {};
}

// The stars under the pointer (`hovered`, a button or -1), while the trigger
// is not held on any: only their pixels are drawn again as that changes.
void Menu::pointAtRating(int hovered) {
  const Button* stars = nullptr;
  if (hovered >= 0 && !ratingDrag_.active && !buttons_[hovered].ratingPath.empty()) {
    const Button& b = buttons_[hovered];
    if (!b.scrolls || steadySeconds() - listMovedAt_ > kPointAfterScroll) stars = &b;
  }
  RatingDrag& now = ratingHover_;
  if (!now.active && !stars) return;
  if (now.active && stars && now.is(*stars)) {  // the same stars: only where along them
    const float rating = ratingAtPointer(now, now.rating);
    if (rating == now.rating) return;
    now.rating = rating;
    ratingChanged(now);
    return;
  }
  if (now.active) ratingChanged(now);
  now = stars ? ratingControl(*stars) : RatingDrag{};
  if (!stars) return;
  now.active = true;
  now.rating = ratingAtPointer(now, 0);
  ratingChanged(now);
}

bool Menu::clearRatingAtPointer() {
  if (pointerPanel_ < 0) return false;
  const int index = buttonAt(pointerPanel_, pointerX_, pointerY_);
  if (index < 0 || buttons_[index].ratingPath.empty()) return false;
  ratingDrag_.active = false;
  rate(buttons_[index].ratingPath, buttons_[index].ratingVideo, 0);
  return true;
}

// Into the library; the strip and the browser show it from there at once.
void Menu::rate(const std::string& path, int64_t video, float rating) {
  if (!library_ || (path.empty() && !video)) return;
  const VideoRecord now = video ? library_->video(video) : library_->record(path);
  if (now.rating == rating) return;  // unchanged: nothing to write
  if (video)
    library_->setRating(video, rating);
  else
    library_->setRating(path, rating);
  lookUpRecords();
  if (page_ == Page::Library) refreshLibraryRecords();
  current_ = library_->record(view_.currentFile);
  dirty_ = true;
}

// Redraws only what the control covers (on a tile, with the strip under it).
void Menu::ratingChanged(const RatingDrag& control) {
  if (control.panel == kMain) {
    stripDirty_ = true;
    return;
  }
  double box[4];
  ratingBox(control, box);
  browserChanged(box[1], box[3], box[0], box[2]);
}

void Menu::ratingBox(const RatingDrag& control, double box[4]) const {
  if (control.card) {
    box[0] = 0, box[1] = control.y - 8, box[2] = kBrowserWidth, box[3] = control.y + control.h + 8;
    return;
  }
  // A row's in its column, or one alone: only the stars' box.
  box[0] = control.x - kButtonReachX, box[1] = control.y - 2;
  box[2] = control.x + control.w + kButtonReachX, box[3] = control.y + control.h + 2;
}

// A star of this size, empty (0), half (1) or full (2), filled in gold or
// white: drawn once and pasted after, as a list draws dozens of them in
// every frame it scrolls.
cairo_surface_t* Menu::starImage(int size, int fill, StarInk ink) const {
  if (fill == 0) ink = StarInk::Gold;  // an outline only, the same in either
  cairo_surface_t*& image = starImages_[(size * 3 + fill) * 2 + static_cast<int>(ink)];
  if (image) return image;
  image = rasterSurface(size, size);
  cairo_t* cr = cairo_create(image);
  const double c = size / 2.0;
  const Rgba& color = ink == StarInk::White ? kStarWhite : kStarGold;
  if (fill > 0) {
    cairo_rectangle(cr, 0, 0, fill == 1 ? c : size, size);
    cairo_clip(cr);
    drawIcon(cr, Icon::StarFilled, c, c, size, color.r, color.g, color.b);
    cairo_reset_clip(cr);
  }
  if (fill == 1)
    drawIcon(cr, Icon::Star, c, c, size, color.r, color.g, color.b);
  else if (fill == 0)
    drawIcon(cr, Icon::Star, c, c, size, kStarEmpty.r, kStarEmpty.g, kStarEmpty.b);
  cairo_destroy(cr);
  return image;
}

// Five stars in equal cells: the saved rating in gold, the rest outlined.
// Pointed at, white up to the pointer; held, gold up to it; either on the
// saved rating, all five empty, as letting go there leaves them. A tile
// or a list row shows no stars while unrated and not pointed at.
void Menu::drawRating(cairo_t* cr, const Button& b) const {
  const bool held = ratingDrag_.active && ratingDrag_.is(b);
  const bool pointed = !held && ratingHover_.active && ratingHover_.is(b);
  float shown = b.rating;
  StarInk ink = StarInk::Gold;
  if (held || pointed) {
    const float at = held ? ratingDrag_.rating : ratingHover_.rating;
    shown = at == b.rating ? 0 : at;
    if (pointed) ink = StarInk::White;
  } else if ((b.card || b.listLook == static_cast<int>(ListLook::RowStars)) && b.rating <= 0) {
    return;  // a row's are shown by the panel shader while the row is pointed at (hoverLook())
  }
  const double cell = b.w / 5, cy = b.y + b.h / 2;
  const int size = b.card            ? static_cast<int>(kCardStarSize)
                   : b.iconSize > 0 ? static_cast<int>(b.iconSize)
                                    : static_cast<int>(std::min(cell * 1.05, b.h * 0.9));
  for (int i = 0; i < 5; ++i) {
    const double cx = b.x + cell * (i + 0.5);
    const int fill = shown >= i + 1 ? 2 : shown > i ? 1 : 0;
    cairo_set_source_surface(cr, starImage(size, fill, ink), onPixel(cx - size / 2.0),
                             onPixel(cy - size / 2.0));
    cairo_paint(cr);
  }
}

// Along the bottom of a tile's picture (x, y, w, h): a fade to dark under its
// stars, and its plays at the right end; with neither, only while its stars
// are pointed at or held, darker then.
void Menu::drawCoverStrip(cairo_t* cr, const Button& card, double x, double y, double w,
                          double h) const {
  bool active = false;
  for (const RatingDrag* d : {&ratingDrag_, &ratingHover_})
    active = active || (d->active && d->panel == card.panel && d->x >= x &&
                        d->x + d->w <= x + w && d->y >= y && d->y + d->h <= y + h + 0.5);
  if (!active && card.rating <= 0 && card.plays.empty()) return;
  const double top = y + h - kCoverStripH;
  cairo_save(cr);
  roundedRect(cr, x, y, w, h, 10);
  cairo_clip(cr);
  cairo_pattern_t* fade = cairo_pattern_create_linear(0, top, 0, y + h);
  cairo_pattern_add_color_stop_rgba(fade, 0, 0, 0, 0, 0);
  cairo_pattern_add_color_stop_rgba(fade, 1, 0, 0, 0, active ? 0.78 : 0.62);
  cairo_rectangle(cr, x, top, w, kCoverStripH);
  cairo_set_source(cr, fade);
  cairo_fill(cr);
  cairo_pattern_destroy(fade);
  cairo_restore(cr);
  if (card.plays.empty()) return;
  // A play mark and the count, on a soft shadow, level with the stars.
  constexpr int kSize = 22;
  constexpr double kMark = 18, kGap = 8;
  const double cy = y + h - kCardStarsUp + kCardStarsH / 2;
  const double textW = textWidth(card.plays, kSize, false);
  const double textX = x + w - 14 - textW, markX = textX - kGap - kMark / 2;
  cairo_save(cr);
  cairo_rectangle(cr, markX - kMark, cy - 20, x + w - markX + kMark, 40);
  cairo_clip(cr);  // a group of only the label's pixels
  cairo_push_group(cr);
  drawIcon(cr, Icon::Play, markX, cy + 1, kMark, 0, 0, 0);
  drawText(cr, card.plays, textX, cy + 1 - 15, textW + 2, 30, kSize, false, false, 0, 0, 0);
  cairo_pop_group_to_source(cr);
  cairo_paint_with_alpha(cr, 0.6);
  cairo_restore(cr);
  drawIcon(cr, Icon::Play, markX, cy, kMark, 1, 1, 1);
  drawText(cr, card.plays, textX, cy - 15, textW + 2, 30, kSize, false, false, 1, 1, 1);
}
