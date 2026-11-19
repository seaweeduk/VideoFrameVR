#pragma once

#include "panel_hover.h"

#include <filesystem>

// How the menu moves: every time and factor of its easing, here alone.
// Times are seconds, and 0 is at once (as the menu was before it eased).
struct MenuFeel {
  double pointIn = 0.090;       // a pointed control's highlight fades in (ease-out)
  double pointOut = 0.120;      // ... and out, once the pointer has left it
  double press = 0.050;         // a control the panel shader presses sinks, and rises back on letting go
  double scrollSettle = 0.100;  // after a list moved, its pointed rows switch at once for this long
  double openIn = 0.120;        // the menu, and each of its panels, fades in as it is shown (ease-out)
  double closeOut = 0.100;      // ... and out as it is taken away, then is not drawn at all
  double dotEase = 0.080;       // the pointer's dot tightens while the trigger is held, and back
  double dotPressed = 0.7;      // the dot's radius while the trigger is held, of its own
};

// The values in use: the defaults, or in a development build feel.conf's.
const MenuFeel& menuFeel();

#ifdef VIDEOFRAMEVR_DEV_BUILD
// Development builds: feel.conf in `dir`, "key = value" a line with
// MenuFeel's names as keys (others are ignored), is read again if it
// changed since the last call (a stat, then a small read). The values in
// use are printed whenever they change.
void reloadMenuFeel(const std::filesystem::path& dir);
#else
inline void reloadMenuFeel(const std::filesystem::path&) {}
#endif

// A value easing out toward its target, 0..1. Turned around on the way, it
// goes back from where it is, in that part of its time. Times in seconds:
// a change starts when the frame's input is read (`now`), and the frame
// shows it as it is when that frame is shown (`at`, a display period
// later), so the first frame after a change already shows a step of it.
class Ease {
 public:
  // Toward `target` in `seconds` for the whole way (0: at once), from now.
  void to(float target, double seconds, double now);
  // At `value` at once, staying there.
  void set(float value) { value_ = from_ = target_ = value; }
  // Advanced to `at`: its value.
  float update(double at);
  float value() const { return value_; }

 private:
  float value_ = 0, from_ = 0, target_ = 0;
  double start_ = 0, seconds_ = 0;
};

// The opacity of something that is shown and taken away, the menu or one
// of its panels: it fades in over openIn and out over closeOut, turning
// around from where it is.
class ShownFade {
 public:
  // This frame's opacity, shown or not (Ease's `now` and `at`). Gone, it
  // only compares.
  float update(bool shown, double now, double at);
  // Gone at once.
  void hide() { ease_.set(0); }
  // Whether anything of it is still drawn.
  bool drawn() const { return ease_.value() > 0; }

 private:
  Ease ease_;
};

// The control under the pointer as the panel shader draws it (PanelHover),
// easing in as it is pointed at, out once it is left, and into and out of
// being pressed. A control pointed at next starts from nothing while the
// last one goes at once: the panel's push constants have room for one.
class PointedEase {
 public:
  // A control, by where it is (Menu::HoverSpot); panel -1 = none.
  struct Spot {
    int panel = -1;
    double x = 0, y = 0, w = 0, h = 0;
    bool operator==(const Spot& o) const {
      return panel == o.panel && x == o.x && y == o.y && w == o.w && h == o.h;
    }
  };
  // This frame's control under the pointer (none: spot.panel < 0) and its
  // look pointed and pressed; `held`: the trigger holds it; `canvas`: the
  // menu's canvas draws it pushed in, and nothing is drawn over it. `snap`:
  // whatever changes does so at once (a list moved, the control left is
  // gone).
  void update(const Spot& spot, const PanelHover& pointed, const PanelHover& pressed, bool held,
              bool canvas, bool snap, double now, double at);
  // Nothing pointed, at once.
  void reset();
  // The control pointed at, or last pointed at while it fades.
  const Spot& spot() const { return spot_; }
  // Its look on this panel now; false if nothing is drawn there.
  bool look(int panel, PanelHover& look) const;
  // The look `shown` (0..1) of the way in and `pressed` (0..1) of the way down.
  static PanelHover blend(const PanelHover& pointed, const PanelHover& pressed, float shown,
                          float pressedBy);

 private:
  Spot spot_;
  PanelHover pointed_, pressed_;
  bool canvas_ = false;
  Ease shown_, press_;
};
