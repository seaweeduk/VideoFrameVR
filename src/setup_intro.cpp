#include "setup_intro.h"

#include "menu_geometry.h"

#include <algorithm>
#include <cmath>

namespace {

using Layer = Menu::IntroLayer;

constexpr double kLength = 4.3;
// The circles start this far apart (190 of the icon's 512), in layout pixels.
constexpr float kSlide = 190.0f / 512 * 230, kRise = 30;

// CSS's cubic-bezier(x1, y1, x2, y2) at p.
float bezier(float x1, float y1, float x2, float y2, float p) {
  auto at = [](float a, float b, float s) {
    return 3 * (1 - s) * (1 - s) * s * a + 3 * (1 - s) * s * s * b + s * s * s;
  };
  float lo = 0, hi = 1;
  for (int i = 0; i < 20; ++i) {
    const float mid = (lo + hi) / 2;
    (at(x1, x2, mid) < p ? lo : hi) = mid;
  }
  return at(y1, y2, (lo + hi) / 2);
}
float easeOut(float p) { return bezier(0, 0, 0.58f, 1, p); }
float glide(float p) { return bezier(0.2f, 0.8f, 0.2f, 1, p); }

// How far along a step from `from` for `length` seconds is, 0..1.
float along(double t, double from, double length) {
  return static_cast<float>(std::clamp((t - from) / length, 0.0, 1.0));
}

// The piece's place on the panel, moved by dx, dy and scaled about its middle.
CurvedPanel placed(const CurvedPanel& panel, const Menu::IntroPiece& p, float dx, float dy,
                   float scale) {
  constexpr float ppm = menu_geometry::kPixelsPerMeter;
  const float w = p.w * scale, h = p.h * scale;
  const float x = p.x + dx + (p.w - w) / 2, y = p.y + dy + (p.h - h) / 2;
  const float width = w / ppm / (p.u1 - p.u0), height = h / ppm / (p.v1 - p.v0);
  CurvedPanel q = panel;
  q.left = panel.left + x / ppm - p.u0 * width;
  q.right = q.left + width;
  q.top = panel.top - y / ppm + p.v0 * height;
  q.bottom = q.top - height;
  q.u0 = p.u0, q.u1 = p.u1, q.v0 = p.v0, q.v1 = p.v1;
  return q;
}

}  // namespace

void SetupIntro::skip(double now) {
  if (playing(now)) start_ = now - kLength;
}

bool SetupIntro::playing(double now) const { return start_ >= 0 && now - start_ < kLength; }

float SetupIntro::pageOpacity(double now) const {
  return playing(now) ? easeOut(along(now - start_, 3.7, 0.6)) : 1.0f;
}

void SetupIntro::addUnder(std::vector<OverlayDraw>& overlays, Menu& menu, const CurvedPanel& panel,
                          float opacity, double now) const {
  if (!playing(now)) return;
  const double t = now - start_;
  const float shown = opacity * easeOut(along(t, 0.9, 0.7));
  auto add = [&](Layer layer, float alpha, float dx = 0, float dy = 0, float scale = 1) {
    const Menu::IntroPiece p = menu.setupIntroPiece(layer);
    if (!p.texture || alpha <= 0) return;
    PanelItem item;
    item.texture = p.texture;
    item.curved = placed(panel, p, dx, dy, scale);
    item.opacity = alpha;
    overlays.push_back(item);
  };
  // The empty panel, under the page as it fades in.
  add(Layer::Backdrop, shown);
  add(Layer::Tile, shown * easeOut(along(t, 2.8, 0.8)));
  const float slide = along(t, 1.2, 1.3);
  const float circles = shown * std::min(1.0f, slide / 0.3f);
  add(Layer::Blue, circles, -kSlide * (1 - glide(slide)));
  add(Layer::Green, circles, kSlide * (1 - glide(slide)));
  add(Layer::Lens, shown * easeOut(along(t, 2.35, 0.35)));
  // The play button grows past its size, then settles.
  const float pop = along(t, 2.6, 0.5);
  const float grow = pop < 0.7f ? 0.3f + 0.82f * easeOut(pop / 0.7f)
                                : 1.12f - 0.12f * easeOut((pop - 0.7f) / 0.3f);
  add(Layer::Play, shown * easeOut(std::min(1.0f, pop / 0.7f)), 0, 0, grow);
  const float rise = glide(along(t, 3.0, 0.9));
  add(Layer::Name, shown * rise, 0, kRise * (1 - rise));
}

void SetupIntro::addCover(std::vector<OverlayDraw>& overlays, Menu& menu, Vec3 headPos,
                          Quat headRot, double now) const {
  const float black = started() ? 1 - easeOut(along(now - start_, 0, 1.6)) : 1.0f;
  if (black <= 0) return;
  const Menu::IntroPiece p = menu.setupIntroPiece(Layer::Black);
  if (!p.texture) return;
  // Wide enough to fill any headset's view, half a metre away.
  const Mat4 gaze = rotation(headRot);
  PanelItem item;
  item.texture = p.texture;
  item.model = translation(headPos + transformDir(gaze, {0.0f, 0.0f, -0.5f})) * gaze *
               scaling(20, 20, 1);
  item.opacity = black;
  overlays.push_back(item);
}
