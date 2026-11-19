#include "menu_finish.h"

#include "menu_geometry.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr double kScale = menu_geometry::kRasterScale;

// The lit line along the top of a lit finish, a row of texture pixels at a
// time down from the box's top edge: how much of its light each row has.
// Softer at its top and bottom than whole rows of it, so that it doesn't
// shimmer where its rows are thinner than the eye's pixels.
constexpr float kLitLine[] = {0.6f, 0.8f, 0.8f, 0.6f};
constexpr int kLitRows = sizeof(kLitLine) / sizeof(kLitLine[0]);

// A shadow as CSS draws one: inside the box (inset) or around it, moved down
// by dy, blurred by `blur` (a Gaussian of half that deviation), grown by
// `spread` (shrunk, inside), in a colour (straight alpha). Layout pixels.
// A lit line (litLine()) is drawn as kLitLine instead, dy deep.
struct Shadow {
  bool inset;
  double dy, blur, spread;
  double rgba[4];
  bool lit = false;
};

struct Spec {
  double top[4], bottom[4];  // the box's fill, top to bottom (straight alpha)
  bool gradient = false;     // ... not flat
  std::vector<Shadow> shadows;  // the first one on top
};

void rgb(double out[4], const double c[3], double a) {
  out[0] = c[0], out[1] = c[1], out[2] = c[2], out[3] = a;
}

void hex(double out[4], unsigned c, double a = 1) {
  out[0] = ((c >> 16) & 255) / 255.0, out[1] = ((c >> 8) & 255) / 255.0, out[2] = (c & 255) / 255.0;
  out[3] = a;
}

Shadow dark(double dy, double blur, double alpha, bool inset = false, double spread = 0) {
  return {inset, dy, blur, spread, {0, 0, 0, alpha}};
}
Shadow light(double dy, double blur, double alpha, bool inset = true, double spread = 0) {
  return {inset, dy, blur, spread, {1, 1, 1, alpha}};
}
Shadow tinted(const double c[3], double dy, double blur, double alpha, bool inset = false) {
  return {inset, dy, blur, 0, {c[0], c[1], c[2], alpha}};
}
// White along the top edge: `alpha` in the share kLitLine gives each row.
Shadow litLine(double alpha) { return {true, kLitRows / kScale, 0, 0, {1, 1, 1, alpha}, true}; }

void flat(Spec& s, double r, double g, double b, double a) {
  s.top[0] = s.bottom[0] = r, s.top[1] = s.bottom[1] = g, s.top[2] = s.bottom[2] = b;
  s.top[3] = s.bottom[3] = a;
}
void white(Spec& s, double top, double bottom) {
  flat(s, 1, 1, 1, top);
  s.bottom[3] = bottom;
  s.gradient = true;
}

// The menu design's finishes (its stylesheet's tokens), in the accent.
Spec specOf(Finish f, const AccentColours& a) {
  Spec s;
  const std::vector<Shadow> raise = {litLine(0.10), dark(1.5, 2, 0.30), dark(3, 8, 0.20)};
  const std::vector<Shadow> sink = {dark(2, 5, 0.45, true), dark(0, 0, 0.12, true, 1)};
  switch (f) {
    case Finish::Sunk:
      flat(s, 0, 0, 0, 0.30);
      s.shadows = {dark(2, 4, 0.36, true), light(-1, 0, 0.045)};
      break;
    case Finish::Raised:
      white(s, 0.115, 0.065);
      s.shadows = raise;
      break;
    case Finish::Chosen:
      white(s, 0.17, 0.10);
      s.shadows = raise;
      break;
    case Finish::Pressed:
      flat(s, 1, 1, 1, 0.04);
      s.shadows = sink;
      break;
    case Finish::Latched:
      flat(s, a.fill[0], a.fill[1], a.fill[2], 0.16);
      s.shadows = sink;
      break;
    case Finish::Primary:
      rgb(s.top, a.filled.top, 1);
      rgb(s.bottom, a.filled.bottom, 1);
      s.gradient = true;
      s.shadows = {litLine(0.26), tinted(a.shadow, 2, 3, 0.45), tinted(a.glow, 4, 12, 0.22)};
      break;
    case Finish::DangerFilled: {
      hex(s.top, 0xf05a5f);
      hex(s.bottom, 0xd63c41);
      s.gradient = true;
      const double red[3] = {60 / 255.0, 0, 0}, glow[3] = {229 / 255.0, 72 / 255.0, 77 / 255.0};
      s.shadows = {litLine(0.22), tinted(red, 2, 3, 0.4), tinted(glow, 4, 12, 0.22)};
      break;
    }
    case Finish::Group:
      flat(s, 0, 0, 0, 0.18);
      break;
    case Finish::Pop:
      hex(s.top, 0x20242d);
      hex(s.bottom, 0x20242d);
      s.shadows = {litLine(0.08), dark(4, 10, 0.35), dark(18, 44, 0.5)};
      break;
    case Finish::Tip:
      hex(s.top, 0x0b0d12);
      hex(s.bottom, 0x0b0d12);
      s.shadows = {light(0, 0, 0.06, false, 1), dark(8, 24, 0.5)};
      break;
    case Finish::SwitchOn:
      rgb(s.top, a.filled.top, 1);
      rgb(s.bottom, a.filled.bottom, 1);
      s.gradient = true;
      s.shadows = {litLine(0.22), dark(-1, 0, 0.15, true)};
      break;
    case Finish::Knob:
      hex(s.top, 0xffffff);
      hex(s.bottom, 0xdde2ea);
      s.gradient = true;
      s.shadows = {dark(1, 2, 0.35), dark(2, 6, 0.25)};
      break;
    case Finish::SliderKnob:
      hex(s.top, 0xffffff);
      hex(s.bottom, 0xdde2ea);
      s.gradient = true;
      s.shadows = {dark(-1, 0, 0.08, true), dark(1, 2, 0.35), dark(3, 8, 0.28)};
      break;
    case Finish::Track:
      flat(s, 0, 0, 0, 0.30);
      s.shadows = {dark(1.5, 2, 0.45, true), light(-1, 0, 0.05)};
      break;
    case Finish::Head:
      flat(s, 1, 1, 1, 1);
      s.shadows = {dark(0, 0, 0.22, false, 1)};
      break;
    case Finish::Filled: {
      // The accent at 92 % over a well, opaque: a fill that replaces what
      // is under it costs less than one that blends.
      constexpr double kAlpha = 0.92, kWell = 0.7 * 0x1a / 255.0;
      for (int c = 0; c < 3; ++c) {
        s.top[c] = a.filled.top[c] * kAlpha + kWell * (1 - kAlpha);
        s.bottom[c] = a.filled.bottom[c] * kAlpha + kWell * (1 - kAlpha);
      }
      s.top[3] = s.bottom[3] = 1;
      s.gradient = true;
      s.shadows = {litLine(0.18 * kAlpha)};
      break;
    }
    case Finish::kCount: break;
  }
  return s;
}

bool accented(Finish f) {
  return f == Finish::Latched || f == Finish::Primary || f == Finish::SwitchOn || f == Finish::Filled;
}

// What a finish's shape needs, whatever its colour: texture pixels its
// shadows need around its box and in from its edges (the Gaussian's two
// deviations, as far as it is still seen: its blur, the move and the spread;
// none around it if it has no shadow outside), how far its shadows reach out
// (layout pixels), whether its fill is a gradient and whether it is opaque.
struct Shape {
  int pad = 0, edge = 0;
  double reach = 0;
  bool gradient = false, opaque = false;
};

const Shape& shapeOf(Finish f) {
  static Shape shapes[static_cast<int>(Finish::kCount)];
  static bool made = false;
  if (!made) {
    const AccentColours accent = accentColours(Accent{});
    for (int i = 0; i < static_cast<int>(Finish::kCount); ++i) {
      const Spec spec = specOf(static_cast<Finish>(i), accent);
      double inner = 0, reach = 0;
      for (const Shadow& s : spec.shadows) {
        const double extent = std::abs(s.dy) + s.blur + s.spread;
        (s.inset ? inner : reach) = std::max(s.inset ? inner : reach, extent);
      }
      const int pad = reach > 0 ? static_cast<int>(std::ceil(reach * kScale)) + 2 : 0;
      shapes[i] = {pad, std::max(pad, static_cast<int>(std::ceil(inner * kScale)) + 2), std::ceil(reach),
                   spec.gradient, spec.top[3] == 1 && spec.bottom[3] == 1};
    }
    made = true;
  }
  return shapes[static_cast<int>(f)];
}

void roundedPath(cairo_t* cr, double x, double y, double w, double h, double r) {
  r = std::max(0.0, std::min(r, std::min(w, h) / 2));
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
  cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
  cairo_close_path(cr);
}

// How much of each pixel of a W × H image a rounded box covers.
std::vector<float> coverage(int W, int H, double x, double y, double w, double h, double r) {
  std::vector<float> out(static_cast<size_t>(W) * H, 0.0f);
  if (w <= 0 || h <= 0) return out;
  cairo_surface_t* mask = cairo_image_surface_create(CAIRO_FORMAT_A8, W, H);
  cairo_t* cr = cairo_create(mask);
  roundedPath(cr, x, y, w, h, r);
  cairo_fill(cr);
  cairo_destroy(cr);
  cairo_surface_flush(mask);
  const unsigned char* data = cairo_image_surface_get_data(mask);
  const int stride = cairo_image_surface_get_stride(mask);
  for (int j = 0; j < H; ++j)
    for (int i = 0; i < W; ++i) out[static_cast<size_t>(j) * W + i] = data[j * stride + i] / 255.0f;
  cairo_surface_destroy(mask);
  return out;
}

// Box blurs of radius r, the edges extended: sliding sums, as cheap for wide
// shadows as for narrow ones. Along the rows, then down the columns a row at
// a time.
void blurRows(std::vector<float>& v, int W, int H, int r) {
  std::vector<float> line(W + 2 * r + 2);
  const float scale = 1.0f / (2 * r + 1);
  for (int j = 0; j < H; ++j) {
    float* row = &v[static_cast<size_t>(j) * W];
    for (int i = 0; i < W + 2 * r + 2; ++i) line[i] = row[std::clamp(i - r - 1, 0, W - 1)];
    float sum = 0;
    for (int i = 1; i <= 2 * r + 1; ++i) sum += line[i];  // centred on row[0]
    for (int i = 0; i < W; ++i) {
      row[i] = sum * scale;
      sum += line[i + 2 * r + 2] - line[i + 1];
    }
  }
}

void blurColumns(std::vector<float>& v, int W, int H, int r) {
  const std::vector<float> in = v;
  std::vector<float> sum(W, 0.0f);
  const float scale = 1.0f / (2 * r + 1);
  auto row = [&](int j) { return &in[static_cast<size_t>(std::clamp(j, 0, H - 1)) * W]; };
  for (int j = -r; j <= r; ++j) {
    const float* s = row(j);
    for (int i = 0; i < W; ++i) sum[i] += s[i];
  }
  for (int j = 0; j < H; ++j) {
    float* out = &v[static_cast<size_t>(j) * W];
    const float *add = row(j + r + 1), *drop = row(j - r);
    for (int i = 0; i < W; ++i) {
      out[i] = sum[i] * scale;
      sum[i] += add[i] - drop[i];
    }
  }
}

// A Gaussian blur of deviation sigma (pixels), the edges extended: three box
// blurs of matched widths, close to it.
void blur(std::vector<float>& v, int W, int H, double sigma) {
  if (sigma < 0.1) return;
  constexpr int passes = 3;
  int lower = static_cast<int>(std::sqrt(12 * sigma * sigma / passes + 1));
  if (lower % 2 == 0) --lower;
  const int upper = lower + 2;
  const int lowerPasses = static_cast<int>(std::lround(
      (12 * sigma * sigma - passes * lower * lower - 4 * passes * lower - 3 * passes) / (-4.0 * lower - 4)));
  for (int pass = 0; pass < passes; ++pass) {
    const int r = ((pass < lowerPasses ? lower : upper) - 1) / 2;
    if (r <= 0) continue;
    blurRows(v, W, H, r);
    blurColumns(v, W, H, r);
  }
}

// A rounded box's shadow on a W × H image: its coverage (or, inverted, what
// is outside it) blurred by sigma. A wide blur is worked out on a coarser
// grid and spread back smoothly: it has no detail to lose.
std::vector<float> shadowMask(int W, int H, double x, double y, double w, double h, double r,
                              double sigma, bool invert) {
  const int f = std::max(1, static_cast<int>(sigma / 3));
  if (f == 1) {
    std::vector<float> v = coverage(W, H, x, y, w, h, r);
    if (invert)
      for (float& c : v) c = 1 - c;
    blur(v, W, H, sigma);
    return v;
  }
  const int w2 = (W + f - 1) / f + 1, h2 = (H + f - 1) / f + 1;
  std::vector<float> small = coverage(w2, h2, x / f, y / f, w / f, h / f, r / f);
  if (invert)
    for (float& c : small) c = 1 - c;
  blur(small, w2, h2, sigma / f);
  // Bilinear, the columns' places worked out once; a row of the coarse grid
  // spread across first, then rows mixed.
  std::vector<int> xs(W);
  std::vector<float> fxs(W);
  for (int i = 0; i < W; ++i) {
    const double sx = std::clamp((i + 0.5) / f - 0.5, 0.0, w2 - 1.0);
    xs[i] = std::min(static_cast<int>(sx), w2 - 2);
    fxs[i] = static_cast<float>(sx - xs[i]);
  }
  std::vector<float> wide(static_cast<size_t>(W) * h2);
  for (int j = 0; j < h2; ++j) {
    const float* a = &small[static_cast<size_t>(j) * w2];
    float* out = &wide[static_cast<size_t>(j) * W];
    for (int i = 0; i < W; ++i) out[i] = a[xs[i]] + (a[xs[i] + 1] - a[xs[i]]) * fxs[i];
  }
  std::vector<float> v(static_cast<size_t>(W) * H);
  for (int j = 0; j < H; ++j) {
    const double sy = std::clamp((j + 0.5) / f - 0.5, 0.0, h2 - 1.0);
    const int y0 = std::min(static_cast<int>(sy), h2 - 2);
    const float fy = static_cast<float>(sy - y0);
    const float* a = &wide[static_cast<size_t>(y0) * W];
    const float* b = a + W;
    float* out = &v[static_cast<size_t>(j) * W];
    for (int i = 0; i < W; ++i) out[i] = a[i] + (b[i] - a[i]) * fy;
  }
  return v;
}

// A lit line's share of its light along row j of a W × H image, from its
// box's coverage (box): the inset shadows of the box moved down 1 to
// kLitRows rows (what it doesn't cover then), each as much as kLitLine
// steps down after that many rows. Moved down whole rows, the box's coverage
// is its own a few rows up. False if none of it is within the box there.
bool litRow(const std::vector<float>& box, int W, int j, float* out) {
  std::fill(out, out + W, kLitLine[0]);
  for (int n = 1; n <= std::min(j, kLitRows); ++n) {
    const float step = kLitLine[n - 1] - (n < kLitRows ? kLitLine[n] : 0.0f);
    if (step == 0) continue;
    const float* above = &box[static_cast<size_t>(j - n) * W];
    for (int i = 0; i < W; ++i) out[i] -= step * above[i];
  }
  const float* in = &box[static_cast<size_t>(j) * W];
  bool any = false;
  for (int i = 0; i < W; ++i) {
    out[i] = std::clamp(out[i], 0.0f, 1.0f);
    any |= out[i] * in[i] > 0;
  }
  return any;
}

}  // namespace

void MenuFinishes::release(Piece& p) {
  for (cairo_pattern_t* view : p.solid)  // before the pixels they show
    if (view) cairo_pattern_destroy(view);
  cairo_pattern_destroy(p.pattern);
  for (cairo_pattern_t* tile : p.tiles)
    if (tile) cairo_pattern_destroy(tile);
  cairo_surface_destroy(p.surface);
}

MenuFinishes::~MenuFinishes() {
  for (auto& p : pieces_) release(p.second);
  for (Placed& p : placed_) release(p);
}

void MenuFinishes::setAccent(const AccentColours& accent) {
  accent_ = accent;
  for (Placed& p : placed_) release(p);
  placed_.clear();
  for (auto it = pieces_.begin(); it != pieces_.end();) {
    if (accented(static_cast<Finish>(it->first >> 56))) {
      release(it->second);
      it = pieces_.erase(it);
    } else {
      ++it;
    }
  }
}

double MenuFinishes::reach(Finish finish) { return shapeOf(finish).reach; }

MenuFinishes::Piece& MenuFinishes::piece(Finish finish, int radius, int w, int h) {
  const uint64_t key = uint64_t(finish) << 56 | uint64_t(radius & 0xffff) << 32 |
                       uint64_t(w & 0xffff) << 16 | uint64_t(h & 0xffff);
  auto known = pieces_.find(key);
  if (known != pieces_.end()) return known->second;
  if (pieces_.size() >= 512) {  // sizes seen long ago: make them again if needed
    for (auto& p : pieces_) release(p.second);
    pieces_.clear();
  }
  Piece& p = pieces_[key];
  p.w = w;
  p.h = h;
  make(p, finish, radius);
  return p;
}

// The finish around a box of p.w × p.h texture pixels, as its stylesheet
// draws it: the shadows around it, its fill, the shadows inside it.
void MenuFinishes::make(Piece& p, Finish finish, double radius) const {
  const Spec spec = specOf(finish, accent_);
  p.pad = shapeOf(finish).pad;
  const int W = p.w + 2 * p.pad, H = p.h + 2 * p.pad;
  const double bx = p.pad, by = p.pad, bw = p.w, bh = p.h;
  std::vector<float> rgba(static_cast<size_t>(W) * H * 4, 0.0f);  // premultiplied
  // amount: of `count` pixels from `at` on.
  auto over = [&](const float* amount, size_t at, size_t count, const double c[4],
                  const std::vector<float>* within) {
    const float r = static_cast<float>(c[0]), g = static_cast<float>(c[1]), b = static_cast<float>(c[2]);
    const float alpha = static_cast<float>(c[3]);
    const float* in = within ? within->data() + at : nullptr;
    for (size_t i = 0; i < count; ++i) {
      const float a = alpha * amount[i] * (in ? in[i] : 1.0f);
      float* d = &rgba[(at + i) * 4];
      d[0] = r * a + d[0] * (1 - a);
      d[1] = g * a + d[1] * (1 - a);
      d[2] = b * a + d[2] * (1 - a);
      d[3] = a + d[3] * (1 - a);
    }
  };
  const std::vector<float> box = coverage(W, H, bx, by, bw, bh, radius);
  for (auto s = spec.shadows.rbegin(); s != spec.shadows.rend(); ++s) {
    if (s->inset) continue;
    const double grow = s->spread * kScale;
    const std::vector<float> shape =
        shadowMask(W, H, bx - grow, by - grow + s->dy * kScale, bw + 2 * grow, bh + 2 * grow,
                   radius + grow, s->blur * kScale / 2, false);
    over(shape.data(), 0, shape.size(), s->rgba, nullptr);
  }
  // Shadows only around the box: under it, its fill shows what is behind.
  for (size_t i = 0; i < box.size(); ++i)
    for (int k = 0; k < 4; ++k) rgba[i * 4 + k] *= 1 - box[i];
  for (int j = 0; j < H; ++j) {
    const double t = std::clamp((j + 0.5 - by) / bh, 0.0, 1.0);
    double c[4];
    for (int k = 0; k < 4; ++k) c[k] = spec.top[k] + (spec.bottom[k] - spec.top[k]) * t;
    for (int i = 0; i < W; ++i) {
      const size_t at = static_cast<size_t>(j) * W + i;
      const float a = static_cast<float>(c[3]) * box[at];
      float* d = &rgba[at * 4];
      for (int k = 0; k < 3; ++k) d[k] = static_cast<float>(c[k]) * a + d[k] * (1 - a);
      d[3] = a + d[3] * (1 - a);
    }
  }
  for (auto s = spec.shadows.rbegin(); s != spec.shadows.rend(); ++s) {
    if (!s->inset) continue;
    if (s->lit) {  // a row at a time, only where it shows
      std::vector<float> row(W);
      for (int j = 0; j < H; ++j)
        if (litRow(box, W, j, row.data())) over(row.data(), static_cast<size_t>(j) * W, W, s->rgba, &box);
      continue;
    }
    // What is outside the box moved and shrunk casts it, within the box.
    const double shrink = s->spread * kScale;
    const std::vector<float> outside =
        shadowMask(W, H, bx + shrink, by + shrink + s->dy * kScale, bw - 2 * shrink,
                   bh - 2 * shrink, std::max(0.0, radius - shrink), s->blur * kScale / 2, true);
    over(outside.data(), 0, outside.size(), s->rgba, &box);
  }
  p.surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
  cairo_surface_flush(p.surface);
  unsigned char* data = cairo_image_surface_get_data(p.surface);
  const int stride = cairo_image_surface_get_stride(p.surface);
  for (int j = 0; j < H; ++j)
    for (int i = 0; i < W; ++i) {
      const float* s = &rgba[(static_cast<size_t>(j) * W + i) * 4];
      auto byte = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255 + 0.5f); };
      const uint32_t pixel = byte(s[3]) << 24 | byte(s[0]) << 16 | byte(s[1]) << 8 | byte(s[2]);
      *reinterpret_cast<uint32_t*>(data + j * stride + i * 4) = pixel;
    }
  cairo_surface_mark_dirty(p.surface);
  p.pattern = cairo_pattern_create_for_surface(p.surface);
  cairo_pattern_set_filter(p.pattern, CAIRO_FILTER_NEAREST);
  // The middles, repeated to tiles: wider boxes repeat those (pixman
  // repeats a tile in steps of its width, so a wide one is quicker).
  const int corner = static_cast<int>(radius) + shapeOf(finish).edge;
  p.corner = corner;
  const bool across = p.w == 2 * corner + kMiddle, down = p.h == 2 * corner + kMiddle;
  const int x = p.pad + corner, y = p.pad + corner;
  bool opaque = true;
  auto tile = [&](int tx, int ty, int tw, int th, int repeatX, int repeatY) {
    opaque = true;
    for (int j = ty; j < ty + th && opaque; ++j)
      for (int i = tx; i < tx + tw && opaque; ++i)
        opaque = data[j * stride + i * 4 + 3] == 255;  // alpha, little-endian ARGB32
    cairo_surface_t* s = cairo_image_surface_create(opaque ? CAIRO_FORMAT_RGB24 : CAIRO_FORMAT_ARGB32,
                                                    tw * repeatX, th * repeatY);
    cairo_t* c = cairo_create(s);
    cairo_set_operator(c, CAIRO_OPERATOR_SOURCE);
    for (int j = 0; j < repeatY; ++j)
      for (int i = 0; i < repeatX; ++i) {
        cairo_set_source_surface(c, p.surface, i * tw - tx, j * th - ty);
        cairo_rectangle(c, i * tw, j * th, tw, th);
        cairo_fill(c);
      }
    cairo_destroy(c);
    cairo_pattern_t* pattern = cairo_pattern_create_for_surface(s);
    cairo_surface_destroy(s);
    cairo_pattern_set_filter(pattern, CAIRO_FILTER_NEAREST);
    cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
    return pattern;
  };
  constexpr int kRepeat = kTile / kMiddle;
  if (across) {
    p.tiles[0] = tile(x, 0, kMiddle, H, kRepeat, 1);
    p.opaqueAcross = opaque;
    bool flat = !opaque;
    for (int j = 0; j < H && flat; ++j) {
      const uint32_t* row = reinterpret_cast<const uint32_t*>(data + j * stride) + x;
      for (int i = 1; i < kMiddle && flat; ++i) flat = row[i] == row[0];
      if (!flat) break;
      if (p.runs.empty() || p.runs.back().argb != row[0])
        p.runs.push_back({j, j + 1, row[0]});
      else
        p.runs.back().y1 = j + 1;
    }
    if (!flat) p.runs.clear();
  }
  if (down) p.tiles[1] = tile(0, y, W, kMiddle, 1, kRepeat);
  if (across && down) {
    const uint32_t first = *reinterpret_cast<const uint32_t*>(data + y * stride + x * 4);
    bool flat = true;
    for (int j = y; j < y + kMiddle && flat; ++j) {
      const uint32_t* row = reinterpret_cast<const uint32_t*>(data + j * stride) + x;
      for (int i = 0; i < kMiddle && flat; ++i) flat = row[i] == first;
    }
    p.flatMiddle = flat;
    p.middleArgb = first;
    if (!flat) p.tiles[2] = tile(x, y, kMiddle, kMiddle, kRepeat / 2, kRepeat / 2);
  }
}

// A part's pixels in steps along each axis: a part's own columns (rows), or
// for a tiled axis one step of the middle's kMiddle. Its clear edges are left
// out; the largest rectangle of steps all opaque is its solid.
void MenuFinishes::cut(const Piece& p, const Part& a, const Part& d, Piece::Cut& c) {
  c.made = true;
  const unsigned char* data = cairo_image_surface_get_data(p.surface);
  const int stride = cairo_image_surface_get_stride(p.surface);
  const int middle = p.pad + p.corner;
  const int nx = a.tiled ? 1 : a.end - a.at, ny = d.tiled ? 1 : d.end - d.at;
  const int x0 = a.tiled ? middle : a.from, y0 = d.tiled ? middle : d.from;
  const int wx = a.tiled ? kMiddle : 1, wy = d.tiled ? kMiddle : 1;
  std::vector<int> height(nx + 1, 0);  // opaque steps up to this row, per column
  int kx0 = nx, ky0 = ny, kx1 = 0, ky1 = 0, best = 0;
  std::vector<int> stack;
  for (int j = 0; j < ny; ++j) {
    for (int i = 0; i < nx; ++i) {
      bool any = false, all = true;
      for (int v = 0; v < wy; ++v) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(data + (y0 + j + v) * stride) + x0 + i;
        for (int u = 0; u < wx; ++u) {
          any |= row[u] != 0;
          all &= (row[u] >> 24) == 255;
        }
      }
      if (any) kx0 = std::min(kx0, i), kx1 = std::max(kx1, i + 1), ky0 = std::min(ky0, j), ky1 = std::max(ky1, j + 1);
      height[i] = all ? height[i] + 1 : 0;
    }
    // The largest rectangle under the heights ending on this row.
    stack.clear();
    for (int i = 0; i <= nx; ++i) {
      while (!stack.empty() && height[stack.back()] >= height[i]) {
        const int h = height[stack.back()];
        stack.pop_back();
        const int left = stack.empty() ? 0 : stack.back() + 1;
        if (h * (i - left) > best) {
          best = h * (i - left);
          c.solid[0] = left, c.solid[1] = j + 1 - h, c.solid[2] = i, c.solid[3] = j + 1;
        }
      }
      stack.push_back(i);
    }
  }
  if (kx1 > kx0) c.keep[0] = kx0, c.keep[1] = ky0, c.keep[2] = kx1, c.keep[3] = ky1;
}

// The pattern's image without alpha, on the same pixels: drawing an opaque
// part of it is a copy.
cairo_pattern_t* MenuFinishes::solidView(cairo_pattern_t* pattern) {
  cairo_surface_t* image = nullptr;
  cairo_pattern_get_surface(pattern, &image);
  cairo_surface_t* view = cairo_image_surface_create_for_data(
      cairo_image_surface_get_data(image), CAIRO_FORMAT_RGB24, cairo_image_surface_get_width(image),
      cairo_image_surface_get_height(image), cairo_image_surface_get_stride(image));
  cairo_pattern_t* out = cairo_pattern_create_for_surface(view);
  cairo_surface_destroy(view);
  cairo_pattern_set_filter(out, CAIRO_FILTER_NEAREST);
  cairo_pattern_set_extend(out, cairo_pattern_get_extend(pattern));
  return out;
}

namespace {

// The panel's gradient: its stops, top to bottom.
constexpr double kPanelStops[3][4] = {{0, 0x1d / 255.0, 0x21 / 255.0, 0x29 / 255.0},
                                      {0.55, 0x18 / 255.0, 0x1b / 255.0, 0x22 / 255.0},
                                      {1, 0x15 / 255.0, 0x18 / 255.0, 0x1e / 255.0}};

// A row's colour (texture pixels) in a gradient from top to bottom (layout
// pixels), 8 bits each as RGBA, as MenuFinishes::fillRows() fills it.
uint32_t rowColour(int row, const MenuFinishes::Stop* stops, int count, double top, double bottom) {
  const double t = std::clamp(((row + 0.5) / kScale - top) / (bottom - top), 0.0, 1.0);
  int k = 0;
  while (k + 2 < count && t > stops[k + 1].at) ++k;
  const MenuFinishes::Stop &a = stops[k], &b = stops[std::min(k + 1, count - 1)];
  const double f = b.at > a.at ? std::clamp((t - a.at) / (b.at - a.at), 0.0, 1.0) : 0.0;
  uint32_t packed = 0;
  for (int c = 0; c < 4; ++c)
    packed = packed << 8 | static_cast<uint32_t>(std::lround((a.rgba[c] + (b.rgba[c] - a.rgba[c]) * f) * 255));
  return packed;
}

const MenuFinishes::Stop* panelStops() {
  static MenuFinishes::Stop stops[3];
  static bool made = false;
  if (!made) {
    for (int i = 0; i < 3; ++i)
      stops[i] = {kPanelStops[i][0], {kPanelStops[i][1], kPanelStops[i][2], kPanelStops[i][3], 1}};
    made = true;
  }
  return stops;
}

// x · a / 255 for 8 bits, rounded as pixman does.
uint32_t mul255(uint32_t x, uint32_t a) {
  const uint32_t t = x * a + 0x80;
  return (t + (t >> 8)) >> 8;
}

}  // namespace

void MenuFinishes::draw(cairo_t* cr, Finish finish, double x, double y, double w, double h,
                        double r, const Behind* behind) {
  // An opaque finish's insides are copied already (cut()).
  if (!behind || !behind->paint || shapeOf(finish).opaque || cairo_get_operator(cr) != CAIRO_OPERATOR_OVER ||
      cairo_get_group_target(cr) != cairo_get_target(cr)) {
    drawParts(cr, finish, x, y, w, h, r, behind);
    return;
  }
  cairo_matrix_t user;
  cairo_get_matrix(cr, &user);
  const int box[4] = {static_cast<int>(std::lround(x * kScale)), static_cast<int>(std::lround(y * kScale)),
                      static_cast<int>(std::lround((x + w) * kScale)),
                      static_cast<int>(std::lround((y + h) * kScale))};
  const int wR = box[2] - box[0], hR = box[3] - box[1];
  if (user.xx != 1 || user.yy != 1 || user.xy != 0 || user.yx != 0 || user.x0 != 0 || user.y0 != 0 ||
      wR <= 0 || hR <= 0 || static_cast<size_t>(wR) * hR * 4 > kPlacedBytes / 4) {
    drawParts(cr, finish, x, y, w, h, r, behind);
    return;
  }
  const int radius = static_cast<int>(std::lround(std::clamp(r * kScale, 0.0, std::min(wR, hR) / 2.0)));
  const Placed& copy = placed(finish, x, y, w, h, r, *behind, radius, box);
  drawParts(cr, finish, x, y, w, h, r, behind, copy.pattern ? &copy : nullptr);
}

void MenuFinishes::release(Placed& p) {
  if (p.pattern) cairo_pattern_destroy(p.pattern);
  if (p.surface) cairo_surface_destroy(p.surface);
}

// The box as it is drawn on the panel: the panel painted under it as on the
// canvas, then the finish's parts over it.
const MenuFinishes::Placed& MenuFinishes::placed(Finish finish, double x, double y, double w,
                                                 double h, double r, const Behind& behind,
                                                 int radius, const int box[4]) {
  for (const Placed& p : placed_)
    if (p.finish == finish && p.radius == radius && std::equal(box, box + 4, p.box) &&
        p.behind == &behind && p.top == behind.top && p.bottom == behind.bottom)
      return p;
  const int W = box[2] - box[0], H = box[3] - box[1];
  size_t bytes = static_cast<size_t>(W) * H * 4;
  for (const Placed& p : placed_)
    if (p.surface) bytes += static_cast<size_t>(p.box[2] - p.box[0]) * (p.box[3] - p.box[1]) * 4;
  if (placed_.size() >= kPlacedMax || bytes > kPlacedBytes) {  // places seen long ago: again if needed
    for (Placed& p : placed_) release(p);
    placed_.clear();
  }
  Placed p{finish, radius, {box[0], box[1], box[2], box[3]}, &behind, behind.top, behind.bottom};
  p.surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
  // The canvas's user space (layout pixels) on it, its pixels where they are there.
  cairo_surface_set_device_scale(p.surface, kScale, kScale);
  cairo_surface_set_device_offset(p.surface, -box[0], -box[1]);
  cairo_t* c = cairo_create(p.surface);
  behind.paint(c);
  drawParts(c, finish, x, y, w, h, r, &behind);
  cairo_destroy(c);
  cairo_surface_flush(p.surface);
  const unsigned char* data = cairo_image_surface_get_data(p.surface);
  const int stride = cairo_image_surface_get_stride(p.surface);
  bool opaque = true;
  for (int j = 0; j < H && opaque; ++j)
    for (int i = 0; i < W && opaque; ++i) opaque = data[j * stride + i * 4 + 3] == 255;
  if (opaque) {  // else drawn as anywhere
    cairo_surface_t* view = cairo_image_surface_create_for_data(
        cairo_image_surface_get_data(p.surface), CAIRO_FORMAT_RGB24, W, H, stride);
    p.pattern = cairo_pattern_create_for_surface(view);
    cairo_surface_destroy(view);
    cairo_pattern_set_filter(p.pattern, CAIRO_FILTER_NEAREST);
    cairo_matrix_t m;
    cairo_matrix_init(&m, kScale, 0, 0, kScale, -box[0], -box[1]);
    cairo_pattern_set_matrix(p.pattern, &m);
  } else {
    cairo_surface_destroy(p.surface);
    p.surface = nullptr;
  }
  placed_.push_back(p);
  return placed_.back();
}

void MenuFinishes::drawParts(cairo_t* cr, Finish finish, double x, double y, double w, double h,
                             double r, const Behind* behind, const Placed* copy) {
  const int x0 = static_cast<int>(std::lround(x * kScale)), x1 = static_cast<int>(std::lround((x + w) * kScale));
  const int y0 = static_cast<int>(std::lround(y * kScale)), y1 = static_cast<int>(std::lround((y + h) * kScale));
  const int wR = x1 - x0, hR = y1 - y0;
  if (wR <= 0 || hR <= 0) return;
  const int radius = static_cast<int>(std::lround(std::clamp(r * kScale, 0.0, std::min(wR, hR) / 2.0)));
  // A box is drawn in parts: the corners and their shadows, and between
  // them its middle tiled (not down a gradient), so that boxes of other
  // widths (or heights) share the piece. One too small for that is drawn
  // whole, from a piece of its own size.
  const Shape& shape = shapeOf(finish);
  const int corner = radius + shape.edge;
  const bool sliceX = wR > 2 * corner + kMiddle;
  const bool sliceY = !shape.gradient && hR > 2 * corner + kMiddle;
  Piece& p = piece(finish, radius, sliceX ? 2 * corner + kMiddle : wR,
                   sliceY ? 2 * corner + kMiddle : hR);
  // Along each axis the parts.
  auto parts = [&](bool slice, int start, int size, Part out[3]) {
    const int pad = p.pad;
    if (!slice) {
      out[0] = {0, start - pad, start + size + pad, false};
      return 1;
    }
    out[0] = {0, start - pad, start + corner, false};
    out[1] = {0, start + corner, start + size - corner, true};
    out[2] = {pad + corner + kMiddle, start + size - corner, start + size + pad, false};
    return 3;
  };
  Part across[3], down[3];
  const int nx = parts(sliceX, x0, wR, across), ny = parts(sliceY, y0, hR, down);
  // Only the parts in what is being redrawn.
  double clipX0 = 0, clipY0 = 0, clipX1 = 0, clipY1 = 0;
  cairo_clip_extents(cr, &clipX0, &clipY0, &clipX1, &clipY1);
  const int clip[4] = {static_cast<int>(std::floor(clipX0 * kScale)), static_cast<int>(std::floor(clipY0 * kScale)),
                       static_cast<int>(std::ceil(clipX1 * kScale)), static_cast<int>(std::ceil(clipY1 * kScale))};
  const bool over = cairo_get_operator(cr) == CAIRO_OPERATOR_OVER;
  auto flatRows = [&](const Part& a, const Part& d) {
    return a.tiled && !d.tiled && !p.runs.empty() &&
           (a.end - a.at) * (p.h + 2 * p.pad) >= 2000 * static_cast<int>(p.runs.size());
  };
  auto drawPart = [&](const Part& a, const Part& d, int i, int j) {
    // From the canvas's user space (layout pixels) to the piece's pixels,
    // one to one: the tiles start where their part does.
    if (flatRows(a, d)) {  // the middle across: its rows flat
      const int shift = d.at - d.from;
      const int top = std::max<double>(d.at, std::floor(clipY0 * kScale));
      const int bottom = std::min<double>(d.end, std::ceil(clipY1 * kScale));
      auto fill = [&](int r0, int r1, uint32_t c) {
        const double alpha = (c >> 24) / 255.0;
        if (r1 <= r0 || alpha <= 0) return;
        cairo_rectangle(cr, a.at / kScale, r0 / kScale, (a.end - a.at) / kScale, (r1 - r0) / kScale);
        cairo_set_source_rgba(cr, ((c >> 16) & 255) / 255.0 / alpha, ((c >> 8) & 255) / 255.0 / alpha,
                              (c & 255) / 255.0 / alpha, alpha);
        cairo_fill(cr);
      };
      for (const Piece::Run& run : p.runs) {
        const int r0 = std::max(top, run.y0 + shift), r1 = std::min(bottom, run.y1 + shift);
        const uint32_t c = run.argb;
        if (!behind || (c >> 24) == 255 || (c >> 24) == 0) {
          fill(r0, r1, c);
          continue;
        }
        // Over the panel's rows, opaque: split where the panel's colour changes.
        const uint32_t inverse = 255 - (c >> 24);
        auto over = [&](int row) {
          const uint32_t panel = rowColour(row, panelStops(), 3, behind->top, behind->bottom) >> 8;
          uint32_t out = 0xff000000u;
          for (int shift8 : {16, 8, 0})
            out |= std::min(255u, ((c >> shift8) & 255) + mul255((panel >> shift8) & 255, inverse)) << shift8;
          return out;
        };
        int from = r0;
        uint32_t now = r0 < r1 ? over(r0) : 0;
        for (int row = r0 + 1; row <= r1; ++row) {
          const uint32_t next = row < r1 ? over(row) : ~now;
          if (next == now) continue;
          fill(from, row, now);
          from = row;
          now = next;
        }
      }
      return;
    }
    if (a.tiled && d.tiled && p.flatMiddle) {  // the middle both ways: one colour
      const uint32_t c = p.middleArgb;
      const double alpha = (c >> 24) / 255.0;
      if (alpha > 0) {
        cairo_rectangle(cr, a.at / kScale, d.at / kScale, (a.end - a.at) / kScale, (d.end - d.at) / kScale);
        cairo_set_source_rgba(cr, ((c >> 16) & 255) / 255.0 / alpha, ((c >> 8) & 255) / 255.0 / alpha,
                              (c & 255) / 255.0 / alpha, alpha);
        cairo_fill(cr);
      }
      return;
    }
    const int which = a.tiled && d.tiled ? 3 : a.tiled ? 1 : d.tiled ? 2 : 0;
    cairo_pattern_t* source = which == 3 ? p.tiles[2] : which == 0 ? p.pattern : p.tiles[which - 1];
    cairo_matrix_t m;
    cairo_matrix_init(&m, kScale, 0, 0, kScale, a.tiled ? -a.at : a.from - a.at,
                      d.tiled ? -d.at : d.from - d.at);
    auto fill = [&](cairo_pattern_t* from, int fx0, int fy0, int fx1, int fy1) {
      cairo_pattern_set_matrix(from, &m);
      cairo_set_source(cr, from);
      cairo_rectangle(cr, fx0 / kScale, fy0 / kScale, (fx1 - fx0) / kScale, (fy1 - fy0) / kScale);
      cairo_fill(cr);
    };
    Piece::Cut& c = p.cuts[nx == 1 ? 3 : i][ny == 1 ? 3 : j];
    if (which == 3 || !over) {
      fill(source, a.at, d.at, a.end, d.end);
      return;
    }
    if (!c.made) cut(p, a, d, c);
    // Offsets in the part (whole along a tiled axis) to the canvas's
    // pixels, within what is redrawn.
    int k[4], s[4];
    auto place = [&](const int in[4], int out[4]) {
      out[0] = a.tiled ? (in[2] > in[0] ? a.at : a.end) : a.at + in[0];
      out[2] = a.tiled ? a.end : a.at + in[2];
      out[1] = d.tiled ? (in[3] > in[1] ? d.at : d.end) : d.at + in[1];
      out[3] = d.tiled ? d.end : d.at + in[3];
      out[0] = std::max(out[0], clip[0]), out[1] = std::max(out[1], clip[1]);
      out[2] = std::min(out[2], clip[2]), out[3] = std::min(out[3], clip[3]);
      return out[2] > out[0] && out[3] > out[1];
    };
    if (!place(c.keep, k)) return;  // clear, or outside what is redrawn
    bool split = false;
    if (place(c.solid, s)) {
      s[0] = std::max(s[0], k[0]), s[1] = std::max(s[1], k[1]);
      s[2] = std::min(s[2], k[2]), s[3] = std::min(s[3], k[3]);
      // Worth a fill for each piece of the rest: one costs about as much
      // as blending 2000 pixels.
      const int rest = (s[1] > k[1]) + (s[3] < k[3]) + (s[0] > k[0]) + (s[2] < k[2]);
      split = s[2] > s[0] && s[3] > s[1] && (s[2] - s[0]) * (s[3] - s[1]) >= 2000 * rest;
    }
    if (!split) {
      fill(source, k[0], k[1], k[2], k[3]);
      return;
    }
    if (!p.solid[which]) p.solid[which] = solidView(source);
    fill(p.solid[which], s[0], s[1], s[2], s[3]);
    if (s[1] > k[1]) fill(source, k[0], k[1], k[2], s[1]);
    if (s[3] < k[3]) fill(source, k[0], s[3], k[2], k[3]);
    if (s[0] > k[0]) fill(source, k[0], s[1], s[0], s[3]);
    if (s[2] < k[2]) fill(source, s[2], s[1], k[2], s[3]);
  };
  cairo_save(cr);
  for (int j = 0; j < ny; ++j)
    for (int i = 0; i < nx; ++i) {
      const Part &a = across[i], &d = down[j];
      if (a.end <= a.at || d.end <= d.at || a.at / kScale >= clipX1 || a.end / kScale <= clipX0 ||
          d.at / kScale >= clipY1 || d.end / kScale <= clipY0)
        continue;
      // Within the box the part is copied as it is there (draw()), unless
      // it is flat rows, filled opaque already.
      const int in[4] = {copy ? std::max(a.at, copy->box[0]) : 0, copy ? std::max(d.at, copy->box[1]) : 0,
                         copy ? std::min(a.end, copy->box[2]) : 0, copy ? std::min(d.end, copy->box[3]) : 0};
      if (!copy || flatRows(a, d) || in[2] <= in[0] || in[3] <= in[1]) {
        drawPart(a, d, i, j);
        continue;
      }
      cairo_set_source(cr, copy->pattern);
      cairo_rectangle(cr, in[0] / kScale, in[1] / kScale, (in[2] - in[0]) / kScale, (in[3] - in[1]) / kScale);
      cairo_fill(cr);
      if (in[0] == a.at && in[1] == d.at && in[2] == a.end && in[3] == d.end) continue;
      cairo_save(cr);  // the rest of it as anywhere
      cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
      cairo_rectangle(cr, a.at / kScale, d.at / kScale, (a.end - a.at) / kScale, (d.end - d.at) / kScale);
      cairo_rectangle(cr, in[0] / kScale, in[1] / kScale, (in[2] - in[0]) / kScale, (in[3] - in[1]) / kScale);
      cairo_clip(cr);
      cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
      drawPart(a, d, i, j);
      cairo_restore(cr);
    }
  cairo_restore(cr);
}

namespace {


// The panel's stops for a panel drawn from top to bottom, its middle stop
// held across `flat`: how many.
int panelStopsFor(double top, double bottom, MenuFinishes::Flat flat, MenuFinishes::Stop out[4]) {
  const MenuFinishes::Stop* s = panelStops();
  if (flat.bottom <= flat.top || bottom <= top) {
    std::copy(s, s + 3, out);
    return 3;
  }
  const double a = std::clamp((flat.top - top) / (bottom - top), 0.0, 1.0);
  const double b = std::clamp((flat.bottom - top) / (bottom - top), a, 1.0);
  out[0] = s[0];
  out[1] = s[1];
  out[1].at = a;
  out[2] = s[1];
  out[2].at = b;
  out[3] = s[2];
  return 4;
}

// The panel's gradient for a panel whose drawn box is y0..y1.
cairo_pattern_t* panelGradient(double y0, double y1, MenuFinishes::Flat flat) {
  MenuFinishes::Stop stops[4];
  const int count = panelStopsFor(y0, y1, flat, stops);
  cairo_pattern_t* g = cairo_pattern_create_linear(0, y0, 0, y1);
  for (int i = 0; i < count; ++i)
    cairo_pattern_add_color_stop_rgb(g, stops[i].at, stops[i].rgba[0], stops[i].rgba[1], stops[i].rgba[2]);
  return g;
}

}  // namespace

// A gradient changes by a step of 8 bits every few rows: rows of one colour,
// each filled flat, cost what one flat fill does (a gradient's fill costs
// two to three times that).
void MenuFinishes::fillRows(cairo_t* cr, double x0, double y0, double x1, double y1,
                            const Stop* stops, int count, double top, double bottom) {
  double clipX0 = 0, clipY0 = 0, clipX1 = 0, clipY1 = 0;
  cairo_clip_extents(cr, &clipX0, &clipY0, &clipX1, &clipY1);
  const int first = static_cast<int>(std::floor(std::max(y0, clipY0) * kScale));
  const int end = static_cast<int>(std::ceil(std::min(y1, clipY1) * kScale));
  if (first >= end || x1 <= x0 || count < 1) return;
  auto colour = [&](int row) { return rowColour(row, stops, count, top, bottom); };
  int row = first;
  uint32_t now = colour(row);
  for (int next = first + 1; next <= end; ++next) {
    const uint32_t c = next < end ? colour(next) : ~now;
    if (c == now) continue;
    if (now & 255) {
      cairo_rectangle(cr, x0, row / kScale, x1 - x0, (next - row) / kScale);
      cairo_set_source_rgba(cr, (now >> 24) / 255.0, ((now >> 16) & 255) / 255.0,
                            ((now >> 8) & 255) / 255.0, (now & 255) / 255.0);
      cairo_fill(cr);
    }
    row = next;
    now = c;
  }
}

void MenuFinishes::fillPanel(cairo_t* cr, double x0, double y0, double x1, double y1, double top,
                             double bottom, Flat flat) {
  Stop stops[4];
  const int count = panelStopsFor(top, bottom, flat, stops);
  fillRows(cr, x0, y0, x1, y1, stops, count, top, bottom);
}

void MenuFinishes::panel(cairo_t* cr, double x, double y, double w, double h, double inset, double r,
                         Flat flat) {
  x += inset, y += inset, w -= 2 * inset, h -= 2 * inset;
  roundedPath(cr, x, y, w, h, r);
  cairo_pattern_t* g = panelGradient(y, y + h, flat);
  cairo_set_source(cr, g);
  cairo_fill(cr);
  cairo_pattern_destroy(g);
  // The lit top edge, as the finishes' lit lines (kLitLine): what the panel
  // moved down its rows doesn't cover of it, and over that its middle rows
  // again, up to their share.
  static_assert(kLitRows == 4 && kLitLine[0] == kLitLine[3] && kLitLine[1] == kLitLine[2],
                "the panel's lit edge draws kLitLine as its rows and its middle ones");
  constexpr double kLight = 0.06;
  const double outer = kLight * kLitLine[0];
  const double inner = (kLight * kLitLine[1] - outer) / (1 - outer);
  cairo_save(cr);
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
  auto rows = [&](int from, int to, double alpha) {
    roundedPath(cr, x, y + from / kScale, w, h - from / kScale, r);
    roundedPath(cr, x, y + to / kScale, w, h - to / kScale, r);
    cairo_set_source_rgba(cr, 1, 1, 1, alpha);
    cairo_fill(cr);
  };
  rows(0, kLitRows, outer);
  rows(1, kLitRows - 1, inner);
  cairo_restore(cr);
}
