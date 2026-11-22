#pragma once

#include "cairo_canvas.h"
#include "menu_view.h"
#include "scene_renderer.h"
#include "xmath.h"

#include <string>
#include <vector>

class Menu;

// Where the messages in front of the eyes show, the hints' (hints.h) too:
// this far in front of them, sized as at 1.5 m, about 16° down, out of the
// way of the picture's centre. Nearer than the menu and the screen, so that
// both eyes see them in front of those rather than through them. Those shown
// at once stack up from there, kit::kSpace apart.
namespace notice_place {
constexpr float kNear = 0.7f, kScale = kNear / 1.5f / 1000, kLow = -0.2f;
constexpr float kGap = 16 * kScale;
}  // namespace notice_place

// One message drawn as the menu draws it (Menu::drawNotice()): its texture,
// drawn and uploaded again only when what it says changes, and then only
// where the old and the new pill are.
class NoticeCanvas {
 public:
  // lines: 1, or 2 for a message with a line under it.
  explicit NoticeCanvas(int lines) : lines_(lines) {}
  bool init(Uploader& uploader);
  // Says `text` (with `icon`) from now on.
  void set(Menu& menu, NoticeIcon icon, const std::string& text);
  // ... or shows a gesture's value (a one-line canvas).
  void set(Menu& menu, const RingNotice& ring);
  const Texture* texture() const { return canvas_.texture(); }
  // In layout pixels: the canvas, and the pill in its middle.
  int width() const { return canvas_.width(); }
  int height() const { return canvas_.height(); }
  double pillHeight() const { return pillHeight_; }
  // Drawn as a flat panel `place` puts the canvas's middle, `scale` meters
  // a layout pixel.
  PanelItem item(const Mat4& place, float scale) const;

 private:
  // Draws it anew with `draw` (Menu's, into the canvas: it says what it
  // covered), and uploads where the old and the new are.
  template <class Draw>
  void redraw(bool lost, Draw draw);

  CairoCanvas canvas_;
  int lines_;
  NoticeIcon icon_ = NoticeIcon::None;
  std::string text_;
  bool ringShown_ = false;  // the ring, not the text
  RingNotice ring_;
  bool drawn_ = false;
  double box_[4] = {};  // what the last drawing covers
  double pillHeight_ = 0;
};

// The short messages over the video, in front of the eyes and following the
// gaze: the recenter countdown, a value the stick or a gesture changes,
// passthrough switching itself on, a drive put in. Nothing is drawn while
// none shows.
class Notices {
 public:
  enum Slot { Countdown, Value, Passthrough, Drive, kSlots };
  bool init(Uploader& uploader);
  // Shows the slot's message this frame (each frame it shows, before addTo()).
  void show(Menu& menu, Slot slot, NoticeIcon icon, const std::string& text);
  // ... a gesture's value in a one-line slot.
  void show(Menu& menu, Slot slot, const RingNotice& ring);
  // Those shown this frame, the first lowest, from the messages' place up
  // (above the hint when one shows there); then none until shown again.
  void addTo(std::vector<OverlayDraw>& overlays, Vec3 headPos, Quat headRot, bool overHint);

 private:
  NoticeCanvas slots_[kSlots] = {NoticeCanvas(1), NoticeCanvas(1), NoticeCanvas(2), NoticeCanvas(2)};
  bool shown_[kSlots] = {};
  bool any_ = false;
};
