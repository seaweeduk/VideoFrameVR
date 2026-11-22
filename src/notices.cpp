#include "notices.h"

#include "menu.h"
#include "menu_geometry.h"
#include "strings.h"

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace {

// The viewpoint's and opacity's overlay stays this long after its gesture lets go.
constexpr double kValueShown = 1.5;
// A microSD card or USB drive put in says so this long.
constexpr double kDriveNoticeShown = 5.0;

}  // namespace

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

void Notices::say(std::string text, double now) { jobSaid = {std::move(text), now + kValueShown}; }

void Notices::viewpointChanged(double now) { viewpointShownUntil = now + kValueShown; }

void Notices::opacityChanged(double now) { opacityShownUntil = now + kValueShown; }

void Notices::drivesChanged(const RemovableDrives::Change& change, bool setupRunning, double now) {
  const DriveStrings& t = strings().drives;
  using Kind = RemovableDrives::Kind;
  if (!change.failures.empty()) {
    // udisks says "Error mounting /dev/… at /run/media/…: why": just why.
    std::string why = change.failures.back();
    const size_t colon = why.rfind(": ");
    if (why.rfind("Error mounting", 0) == 0 && colon != std::string::npos)
      why = why.substr(colon + 2);
    if (why.size() > 80) why = why.substr(0, 77) + "...";
    const bool usb = change.failedKinds.back() == Kind::Usb;
    driveText = std::string(usb ? t.usbFailed : t.cardFailed) + "\n" + why;
    driveIcon = NoticeIcon::Warning;
    driveShownUntil = now + kDriveNoticeShown;
  } else if (!change.added.empty() && !setupRunning) {  // setup shows it as a card
    const RemovableDrives::Drive& d = change.added.back();
    const char* kind = d.kind == Kind::Card  ? t.card
                       : d.kind == Kind::Usb ? t.usbDrive
                                             : t.drive;
    driveText = kind + (d.label.empty() ? "" : ": " + d.label) + "\n" + t.inExplorer;
    driveIcon = d.kind == Kind::Card  ? NoticeIcon::Card
                : d.kind == Kind::Usb ? NoticeIcon::Usb
                                      : NoticeIcon::Drive;
    driveShownUntil = now + kDriveNoticeShown;
  }
}

void Notices::update(Menu& menu, const Frame& f) {
  const double now = f.now;
  const bool headValid = f.headValid;
  if (f.recenterAt > 0 && headValid) {
    char text[64];
    std::snprintf(text, sizeof(text), strings().recenterIn,
                  static_cast<int>(std::ceil(f.recenterAt - now)));
    show(menu, Countdown, NoticeIcon::Recenter, text);
  }
  // One at a time, the newest: a gesture's value shows on the flat
  // screen too, as does the screen's distance or height the stick sets.
  // The flat screen has no viewpoint, but See-through's opacity.
  const bool flat = f.projection == Projection::Flat;
  const double valueUntil =
      flat ? opacityShownUntil : std::max(viewpointShownUntil, opacityShownUntil);
  const bool screenNewer =
      f.screenShown != GestureValue::Count && f.screenShownAt > f.gestures->shownAt();
  const GestureValue gestureShown = screenNewer ? f.screenShown : f.gestures->shown();
  const double gestureUntil =
      gestureShown != GestureValue::Count
          ? (screenNewer ? f.screenShownAt : f.gestures->shownAt()) + kValueShown
          : 0;
  if (now < gestureUntil && gestureUntil >= std::max(valueUntil, jobSaid.until) && headValid) {
    // Its ring, filled to the value along its range.
    const GestureValueInfo& info = gestureValueInfo(gestureShown);
    const double value = f.gestureHost->read(gestureShown);
    double least = info.range.least, most = info.range.most;
    f.gestureHost->range(gestureShown, least, most);
    const PlaybackState& p = *f.playback;
    if (info.format == GestureFormat::Clock) most = std::max(0.0, p.duration);
    const auto along = [&](double x) { return most > least ? std::clamp((x - least) / (most - least), 0.0, 1.0) : 0.0; };
    RingNotice ring;
    if (info.format == GestureFormat::Stars) {
      ring.kind = RingNotice::Kind::Stars;
      ring.halves = static_cast<int>(std::lround(value));
      ring.name = gestureValueLabel(gestureShown, strings());
    } else if (info.format == GestureFormat::Clock) {
      ring.kind = RingNotice::Kind::Position;
      ring.at = along(value);
      for (const Chapter& c : p.chapters) ring.ticks.push_back(along(c.time));
      ring.number = clockTime(value);
      ring.after = "/ " + clockTime(most);
    } else {
      ring.at = along(value);
      ring.centred = info.centred;
      ring.from = info.centred ? along(0) : 0;
      if (info.mark != kNoMark) ring.mark = along(info.mark);
      // Within float rounding of an end (the screen's 0.1 m is 10.0000001 cm).
      const double near = (most - least) * 1e-6;
      ring.limit = value <= least + near || value >= most - near;
      ring.name = gestureValueLabel(gestureShown, strings());
      ring.number = gestureValueText(gestureShown, value);
    }
    show(menu, Value, ring);
  } else if (now < jobSaid.until && jobSaid.until > valueUntil && headValid) {
    show(menu, Value, NoticeIcon::None, jobSaid.text);
  } else if (now < valueUntil && headValid) {
    char text[160];
    const bool opacity = flat || opacityShownUntil > viewpointShownUntil;
    if (opacity) {
      std::snprintf(text, sizeof(text), strings().opacityOverlay, f.video->opacity);
    } else {
      const int percent = static_cast<int>(std::lround(f.video->originForward * 100));
      std::snprintf(text, sizeof(text), strings().viewpoint.overlay,
                    viewpointText(percent).c_str());
    }
    show(menu, Value, opacity ? NoticeIcon::None : NoticeIcon::Viewpoint, text);
  }
  if (const char* why = f.passthrough; why && headValid)
    show(menu, Passthrough, NoticeIcon::Passthrough,
         std::string(why) + "\n" + strings().autoPassthrough.undo);
  if (now < driveShownUntil && headValid)
    show(menu, Drive, driveIcon, driveText);
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
