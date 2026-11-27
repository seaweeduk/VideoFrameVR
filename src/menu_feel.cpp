#include "menu_feel.h"

#include <algorithm>
#include <cmath>

#ifdef VIDEOFRAMEVR_DEV_BUILD
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#endif

namespace {

MenuFeel& feelInUse() {
  static MenuFeel feel;
  return feel;
}

uint8_t byte(float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 255.0f))); }

}  // namespace

const MenuFeel& menuFeel() { return feelInUse(); }

#ifdef VIDEOFRAMEVR_DEV_BUILD
namespace {

struct FeelKey {
  const char* name;
  double MenuFeel::*value;
};
constexpr FeelKey kFeelKeys[] = {
    {"point_in", &MenuFeel::pointIn},
    {"point_out", &MenuFeel::pointOut},
    {"press", &MenuFeel::press},
    {"scroll_settle", &MenuFeel::scrollSettle},
    {"open_in", &MenuFeel::openIn},
    {"close_out", &MenuFeel::closeOut},
    {"dot_ease", &MenuFeel::dotEase},
    {"dot_pressed", &MenuFeel::dotPressed},
    {"point_min_cutoff", &MenuFeel::pointMinCutoff},
    {"point_beta", &MenuFeel::pointBeta},
    {"point_speed_cutoff", &MenuFeel::pointSpeedCutoff},
};

std::string trimmed(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r");
  if (a == std::string::npos) return {};
  return s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
}

}  // namespace

void reloadMenuFeel(const std::filesystem::path& dir) {
  static bool found = false;
  static struct stat seen {};
  const std::string path = (dir / "feel.conf").string();
  struct stat now {};
  const bool there = ::stat(path.c_str(), &now) == 0 && S_ISREG(now.st_mode);
  if (there == found && (!there || (now.st_size == seen.st_size && now.st_mtim.tv_sec == seen.st_mtim.tv_sec &&
                                    now.st_mtim.tv_nsec == seen.st_mtim.tv_nsec)))
    return;
  found = there;
  seen = now;
  MenuFeel feel;
  if (there) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
      const size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      const std::string key = trimmed(line.substr(0, eq)), text = trimmed(line.substr(eq + 1));
      char* end = nullptr;
      const double value = std::strtod(text.c_str(), &end);
      if (text.empty() || *end != '\0' || !std::isfinite(value) || value < 0) continue;
      for (const FeelKey& k : kFeelKeys)
        if (key == k.name) feel.*k.value = value;
    }
  }
  feelInUse() = feel;
  std::string shown;
  for (const FeelKey& k : kFeelKeys) {
    char item[64];
    std::snprintf(item, sizeof(item), "%s%s=%g", shown.empty() ? "" : " ", k.name, feel.*k.value);
    shown += item;
  }
  std::printf("[feel] %s%s\n", there ? "" : "defaults: ", shown.c_str());
}
#endif

void Ease::to(float target, double seconds, double now) {
  if (target == target_) return;
  from_ = value_;
  target_ = target;
  start_ = now;
  seconds_ = seconds * std::abs(target - value_);
  if (seconds_ <= 0) value_ = from_ = target_;
}

float Ease::update(double at) {
  if (value_ == target_) return value_;
  const double t = (at - start_) / seconds_;
  if (t >= 1) {
    value_ = from_ = target_;
  } else if (t > 0) {
    const double out = 1 - (1 - t) * (1 - t);  // ease-out
    value_ = from_ + (target_ - from_) * static_cast<float>(out);
  }
  return value_;
}

float ShownFade::update(bool shown, double now, double at) {
  if (!shown && ease_.value() <= 0) return 0;
  const MenuFeel& feel = menuFeel();
  ease_.to(shown ? 1.0f : 0.0f, shown ? feel.openIn : feel.closeOut, now);
  return ease_.update(at);
}

void PointedEase::update(const Spot& spot, const PanelHover& pointed, const PanelHover& pressed,
                         bool held, bool canvas, bool snap, double now, double at) {
  const MenuFeel& feel = menuFeel();
  if (spot.panel < 0) {
    if (spot_.panel < 0) return;
    // Left: it goes from where it is, and rises if it was pressed; at once
    // if the canvas still draws it pushed in (until the trigger lets go).
    if (snap || canvas_) {
      reset();
      return;
    }
    shown_.to(0, feel.pointOut, now);
    press_.to(0, feel.press, now);
    if (shown_.update(at) <= 0) reset();
    press_.update(at);
    return;
  }
  if (!(spot == spot_)) {
    // Another control: the last one goes at once (there is room for one).
    spot_ = spot;
    shown_.set(snap ? 1.0f : 0.0f);
    press_.set(0);
  }
  pointed_ = pointed;
  pressed_ = pressed;
  canvas_ = canvas;
  if (canvas) {
    // The canvas draws it pushed in, and at rest again as it is let go:
    // pointed again at once then, as before.
    shown_.set(1);
    press_.set(0);
    return;
  }
  if (snap) {
    shown_.set(1);
    press_.set(held ? 1.0f : 0.0f);
    return;
  }
  shown_.to(1, feel.pointIn, now);
  press_.to(held ? 1.0f : 0.0f, feel.press, now);
  shown_.update(at);
  press_.update(at);
}

void PointedEase::reset() {
  spot_ = {};
  canvas_ = false;
  shown_.set(0);
  press_.set(0);
}

bool PointedEase::look(int panel, PanelHover& look) const {
  if (spot_.panel < 0 || spot_.panel != panel || canvas_ || shown_.value() <= 0) return false;
  look = blend(pointed_, pressed_, shown_.value(), press_.value());
  return true;
}

PanelHover PointedEase::blend(const PanelHover& pointed, const PanelHover& pressed, float shown,
                              float pressedBy) {
  // Its place and kind from the nearer of the two; the shadow is outside
  // until halfway down, then inside (the shader draws one of them).
  const float q = std::clamp(pressedBy, 0.0f, 1.0f), a = std::clamp(shown, 0.0f, 1.0f);
  PanelHover look = q < 0.5f ? pointed : pressed;
  if (q > 0 && q < 1) {
    const float pa = pointed.fill[3] / 255.0f, qa = pressed.fill[3] / 255.0f;
    const float alpha = pa + (qa - pa) * q;
    for (int i = 0; i < 3 && alpha > 0; ++i)
      look.fill[i] = byte((pointed.fill[i] * pa * (1 - q) + pressed.fill[i] * qa * q) / alpha);
    look.fill[3] = byte(alpha * 255);
    look.highlight = byte(pointed.highlight + (pressed.highlight - pointed.highlight) * q);
    look.ink = byte(pointed.ink + (pressed.ink - pointed.ink) * q);
    look.shadow = byte(q < 0.5f ? pointed.shadow * (1 - 2 * q) : pressed.shadow * (2 * q - 1));
  }
  if (a < 1) {
    look.fill[3] = byte(look.fill[3] * a);
    look.highlight = byte(look.highlight * a);
    look.shadow = byte(look.shadow * a);
    look.ink = byte(look.ink * a);
  }
  look.shown = a;
  return look;
}
