#include "menu_style.h"

#include "value_adjust.h"

#include <algorithm>
#include <cmath>

// Adjusting a value row's field (GroupLayout::value()). Pressed anywhere on
// it, the value stays as it is: one with fine steps opens Magnify at once, a
// magnified scale over the row centred on the laser with the value under
// it, and moving along it from there goes in single steps, sliding on near
// its ends. One with wider steps follows the laser along its field from
// where it was pressed, snapping to them. Letting go keeps the value. Each
// change is sent at once, as a slider's.

namespace {

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

}  // namespace

MenuCommand Menu::valueCommand(int value) const {
  MenuCommand c;
  c.type = adjust_.type;
  c.option = adjust_.option;
  c.value = value;
  return c;
}

std::string Menu::valueText(int value, bool tick) const {
  return adjust_.format ? adjust_.format(value, tick) : std::to_string(value);
}

bool Menu::magnifies(const Button& b) {
  const double range = b.sliderMax - b.sliderMin;
  if (range <= 0 || b.w <= 0) return false;
  const double step = std::max(1, b.sliderStep);
  // A logarithmic field's steps are narrowest at its top end.
  const double px = b.sliderLog && b.sliderMin > 0
                        ? b.w * step / (b.sliderMax * std::log(double(b.sliderMax) / b.sliderMin))
                        : b.w * step / range;
  return px < adjust::kMagnifyBelow;
}

MenuCommand Menu::pressValue(int index) {
  const Button& b = buttons_[index];
  Adjust& a = adjust_;
  a = Adjust{};
  a.held = true;
  a.type = b.slider;
  a.option = b.sliderOption;
  a.fieldW = b.w;
  a.min = b.sliderMin;
  a.max = b.sliderMax;
  a.step = std::max(1, b.sliderStep);
  a.log = b.sliderLog && b.sliderMin > 0;
  a.mark = b.sliderDefault >= 0 ? b.sliderDefault : (a.min < 0 && a.max > 0 ? 0 : -1);
  a.format = b.sliderFormat;
  // Its name: the row's label, before its − and + (GroupLayout::value()).
  for (int i = index - 1; i >= 0 && i >= index - 3; --i)
    if (buttons_[i].staticText && buttons_[i].y < b.y + b.h && buttons_[i].y + buttons_[i].h > b.y)
      a.name = buttons_[i].label;
  // The row: from its group's inside edge to its + (the two buttons before the field: − and +).
  a.row[0] = b.x;
  a.row[1] = b.y;
  a.row[3] = b.h;
  double right = b.x + b.w;
  if (index >= 1 && buttons_[index - 1].stepper == 1)
    right = buttons_[index - 1].x + buttons_[index - 1].w;
  for (const Group& g : groups_)
    if (b.x >= g.x && b.x + b.w <= g.x + g.w && b.y >= g.y && b.y < g.y + g.h)
      a.row[0] = g.x + kit::kGroupPadX;
  a.row[2] = right - a.row[0];
  // Picked up as it is, wherever it was pressed: it moves only as the laser does.
  a.value = b.sliderValue;
  a.pressX = pointerX_;
  a.pressAt = clamp01(sliderFraction(b, b.sliderValue));
  a.last = steadySeconds();
  trackValue_ = a.value;
  if (tooltip_ >= 0 && tooltip_ < static_cast<int>(buttons_.size())) tooltipChanged(tooltip_);
  tooltip_ = -1;
  fadeTowards(1);
  if (magnifies(b)) openLoupe(pointerX_);
  return {};
}

MenuCommand Menu::dragValue(bool held) {
  Adjust& a = adjust_;
  if (!held) {
    releaseValue();
    return {};
  }
  const double now = steadySeconds();
  const double dt = std::clamp(now - a.last, 0.0, 0.05);
  a.last = now;
  if (pointerPanel_ != kBrowser) return {};
  const double x = pointerX_;
  const double range = a.max - a.min;
  double v;
  if (!a.open) {
    const double f = clamp01(a.pressAt + (x - a.pressX) / a.fieldW);
    v = a.log ? a.min * std::pow(double(a.max) / a.min, f) : a.min + f * range;
  } else {
    const double r0 = a.loupe[0] + adjust::kRulerInset, rw = a.loupe[2] - 2 * adjust::kRulerInset;
    // Its ends from kEdge in, or (pressed nearer an end than that) from
    // where it was pressed: holding still never slides it.
    constexpr double edge = adjust::kEdge;
    const double left = std::min(r0 + edge, a.pressX), right = std::max(r0 + rw - edge, a.pressX);
    const double e = x < left ? (x - left) / edge : x > right ? (x - right) / edge : 0;
    if (e != 0) {
      // Slid on, the faster the nearer the end, and the longer it is held there.
      if (a.edgeSince < 0) a.edgeSince = now;
      const double held = now - a.edgeSince - adjust::kPanBoostAfter;
      const double boost = held > 0 ? std::pow(2.0, held / adjust::kPanDouble) : 1;
      const double base = a.span * adjust::kPanRate;
      const double fastest = std::max(base * adjust::kPanMost, range / adjust::kPanRangeTime);
      const double rate =
          std::clamp(std::clamp(e, -adjust::kPanMost, adjust::kPanMost) * base * boost, -fastest, fastest);
      // A ruler opened past its range (openLoupe()) only slides back into it.
      a.lo = std::clamp(a.lo + rate * dt, std::min(double(a.min), a.lo), std::max(a.max - a.span, a.lo));
    } else {
      a.edgeSince = -1;
    }
    v = a.lo + clamp01((x - r0) / rw) * a.span;
  }
  const int value = std::clamp(a.min + static_cast<int>(std::lround((v - a.min) / a.step)) * a.step,
                               a.min, a.max);
  if (!a.open) {
    const bool middle = a.mark >= 0 || (a.min < 0 && a.max > 0);
    const int lo = a.min, hi = a.max;
    const bool log = a.log;
    feelTrack(value, lo, hi, middle, std::max(0, a.mark), [lo, hi, log](double v) {
      return log ? std::log(v / lo) / std::log(double(hi) / lo) : (v - lo) / std::max(1, hi - lo);
    });
  } else if (value != a.value) {
    feel(value == a.min || value == a.max ? Feel::Edge : Feel::Tick);
  }
  const bool changed = value != a.value;
  a.value = value;
  // The scale is drawn again as its value or its ruler moves (by a pixel).
  if (a.open && (value != a.drawnValue ||
                 std::abs(a.lo - a.drawnLo) * (a.loupe[2] - 2 * adjust::kRulerInset) / a.span >= 0.5))
    loupeChanged(false);
  return changed ? valueCommand(value) : MenuCommand{};
}

// Opens the scale centred on the laser (at x), its ruler placed so the value
// is under it: nothing jumps. At the range's ends the scale moves instead,
// and where the page stops it the ruler runs on past the range, blank. Its
// box is in whole layout pixels, where the panel shader cuts it out
// (held()) just as its finish is drawn.
void Menu::openLoupe(double x) {
  Adjust& a = adjust_;
  const double l = kPageX + kit::kPanelInset + adjust::kLoupeMargin;
  const double r = kPageX + kPageWidth - kit::kPanelInset - adjust::kLoupeMargin;
  const double w = std::floor(std::min(adjust::kLoupeW, r - l));
  const double rw = w - 2 * adjust::kRulerInset;
  const double range = a.max - a.min;
  a.span = std::min(range, double(a.step) * std::max<double>(adjust::kMinSteps,
                                                            std::round(rw / adjust::kStepPx)));
  double lx = std::clamp(x - w / 2, l, r - w);
  a.lo = std::clamp(a.value - (x - lx - adjust::kRulerInset) / rw * a.span, double(a.min),
                    a.max - a.span);
  // Where the value is on the clamped ruler: the scale moves to put it under the laser.
  const double at = lx + adjust::kRulerInset + (a.value - a.lo) / a.span * rw;
  const double want = lx + x - at;
  lx = std::round(std::clamp(want, l, r - w));
  if (lx != want) a.lo = a.value - (x - lx - adjust::kRulerInset) / rw * a.span;
  a.loupe[0] = lx;
  a.loupe[1] = std::round(a.row[1] + a.row[3] / 2 - adjust::kLoupeH / 2);
  a.loupe[2] = w;
  a.loupe[3] = adjust::kLoupeH;
  a.open = true;
  feel(Feel::Detent);
  loupeChanged(true);
}

void Menu::releaseValue() {
  Adjust& a = adjust_;
  if (a.open) loupeChanged(true);
  a.open = false;
  a.held = false;
  draggingSlider_ = MenuCommand::None;
  fadeTowards(0);
}

double Menu::heldFade() const {
  const double k = std::clamp((steadySeconds() - fadeAt_) / adjust::kFadeSeconds, 0.0, 1.0);
  return fadeFrom_ + (fadeTo_ - fadeFrom_) * k * k * (3 - 2 * k);
}

void Menu::fadeTowards(double to) {
  fadeFrom_ = heldFade();
  fadeTo_ = to;
  fadeAt_ = steadySeconds();
}

bool Menu::loupeBox(double r[4]) const {
  if (!adjust_.open) return false;
  for (int i = 0; i < 4; ++i) r[i] = adjust_.loupe[i];
  return true;
}

// What changes inside the scale as it moves: its ruler and its value, clear
// of its rounded corners (drawLoupeOnly()).
void Menu::loupeInside(double r[4]) const {
  // 10 in from its sides (the needle at either end of the ruler, and its
  // shadow), 12 from its top, 6 from its bottom: 2 inside its corners.
  // Whole pixels, as browserChanged() takes them.
  const double* l = adjust_.loupe;
  r[0] = std::ceil(l[0] + 10), r[1] = std::ceil(l[1] + 12);
  r[2] = std::floor(l[0] + l[2] - 10), r[3] = std::floor(l[1] + l[3] - 6);
}

void Menu::loupeChanged(bool all) {
  const double* r = adjust_.loupe;
  if (all) {
    const double reach = MenuFinishes::reach(Finish::Pop) + adjust::kLoupeUnder;
    browserChanged(r[1] - reach, r[1] + r[3] + reach, r[0] - reach, r[0] + r[2] + reach);
  } else {
    double in[4];
    loupeInside(in);
    if (adjust_.lo == adjust_.drawnLo) {  // the ruler as it was: only the needle moved
      double was[2], now[2];
      loupeNeedle(adjust_.drawnValue, was);
      loupeNeedle(adjust_.value, now);
      in[0] = std::max(in[0], std::floor(std::min(was[0], now[0])));
      in[2] = std::min(in[2], std::ceil(std::max(was[1], now[1])));
    }
    browserChanged(in[1], in[3], in[0], in[2]);
  }
  adjust_.drawnValue = adjust_.value;
  adjust_.drawnLo = adjust_.lo;
}

// Where the needle for `value` is on the ruler as it is, and its value
// above it (left edge and width).
void Menu::loupeValue(int value, double& needle, double& textX, double& textW) const {
  const Adjust& a = adjust_;
  const double x = a.loupe[0], w = a.loupe[2];
  const double rx = x + adjust::kRulerInset, rw = w - 2 * adjust::kRulerInset;
  needle = rx + (value - a.lo) / a.span * rw;
  textW = textWidth(valueText(value, false), adjust::kValueSize, true) + 4;
  const double name = loupeNameW();
  const double left = x + adjust::kRulerInset + (name > 0 ? name + adjust::kNameGap : 0);
  textX = std::clamp(needle - textW / 2, std::min(left, x + w - adjust::kRulerInset - textW),
                     x + w - adjust::kRulerInset - textW);
}

// The setting's name on the scale: as wide as it is, up to its share.
double Menu::loupeNameW() const {
  const Adjust& a = adjust_;
  if (a.name.empty()) return 0;
  return std::min(textWidth(a.name, adjust::kNameSize, false) + 2, a.loupe[2] * adjust::kNameShare);
}

// The columns that show `value`: its needle with its shadow, its value
// above it, and the fill up to it.
void Menu::loupeNeedle(int value, double x[2]) const {
  double needle, textX, textW;
  loupeValue(value, needle, textX, textW);
  const double reach = 2 + MenuFinishes::reach(Finish::Head) + 1;  // half the needle, its shadow
  x[0] = std::min(needle - reach, textX - 2);
  x[1] = std::max(needle + reach, textX + textW + 2);
}

bool Menu::drawLoupeOnly(cairo_t* cr) const {
  if (!adjust_.open) return false;
  double x0, y0, x1, y1, in[4];
  cairo_clip_extents(cr, &x0, &y0, &x1, &y1);
  loupeInside(in);
  if (x0 < in[0] || y0 < in[1] || x1 > in[2] || y1 > in[3]) return false;
  drawLoupe(cr);  // the page under it doesn't show
  return true;
}

int Menu::pointedField() const {
  const int n = static_cast<int>(buttons_.size());
  if (adjust_.held || hovered_ < 0 || hovered_ >= n) return -1;
  const Button& b = buttons_[hovered_];
  const int field = b.magnify ? hovered_ : b.stepper < 0 ? hovered_ + 2 : b.stepper > 0 ? hovered_ + 1 : -1;
  if (field < 2 || field >= n || !buttons_[field].magnify || buttons_[field].disabled) return -1;
  return field;
}

MenuCommand Menu::stepValue(int direction) {
  const int field = pointedField();
  if (field < 0) return {};
  const Button& f = buttons_[field];
  const Button& step = buttons_[direction < 0 ? field - 2 : field - 1];
  if (!step.action || step.disabled) return {};
  const bool end = direction < 0 ? f.sliderValue - f.sliderStep <= f.sliderMin
                                 : f.sliderValue + f.sliderStep >= f.sliderMax;
  feel(end ? Feel::Edge : Feel::Tick);
  const std::function<MenuCommand()> action = step.action;  // a copy: it may lay the page out again
  return action();
}

// The magnified scale: a raised surface with the value over a sunk ruler,
// its steps as ticks (labelled at round values as far apart as their labels
// need), the default marked, filled to the value from the range's start (or
// its middle, for a value that goes both ways), and the needle.
void Menu::drawLoupe(cairo_t* cr) const {
  const Adjust& a = adjust_;
  if (!a.open) return;
  const Palette& pal = palette();
  const double x = a.loupe[0], y = a.loupe[1], w = a.loupe[2], h = a.loupe[3];
  // Its surface also a little way around it, under its edge: the panel
  // shader shows the scale to its edge, smoothed across a fragment and
  // sampled from coarser mip levels, which would otherwise take in the
  // faded page around it (the rows above and below) along its outline.
  // Not when only its inside is redrawn, as it moves (drawLoupeOnly()).
  constexpr double m = adjust::kLoupeUnder;
  double c0, c1, c2, c3;
  cairo_clip_extents(cr, &c0, &c1, &c2, &c3);
  if (c0 < x || c1 < y || c2 > x + w || c3 > y + h)
    drawFinish(cr, Finish::Pop, x - m, y - m, w + 2 * m, h + 2 * m, adjust::kLoupeCorner + m);
  drawFinish(cr, Finish::Pop, x, y, w, h, adjust::kLoupeCorner);
  const double rx = x + adjust::kRulerInset, rw = w - 2 * adjust::kRulerInset;
  const double ry = y + adjust::kRulerTop, rh = adjust::kRulerH;
  drawFinish(cr, Finish::Sunk, rx, ry, rw, rh, adjust::kRulerCorner);
  const double lo = a.lo, hi = a.lo + a.span;
  auto at = [&](double v) { return rx + (v - lo) / a.span * rw; };
  cairo_save(cr);
  roundedRect(cr, rx, ry, rw, rh, adjust::kRulerCorner);
  cairo_clip(cr);
  // The fill.
  const double from = a.min < 0 && a.max > 0 ? 0 : a.min;
  const double f0 = std::max(lo, std::min(from, double(a.value)));
  const double f1 = std::min(hi, std::max(from, double(a.value)));
  if (f1 > f0) {
    cairo_rectangle(cr, at(f0), ry, at(f1) - at(f0), rh);
    setColor(cr, pal.accent, 0.3);
    cairo_fill(cr);
  }
  // Ticks at every step, the larger ones labelled: as many steps apart as
  // their labels need room.
  const double stepPx = rw * a.step / a.span;
  const double labelW = std::max(textWidth(valueText(a.min, true), adjust::kLabelSize, false),
                                 textWidth(valueText(a.max, true), adjust::kLabelSize, false));
  int every = 1;
  for (int n : {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000}) {
    every = n;
    if (n * stepPx >= labelW + 16) break;
  }
  const int first = std::max(0, static_cast<int>(std::ceil((lo - a.min) / a.step - 1e-6)));
  const int last = std::min((a.max - a.min) / a.step,
                            static_cast<int>(std::floor((hi - a.min) / a.step + 1e-6)));
  for (int k = first; k <= last; ++k) {
    const int v = a.min + k * a.step;
    const bool major = (v / a.step) % every == 0;
    const double tx = at(v), th = major ? 22 : 11;
    cairo_rectangle(cr, tx - 1, ry + rh - th, 2, th);
    cairo_set_source_rgba(cr, 1, 1, 1, major ? 0.7 : 0.32);
    cairo_fill(cr);
    if (major) {
      const std::string label = valueText(v, true);
      const double lw = textWidth(label, adjust::kLabelSize, false) + 4;
      drawText(cr, label, tx - lw / 2, ry + 3, lw, 26, adjust::kLabelSize, false, true, 1, 1, 1, 0.62);
    }
  }
  if (a.mark > lo && a.mark < hi) {
    cairo_rectangle(cr, at(a.mark) - 1.5, ry, 3, rh);
    setColor(cr, pal.textAccent, 0.9);
    cairo_fill(cr);
  }
  cairo_restore(cr);
  // The setting's name, the needle, and the value over it.
  if (const double nameW = loupeNameW(); nameW > 0)
    drawText(cr, a.name, x + adjust::kRulerInset, y + 12, nameW, 40, adjust::kNameSize, false, false,
             pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a);
  double nx, vx, vw;
  loupeValue(a.value, nx, vx, vw);
  drawHead(cr, nx, ry - 4, rh + 8);
  drawText(cr, valueText(a.value, false), vx, y + 12, vw, 40, adjust::kValueSize, true, true, 1, 1, 1);
}
