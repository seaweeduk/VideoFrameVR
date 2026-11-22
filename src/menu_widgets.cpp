#include "menu_style.h"

#include "menu_list_parts.h"
#include "menu_stash.h"
#include "menu_stash_covers.h"
#include "thumbnailer.h"
#include "value_adjust.h"

#include <pango/pangocairo.h>

// Buttons and sliders: building them, laying them out, drawing them.

double Menu::textWidth(const std::string& text, int size, bool bold) const {
  // Measured again for the same labels on every redraw (while scrolling, every frame).
  const auto known = textWidths_.find(std::pair<std::string_view, int>(text, size * 2 + bold));
  if (known != textWidths_.end()) return known->second;
  if (textWidths_.size() > 4096) textWidths_.clear();
  PangoLayout* layout = pango_cairo_create_layout(canvases_[kMain].cr());
  PangoFontDescription* font = pango_font_description_from_string(bold ? kFontBold : kFont);
  pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
  pango_layout_set_font_description(layout, font);
  pango_layout_set_text(layout, text.c_str(), -1);
  int w = 0, h = 0;
  pango_layout_get_pixel_size(layout, &w, &h);
  pango_font_description_free(font);
  g_object_unref(layout);
  textWidths_.emplace(std::make_pair(text, size * 2 + bold), w);
  return w;
}

Menu::Button Menu::iconButton(Icon icon, std::function<MenuCommand()> action, bool selected) const {
  Button b;
  b.icon = icon;
  b.w = kIconButtonW;
  b.selected = selected;
  b.action = std::move(action);
  return b;
}

Menu::Button Menu::textButton(const std::string& label, Icon icon,
                              std::function<MenuCommand()> action, bool selected) const {
  Button b;
  b.label = label;
  b.icon = icon;
  b.w = textWidth(label, kTextSize, false) + (icon != Icon::None ? 70 + kIconSize * 0.75 + 20 : 50);
  b.selected = selected;
  b.action = std::move(action);
  return b;
}

Menu::Button& Menu::addRound(Icon icon, const std::string& tooltip,
                             std::function<MenuCommand()> action, double x, double y,
                             double size, bool selected) {
  Button b = iconButton(icon, std::move(action), selected);
  b.x = x;
  b.y = y;
  b.w = b.h = size;
  b.radius = size / 2;
  b.tooltip = tooltip;
  buttons_.push_back(b);
  return buttons_.back();
}

// Stash's sort choices below their opener, spilling into columns if needed.
void Menu::addDropdownBelow(const Button& opener, std::vector<Button> entries, bool oneColumn) {
  double w = 0;
  for (const Button& e : entries) w = std::max(w, textWidth(e.label, 26, false));
  const int count = static_cast<int>(entries.size());
  double top = opener.y + opener.h + 16;
  int rows = std::max(1, static_cast<int>((kBrowserHeight - kPagePad - top) / 64));
  if (oneColumn) {
    // The page as shown: a grouped page is laid out unscrolled, then moved up.
    const double scrolled = groupedPage() ? pageScroll_ : 0;
    if (top + count * 64 - 8 > kBrowserHeight - kPagePad + scrolled)
      top = opener.y - 16 - (count * 64 - 8);
    rows = count;
  }
  for (size_t i = 0; i < entries.size(); ++i) {
    Button& e = entries[i];
    e.panel = opener.panel;
    e.popup = true;
    e.fontSize = 26;
    e.radius = kit::kCorner;
    e.x = opener.x + (i / rows) * (w + 60 + kGap);
    e.y = top + (i % rows) * 64;
    e.w = w + 60;
    e.h = 56;
    buttons_.push_back(e);
  }
}

// A slider's whole width is its range.
MenuCommand Menu::sliderAtPointer(const Button& slider) const {
  const double x0 = slider.x, x1 = slider.x + slider.w;
  const double fraction = std::clamp((pointerX_ - x0) / (x1 - x0), 0.0, 1.0);
  const double value = slider.sliderLog
                           ? slider.sliderMin * std::pow(double(slider.sliderMax) / slider.sliderMin, fraction)
                           : slider.sliderMin + fraction * (slider.sliderMax - slider.sliderMin);
  MenuCommand c;
  c.type = slider.slider;
  c.option = slider.sliderOption;
  c.value = std::clamp(slider.sliderMin +
                          std::round((value - slider.sliderMin) / slider.sliderStep) * slider.sliderStep,
                      double(slider.sliderMin), double(slider.sliderMax));
  return c;
}

// Where `value` is along the slider, 0..1 (as sliderAtPointer() places it).
double Menu::sliderFraction(const Button& slider, double value) {
  return slider.sliderLog
             ? std::log(value / slider.sliderMin) / std::log(double(slider.sliderMax) / slider.sliderMin)
             : (value - slider.sliderMin) / std::max(1, slider.sliderMax - slider.sliderMin);
}

MenuCommand Menu::volumeAtPointer(const Button& slider) const {
  MenuCommand c;
  c.type = MenuCommand::SetVolume;
  c.value = std::round(std::clamp((pointerX_ - slider.x) / slider.w, 0.0, 1.0) * value_range::kVolume.most);
  return c;
}

// The volume: a track across the slider's box, its knob at the volume; the
// number after it, right-aligned, faint while muted.
void Menu::drawVolumeSlider(cairo_t* cr, const Button& b) const {
  const Palette& pal = palette();
  const bool muted = view_.playback.muted;
  const double fraction = std::clamp(view_.playback.volume / value_range::kVolume.most, 0.0, 1.0);
  drawTrack(cr, b.x, b.x + b.w, b.y + b.h / 2, fraction, muted ? 0.4 : 1.0);
  const std::string number = std::to_string(static_cast<int>(std::round(view_.playback.volume)));
  const double right = b.x + b.w + kit::kSpace + strip::kNumberW;
  const double w = textWidth(number, strip::kNumberSize, false);
  const Rgba& ink = muted ? pal.faint : pal.text;
  drawText(cr, number, right - w - 2, b.y, w + 4, b.h, strip::kNumberSize, false, false, ink.r,
           ink.g, ink.b);
}

namespace {

// The custom accent's hues around the circle, as readable as the presets
// (Custom's swatch, the Hue slider's track).
constexpr int kHueSteps = 24;
const AccentColours& hueColour(int step) {
  static AccentColours hues[kHueSteps];
  static bool made = false;
  if (!made) {
    for (int i = 0; i < kHueSteps; ++i) hues[i] = accentColours({kAccentCustom, i * 360 / kHueSteps, 50});
    made = true;
  }
  return hues[step % kHueSteps];
}

}  // namespace

// A value: a well filled in the accent to it, a white head there, the
// value on it; the pointer's x along it sets it.
void Menu::drawSlider(cairo_t* cr, const Button& b) const {
  const double radius = std::min(kit::kCorner, b.h / 2);
  if (b.slider == MenuCommand::SetAccentHue) {
    drawFinish(cr, Finish::Sunk, b.x, b.y, b.w, b.h, radius);
    // The colours to choose from along the well, a knob on the chosen one.
    constexpr double in = kit::kSpacePiece;
    cairo_pattern_t* rainbow = cairo_pattern_create_linear(b.x + in, 0, b.x + b.w - in, 0);
    for (int i = 0; i < kHueSteps; ++i) {  // where sliderAtPointer() puts each hue
      const AccentColours& c = hueColour(i);
      cairo_pattern_add_color_stop_rgb(rainbow, std::min(1.0, i * 360.0 / kHueSteps / b.sliderMax),
                                       c.fill[0], c.fill[1], c.fill[2]);
    }
    roundedRect(cr, b.x + in, b.y + in, b.w - 2 * in, b.h - 2 * in, std::max(0.0, radius - in));
    cairo_set_source(cr, rainbow);
    cairo_fill(cr);
    cairo_pattern_destroy(rainbow);
    const double r = std::min(kit::kSliderKnob, b.h - 2 * in) / 2;
    const double cx = std::clamp(b.x + b.w * b.sliderValue / std::max(1, b.sliderMax), b.x + in + r,
                                 b.x + b.w - in - r);
    drawFinish(cr, Finish::SliderKnob, cx - r, b.y + b.h / 2 - r, 2 * r, 2 * r, r);
    return;
  }
  // Filled to the value: from the middle for a value that goes both ways.
  const double fraction = std::clamp(sliderFraction(b, b.sliderValue), 0.0, 1.0);
  const double at = b.x + b.w * fraction;
  const double from = b.sliderMin < 0 && b.sliderMax > 0 ? b.x + b.w * sliderFraction(b, 0) : b.x;
  // The well only where the fill doesn't cover it (and its lit edges), as
  // drawAccentFill() snaps the fill's ends.
  {
    constexpr double s = menu_geometry::kRasterScale;
    const bool left = from == b.x;
    const double x0 = left ? b.x : std::round(std::min(from, at) * s) / s;
    const double x1 = std::round(std::max(from, at) * s) / s;
    const bool skip = left ? x1 > b.x + radius : x1 - x0 >= 1;
    if (skip) {
      const double reach = MenuFinishes::reach(Finish::Sunk) + 2;
      const double l = b.x - reach, r = b.x + b.w + reach, y = b.y - reach, h = b.h + 2 * reach;
      cairo_save(cr);
      if (!left) cairo_rectangle(cr, l, y, x0 - l, h);
      if (r > x1) cairo_rectangle(cr, x1, y, r - x1, h);
      cairo_rectangle(cr, left ? l : x0, y, x1 - (left ? l : x0), reach);
      cairo_rectangle(cr, left ? l : x0, b.y + b.h, x1 - (left ? l : x0), reach);
      cairo_clip(cr);
    }
    drawFinish(cr, Finish::Sunk, b.x, b.y, b.w, b.h, radius);
    if (skip) cairo_restore(cr);
  }
  drawAccentFill(cr, std::min(from, at), b.y, std::abs(at - from), b.h, 0.88, radius, b.w, b.x);
  if (b.sliderDefault >= 0) {  // the marked stop, on its bottom edge
    const double x = b.x + b.w * (b.sliderDefault - b.sliderMin) / std::max(1, b.sliderMax - b.sliderMin);
    cairo_rectangle(cr, std::clamp(x, b.x + radius, b.x + b.w - radius) - 1, b.y + b.h - 12, 2, 12);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.4);
    cairo_fill(cr);
  }
  drawHead(cr, at, b.y + 8, b.h - 16);
  // The value at its right end; where the head would run into it, just left of the head.
  constexpr double in = kit::kSpace, clear = 12;
  const std::string value = b.sliderText.empty() ? std::to_string(b.sliderValue) : b.sliderText;
  const double valueW = std::min(b.w - 2 * in, textWidth(value, kTextSize, false) + 2);
  double right = b.x + b.w - in;
  if (at + clear > right - valueW && at - clear - valueW >= b.x + in) right = at - clear;
  drawText(cr, value, right - valueW, b.y, valueW, b.h, kTextSize, false, false, 1, 1, 1);
}

// The left w of a well `full` wide (corners r) filled in the accent, lit
// along its top (the band, a value's well, a slider's track).
void Menu::drawAccentFill(cairo_t* cr, double x, double y, double w, double h, double alpha,
                          double r, double full, double wellX) const {
  if (w < 0.5) return;
  constexpr double s = menu_geometry::kRasterScale;
  if (alpha > 0.85) {  // the whole well's fill, cut off at w: a few copies of a piece
    if (wellX < 0) wellX = x;
    cairo_save(cr);
    const double x0 = x == wellX ? std::floor(x * s) / s - 1 : std::round(x * s) / s;
    const double x1 = std::round((x + w) * s) / s;
    cairo_rectangle(cr, x0, std::floor(y * s) / s - 1, x1 - x0, std::ceil(h * s) / s + 2);
    cairo_clip(cr);
    drawFinish(cr, Finish::Filled, wellX, y, full, h, r);
    cairo_restore(cr);
    return;
  }
  // Faded (a muted volume): its gradient, at alpha.
  const AccentColours::Gradient& f = accent_.filled;
  cairo_pattern_t* g = cairo_pattern_create_linear(0, y, 0, y + h);
  cairo_pattern_add_color_stop_rgba(g, 0, f.top[0], f.top[1], f.top[2], alpha);
  cairo_pattern_add_color_stop_rgba(g, 1, f.bottom[0], f.bottom[1], f.bottom[2], alpha);
  barPath(cr, x, y, w, h, r, full);
  cairo_set_source(cr, g);
  cairo_fill(cr);
  cairo_pattern_destroy(g);
}

// A value's head: a slim white bar w wide, x its middle, from y down h.
void Menu::drawHead(cairo_t* cr, double x, double y, double h, double w) const {
  drawFinish(cr, Finish::Head, x - w / 2, y, w, h, w / 2);
}

// A disc in the accent colour it chooses, Custom's with the hues around it,
// lit along its top and resting on the panel; the chosen one ringed in white.
void Menu::drawSwatch(cairo_t* cr, const Button& b) const {
  // In a well (swatches): a dot of 30, the chosen one a little larger.
  const double cx = b.x + b.w / 2, cy = b.y + b.h / 2;
  const double r = b.piece ? 15 * (b.selected ? 1.12 : 1) : std::min(b.w, b.h) / 2 - 6;
  cairo_arc(cr, cx, cy + 1, r + 0.5, 0, 2 * M_PI);
  cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
  cairo_fill(cr);
  if (b.swatch == kAccentCustom) {
    for (int i = 0; i < kHueSteps; ++i) {
      const AccentColours& c = hueColour(i);
      cairo_move_to(cr, cx, cy);
      cairo_arc(cr, cx, cy, r, 2 * M_PI * i / kHueSteps, 2 * M_PI * (i + 1) / kHueSteps + 0.02);
      cairo_close_path(cr);
      cairo_set_source_rgb(cr, c.fill[0], c.fill[1], c.fill[2]);
      cairo_fill(cr);
    }
  } else {
    const AccentColours c = accentColours({b.swatch, 0, 50});
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_set_source_rgb(cr, c.fill[0], c.fill[1], c.fill[2]);
    cairo_fill(cr);
  }
  cairo_save(cr);  // the lit top: what the disc moved down 1.5 doesn't cover
  cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
  cairo_clip(cr);
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
  cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
  cairo_new_sub_path(cr);
  cairo_arc(cr, cx, cy + 1.5, r, 0, 2 * M_PI);
  cairo_set_source_rgba(cr, 1, 1, 1, 0.28);
  cairo_fill(cr);
  cairo_restore(cr);
  if (b.on && !b.piece) {
    cairo_arc(cr, cx, cy, r + 3.5, 0, 2 * M_PI);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_line_width(cr, 3);
    cairo_stroke(cr);
  }
}

// A background key's swatch: its colour (0xRRGGBB, display values), or the
// green keyer's greens, lit to shaded, as a green screen's are; with
// nothing removed, a grey circle crossed out; with no colour yet, an empty
// dashed ring.
void Menu::drawKeySwatch(cairo_t* cr, int colour, double cx, double cy, double r) const {
  cairo_new_path(cr);  // not joined to where the last text ended
  cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
  if (colour == kPendingKeySwatch) {
    const double dash = M_PI * r / 6;  // twelve dashes around
    setColor(cr, palette().text, 0.6);
    cairo_set_line_width(cr, 2.5);
    cairo_set_dash(cr, &dash, 1, 0);
    cairo_stroke(cr);
    cairo_set_dash(cr, nullptr, 0, 0);
    return;
  }
  if (colour == kNoKeySwatch) {
    setColor(cr, palette().text, 0.15);
    cairo_fill_preserve(cr);
    const double d = r * M_SQRT1_2;
    cairo_move_to(cr, cx - d, cy + d);
    cairo_line_to(cr, cx + d, cy - d);
    setColor(cr, palette().text, 0.6);
    cairo_set_line_width(cr, 2.5);
    cairo_stroke(cr);
    return;
  }
  if (colour == kGreenKeySwatch) {
    cairo_pattern_t* greens = cairo_pattern_create_linear(cx, cy - r, cx, cy + r);
    cairo_pattern_add_color_stop_rgb(greens, 0, 0.45, 0.92, 0.35);
    cairo_pattern_add_color_stop_rgb(greens, 1, 0.04, 0.38, 0.1);
    cairo_set_source(cr, greens);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(greens);
  } else {
    cairo_set_source_rgb(cr, ((colour >> 16) & 255) / 255.0, ((colour >> 8) & 255) / 255.0,
                         (colour & 255) / 255.0);
    cairo_fill_preserve(cr);
  }
  setColor(cr, palette().text, 0.5);  // an edge, so that a dark colour shows
  cairo_set_line_width(cr, 2);
  cairo_stroke(cr);
}

// A slider from x0 to x1 at height y: a thin sunk track filled in the accent
// (by alpha) to `fraction`, and a knob there.
void Menu::drawTrack(cairo_t* cr, double x0, double x1, double y, double fraction,
                     double alpha) const {
  constexpr double h = kit::kSliderTrackH, knob = kit::kSliderKnob;
  const double at = x0 + (x1 - x0) * fraction;
  drawFinish(cr, Finish::Track, x0, y - h / 2, x1 - x0, h, h / 2);
  drawAccentFill(cr, x0, y - h / 2, at - x0, h, alpha, h / 2, x1 - x0);
  drawFinish(cr, Finish::SliderKnob, at - knob / 2, y - knob / 2, knob, knob, knob / 2);
}

namespace {

bool outsideClip(cairo_t* cr, double x, double y, double w, double h) {
  constexpr double kSlack = 4;  // glyphs a little out of their box
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  cairo_clip_extents(cr, &x0, &y0, &x1, &y1);
  return (w > 0 && (x - kSlack >= x1 || x + w + kSlack <= x0)) || y >= y1 || y + h <= y0;
}

}  // namespace

void Menu::drawText(cairo_t* cr, const std::string& text, double x, double y, double w, double h,
                    int size, bool bold, bool center, double r, double g, double b,
                    double a) const {
  // Nothing to lay out where none of it shows (a strip of a list being
  // redrawn): the text stays within its width, and about its height.
  const double tall = std::max(h, size * 2.0);
  if (text.find('\n') == std::string::npos && outsideClip(cr, x, y + (h - tall) / 2, w, tall))
    return;
  PangoLayout* layout = textLayout(cr, text, size, bold, w, 0, center);
  int textW = 0, textH = 0;
  pango_layout_get_pixel_size(layout, &textW, &textH);
  cairo_set_source_rgba(cr, r, g, b, a);
  cairo_move_to(cr, x, onPixel(y + (h - textH) / 2));
  pango_cairo_show_layout(cr, layout);
}

void Menu::drawTextEnd(cairo_t* cr, const std::string& text, double x, double y, double w, double h, int size,
                       bool bold, double r, double g, double b, double a) const {
  if (w <= 0 || outsideClip(cr, x, y + (h - std::max(h, size * 2.0)) / 2, w, std::max(h, size * 2.0))) return;
  PangoLayout* layout = textLayout(cr, text, size, bold, w, 1, false);
  int textW = 0, textH = 0;
  pango_layout_get_pixel_size(layout, &textW, &textH);
  cairo_set_source_rgba(cr, r, g, b, a);
  cairo_move_to(cr, x, onPixel(y + (h - textH) / 2));
  pango_cairo_show_layout(cr, layout);
}

namespace {

// A file's or folder's name as laid out over lines: a break allowed after
// `_`, `.` and `-` and before `(` and `[` (a zero-width space there), as
// between words.
std::string breakableName(const std::string& name) {
  static const std::string kBreak = "\xe2\x80\x8b";  // U+200B
  std::string out;
  out.reserve(name.size() + name.size() / 4);
  for (size_t i = 0; i < name.size(); ++i) {
    const char c = name[i];
    if ((c == '(' || c == '[') && i > 0 && name[i - 1] != ' ') out += kBreak;
    out += c;
    if ((c == '_' || c == '.' || c == '-') && i + 1 < name.size() && name[i + 1] != ' ') out += kBreak;
  }
  return out;
}

}  // namespace

PangoLayout* Menu::textLayout(cairo_t* cr, const std::string& text, int size, bool bold, double w,
                              int lines, bool center, bool fileName) const {
  const int width = w < 0 ? -1 : static_cast<int>(w * PANGO_SCALE);
  fileName = fileName && lines > 1;
  std::string key = text;
  key += '\x1f' + std::to_string(size * 4 + bold * 2 + center) + ' ' + std::to_string(width) +
         ' ' + std::to_string(lines) + (fileName ? " f" : "");
  const auto known = textLayouts_.find(key);
  if (known != textLayouts_.end()) {
    pango_cairo_update_layout(cr, known->second);
    return known->second;
  }
  if (textLayouts_.size() > 2048) {
    for (const auto& layout : textLayouts_) g_object_unref(layout.second);
    textLayouts_.clear();
  }
  PangoLayout* layout = pango_cairo_create_layout(cr);
  PangoFontDescription* font = pango_font_description_from_string(bold ? kFontBold : kFont);
  pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
  pango_layout_set_font_description(layout, font);
  pango_font_description_free(font);
  if (fileName) {
    // Cut inside a word only where one is longer than the line, and then
    // without a hyphen the name doesn't have.
    pango_layout_set_text(layout, breakableName(text).c_str(), -1);
    PangoAttrList* attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_insert_hyphens_new(FALSE));
    pango_layout_set_attributes(layout, attrs);
    pango_attr_list_unref(attrs);
  } else {
    pango_layout_set_text(layout, text.c_str(), -1);
  }
  pango_layout_set_width(layout, width);
  if (lines > 0) {
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_height(layout, -lines);
  }
  pango_layout_set_ellipsize(layout, lines > 0 ? PANGO_ELLIPSIZE_END : PANGO_ELLIPSIZE_MIDDLE);
  pango_layout_set_alignment(layout, center ? PANGO_ALIGN_CENTER : PANGO_ALIGN_LEFT);
  textLayouts_.emplace(std::move(key), layout);
  return layout;
}

void Menu::drawButtons(cairo_t* cr, int panel, bool popups) {
  // Only those reaching into the clip (a part of the panel redrawn, such as
  // a preview frame), with room for what a button draws out of its box: its
  // finish's shadow, a badge, text taller than its row, a label's lines below
  // its top. Sideways only a badge and a shadow reach out: the scroll bar's
  // track and the side column's mark are redrawn alone, without the rows
  // beside them.
  constexpr double kReach = kButtonReach, kReachX = kButtonReachX;
  double clipX0 = 0, clipY0 = 0, clipX1 = 0, clipY1 = 0;
  cairo_clip_extents(cr, &clipX0, &clipY0, &clipX1, &clipY1);
  for (size_t i = 0; i < buttons_.size(); ++i) {
    const Button& b = buttons_[i];
    if (b.panel != panel || b.popup != popups) continue;
    if (b.hoverOnly && hovered_ != static_cast<int>(i) && hovered_ != static_cast<int>(i) - 1)
      continue;
    const double above = b.staticText ? b.fontSize : 0;
    const double below = b.staticText ? std::max(b.h, b.lines * b.fontSize * 1.5) : b.h;
    Finish finish = Finish::Raised;
    const bool surface = buttonFinish(b, finish);
    const double shadow = surface ? MenuFinishes::reach(finish) : 0;
    // A list's rows and cards keep within their boxes (a preview frame
    // redraws its own row alone).
    const bool inside = (b.listRow || b.card) && !b.staticText && b.badge.empty() && !surface;
    const double reachX = inside ? 1 : std::max(kReachX, shadow);
    const double reach = inside ? 1 : std::max(kReach, shadow);
    if (b.x - reachX >= clipX1 || b.x + b.w + reachX <= clipX0 ||
        b.y - above - reach >= clipY1 || b.y + below + reach <= clipY0)
      continue;
    // Rows of a smoothly scrolled list are cut off at the list's edges.
    struct Clip {
      cairo_t* cr;
      bool on;
      ~Clip() {
        if (on) cairo_restore(cr);
      }
    } clip{cr, b.scrolls || b.side};
    if (b.scrolls) {
      cairo_save(cr);
      cairo_rectangle(cr, 0, listTop_ - 2, kBrowserWidth, listBottom_ - listTop_ + 2);
      cairo_clip(cr);
    } else if (b.side) {
      cairo_save(cr);
      cairo_rectangle(cr, side_.x, side_.y, side_.w, side_.h);
      cairo_clip(cr);
    }
    const Palette& pal = palette();
    const double fade = b.disabled ? 0.4 : 1.0;
    if (b.staticText) {
      const Rgba& c = b.faint ? pal.faint : b.dim ? pal.textDim : pal.text;
      const double rgba[4] = {c.r, c.g, c.b, c.a * fade};
      double x = b.x, w = b.w;
      if (b.lamp > 0) {  // a state's lamp: ok, busy or failed (Lamp)
        const Rgba& lamp = b.lamp == 1 ? pal.ok : b.lamp == 2 ? pal.gold : pal.danger;
        const double cx = b.x + 6, cy = b.y + b.h / 2;
        cairo_arc(cr, cx, cy, 9, 0, 2 * M_PI);
        setColor(cr, lamp, 0.16 * fade);
        cairo_fill(cr);
        cairo_arc(cr, cx, cy, 6, 0, 2 * M_PI);
        setColor(cr, lamp, fade);
        cairo_fill(cr);
        x += 24;
        w -= 24;
      }
      if (b.lines > 1) {
        drawTextLines(cr, b.label, x, b.y, w, b.fontSize, b.lines, rgba, false);
      } else {
        drawText(cr, b.label, x, b.y, w, b.h, b.fontSize, false, false, rgba[0], rgba[1],
                 rgba[2], rgba[3]);
      }
      continue;
    }
    if (!b.action && !b.selected && !b.well && !b.listLook) {  // list heading
      drawText(cr, b.label, b.x + 8, b.y, b.w - 16, b.h, kTextSize, true, false, pal.textDim.r,
               pal.textDim.g, pal.textDim.b);
      continue;
    }
    double ink[4];
    buttonInk(b, ink);
    const bool pressed = drawnPressed(b);
    if (pressed && (b.danger || b.dangerHover)) ink[0] = ink[1] = ink[2] = ink[3] = 1;  // white on red
    // Faded as a whole: what draws in one paint is drawn faded as it is,
    // anything else in a temporary surface kept to its box.
    const bool group = b.disabled && !fadesInPlace(b, surface);
    if (group) {
      cairo_save(cr);
      cairo_rectangle(cr, b.x - reach, b.y - above - reach, b.w + 2 * reach,
                      above + below + 2 * reach);
      cairo_clip(cr);
      cairo_push_group(cr);
    }
    const double radius =
        b.radius >= 0 ? std::min(b.radius, std::min(b.w, b.h) / 2) : kButtonRadius;
    if (pressed) {
      drawPressed(cr, b, surface ? finish : Finish::Pressed, radius);
    } else if (surface) {
      // Straight on the strip's panel: not a piece in its well, not faded
      // (drawn apart from the panel first).
      const bool onStrip = b.panel == kMain && !b.popup && !b.piece && !b.disabled;
      drawFinish(cr, finish, b.x, b.y, b.w, b.h, radius, onStrip ? stripPanel() : nullptr);
    }
    if (pressed && pressShrinks(b)) {  // its content shrinks with it
      const double cx = b.x + b.w / 2, cy = b.y + b.h / 2;
      cairo_save(cr);
      cairo_translate(cr, cx, cy);
      cairo_scale(cr, kPressedScale, kPressedScale);
      cairo_translate(cr, -cx, -cy);
      if (!b.well) drawButtonContent(cr, b, ink);
      cairo_restore(cr);
    } else if (!b.well) {
      drawButtonContent(cr, b, ink, group ? 1 : fade);
    }
    if (b.popup && b.selected && !b.key && !b.piece && !filled(b)) {  // the chosen entry of an open list: a dot at its end
      cairo_arc(cr, b.x + b.w - 20, b.y + b.h / 2, 4, 0, 2 * M_PI);
      setColor(cr, pal.accent);
      cairo_fill(cr);
    }
    if (!b.badge.empty()) {  // a count on an accent pill, at the top-right edge (a navigation row's end)
      const double h = 30, w = std::max(h, textWidth(b.badge, 19, true) + 16);
      const double cx = b.nav ? b.x + b.w - 32 : b.x + b.w - w * 0.3, cy = b.nav ? b.y + b.h / 2 : b.y + h * 0.3;
      roundedRect(cr, cx - w / 2, cy - h / 2, w, h, h / 2);
      setColor(cr, pal.accent);
      cairo_fill(cr);
      drawText(cr, b.badge, cx - w / 2, cy - h / 2, w, h, 19, true, true, 1, 1, 1);
    }
    if (group) {
      cairo_pop_group_to_source(cr);
      cairo_paint_with_alpha(cr, fade);
      cairo_restore(cr);
    }
  }
}

// Pressed controls are drawn here, pushed in as the menu design's are, and
// redrawn as the trigger presses and lets go (pressChanged()): the panel
// shader can't take the raised finish (its lit edge, its shadow) out of the
// canvas. Lists' rows and what draws itself are pressed by the shader.
bool Menu::pressDrawn(const Button& b) const {
  if (b.keyCap) return !b.disabled;  // on setup's panel too
  return (b.panel == kMain || b.panel == kBrowser) && !b.scrolls && !b.side && !b.card &&
         !b.stashLook && !b.offlineLook && b.ratingPath.empty() && b.swatch < 0 && b.keySwatch < 0 &&
         b.slider == MenuCommand::None && !b.volumeSlider && !b.band && !b.chip && !b.field &&
         !b.staticText && !b.disabled && !b.well && b.action;
}

bool Menu::drawnPressed(const Button& b) const {
  // A key: while it is down (keyDown_: held on it, and a moment after a
  // quick click, long enough to be seen).
  if (b.keyCap) return pressDrawn(b) && keyDown_.at >= 0 && keyDown_.x == b.x && keyDown_.y == b.y;
  return pressSpot_.panel == b.panel && pressSpot_ == HoverSpot{b.panel, b.x, b.y, b.w, b.h} &&
         pressDrawn(b);
}

// Buttons and keys shrink a little as they are pushed in; rows, list items,
// tabs and the pieces of a choice only sink.
bool Menu::pressShrinks(const Button& b) {
  return !(b.toggle || b.nav || b.piece || b.listItem || b.listRow || b.tab ||
           (b.popup && !b.raised));
}

// Pushed in: shrunk (pressShrinks()), its surface sunk with a shadow inside
// its top edge and none outside; one filled in the accent or red keeps its
// colour, darker. `rest`: its finish at rest.
void Menu::drawPressed(cairo_t* cr, const Button& b, Finish rest, double radius) const {
  const double s = pressShrinks(b) ? kPressedScale : 1;
  const double w = b.w * s, h = b.h * s, x = b.x + (b.w - w) / 2, y = b.y + (b.h - h) / 2;
  const double r = radius * s;
  if (b.danger || b.dangerHover || rest == Finish::DangerFilled) {
    roundedRect(cr, x, y, w, h, r);
    cairo_set_source_rgb(cr, 0xc4 / 255.0, 0x35 / 255.0, 0x3a / 255.0);
    cairo_fill(cr);
  } else if (rest == Finish::Primary) {
    const AccentColours::Gradient& g = accent_.pressed;
    cairo_pattern_t* fill = cairo_pattern_create_linear(0, y, 0, y + h);
    cairo_pattern_add_color_stop_rgb(fill, 0, g.top[0], g.top[1], g.top[2]);
    cairo_pattern_add_color_stop_rgb(fill, 1, g.bottom[0], g.bottom[1], g.bottom[2]);
    roundedRect(cr, x, y, w, h, r);
    cairo_set_source(cr, fill);
    cairo_fill(cr);
    cairo_pattern_destroy(fill);
  } else if (rest == Finish::Latched) {
    roundedRect(cr, x, y, w, h, r);
    setColor(cr, palette().accentSoft);
    cairo_fill(cr);
  }
  drawFinish(cr, Finish::Pressed, x, y, w, h, r);
}

// Which depth a button has at rest (menu_finish.h): flush buttons and those
// that draw themselves (sliders, stars, swatches) have none.
bool Menu::buttonFinish(const Button& b, Finish& finish) const {
  if (b.staticText || b.scrollbar || b.volumeSlider || b.slider != MenuCommand::None ||
      (b.swatch >= 0 && !b.piece) || !b.ratingPath.empty() || b.chip || b.band)
    return false;
  if (b.well) finish = Finish::Sunk;
  else if (b.danger) finish = Finish::DangerFilled;
  else if (b.primary) finish = Finish::Primary;
  else if (b.latched) finish = Finish::Latched;
  else if (b.piece || b.nav) {
    if (!(b.piece ? b.selected : b.on)) return false;  // flush unless chosen
    finish = Finish::Chosen;
  } else if ((b.listRow || b.card) && b.selected && !b.stashLook) {
    finish = Finish::Chosen;  // the playing video's row or card: raised
  } else if (b.listRow || b.card || (b.popup && !b.raised) || b.toggle || b.bare)
    return false;
  else if (b.selected) finish = Finish::Primary;
  else if (b.field) finish = Finish::Sunk;
  else if (!b.action) return false;
  else finish = Finish::Raised;
  return true;
}

void Menu::buttonInk(const Button& b, double ink[4]) const {
  const Palette& pal = palette();
  Rgba k = pal.text;
  if (b.piece && !b.selected) {
    k = pal.textDim;  // a piece in a well, not chosen
  } else if ((b.listRow || b.card || (b.popup && !b.piece)) && b.selected) {
    k = pal.textAccent;  // the playing file, a list's chosen entry (cards also get an outline)
  } else if (b.latched) {
    k = pal.textAccent;
  } else if (b.bare && b.on && !b.nav && !b.toggle) {
    k = pal.textAccent;  // a chosen icon or heading
  } else if (b.dimInk) {
    k = pal.textDim;
  }
  if (b.placeholder) k = pal.faint;
  // The ink comes from the finish: on a filled button (its colour, pointed
  // and pressed too) white, as the kit's .primary and .fill-danger.
  if (filled(b)) k = {1, 1, 1, 1};
  ink[0] = k.r, ink[1] = k.g, ink[2] = k.b, ink[3] = k.a;
}

bool Menu::filled(const Button& b) const {
  Finish finish;
  return buttonFinish(b, finish) && (finish == Finish::Primary || finish == Finish::DangerFilled);
}

// Icons are dim on navigation rows and in lists, in the accent on a chosen
// row, piece or item.
void Menu::iconInk(const Button& b, const double ink[4], double out[4]) const {
  const Palette& pal = palette();
  const Rgba* k = nullptr;
  if (filled(b)) {  // as its text
    for (int i = 0; i < 4; ++i) out[i] = ink[i];
    return;
  }
  if ((b.nav && b.on) || (b.piece && b.selected) || (b.listItem && b.selected)) k = &pal.textAccent;
  else if (b.nav || b.listItem) k = &pal.textDim;
  if (!k) {
    for (int i = 0; i < 4; ++i) out[i] = ink[i];
    return;
  }
  out[0] = k->r, out[1] = k->g, out[2] = k->b, out[3] = k->a;
}

// The pointed control raised a step (a flush one gains a surface), lit, its
// shadow wider; a destructive one red; pressed, pushed in. The panel shader
// draws it over the canvas (shaders/panel.frag), so pointing redraws nothing.
bool Menu::hoverLook(PanelId panel, PanelHover& look) const {
  // A value held, and as the menu comes back: nothing is pointed, the
  // browser fades away but the held row and Magnify's scale.
  const double fade = heldFade();
  if (adjust_.held || fade > 0) {
    if (panel != kBrowser) return false;
    look.fade = std::max(1e-4f, static_cast<float>(fade));  // above 0 from the press on
    const double* row = adjust_.row;
    const float rect[4] = {float(row[0]), float(row[1]), float(row[0] + row[2]), float(row[1] + row[3])};
    std::copy(rect, rect + 4, look.rect);
    look.radius = static_cast<float>(adjust::kRowCorner);
    double box[4];
    if (loupeBox(box)) {
      const float clip[4] = {float(box[0]), float(box[1]), float(box[0] + box[2]), float(box[1] + box[3])};
      std::copy(clip, clip + 4, look.clip);
      look.scaleRadius = static_cast<float>(adjust::kLoupeCorner);
    } else {
      std::fill(look.clip, look.clip + 4, 0.0f);
    }
    return true;
  }
  return pointedEase_.look(panel, look);
}

void Menu::easeHover(double now, double at) {
  if (adjust_.held || heldFade() > 0) {  // nothing is pointed meanwhile
    pointedEase_.reset();
    return;
  }
  PointedEase::Spot spot;
  PanelHover pointed, pressed;
  bool held = false, canvas = false;
  if (hovered_ >= 0 && hovered_ < static_cast<int>(buttons_.size())) {
    const Button& b = buttons_[hovered_];
    if (pointedLooks(static_cast<PanelId>(b.panel), pointed, pressed, held, canvas))
      spot = {b.panel, b.x, b.y, b.w, b.h};
  }
  // While a list moves under the pointer its rows switch at once; a control
  // left that is no longer there (its list moved, a new page) goes at once.
  bool snap = steadySeconds() - scrolledAt_ < menuFeel().scrollSettle;
  if (!snap && spot.panel < 0 && pointedEase_.spot().panel >= 0) {
    const PointedEase::Spot& last = pointedEase_.spot();
    snap = std::none_of(buttons_.begin(), buttons_.end(), [&last](const Button& b) {
      return PointedEase::Spot{b.panel, b.x, b.y, b.w, b.h} == last;
    });
  }
  pointedEase_.update(spot, pointed, pressed, held, canvas, snap, now, at);
}

bool Menu::pointedLooks(PanelId panel, PanelHover& pointed, PanelHover& pressed, bool& held,
                        bool& canvas) const {
  bool danger = false;
  if (!hoverRect(panel, pointed.rect, pointed.radius, danger, pointed.clip)) return false;
  const Button& b = buttons_[hovered_];
  Finish finish = Finish::Raised;
  const bool surface = buttonFinish(b, finish);
  auto set = [](PanelHover& look, const double rgb[3], double alpha, double highlight, double shadow) {
    for (int i = 0; i < 3; ++i) look.fill[i] = static_cast<uint8_t>(std::lround(rgb[i] * 255));
    look.fill[3] = static_cast<uint8_t>(std::lround(alpha * 255));
    look.highlight = static_cast<uint8_t>(std::lround(highlight * 255));
    look.shadow = static_cast<uint8_t>(std::lround(shadow * 255));
  };
  static constexpr double kWhite[3] = {1, 1, 1}, kBlack[3] = {0, 0, 0};
  static constexpr double kRed[3] = {0xe3 / 255.0, 0x4b / 255.0, 0x50 / 255.0};
  static constexpr double kRedDown[3] = {0xc4 / 255.0, 0x35 / 255.0, 0x3a / 255.0};
  // An unrated row's stars (after it, hidden at rest by drawRating()).
  if (b.listRow && !b.stashLook)
    for (size_t i = hovered_ + 1; i < buttons_.size() && i <= static_cast<size_t>(hovered_) + 4; ++i) {
      const Button& stars = buttons_[i];
      if (stars.listRow || stars.card) break;
      if (stars.listLook == static_cast<int>(ListLook::RowStars) && stars.rating <= 0 &&
          !ratingDrag_.is(stars)) {
        pointed.stars = static_cast<float>(stars.x);
        break;
      }
    }
  pressed = pointed;
  const HoverSpot spot{b.panel, b.x, b.y, b.w, b.h};
  held = triggerHeld_ && spot == pressSpot_;
  canvas = held && pressDrawn(b);  // the canvas has it pushed in
  pressed.pressed = true;
  if (danger || finish == Finish::DangerFilled) {
    set(pressed, kRedDown, 1, 0, 1);
    pressed.ink = 255;
  } else if (surface && finish == Finish::Primary) {
    set(pressed, accent_.pressed.top, 0.85, 0, 1);
    pressed.ink = 255;  // its text and icon stay white on the accent
  } else if (b.popup || b.piece) {
    set(pressed, kBlack, 0.18, 0, 0.8);
  } else {
    set(pressed, kBlack, 0.2, 0, 1);
  }
  if (danger) {
    set(pointed, kRed, 1, 0.22, 1);
    pointed.ink = 255;
  } else if (b.piece && !b.selected) {  // in a well, on a page or a dialog
    set(pointed, kWhite, 0.06, 0, 0);
  } else if (b.popup && !surface) {
    set(pointed, kWhite, 0.08, 0, 0);
  } else if (b.field || b.band) {
    set(pointed, kWhite, 0.10, 0, 0);
    pointed.ring = true;
  } else if (!b.ratingPath.empty() && !b.card) {
    set(pointed, kWhite, 0.05, 0, 0);  // stars: only lit, their own pointed look is white
  } else if (!surface) {
    set(pointed, kWhite, 0.135, 0.15, 1);  // flush: raised from the panel
    pointed.ink = b.dimInk ? 255 : 0;      // its dim text lit
  } else if (finish == Finish::Chosen) {
    set(pointed, kWhite, 0, 0, 1);
  } else if (finish == Finish::Latched) {
    set(pointed, accent_.fill, 0.10, 0, 0);
  } else if (finish == Finish::Primary) {
    set(pointed, kWhite, 0.10, 0.06, 1);
  } else if (finish == Finish::DangerFilled) {
    set(pointed, kWhite, 0.08, 0, 1);
  } else {
    set(pointed, kWhite, 0.06, 0.05, 1);  // raised a step higher
  }
  return true;
}

namespace {

// The colour that, drawn at `alpha`, gives where it covers a pixel whole what
// it gives drawn at 1 into a temporary surface painted at `alpha` (cairo
// keeps a colour in 16 bits and pixman a surface in 8: rounded twice there).
// Partly covered pixels at its edges can still differ by a level.
void fadedColour(const double rgb[3], double alpha, double out[3]) {
  const auto to8 = [](double v) { return static_cast<int>(std::floor(v * 65535.0 + 0.5)) >> 8; };
  const int a = to8(alpha);
  for (int i = 0; i < 3; ++i) {
    int x = to8(rgb[i]) * a + 128;
    x = (x + (x >> 8)) >> 8;  // pixman's division by 255
    out[i] = (x * 256 + 128) / 65535.0 / alpha;
  }
}

}  // namespace

void Menu::drawButtonContent(cairo_t* cr, const Button& b, const double ink[4], double alpha) const {
  if (b.controlPart >= 0) {
    drawControlPart(cr, b, ink);
    return;
  }
  if (b.offlineLook) {
    drawOfflineButton(cr, b, ink);
    return;
  }
  if (b.stashLook) {
    StashPages::drawButton(*this, cr, b, ink);
    return;
  }
  {
    if (!b.ratingPath.empty()) {
      drawRating(cr, b);
      return;
    }
    if (b.listLook) {
      drawListPart(cr, b, ink);
      return;
    }
    if (b.volumeSlider) {
      drawVolumeSlider(cr, b);
      return;
    }
    if (b.slider != MenuCommand::None) {
      drawSlider(cr, b);
      return;
    }
    if (b.swatch >= 0) {
      drawSwatch(cr, b);
      return;
    }
    if (b.select) {  // a list's button: what is chosen, from the left, and a caret
      constexpr double caret = 24;
      double x = b.x + kit::kSpace;
      const double end = b.x + b.w - kit::kSpace - caret - 12;
      if (b.keySwatch != -1) {  // a key's colour before it
        const double r = 12;
        drawKeySwatch(cr, b.keySwatch, x + r, b.y + b.h / 2, r);
        x += 2 * r + 12;
      }
      drawText(cr, b.label, x, b.y, end - x, b.h, b.fontSize > 0 ? b.fontSize : kTextSize, false,
               false, ink[0], ink[1], ink[2]);
      drawIcon(cr, Icon::Caret, b.x + b.w - kit::kSpace - caret / 2, b.y + b.h / 2, caret, ink[0],
               ink[1], ink[2]);
      return;
    }
    if (b.scrollbar) {
      drawScrollbar(cr, b);
      return;
    }
    if (b.dialog) {  // a dialog's button: its icon and label centred
      constexpr double iconSize = 26, gap = 12;
      double icon[4];
      iconInk(b, ink, icon);
      const int size = b.fontSize > 0 ? b.fontSize : kTextSize;
      const double textW = std::min(textWidth(b.label, size, false),
                                    b.w - 2 * kit::kSpace - (b.icon != Icon::None ? iconSize + gap : 0));
      const double iconW = b.icon != Icon::None ? iconSize + gap : 0;
      const double x = b.x + (b.w - iconW - textW) / 2;
      if (b.icon != Icon::None)
        drawIcon(cr, b.icon, x + iconSize / 2, b.y + b.h / 2, iconSize, icon[0], icon[1], icon[2]);
      drawText(cr, b.label, x + iconW, b.y, textW + 4, b.h, size, false, false, ink[0], ink[1], ink[2]);
      return;
    }
    if (b.field && b.on) {  // a text field being typed into: an accent ring inside its edge
      const Palette& pal = palette();
      const double r = (b.radius >= 0 ? std::min(b.radius, std::min(b.w, b.h) / 2) : kButtonRadius);
      roundedRect(cr, b.x + 1, b.y + 1, b.w - 2, b.h - 2, std::max(0.0, r - 1));
      setColor(cr, pal.accent);
      cairo_set_line_width(cr, 2);
      cairo_stroke(cr);
    }
    if (b.field && b.icon == Icon::None) {  // a form's field: its text from the left
      drawFieldText(cr, b, ink);
      return;
    }
    double icon[4];
    iconInk(b, ink, icon);
    if (b.tab) {  // a strip tab, or Quit: its icon and label
      const double iconX = b.x + kit::kSpace, textX = iconX + strip::kTabIcon + strip::kTabIconGap;
      drawIcon(cr, b.icon, iconX + strip::kTabIcon / 2, b.y + b.h / 2, strip::kTabIcon, icon[0],
               icon[1], icon[2]);
      drawText(cr, b.label, textX, b.y, b.x + b.w - textX - kit::kSpace + 4, b.h, b.fontSize, false,
               false, ink[0], ink[1], ink[2]);
      return;
    }
    if (b.chip) {  // a label on the strip: red, or in the accent
      const Palette& pal = palette();
      roundedRect(cr, b.x, b.y, b.w, b.h, b.radius);
      setColor(cr, b.chip == 1 ? pal.danger : pal.accentSoft, b.chip == 1 ? 0.16 : 1);
      cairo_fill(cr);
      const Rgba k = b.chip == 1 ? hexColour(0xff8a8d) : pal.textAccent;
      drawText(cr, b.label, b.x + strip::kChipPad, b.y, b.w - 2 * strip::kChipPad + 4, b.h,
               b.fontSize, false, false, k.r, k.g, k.b);
      return;
    }
    if (b.band) return;  // drawn by drawMainPage()
    if (b.listItem) {  // its icon, label, and the second text at its right end
      double x = b.x + kit::kSpace;
      if (b.icon != Icon::None) {
        drawIcon(cr, b.icon, x + strip::kTabIcon / 2, b.y + b.h / 2, strip::kTabIcon, icon[0],
                 icon[1], icon[2]);
        x += strip::kTabIcon + 14;
      }
      double right = b.x + b.w - kit::kSpace - (b.selected ? 24 : 0);  // the dot's room
      if (!b.detail.empty()) {
        const Rgba& dim = palette().textDim;
        const double w = textWidth(b.detail, strip::kListSubSize, false);
        drawText(cr, b.detail, right - w - 2, b.y, w + 4, b.h, strip::kListSubSize, false, false,
                 dim.r, dim.g, dim.b);
        right -= w + 24;
      }
      drawText(cr, b.label, x, b.y, right - x, b.h, b.fontSize, false, false, ink[0], ink[1], ink[2]);
      return;
    }
    if (b.toggle) {  // the label, and a switch at the right end: a sunk track, a raised knob
      constexpr double tw = kit::kSwitchW, th = kit::kSwitchH, knob = kit::kSwitchKnob;
      constexpr double in = kit::kSpace;  // the label and the switch from the row's ends
      const double tx = b.x + b.w - tw - in, ty = b.y + (b.h - th) / 2;
      const double labelW = b.wrapW > 0 ? b.wrapW : tx - b.inner - b.x - 2 * in;
      const int size = b.fontSize > 0 ? b.fontSize : kTextSize;
      if (b.lines > 1) {  // wrapped, centred in the row
        int lw, lh;
        pango_layout_get_pixel_size(textLayout(cr, b.label, size, false, labelW, b.lines, false),
                                    &lw, &lh);
        const double rgba[4] = {ink[0], ink[1], ink[2], 1};
        drawTextLines(cr, b.label, b.x + in, b.y + (b.h - lh) / 2, labelW, size, b.lines, rgba,
                      false);
      } else {
        drawText(cr, b.label, b.x + in, b.y, labelW, b.h, size, false, false, ink[0], ink[1],
                 ink[2]);
      }
      drawFinish(cr, b.on ? Finish::SwitchOn : Finish::Sunk, tx, ty, tw, th, th / 2);
      const double inset = (th - knob) / 2;
      drawFinish(cr, Finish::Knob, b.on ? tx + tw - inset - knob : tx + inset, ty + inset, knob, knob,
                 knob / 2);
      return;
    }
    if (!b.caption.empty()) {  // a tile: its icon over its caption, and a second line
      constexpr double iconSize = 40, captionH = 28, subH = 24, gap = 6;
      const bool picture = b.icon != Icon::None, sub = !b.detail.empty();
      const double h = (picture ? iconSize + gap : 0) + captionH + (sub ? subH : 0);
      double y = b.y + (b.h - h) / 2;
      if (picture) {
        drawIcon(cr, b.icon, b.x + b.w / 2, y + iconSize / 2, iconSize, icon[0], icon[1], icon[2]);
        y += iconSize + gap;
      }
      drawText(cr, b.caption, b.x + 6, y, b.w - 12, captionH, 22, false, true, ink[0], ink[1], ink[2]);
      if (sub) {
        const Rgba& k = b.selected ? palette().textDim : palette().faint;
        drawText(cr, b.detail, b.x + 6, y + captionH, b.w - 12, subH, 19, false, true, k.r, k.g, k.b);
      }
      return;
    }
    if (b.card) {  // file browser grid: picture (or folder), name in two lines, details
      const Palette& pal = palette();
      double tx, ty, tw, th;
      listPicture(b, tx, ty, tw, th);
      if (!b.thumbPath.empty() &&
          (previewFrame(b.thumbPath, tx, ty) || pictureOf(cr, b.thumbPath, tx, ty, tw, th))) {
        drawThumbnail(cr, b.thumbPath, tx, ty, tw, th, kit::kPieceCorner);
      } else {  // a sunk tile with its icon
        drawFinish(cr, Finish::Sunk, tx, ty, tw, th, kit::kPieceCorner);
        const Rgba& k = b.icon == Icon::Folder ? pal.textDim : pal.faint;
        if (b.icon != Icon::None)
          drawIcon(cr, b.icon, tx + tw / 2, ty + th / 2, b.icon == Icon::Folder ? 96 : 77, k.r, k.g, k.b);
      }
      if (stashCovers_ && !b.thumbPath.empty()) drawStudioMark(cr, b.thumbPath, tx, ty, tw, th);
      drawCoverStrip(cr, b, tx, ty, tw, th);  // under the stars, and the plays
      drawOfflineState(cr, b, tx, ty, tw, th);
      // The name in two lines under the picture (the playing one's after
      // its mark), the size and date on the last line; a name that would
      // reach them gets fewer lines. With a byline, the name has one line
      // and the byline the next.
      const double detailY = b.y + b.h - kit::kSpace - kCardLine;
      const double nameY = ty + th + kCardNameGap;  // its first line's box
      double nameX = b.x + kit::kSpace;
      if (b.selected) {
        drawPlayMark(cr, nameX + kPlayMarkSize / 2, nameY + kCardLine / 2);
        nameX += kPlayMarkW;
      }
      const double nameW = b.x + b.w - kit::kSpace - nameX;
      const double dim[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a};
      drawLinesIn(cr, b.label, nameX, nameY, nameW, kit::kRowSize, b.byline.empty() ? 2 : 1, kCardLine,
                  ink, b.fileName);
      if (!b.byline.empty())
        drawLinesIn(cr, b.byline, nameX, nameY + kCardLine, nameW, 22, 1, kCardLine, dim);
      // A downloaded video's tick starts the last line.
      const double tick = downloadedMark(b.offlinePath) ? 30 : 0;
      if (tick > 0) drawOfflineTick(cr, b.x + kit::kSpace + 11, detailY + kCardLine / 2, 22);
      if (!b.detail.empty())
        drawLinesIn(cr, b.detail, b.x + kit::kSpace + tick, detailY, b.w - 2 * kit::kSpace - tick, 22, 1,
                    kCardLine, dim);
      return;
    }
    if (b.listRow) {  // a list row: picture or icon, name, columns
      const Palette& pal = palette();
      double tx, ty, tw, th;
      listPicture(b, tx, ty, tw, th);
      drawRowLead(cr, b, ink, tx + tw + kRowNameGap + listNameWidth(b), b.y, b.h);
      if (listColumns_) {  // right-aligned numbers, then the date
        const ListColumns c = listColumns(b.x + b.w, listFit());
        if (downloadedMark(b.offlinePath))
          drawOfflineTick(cr, c.status + kStatusColumnW / 2, b.y + b.h / 2, 22);
        const ListFit& fit = listFit();
        const double sizeW = textWidth(b.size, 24, false), playsW = textWidth(b.plays, 24, false);
        const double lengthW = textWidth(b.length, 24, false);
        drawText(cr, b.plays, c.plays + kPlaysColumnW - playsW, b.y, playsW + 2, b.h, 24, false,
                 false, pal.textDim.r, pal.textDim.g, pal.textDim.b);
        drawText(cr, b.length, c.length + fit.length - lengthW, b.y, lengthW + 2, b.h, 24, false,
                 false, pal.textDim.r, pal.textDim.g, pal.textDim.b);
        drawText(cr, b.size, c.size + fit.size - sizeW, b.y, sizeW + 2, b.h, 24, false, false,
                 pal.textDim.r, pal.textDim.g, pal.textDim.b);
        drawText(cr, b.date, c.date, b.y, fit.date, b.h, 24, false, false, pal.textDim.r,
                 pal.textDim.g, pal.textDim.b);
      }
      return;
    }
    if (b.label.empty() && !b.field) {  // icon only: round buttons scale it, slim list rows shrink it
      const double size = b.iconSize > 0 ? b.iconSize
                          : b.w == b.h   ? b.w * (b.w >= 72 ? 0.42 : 0.5)
                                         : std::min(kIconSize, b.h * 0.62);
      if (alpha < 1) fadedColour(icon, alpha, icon);
      drawIcon(cr, b.icon, b.x + b.w / 2, b.y + b.h / 2, size, icon[0], icon[1], icon[2], alpha);
      return;
    }
    double textX = b.x + 8, textW = b.w - 16;
    bool center = true;
    if (b.alignLeft) {
      textX = b.x + 22;
      textW = b.w - 44;
      center = false;
    }
    const Rgba& dim = palette().textDim;
    const double faded[4] = {dim.r, dim.g, dim.b, dim.a};
    if (b.crossed) ink = faded;
    if (b.icon != Icon::None) {
      const double iconSize = kIconSize * 0.75;
      const double cx = b.x + 35 + iconSize / 2, cy = b.y + b.h / 2;
      const double* k = b.crossed ? ink : icon;
      drawIcon(cr, b.icon, cx, cy, iconSize, k[0], k[1], k[2]);
      if (b.crossed) {
        const double r = iconSize * 0.55;
        cairo_move_to(cr, cx - r, cy - r);
        cairo_line_to(cr, cx + r, cy + r);
        cairo_set_source_rgba(cr, ink[0], ink[1], ink[2], 1);
        cairo_set_line_width(cr, 3);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_stroke(cr);
      }
      textX = b.x + 35 + iconSize + 20;
      textW = b.w - (textX - b.x) - 35;
      center = false;
    }
    if (b.field) {  // a search
      drawFieldText(cr, b, ink);
      return;
    }
    if (b.keySwatch != -1) {  // a key's colour before the label
      const double r = b.h * 0.34;
      drawKeySwatch(cr, b.keySwatch, textX + r, b.y + b.h / 2, r);
      textX += 2 * r + 14;
      textW -= 2 * r + 14;
    }
    double k[3] = {ink[0], ink[1], ink[2]};
    if (alpha < 1) fadedColour(k, alpha, k);
    drawText(cr, b.label, textX, b.y, textW, b.h, b.fontSize > 0 ? b.fontSize : kTextSize, false,
             center, k[0], k[1], k[2], alpha);
  }
}

bool Menu::fadesInPlace(const Button& b, bool surface) const {
  // drawButtonContent()'s last two cases: an icon alone, a label alone.
  if (surface || !b.badge.empty() || b.popup || b.well || b.offlineLook || b.stashLook ||
      !b.ratingPath.empty() || b.listLook || b.volumeSlider || b.slider != MenuCommand::None ||
      b.swatch >= 0 || b.select || b.scrollbar || b.dialog || b.field || b.tab || b.chip || b.band ||
      b.listItem || b.toggle || !b.caption.empty() || b.card || b.listRow)
    return false;
  if (b.label.empty()) return onePaint(b.icon);
  return b.icon == Icon::None && !b.crossed && b.keySwatch == -1;
}

void Menu::fieldTextBox(const Button& b, double& x, double& w, int& size) const {
  size = b.fontSize > 0 ? b.fontSize : kTextSize;
  x = b.x + kit::kSpace;
  w = b.w - 2 * kit::kSpace - b.inner;  // before its own button, if it has one
  if (b.listLook) {  // a tools row's search: after its icon, before what it finds (drawListPart())
    x = b.x + kit::kSpace + 26 + 12;
    w = b.x + b.w - kit::kSpace - b.inner - x;
  } else if (b.icon != Icon::None) {  // a search: after its icon, as drawButtonContent() has it
    x = b.x + 35 + kIconSize * 0.75 + 20;
    w = b.w - (x - b.x) - 35;
  }
}

// Its text, or its example faint while it is empty. Typed into, the caret
// in the accent's text colour (the kit's: 2 wide, a fifth taller than the
// letters), before the example in an empty field; the whole text, scrolled
// only as far as it takes to keep the caret and a little beside it in view.
void Menu::drawFieldText(cairo_t* cr, const Button& b, const double ink[4]) const {
  double x = 0, w = 0;
  int size = 0;
  fieldTextBox(b, x, w, size);
  const Palette& pal = palette();
  const double caretH = std::round(size * 1.25);
  auto caretAt = [&](double cx) {
    setColor(cr, pal.textAccent);
    cairo_rectangle(cr, std::round(cx) - 1, std::round(b.y + (b.h - caretH) / 2), 2, caretH);
    cairo_fill(cr);
  };
  if (b.label.empty() && !b.example.empty()) {
    const double in = b.caret >= 0 ? 4 : 0;  // clear of the caret
    drawText(cr, b.example, x + in, b.y, w - in, b.h, size, false, false, pal.faint.r, pal.faint.g,
             pal.faint.b);
    if (b.caret >= 0) caretAt(x + 1);
    return;
  }
  if (b.caret < 0) {
    drawText(cr, b.label, x, b.y, w, b.h, size, false, false, ink[0], ink[1], ink[2]);
    return;
  }
  PangoLayout* layout = textLayout(cr, b.label, size, false, -1, 0, false);
  int textW = 0, textH = 0;
  pango_layout_get_pixel_size(layout, &textW, &textH);
  const char* text = b.label.c_str();
  PangoRectangle at;
  pango_layout_index_to_pos(layout, static_cast<int>(g_utf8_offset_to_pointer(text, b.caret) - text), &at);
  const double caretX = static_cast<double>(at.x) / PANGO_SCALE;
  constexpr double kBeside = 24;
  fieldShift_ = std::clamp(fieldShift_, caretX + kBeside - w, caretX - kBeside);
  fieldShift_ = std::clamp(fieldShift_, 0.0, std::max(0.0, textW + 4 - w));
  const double left = x - fieldShift_;
  cairo_save(cr);
  cairo_rectangle(cr, x - 2, b.y, w + 4, b.h);
  cairo_clip(cr);
  cairo_set_source_rgba(cr, ink[0], ink[1], ink[2], ink[3]);
  cairo_move_to(cr, left, onPixel(b.y + (b.h - textH) / 2));
  pango_cairo_show_layout(cr, layout);
  caretAt(left + caretX);
  cairo_restore(cr);
}

MenuCommand Menu::clickField(const Button& b) {
  int chars = -1;  // where the pointer is in its text
  if (!b.placeholder) {
    double x = 0, w = 0;
    int size = 0;
    fieldTextBox(b, x, w, size);
    PangoLayout* layout = textLayout(canvases_[kMain].cr(), b.label, size, false, -1, 0, false);
    const double shift = b.caret >= 0 ? fieldShift_ : 0;
    int index = 0, trailing = 0;
    pango_layout_xy_to_index(layout, static_cast<int>((pointerX_ - x + shift) * PANGO_SCALE), 0, &index,
                             &trailing);
    chars = static_cast<int>(g_utf8_pointer_to_offset(b.label.c_str(), b.label.c_str() + index)) + trailing;
  }
  auto place = [this, chars] {
    const char* text = filterText().c_str();
    const long n = std::min<long>(chars, g_utf8_strlen(text, -1));
    setCursor(static_cast<size_t>(g_utf8_offset_to_pointer(text, n) - text));
  };
  if (b.caret >= 0) {  // only the caret moves
    if (chars >= 0) place();
    typedChanged();
    return {};
  }
  dirty_ = true;
  const MenuCommand c = b.action ? b.action() : MenuCommand{};
  if (chars >= 0 && textInputActive() && filterText() == b.label) place();
  return c;
}

void Menu::drawTextLines(cairo_t* cr, const std::string& text, double x, double y, double w,
                         int size, int lines, const double rgba[4], bool center,
                         double inkBottom, bool bold) const {
  if (outsideClip(cr, x, y - size, w, (lines + 1) * size * 2.0)) return;
  PangoLayout* layout = textLayout(cr, text, size, bold, w, std::max(1, lines), center);
  while (inkBottom > 0 && lines > 1) {
    PangoRectangle ink;
    pango_layout_get_pixel_extents(layout, &ink, nullptr);
    if (y + ink.y + ink.height <= inkBottom) break;
    layout = textLayout(cr, text, size, bold, w, --lines, center);
  }
  cairo_set_source_rgba(cr, rgba[0], rgba[1], rgba[2], rgba[3]);
  cairo_move_to(cr, x, onPixel(y));
  pango_cairo_show_layout(cr, layout);
}

int Menu::wrappedLines(const std::string& text, double w, int size, int lines, bool bold) const {
  // Counted again for the same notes on every layout.
  std::string key = text;
  key += '\x1f' + std::to_string(size * 2 + bold) + ' ' + std::to_string(std::lround(w * 4)) + ' ' +
         std::to_string(lines);
  const auto known = lineCounts_.find(key);
  if (known != lineCounts_.end()) return known->second;
  if (lineCounts_.size() > 1024) lineCounts_.clear();
  PangoLayout* layout = textLayout(canvases_[kMain].cr(), text, size, bold, w, std::max(1, lines), false);
  return lineCounts_[key] = std::max(1, pango_layout_get_line_count(layout));
}

void Menu::twoLines(const std::string& text, double room, int size, double& w, double& h) const {
  w = room;
  const bool fits = wrappedLines(text, room, size, 3) <= 2;
#ifndef NDEBUG
  if (!fits)
    std::fprintf(stderr, "[fit] \"%s\" needs more than two lines of %.0f px\n", text.c_str(), room);
#endif
  // A break hint in the text (a zero-width space): the lines part there when
  // both halves fit and the wrap then breaks at it.
  bool atHint = false;
  static const std::string kHint = "\xe2\x80\x8b";  // U+200B
  const size_t hint = fits ? text.find(kHint) : std::string::npos;
  if (hint != std::string::npos) {
    const double part = std::ceil(std::max(textWidth(text.substr(0, hint), size, false),
                                           textWidth(text.substr(hint + kHint.size()), size, false))) +
                        1;
    if (part <= room) {
      PangoLayout* l = textLayout(canvases_[kMain].cr(), text, size, false, part, 2, false);
      atHint = pango_layout_get_line_count(l) == 2 &&
               pango_layout_get_line_readonly(l, 1)->start_index == static_cast<int>(hint + kHint.size());
      if (atHint) w = part;
    }
  }
  if (fits && !atHint) w = narrowest(text, room, size, 2);
  int textW = 0, textH = 0;
  pango_layout_get_pixel_size(textLayout(canvases_[kMain].cr(), text, size, false, w, 2, false), &textW,
                              &textH);
  h = textH;
}

double Menu::narrowest(const std::string& text, double room, int size, int lines) const {
  char args[64];
  std::snprintf(args, sizeof args, "\x1f%d %a %d", size, room, lines);
  const std::string key = text + args;
  const auto known = narrowest_.find(key);
  if (known != narrowest_.end()) return known->second;
  if (narrowest_.size() > 1024) narrowest_.clear();
  double w = room, narrow = room / lines;  // `lines` lines don't fit in less
  for (int i = 0; i < 10; ++i) {
    const double mid = (narrow + w) / 2;
    (wrappedLines(text, mid, size, lines + 1) <= lines ? w : narrow) = mid;
  }
  return narrowest_[key] = std::ceil(w);
}

double Menu::drawMono(cairo_t* cr, const std::string& text, double x, double y, double w, int size,
                      const double rgba[4]) const {
  PangoLayout* layout = pango_cairo_create_layout(cr ? cr : canvases_[kMain].cr());
  PangoFontDescription* font = pango_font_description_from_string(kFontMono);
  pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
  pango_layout_set_font_description(layout, font);
  pango_font_description_free(font);
  pango_layout_set_text(layout, text.c_str(), -1);
  pango_layout_set_width(layout, static_cast<int>(w * PANGO_SCALE));
  pango_layout_set_wrap(layout, PANGO_WRAP_CHAR);
  // A key copied by hand mustn't gain a hyphen where its line breaks.
  PangoAttrList* attrs = pango_attr_list_new();
  pango_attr_list_insert(attrs, pango_attr_insert_hyphens_new(FALSE));
  pango_layout_set_attributes(layout, attrs);
  pango_attr_list_unref(attrs);
  int textW = 0, textH = 0;
  pango_layout_get_pixel_size(layout, &textW, &textH);
  if (cr) {
    cairo_set_source_rgba(cr, rgba[0], rgba[1], rgba[2], rgba[3]);
    cairo_move_to(cr, x, onPixel(y));
    pango_cairo_show_layout(cr, layout);
  }
  g_object_unref(layout);
  return textH;
}

void Menu::addGroup(double x, double y, double w, double h, const std::string& title) {
  groups_.push_back({x, y, w, h, title});
}

void Menu::drawGroups(cairo_t* cr) {
  const Palette& pal = palette();
  cairo_save(cr);
  if (pageScrollMax_ > 0) {  // cut off at the edges, as the rows in them
    cairo_rectangle(cr, 0, listTop_ - 2, kBrowserWidth, listBottom_ - listTop_ + 2);
    cairo_clip(cr);
  }
  // Only what is being redrawn (a row's change redraws only that row).
  double clipX0 = 0, clipY0 = 0, clipX1 = 0, clipY1 = 0;
  cairo_clip_extents(cr, &clipX0, &clipY0, &clipX1, &clipY1);
  auto shown = [&](double x, double y, double w, double h) {
    return x < clipX1 && x + w > clipX0 && y < clipY1 && y + h > clipY0;
  };
  for (const Group& g : groups_) {
    if (!shown(g.x, g.y, g.w, g.h)) continue;
    // Where what is redrawn misses its corners, its box there is a
    // rectangle: filled as one, without working out its rounded path.
    const double r = kit::kGroupCorner;
    const double x0 = std::max(g.x, clipX0), x1 = std::min(g.x + g.w, clipX1);
    const double y0 = std::max(g.y, clipY0), y1 = std::min(g.y + g.h, clipY1);
    if ((x0 >= g.x + r && x1 <= g.x + g.w - r) || (y0 >= g.y + r && y1 <= g.y + g.h - r)) {
      cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
      cairo_set_source_rgba(cr, 0, 0, 0, 0.18);
      cairo_fill(cr);
    } else {
      groupFrame(cr, g.x, g.y, g.w, g.h);
    }
    if (shown(g.x, g.y, g.w, g.titleH))
      drawText(cr, g.title, g.x + kit::kTextInset, g.y, g.w - 2 * kit::kTextInset, g.titleH,
               kit::kGroupTitleSize, true, false, pal.textDim.r, pal.textDim.g, pal.textDim.b);
  }
  cairo_restore(cr);
  drawPageHead(cr);
}

// A page's header (menu_parts.h, contentTitle()): its title, what it is
// about, and a content page's state at its right.
void Menu::drawPageHead(cairo_t* cr) {
  const Palette& pal = palette();
  const PageHead& head = pageHead_;
  const double top = pageTop_ + kit::kPanelInset + kit::kSpace, h = kit::kPageHeaderH;
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  cairo_clip_extents(cr, &x0, &y0, &x1, &y1);
  if (head.title.empty() || top >= y1 || top + h <= y0 || kPageX >= x1) return;
  drawText(cr, head.title, head.titleX, top, textWidth(head.title, kit::kTitleSize, true) + 2, h,
           kit::kTitleSize, true, false, pal.text.r, pal.text.g, pal.text.b);
  if (!head.about.empty())
    drawText(cr, head.about, head.aboutX, top, head.aboutW, h, 23, false, false, pal.textDim.r,
             pal.textDim.g, pal.textDim.b);
  if (!head.status.empty()) {
    const double w = std::min(textWidth(head.status, 23, false) + 2, head.statusRight - head.aboutX);
    drawText(cr, head.status, head.statusRight - w, top, w, h, 23, false, false, pal.textDim.r,
             pal.textDim.g, pal.textDim.b);
  }
}

// The page scrolls between its header (or its top margin's middle, without
// one) and its bottom margin's middle; the header stays. The scroll bar is
// in its right margin, beside the groups.
void Menu::scrollGroups(size_t first) {
  double bottom = 0;
  for (const Group& g : groups_) bottom = std::max(bottom, g.y + g.h);
  for (size_t i = first; i < buttons_.size(); ++i)
    if (buttons_[i].panel == kBrowser && !buttons_[i].popup)
      bottom = std::max(bottom, buttons_[i].y + buttons_[i].h);
  const double contentTop = pageTop_ + kit::kPanelInset + kit::kSpace + kit::kPageHeaderH + kit::kSpace;
  const bool headed = !pageHead_.title.empty();
  listTop_ = headed ? contentTop - kit::kSpace / 2 : pageTop_ + kPagePad / 2;
  listBottom_ = kBrowserHeight - kPagePad / 2;
  // A page alone may end at its page's bottom (PageLayout::kBottom), as tall
  // as it is measured.
  const double pad = pageAlone() ? kit::kPanelInset + kit::kSpace : kPagePad;
  pageScrollMax_ = std::max(0.0, bottom + pad - kBrowserHeight);
  pageScroll_ = std::clamp(pageScroll_, 0.0, pageScrollMax_);
  if (pageScrollMax_ <= 0) return;
  for (Group& g : groups_) g.y -= pageScroll_;
  for (size_t i = first; i < buttons_.size(); ++i) {
    Button& b = buttons_[i];
    if (b.panel != kBrowser) continue;
    if (!b.popup && b.y + b.h <= listTop_) continue;  // the header's sections and switches
    b.y -= pageScroll_;
    b.scrolls = true;
  }
  Button bar;
  bar.panel = kBrowser;
  bar.x = kBrowserWidth - kPagePad - 2;
  bar.y = headed ? contentTop : pageTop_ + kPagePad;
  bar.w = 20;
  bar.h = kBrowserHeight - kPagePad - bar.y;
  bar.radius = 10;
  bar.bare = true;
  bar.scrollbar = true;
  bar.action = [] { return MenuCommand{}; };  // handled in click()/drag()
  buttons_.push_back(bar);
}

Menu::Button& Menu::addLabel(double x, double y, double w, double h, const std::string& text,
                             int size, bool dim, int lines) {
  Button b;
  b.panel = kBrowser;
  b.x = x;
  b.y = y;
  b.w = w;
  b.h = h;
  b.label = text;
  b.staticText = true;
  b.fontSize = size;
  b.dim = dim;
  b.lines = lines;
  buttons_.push_back(b);
  return buttons_.back();
}

void Menu::drawRowLead(cairo_t* cr, const Button& b, const double ink[4], double nameEnd,
                       double nameY, double nameH) const {
  const Palette& pal = palette();
  double tx, ty, tw, th;
  listPicture(b, tx, ty, tw, th);
  // A video's picture; with thumbnails off its icon, unless Stash has a cover.
  if (!b.thumbPath.empty() && (b.icon == Icon::None || previewFrame(b.thumbPath, tx, ty) ||
                              pictureOf(cr, b.thumbPath, tx, ty, tw, th))) {
    drawThumbnail(cr, b.thumbPath, tx, ty, tw, th, kit::kPieceCorner);
  } else if (b.icon != Icon::None) {  // a sunk tile with its icon
    drawFinish(cr, Finish::Sunk, tx, ty, tw, th, kit::kPieceCorner);
    const bool folder = b.icon == Icon::Folder || b.icon == Icon::Computer;
    const Rgba& k = folder ? pal.textDim : pal.faint;
    drawIcon(cr, b.icon, tx + tw / 2, ty + th / 2, std::round(th * (folder ? 0.68 : 0.55)), k.r, k.g, k.b);
  }
  drawOfflineState(cr, b, tx, ty, tw, th);
  const int size = b.fontSize > 0 ? b.fontSize : kit::kRowSize;
  double nameX = tx + tw + kRowNameGap, nameW = nameEnd - nameX;
  const double w = rowNameWidth(b, nameW);
  if (b.selected) {  // the playing video: its mark before its name
    drawPlayMark(cr, nameX + kPlayMarkSize / 2, nameY + nameH / 2);
    nameX += kPlayMarkW;
    nameW -= kPlayMarkW;
  }
  drawText(cr, b.label, nameX, nameY, w, nameH, size, false, false, ink[0], ink[1], ink[2]);
  // The byline dimmed after it while there is room.
  if (!b.byline.empty() && nameW - w - kRowBylineGap > 60)
    drawText(cr, b.byline, nameX + w + kRowBylineGap, nameY, nameW - w - kRowBylineGap, nameH, 24, false,
             false, pal.textDim.r, pal.textDim.g, pal.textDim.b);
}

double Menu::rowNameWidth(const Button& b, double room) const {
  if (b.selected) room -= kPlayMarkW;
  if (b.byline.empty()) return room;
  const double labelW = textWidth(b.label, b.fontSize > 0 ? b.fontSize : kit::kRowSize, false);
  const double bylineW = textWidth(b.byline, 24, false);
  return std::min(labelW + 2, std::max(room * kBylineShare, room - bylineW - kRowBylineGap));
}

bool Menu::shortened(const std::string& text, int size, double w, int lines, bool fileName) const {
  return pango_layout_is_ellipsized(
      textLayout(canvases_[kMain].cr(), text, size, false, w, lines, false, fileName));
}

double Menu::listNameWidth(const Button& row) const {
  const double end = listColumns_ ? listColumns(row.x + row.w, listFit()).status - kColumnGap
                                  : row.x + row.w - kit::kSpace - row.inner;
  double x, y, w, h;
  listPicture(row, x, y, w, h);
  return end - (x + w + kRowNameGap);
}

cairo_surface_t* Menu::pictureOf(cairo_t* cr, const std::string& path, double x, double y,
                                 double w, double h) const {
  cairo_surface_t* picture = findPicture(path, w);
  const ShownPicture now{w, picture, !picture && !pictureComing(path)};
  const auto shown = shownPictures_.find(path);
  // Only what reached the box counts as shown: a strip of it that took a
  // new picture leaves the rest showing the old one until picturesChanged()
  // redraws all of it.
  if (shown == shownPictures_.end()) {
    if (paintedPart(cr, x, y, w, h) != Painted::None) shownPictures_.emplace(path, now);
    return picture;
  }
  ShownPicture& s = shown->second;
  if (s.surface == picture && (picture || s.icon == now.icon) && !s.torn) {
    s = now;
    return picture;
  }
  // A page drawn in bands draws a picture that came after its first band
  // reached the box once it is published (picturesChanged()): the rest of
  // its bands draw what that one did.
  if (browserRow_ >= 0 && !s.surface) return nullptr;
  const Painted painted = paintedPart(cr, x, y, w, h);
  if (painted == Painted::None) return picture;
  s = now;
  s.torn = painted == Painted::Part;
  return picture;
}

Menu::Painted Menu::paintedPart(cairo_t* cr, double x, double y, double w, double h) const {
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  cairo_clip_extents(cr, &x0, &y0, &x1, &y1);
  // Pictures are in a list's rows, cut off at its edges (drawButtons()).
  const double top = std::max(y, listTop_ - 2), bottom = std::min(y + h, listBottom_);
  if (top >= bottom) return Painted::All;  // none of it shows
  if (x0 >= x + w || x1 <= x || y0 >= bottom || y1 <= top) return Painted::None;
  return x0 <= x && x1 >= x + w && y0 <= top && y1 >= bottom ? Painted::All : Painted::Part;
}

bool Menu::pictureComing(const std::string& path) const {
  if (stashCovers_ && stashCovers_->coverComing(path)) return true;
  // findPicture()'s frames.
  return thumbnailer_ && (view_.thumbnails || !stashCovers_) && !thumbnailer_->tried(path);
}

cairo_surface_t* Menu::findPicture(const std::string& path, double w) const {
  if (stashCovers_) {
    // Covers are made to fill their pictures at their own size (StashImages).
    const int width = static_cast<int>(std::ceil(w * menu_geometry::kRasterScale));
    if (cairo_surface_t* cover = stashCovers_->cover(path, width)) return cover;
  }
  // With thumbnails off, no frames (some may still be in memory).
  return thumbnailer_ && (view_.thumbnails || !stashCovers_) ? thumbnailer_->find(path) : nullptr;
}

bool Menu::picturesChanged() {
  if (!picturesAlone()) {
    shownPictures_.clear();
    logosComing_.clear();
    return false;
  }
  // Only what is on screen is kept: the rest is drawn anew as it comes into view.
  std::unordered_map<std::string, ShownPicture> shown;
  std::unordered_set<std::string> logos;
  for (const Button& b : buttons_) {
    if (b.panel != kBrowser || b.thumbPath.empty()) continue;
    const auto picture = shownPictures_.find(b.thumbPath);
    const bool logo = logosComing_.count(b.thumbPath) > 0;
    if (picture == shownPictures_.end() && !logo) continue;
    if (logo ||
        (picture != shownPictures_.end() &&
         (picture->second.torn ||
          findPicture(b.thumbPath, picture->second.w) != picture->second.surface ||
          (!picture->second.surface && picture->second.icon != !pictureComing(b.thumbPath))))) {
      browserChanged(b.y - 4, b.y + b.h + 4, b.x - 4, b.x + b.w + 4);
      continue;
    }
    shown.insert(*picture);
  }
  shownPictures_.swap(shown);
  logosComing_.swap(logos);
  return true;
}

// A thumbnail (or Stash's cover) filling the box, cropping what sticks out,
// or a placeholder until it is made.
void Menu::drawThumbnail(cairo_t* cr, const std::string& path, double x, double y, double w,
                         double h, double radius) const {
  cairo_surface_t* thumb = previewFrame(path, x, y);
  if (!thumb) thumb = pictureOf(cr, path, x, y, w, h);
  // A strip of a scrolling list clear of its corners: their straight edges
  // alone, much cheaper to paint within.
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  cairo_clip_extents(cr, &x0, &y0, &x1, &y1);
  if ((y0 >= y + radius && y1 <= y + h - radius) || (x0 >= x + radius && x1 <= x + w - radius))
    cairo_rectangle(cr, x, y, w, h);
  else
    roundedRect(cr, x, y, w, h, radius);
  if (!thumb) {
    cairo_set_source_rgba(cr, 1, 1, 1, 0.06);
    cairo_fill(cr);
    // No cover (Stash's placeholder for one is not taken) and no frame; in
    // bands, as the first that reached it drew it (pictureOf()).
    const auto shown = shownPictures_.find(path);
    if ((browserRow_ >= 0 && shown != shownPictures_.end()) ? shown->second.icon
                                                            : !pictureComing(path)) {
      const Rgba k = palette().text;
      drawIcon(cr, Icon::Video, x + w / 2, y + h / 2, std::min(96.0, h * 0.6), k.r, k.g, k.b);
    }
    return;
  }
  const double sw = cairo_image_surface_get_width(thumb);
  const double sh = cairo_image_surface_get_height(thumb);
  double scale = std::max(w / sw, h / sh);
  // A picture made for this box (a fitted cover) covering it at its own
  // size: pasted pixel for pixel, much cheaper than scaling it.
  constexpr double pixel = 1 / menu_geometry::kRasterScale;
  const bool own = sw * pixel > w - 0.01 && sh * pixel > h - 0.01 && scale > pixel * 0.99;
  if (own) scale = pixel;
  cairo_save(cr);
  cairo_clip(cr);
  const double left = x + (w - sw * scale) / 2, top = y + (h - sh * scale) / 2;
  cairo_translate(cr, own ? onPixel(left) : left, own ? onPixel(top) : top);
  cairo_scale(cr, scale, scale);
  cairo_set_source_surface(cr, thumb, 0, 0);
  cairo_paint(cr);
  cairo_restore(cr);
}
