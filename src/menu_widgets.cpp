#include "menu_style.h"

#include "menu_stash.h"
#include "menu_stash_covers.h"
#include "thumbnailer.h"

#include <pango/pangocairo.h>

// Buttons and sliders: building them, laying them out, drawing them.

double Menu::textWidth(const std::string& text, int size, bool bold) const {
  // Measured again for the same labels on every redraw (while scrolling, every frame).
  const auto key = std::make_pair(text, size * 2 + bold);
  const auto known = textWidths_.find(key);
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
  textWidths_[key] = w;
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

// Lays out buttons with their own widths, centered on the main panel.
void Menu::addCenteredRow(double y, double h, std::vector<Button> items, double cx) {
  double total = 0;
  for (size_t i = 0; i < items.size(); ++i) total += items[i].w + (i ? kGap : 0);
  double x = cx - total / 2;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i) x += kGap;
    items[i].x = x;
    items[i].y = y;
    items[i].h = h;
    buttons_.push_back(items[i]);
    x += items[i].w;
  }
}

// Lays out equally wide buttons from x0 to the right margin of the main panel.
void Menu::addRow(double y, double h, std::vector<Button> items, double x0) {
  const double w = (kMainWidth - kMargin - x0 - kGap * (items.size() - 1)) / items.size();
  for (size_t i = 0; i < items.size(); ++i) {
    items[i].x = x0 + i * (w + kGap);
    items[i].y = y;
    items[i].w = w;
    items[i].h = h;
    buttons_.push_back(items[i]);
  }
}

// The choices as a strip of buttons just above their opener, centred on it
// where there is room, drawn on top of everything else.
void Menu::addDropdown(const Button& opener, std::vector<Button> entries) {
  double total = 0;
  for (Button& e : entries) {
    e.w = std::max(120.0, textWidth(e.label, kTextSize, false) + 60);
    total += e.w;
  }
  total += kGap * (entries.size() - 1);
  const double panelW = opener.panel == kBrowser ? kBrowserWidth : kMainWidth;
  const double minX = opener.panel == kBrowser ? kPageX + kPagePad : kMargin;
  double x = std::max(minX, std::min(opener.x + opener.w / 2 - total / 2, panelW - kMargin - total));
  for (Button& e : entries) {
    e.panel = opener.panel;
    e.popup = true;
    e.radius = glass() ? 36 : 20;  // pills on glass
    e.x = x;
    e.h = 72;
    e.y = opener.y - 18 - e.h;
    buttons_.push_back(e);
    x += e.w + kGap;
  }
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
    e.radius = glass() ? 28 : 16;
    e.x = opener.x + (i / rows) * (w + 60 + kGap);
    e.y = top + (i % rows) * 64;
    e.w = w + 60;
    e.h = 56;
    buttons_.push_back(e);
  }
}

// The choices stacked above their opener, in columns of five if there are
// many, drawn on top of everything else.
void Menu::addDropdownList(const Button& opener, std::vector<Button> entries) {
  constexpr int perColumn = 5;
  constexpr double h = 60, step = 68;
  double w = 0;
  for (const Button& e : entries) w = std::max(w, textWidth(e.label, 26, false) + 60);
  const int count = static_cast<int>(entries.size());
  const int columns = (count + perColumn - 1) / perColumn;
  const int rows = std::min(count, perColumn);
  // At most 520 wide, and all columns within the panel.
  w = std::min({w, 520.0, (kMainWidth - 2 * kMargin - (columns - 1) * kGap) / columns});
  const double total = columns * w + (columns - 1) * kGap;
  const double x0 = std::max(kMargin, std::min(opener.x + opener.w / 2 - total / 2,
                                               kMainWidth - kMargin - total));
  for (int i = 0; i < count; ++i) {
    Button& e = entries[i];
    e.popup = true;
    e.fontSize = 26;
    e.radius = glass() ? h / 2 : 16;
    e.x = x0 + (i / perColumn) * (w + kGap);
    e.y = opener.y - 14 - (rows - i % perColumn) * step;
    e.w = w;
    e.h = h;
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
  const double x0 = slider.x + kSliderInset, x1 = slider.x + slider.w - kSliderValueW;
  MenuCommand c;
  c.type = MenuCommand::SetVolume;
  c.value = std::round(std::clamp((pointerX_ - x0) / (x1 - x0), 0.0, 1.0) * 100.0);
  return c;
}

void Menu::drawVolumeSlider(cairo_t* cr, const Button& b) const {
  const Palette& pal = palette();
  const double x0 = b.x + kSliderInset, x1 = b.x + b.w - kSliderValueW;
  const double y = b.y + b.h / 2;
  const double fraction = std::clamp(view_.playback.volume / 100.0, 0.0, 1.0);
  drawTrack(cr, x0, x1, y, fraction, -1, view_.playback.muted ? 0.4 : 1.0);
  drawText(cr, std::to_string(static_cast<int>(std::round(view_.playback.volume))), x1 + 28, b.y,
           kSliderValueW - 36, b.h, 26, true, true, pal.text.r, pal.text.g, pal.text.b);
}

// A slider: a field filled blue to its value, the value on it; the
// pointer's x along it sets it.
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

void Menu::drawSlider(cairo_t* cr, const Button& b) const {
  const Palette& pal = palette();
  if (b.slider == MenuCommand::SetAccentHue) {
    // The colours to choose from along the track, a white handle on the chosen one.
    cairo_pattern_t* rainbow = cairo_pattern_create_linear(b.x, 0, b.x + b.w, 0);
    for (int i = 0; i < kHueSteps; ++i) {  // where sliderAtPointer() puts each hue
      const AccentColours& c = hueColour(i);
      cairo_pattern_add_color_stop_rgb(rainbow, std::min(1.0, i * 360.0 / kHueSteps / b.sliderMax),
                                       c.fill[0], c.fill[1], c.fill[2]);
    }
    roundedRect(cr, b.x, b.y, b.w, b.h, 10);
    cairo_set_source(cr, rainbow);
    cairo_fill(cr);
    cairo_pattern_destroy(rainbow);
    const double r = b.h / 2 - 5;
    const double cx = std::clamp(b.x + b.w * b.sliderValue / std::max(1, b.sliderMax), b.x + 5 + r,
                                 b.x + b.w - 5 - r);
    cairo_arc(cr, cx, b.y + b.h / 2, r, 0, 2 * M_PI);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
    cairo_set_line_width(cr, 2);
    cairo_stroke(cr);
    return;
  }
  roundedRect(cr, b.x, b.y, b.w, b.h, 10);
  setColor(cr, pal.field);
  cairo_fill_preserve(cr);
  cairo_save(cr);
  cairo_clip(cr);
  const double fraction = sliderFraction(b, b.sliderValue);
  cairo_rectangle(cr, b.x, b.y, b.w * std::clamp(fraction, 0.0, 1.0), b.h);
  setColor(cr, pal.accent, 0.85);
  cairo_fill(cr);
  if (b.sliderDefault >= 0) {
    for (int i = b.sliderMin; i <= b.sliderMax; i += b.sliderStep) {
      const double x = b.x + b.w * (i - b.sliderMin) / std::max(1, b.sliderMax - b.sliderMin);
      cairo_rectangle(cr, x - 1, b.y + b.h - (i == b.sliderDefault ? 9 : 3), 2,
                      i == b.sliderDefault ? 9 : 3);
    }
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill(cr);
  }
  cairo_restore(cr);
  drawText(cr, b.sliderText.empty() ? std::to_string(b.sliderValue) : b.sliderText, b.x, b.y, b.w,
           b.h, kTextSize, false, true, 1, 1, 1);
}

// A disc in the accent colour it chooses, Custom's with the hues around it;
// the chosen one ringed in white.
void Menu::drawSwatch(cairo_t* cr, const Button& b) const {
  const double cx = b.x + b.w / 2, cy = b.y + b.h / 2, r = std::min(b.w, b.h) / 2 - 6;
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
  if (b.on) {
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

// A slider track from x0 to x1 at height y, filled blue to `fraction`, with
// an optional buffered part (< 0: none) and a white knob.
void Menu::drawTrack(cairo_t* cr, double x0, double x1, double y, double fraction,
                     double buffered, double alpha) const {
  const Palette& pal = palette();
  const double knob = x0 + (x1 - x0) * fraction;
  auto bar = [&](double from, double to, double h, const Rgba& color, double a) {
    if (to - from < 1) return;
    roundedRect(cr, from, y - h / 2, to - from, h, std::min(h / 2, (to - from) / 2));
    setColor(cr, color, a);
    cairo_fill(cr);
  };
  bar(x0, x1, 10, pal.field, 1);
  if (buffered > fraction) bar(x0, x0 + (x1 - x0) * buffered, 10, pal.buffered, 1);
  bar(x0, knob, 10, pal.accent, alpha);
  cairo_arc(cr, knob, y, 15, 0, 2 * M_PI);
  cairo_set_source_rgba(cr, 1, 1, 1, alpha);
  cairo_fill(cr);
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

PangoLayout* Menu::textLayout(cairo_t* cr, const std::string& text, int size, bool bold, double w,
                              int lines, bool center) const {
  const int width = w < 0 ? -1 : static_cast<int>(w * PANGO_SCALE);
  std::string key = text;
  key += '\x1f' + std::to_string(size * 4 + bold * 2 + center) + ' ' + std::to_string(width) +
         ' ' + std::to_string(lines);
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
  pango_layout_set_text(layout, text.c_str(), -1);
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
  // a preview frame), with room for what a button draws out of its box: a
  // badge, text taller than its row, a label's lines below its top. Sideways
  // only a badge reaches out, by under 7: the scroll bar's track and the side
  // column's mark are redrawn alone, without the rows beside them.
  constexpr double kReach = 16, kReachX = 7;
  double clipX0 = 0, clipY0 = 0, clipX1 = 0, clipY1 = 0;
  cairo_clip_extents(cr, &clipX0, &clipY0, &clipX1, &clipY1);
  for (size_t i = 0; i < buttons_.size(); ++i) {
    const Button& b = buttons_[i];
    if (b.panel != panel || b.popup != popups) continue;
    if (b.hoverOnly && hovered_ != static_cast<int>(i) && hovered_ != static_cast<int>(i) - 1)
      continue;
    const double above = b.staticText ? b.fontSize : 0;
    const double below = b.staticText ? std::max(b.h, b.lines * b.fontSize * 1.5) : b.h;
    // A list's rows and cards keep within their boxes (a preview frame
    // redraws its own row alone).
    const bool inside = (b.listRow || b.card) && !b.staticText && b.badge.empty();
    const double reachX = inside ? 1 : kReachX, reach = inside ? 1 : kReach;
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
      const Rgba& c = b.dim ? pal.textDim : pal.text;
      const double rgba[4] = {c.r, c.g, c.b, c.a * fade};
      if (b.lines > 1) {
        drawTextLines(cr, b.label, b.x, b.y, b.w, b.fontSize, b.lines, rgba, false);
      } else {
        drawText(cr, b.label, b.x, b.y, b.w, b.h, b.fontSize, false, false, rgba[0], rgba[1],
                 rgba[2], rgba[3]);
      }
      continue;
    }
    if (!b.action && !b.selected) {  // list heading
      drawText(cr, b.label, b.x + 8, b.y, b.w - 16, b.h, kTextSize, true, false, pal.textDim.r,
               pal.textDim.g, pal.textDim.b);
      continue;
    }
    double fill[4], ink[4];
    buttonColors(b, fill, ink);
    if (b.disabled) {  // faded as a whole below; keep its temporary surface local
      cairo_save(cr);
      cairo_rectangle(cr, b.x - kReach, b.y - above - kReach, b.w + 2 * kReach,
                      above + below + 2 * kReach);
      cairo_clip(cr);
      cairo_push_group(cr);
    }
    if (!b.volumeSlider && b.slider == MenuCommand::None) {  // sliders have no button behind
      const double radius =
          b.radius >= 0 ? std::min(b.radius, std::min(b.w, b.h) / 2) : kButtonRadius;
      if (fill[3] > 0) {
        roundedRect(cr, b.x, b.y, b.w, b.h, radius);
        cairo_set_source_rgba(cr, fill[0], fill[1], fill[2], fill[3]);
        cairo_fill(cr);
      }
    }
    drawButtonContent(cr, b, ink);
    if (!b.badge.empty()) {  // a small bright disc on the top-right edge (a navigation row's end)
      const double r = 17, bx = b.nav ? b.x + b.w - 32 : b.x + b.w - r * 0.6,
                   by = b.nav ? b.y + b.h / 2 : b.y + r * 0.6;
      cairo_arc(cr, bx, by, r, 0, 2 * M_PI);
      setColor(cr, pal.accent);
      cairo_fill(cr);
      drawText(cr, b.badge, bx - r, by - r, 2 * r, 2 * r, 20, true, true, 1, 1, 1);
    }
    if (b.disabled) {
      cairo_pop_group_to_source(cr);
      cairo_paint_with_alpha(cr, 0.35);
      cairo_restore(cr);
    }
  }
}

void Menu::buttonColors(const Button& b, double fill[4], double ink[4]) const {
  const Palette& pal = palette();
  Rgba f = pal.button, k = pal.text;
  if (b.nav) {
    // Navigation: the chosen row darker, with blue text.
    f = b.on ? Rgba{0, 0, 0, 0.30} : Rgba{0, 0, 0, 0};
    if (b.on) k = pal.textAccent;
  } else if ((b.listRow || b.card) && b.selected) {
    k = pal.textAccent;  // the playing file: blue name (cards also get a blue outline)
  } else if (b.selected) {
    f = pal.accent;  // chosen: filled blue
  } else if (b.bare || b.plain) {
    f = {0, 0, 0, 0};
    if (b.on) k = pal.textAccent;  // a chosen icon or heading: blue
  } else if (b.field) {
    f = pal.field;  // a text field, sunk into the panel
  }
  if (b.placeholder) k = pal.textDim;
  if (b.danger) {  // confirm button of a destructive dialog
    f = {0.72, 0.18, 0.16, 1};
    k = {1, 1, 1, 1};
  }
  fill[0] = f.r, fill[1] = f.g, fill[2] = f.b, fill[3] = f.a;
  ink[0] = k.r, ink[1] = k.g, ink[2] = k.b, ink[3] = k.a;
}

void Menu::drawButtonContent(cairo_t* cr, const Button& b, const double ink[4]) const {
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
    if (b.scrollbar) {
      drawScrollbar(cr, b);
      return;
    }
    if (b.field && b.on) {  // a text field being typed into: a blue outline
      const Palette& pal = palette();
      roundedRect(cr, b.x + 1.5, b.y + 1.5, b.w - 3, b.h - 3, b.radius >= 0 ? b.radius : 12);
      setColor(cr, pal.edge);
      cairo_set_line_width(cr, 3);
      cairo_stroke(cr);
    }
    if (b.field && b.icon == Icon::None) {  // a form's field: its text from the left
      drawFieldText(cr, b, ink);
      return;
    }
    if (b.tab) {
      drawIcon(cr, b.icon, b.x + 20 + 15, b.y + b.h / 2, 30, ink[0], ink[1], ink[2]);
      drawText(cr, b.label, b.x + 60, b.y, b.w - 78, b.h, b.fontSize, false, false, ink[0], ink[1],
               ink[2]);
      return;
    }
    if (b.toggle) {  // the label, and a pill switch at the right end
      const Palette& pal = palette();
      const double tw = 56, th = 30, tx = b.x + b.w - tw - 8, ty = b.y + (b.h - th) / 2;
      const int size = b.fontSize > 0 ? b.fontSize : kTextSize;
      if (b.lines > 1) {  // wrapped, centred in the row
        int lw, lh;
        pango_layout_get_pixel_size(textLayout(cr, b.label, size, false, tx - b.x - 24, b.lines, false),
                                    &lw, &lh);
        const double rgba[4] = {ink[0], ink[1], ink[2], 1};
        drawTextLines(cr, b.label, b.x + 12, b.y + (b.h - lh) / 2, tx - b.x - 24, size, b.lines, rgba,
                      false);
      } else {
        drawText(cr, b.label, b.x + 12, b.y, tx - b.x - 24, b.h, size, false, false, ink[0], ink[1],
                 ink[2]);
      }
      roundedRect(cr, tx, ty, tw, th, th / 2);
      if (b.on)
        setColor(cr, pal.accent);
      else
        cairo_set_source_rgba(cr, 0.42, 0.42, 0.45, 1);
      cairo_fill(cr);
      cairo_arc(cr, b.on ? tx + tw - th / 2 : tx + th / 2, ty + th / 2, th / 2 - 4, 0, 2 * M_PI);
      cairo_set_source_rgba(cr, 1, 1, 1, 1);
      cairo_fill(cr);
      return;
    }
    if (!b.caption.empty()) {  // format tile: icon, short label under it
      drawIcon(cr, b.icon, b.x + b.w / 2, b.y + (b.h - 28) / 2, b.h > 100 ? 46 : 36, ink[0], ink[1],
               ink[2]);
      drawText(cr, b.caption, b.x + 4, b.y + b.h - 34, b.w - 8, 28, 19, true, true, ink[0], ink[1],
               ink[2]);
      return;
    }
    if (b.card) {  // file browser grid: thumbnail (or folder), name in two lines, details
      const Palette& pal = palette();
      const double tw = b.w - 16, th = tw * 9 / 16, tx = b.x + 8, ty = b.y + 8;
      if (!b.thumbPath.empty() && (previewFrame(b.thumbPath, tx, ty) || pictureOf(b.thumbPath, tw))) {
        drawThumbnail(cr, b.thumbPath, tx, ty, tw, th, 10);
      } else {
        roundedRect(cr, tx, ty, tw, th, 10);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.06);
        cairo_fill(cr);
        if (b.icon != Icon::None)
          drawIcon(cr, b.icon, tx + tw / 2, ty + th / 2, 96, ink[0], ink[1], ink[2]);
      }
      if (stashCovers_ && !b.thumbPath.empty()) drawStudioMark(cr, b.thumbPath, tx, ty, tw, th);
      drawCoverStrip(cr, b, tx, ty, tw, th);  // under the stars, and the plays
      drawOfflineState(cr, b, tx, ty, tw, th);
      // The name in two lines right under the picture (its letters start a
      // little below the line's top), the size and date on the last line;
      // a name that would reach them gets fewer lines. With a byline, the
      // name has one line and the byline the next.
      const double detailY = b.y + b.h - 40;
      const double dim[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a};
      drawTextLines(cr, b.label, b.x + 14, ty + th - 4, b.w - 28, 26, b.byline.empty() ? 2 : 1,
                    ink, false, b.detail.empty() ? 0 : detailY + 4);
      if (!b.byline.empty())
        drawTextLines(cr, b.byline, b.x + 14, ty + th + 30, b.w - 28, 22, 1, dim, false);
      // A downloaded video's tick starts the last line.
      const double tick = downloadedMark(b.offlinePath) ? 30 : 0;
      if (tick > 0) drawOfflineTick(cr, b.x + 14 + 11, detailY + 15, 22);
      if (!b.detail.empty())
        drawTextLines(cr, b.detail, b.x + 14 + tick, detailY, b.w - 28 - tick, 22, 1, dim, false);
      if (b.selected) {  // the playing file: a blue outline
        roundedRect(cr, b.x + 1.5, b.y + 1.5, b.w - 3, b.h - 3, kButtonRadius);
        setColor(cr, pal.edge);
        cairo_set_line_width(cr, 3);
        cairo_stroke(cr);
      }
      return;
    }
    if (b.listRow) {  // a list row: thumbnail or icon, name, columns
      const double th = b.h - 4, tw = listThumbW(b), tx = b.x + 10, ty = b.y + 2;
      // A video's picture; with thumbnails off its icon, unless Stash has a cover.
      if (!b.thumbPath.empty() && (b.icon == Icon::None || previewFrame(b.thumbPath, tx, ty) ||
                                  pictureOf(b.thumbPath, tw))) {
        drawThumbnail(cr, b.thumbPath, tx, ty, tw, th, 5);
      } else if (b.icon != Icon::None) {
        drawIcon(cr, b.icon, tx + tw / 2, ty + th / 2, th * 0.7, ink[0], ink[1], ink[2]);
      }
      drawOfflineState(cr, b, tx, ty, tw, th);
      const Palette& pal = palette();
      const int size = b.fontSize > 0 ? b.fontSize : kTextSize;
      const double nameX = tx + tw + 16, nameW = listNameWidth(b);
      if (b.byline.empty()) {
        drawText(cr, b.label, nameX, b.y, nameW, b.h, size, false, false, ink[0], ink[1], ink[2]);
      } else {  // the name, then the byline dimmed after it while there is room
        constexpr double gap = 18;
        const double labelW = textWidth(b.label, size, false);
        const double bylineW = textWidth(b.byline, 24, false);
        const double w = std::min(labelW + 2, std::max(nameW * kBylineShare, nameW - bylineW - gap));
        drawText(cr, b.label, nameX, b.y, w, b.h, size, false, false, ink[0], ink[1], ink[2]);
        if (nameW - w - gap > 60)
          drawText(cr, b.byline, nameX + w + gap, b.y, nameW - w - gap, b.h, 24, false, false,
                   pal.textDim.r, pal.textDim.g, pal.textDim.b);
      }
      if (listColumns_) {  // right-aligned numbers, then the date
        const ListColumns c = listColumns(b.x + b.w);
        if (downloadedMark(b.offlinePath))
          drawOfflineTick(cr, c.status + kStatusColumnW / 2, b.y + b.h / 2, 22);
        const double sizeW = textWidth(b.size, 24, false), playsW = textWidth(b.plays, 24, false);
        drawText(cr, b.plays, c.plays + kPlaysColumnW - playsW, b.y, playsW + 2, b.h, 24, false,
                 false, pal.textDim.r, pal.textDim.g, pal.textDim.b);
        drawText(cr, b.size, c.size + kSizeColumnW - sizeW, b.y, sizeW + 2, b.h, 24, false, false,
                 pal.textDim.r, pal.textDim.g, pal.textDim.b);
        drawText(cr, b.date, c.date, b.y, kDateColumnW, b.h, 24, false, false, pal.textDim.r,
                 pal.textDim.g, pal.textDim.b);
      }
      return;
    }
    if (b.label.empty()) {  // icon only: round buttons scale it, slim list rows shrink it
      const double size = b.w == b.h ? b.w * (b.w >= kPlayButton ? 0.42 : 0.5)
                                     : std::min(kIconSize, b.h * 0.62);
      drawIcon(cr, b.icon, b.x + b.w / 2, b.y + b.h / 2, size, ink[0], ink[1], ink[2]);
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
      drawIcon(cr, b.icon, cx, cy, iconSize, ink[0], ink[1], ink[2]);
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
    drawText(cr, b.label, textX, b.y, textW, b.h, b.fontSize > 0 ? b.fontSize : kTextSize, false,
             center, ink[0], ink[1], ink[2]);
  }
}

void Menu::fieldTextBox(const Button& b, double& x, double& w, int& size) const {
  size = b.fontSize > 0 ? b.fontSize : kTextSize;
  x = b.x + 16;
  w = b.w - 32;
  if (b.icon != Icon::None) {  // a search: after its icon, as drawButtonContent() has it
    x = b.x + 35 + kIconSize * 0.75 + 20;
    w = b.w - (x - b.x) - 35;
  }
}

// Typed into, the whole text, scrolled only as far as it takes to keep the
// caret and a little beside it in view.
void Menu::drawFieldText(cairo_t* cr, const Button& b, const double ink[4]) const {
  double x = 0, w = 0;
  int size = 0;
  fieldTextBox(b, x, w, size);
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
  const double left = x - fieldShift_, caretH = std::round(size * 1.2);
  cairo_save(cr);
  cairo_rectangle(cr, x - 2, b.y, w + 4, b.h);
  cairo_clip(cr);
  cairo_set_source_rgba(cr, ink[0], ink[1], ink[2], ink[3]);
  cairo_move_to(cr, left, onPixel(b.y + (b.h - textH) / 2));
  pango_cairo_show_layout(cr, layout);
  cairo_rectangle(cr, std::round(left + caretX) - 1, std::round(b.y + (b.h - caretH) / 2), 2, caretH);
  cairo_fill(cr);
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
  PangoLayout* layout = textLayout(canvases_[kMain].cr(), text, size, bold, w, std::max(1, lines), false);
  return std::max(1, pango_layout_get_line_count(layout));
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
  for (const Group& g : groups_) {
    if (g.rule) {
      cairo_rectangle(cr, g.x, g.y, g.w, g.h);
      setColor(cr, pal.edge, 0.5);
      cairo_fill(cr);
      continue;
    }
    groupFrame(cr, g.x, g.y, g.w, g.h, pal);
    drawText(cr, g.title, g.x + 16, g.y, g.w - 32, kGroupTitleH, kTextSize, true, true, pal.text.r,
             pal.text.g, pal.text.b);
  }
  cairo_restore(cr);
}

// The page scrolls between its top and bottom margins' middles; the scroll
// bar is in its right margin, beside the groups.
void Menu::scrollGroups(size_t first) {
  double bottom = 0;
  for (const Group& g : groups_) bottom = std::max(bottom, g.y + g.h);
  for (size_t i = first; i < buttons_.size(); ++i)
    if (buttons_[i].panel == kBrowser && !buttons_[i].popup)
      bottom = std::max(bottom, buttons_[i].y + buttons_[i].h);
  listTop_ = kPagePad / 2;
  listBottom_ = kBrowserHeight - kPagePad / 2;
  pageScrollMax_ = std::max(0.0, bottom + kPagePad - kBrowserHeight);
  pageScroll_ = std::clamp(pageScroll_, 0.0, pageScrollMax_);
  if (pageScrollMax_ <= 0) return;
  for (Group& g : groups_) g.y -= pageScroll_;
  for (size_t i = first; i < buttons_.size(); ++i) {
    Button& b = buttons_[i];
    if (b.panel != kBrowser) continue;
    b.y -= pageScroll_;
    b.scrolls = true;
  }
  Button bar;
  bar.panel = kBrowser;
  bar.x = kBrowserWidth - kPagePad - 2;
  bar.y = kPagePad;
  bar.w = 20;
  bar.h = kBrowserHeight - 2 * kPagePad;
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

// A row that switches something on or off: its label, and a pill switch at
// the right end. The whole row is the button.
Menu::Button& Menu::addToggleRow(double x, double y, double w, const std::string& label, bool on,
                                 std::function<MenuCommand()> action, bool disabled) {
  Button b;
  b.panel = kBrowser;
  b.x = x;
  b.y = y;
  b.w = w;
  b.h = kSettingRowH;
  b.label = label;
  b.toggle = true;
  b.on = on;
  b.bare = true;
  b.disabled = disabled;
  b.action = std::move(action);
  buttons_.push_back(b);
  return buttons_.back();
}

// A number: its label, then "−", the value on a slider and "+". The buttons
// and slider use the same single-unit steps for fine adjustment.
void Menu::addValueRow(double x, double y, double w, double labelW, const std::string& label,
                       const std::string& value, int now, int lo, int hi,
                       std::function<MenuCommand()> less, std::function<MenuCommand()> more,
                       bool disabled, bool logarithmic, const char* tooltip) {
  addLabel(x + 12, y, labelW - 12, kSettingRowH, label).disabled = disabled;
  constexpr double stepW = 44, gap = 6;
  const MenuCommand c = less();
  Button minus = textButton("−", Icon::None, std::move(less));
  Button plus = textButton("+", Icon::None, std::move(more));
  minus.x = x + labelW;
  plus.x = x + w - stepW;
  for (Button* b : {&minus, &plus}) {
    b->panel = kBrowser;
    b->y = y;
    b->w = stepW;
    b->h = kSettingRowH;
    b->disabled = disabled;
    b->tooltip = tooltip;
    buttons_.push_back(*b);
  }
  Button field;
  field.panel = kBrowser;
  field.x = minus.x + stepW + gap;
  field.y = y;
  field.w = plus.x - gap - field.x;
  field.h = kSettingRowH;
  field.slider = c.type;
  field.sliderOption = c.option;
  field.sliderValue = now;
  field.sliderMin = lo;
  field.sliderMax = hi;
  field.sliderLog = logarithmic;
  field.sliderText = value;
  field.action = [] { return MenuCommand{}; };  // clickable; click() reads the slider
  field.disabled = disabled;
  field.tooltip = tooltip;
  buttons_.push_back(field);
}

// One of several: the label, then the options side by side, the chosen one blue.
void Menu::addChoiceRow(double x, double y, double w, double labelW, const std::string& label,
                        const std::vector<std::string>& options, int chosen,
                        const std::function<MenuCommand(int)>& choose, bool disabled,
                        const char* tooltip) {
  if (labelW > 0) addLabel(x + 12, y, labelW - 12, kSettingRowH, label).disabled = disabled;
  const double gap = 8, x0 = x + labelW;
  const double each = (w - labelW - gap * (options.size() - 1)) / options.size();
  for (size_t i = 0; i < options.size(); ++i) {
    Button b = textButton(options[i], Icon::None, [choose, i] { return choose(static_cast<int>(i)); },
                          static_cast<int>(i) == chosen);
    b.panel = kBrowser;
    b.x = x0 + i * (each + gap);
    b.y = y;
    b.w = each;
    b.h = kSettingRowH;
    b.fontSize = 24;
    b.disabled = disabled;
    b.tooltip = tooltip;
    buttons_.push_back(b);
  }
}

double Menu::listNameWidth(const Button& row) const {
  const double end = listColumns_ ? listColumns(row.x + row.w).status : row.x + row.w;
  return end - 16 - (row.x + 10 + listThumbW(row) + 16);
}

cairo_surface_t* Menu::pictureOf(const std::string& path, double w) const {
  cairo_surface_t* picture = findPicture(path, w);
  shownPictures_[path] = {w, picture, !picture && !pictureComing(path)};
  return picture;
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
         (findPicture(b.thumbPath, picture->second.w) != picture->second.surface ||
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
  if (!thumb) thumb = pictureOf(path, w);
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
    // No cover (Stash's placeholder for one is not taken) and no frame.
    if (!pictureComing(path)) {
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
