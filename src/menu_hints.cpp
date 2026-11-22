// The one-line hints after first-run setup (hints.h): each line with the
// glyphs and names of the controls it is about, as the app binds them;
// and the messages over the video (notices.h) in the same pill.

#include "menu_setup.h"

#include "menu_style.h"

#include <cassert>

namespace {

using Source = ControllerBindings::Source;
using PillGlyph = SetupPages::PillGlyph;

// The hint's canvas, in layout pixels: room for a longer line than setup's.
constexpr int kHintW = 1400 + 2 * kPillShadow, kHintH = kPillHeight;

}  // namespace

void Menu::setHintBindings(const ControllerBindings& bindings) {
  if (!hints_) hints_ = std::make_unique<MenuHints>();
  MenuHints& h = *hints_;
  if (bindings == h.bindings && (h.art || h.loading.running())) return;
  h.bindings = bindings;
  h.loading.start([bindings, reuse = h.art] {
    return loadControllerArt(bindings, 0, 0, kSetupGlyph, menu_geometry::kRasterScale, reuse);
  });
}

const Texture* Menu::drawHint(Hint hint, int& width, int& height) {
  if (!hints_) hints_ = std::make_unique<MenuHints>();
  MenuHints& h = *hints_;
  if (h.loading.running()) h.loading.take(h.art);
  if (!h.ready && !initCanvas(h.canvas, kHintW, kHintH, menu_geometry::kRasterScale))
    return nullptr;
  h.ready = true;
  const ControllerStrings& t = text().controls;
  const ControllerBindings& bound = h.art ? h.art->bindings : h.bindings;
  auto glyph = [&](const std::vector<Source>& sources, ControlAction action, bool upDown,
                   const std::string& fallback) {
    return SetupPages::pillGlyph(h.art.get(), sources, action, upDown, fallback);
  };
  // Its name, as Your controllers names it; `fallback` without a binding.
  auto named = [&](const std::vector<Source>& sources, const char* fallback) {
    return sources.empty() ? std::string(fallback) : controlNames({sources[0]}, t);
  };
  std::vector<PillGlyph> glyphs;
  char line[512];
  switch (hint) {
    case Hint::Menu: {
      const auto sources = handSources(bound, ControlAction::Menu, 1);
      glyphs.push_back(glyph(sources, ControlAction::Menu, false, "button_b"));
      std::snprintf(line, sizeof(line), t.menuHint, named(sources, "B").c_str());
      break;
    }
    case Hint::Scroll: {
      const auto sources = handSources(bound, ControlAction::Stick, 1);
      glyphs.push_back(glyph(sources, ControlAction::Stick, true, "button_thumbstick_scroll"));
      std::snprintf(line, sizeof(line), "%s", t.scrollHint);
      break;
    }
    case Hint::Recenter: {
      // As bound: a button, else a button with grip held (main.cpp shows
      // none without either).
      const auto sources = handSources(bound, ControlAction::Recenter, 1);
      if (sources.empty()) {
        std::vector<Source> grip;
        for (const Source& s : bound.gripSources[static_cast<int>(ControlAction::Recenter)])
          if (grip.empty() || s.hand == 1) grip = {s};
        glyphs.push_back(glyph(handSources(bound, ControlAction::Grab, 1), ControlAction::Grab, false,
                               "button_grip"));
        glyphs.push_back(glyph(grip, ControlAction::Recenter, false, "button_thumbstick_click"));
        // A control both hands have: which hand's.
        std::string name = named(grip, "");
        if (!grip.empty()) {
          const Source& s = grip[0];
          const ControllerType type = controllerType(bound.profiles[s.hand]);
          bool both = false;
          if (type != ControllerType::Count)
            for (const LayoutControl& c : controllerLayout(type).controls)
              both = both || (c.hand != s.hand && c.input == s.input);
          if (both) {
            const ControlsPageStrings& cp = text().controlsPage;
            if (s.input.find("thumbstick/click") != std::string::npos) name = cp.stickClick;
            char hand[256];
            std::snprintf(hand, sizeof(hand), s.hand ? cp.rightOne : cp.leftOne, name.c_str());
            name = hand;
          }
        }
        std::snprintf(line, sizeof(line), t.recenterGripHint, name.c_str());
        break;
      }
      glyphs.push_back(glyph(sources, ControlAction::Recenter, false, "button_thumbstick_click"));
      if (controlOf(sources[0].input) == "thumbstick")
        std::snprintf(line, sizeof(line), "%s", sources[0].hand == 0 ? t.recenterLeftHint : t.recenterRightHint);
      else
        std::snprintf(line, sizeof(line), t.recenterHint, named(sources, "").c_str());
      break;
    }
    case Hint::Volume:
    case Hint::Count: {
      // The gesture that sets it (the right hand's where both have one):
      // its buttons, then the twist (main.cpp shows none without one).
      const ControllerBindings::Twist* twist = nullptr;
      for (const ControllerBindings::Twist& w : bound.twists)
        if (w.value == static_cast<int>(GestureValue::Volume) && !w.controls.empty() &&
            (!twist || w.controls[0].hand == 1))
          twist = &w;
      if (!twist) {
        const auto grip = handSources(bound, ControlAction::Grab, 1);
        const auto trigger = handSources(bound, ControlAction::Select, 1);
        glyphs.push_back(glyph(grip, ControlAction::Grab, false, "button_grip"));
        glyphs.push_back(glyph(trigger, ControlAction::Select, false, "button_trigger"));
        std::snprintf(line, sizeof(line), t.volumeHint, named(grip, t.grip).c_str(),
                      named(trigger, t.trigger).c_str());
        break;
      }
      const std::vector<Source>& controls = twist->controls;
      for (const Source& s : controls) {
        const bool grip = controlOf(s.input) == "squeeze";
        glyphs.push_back(glyph({s}, grip ? ControlAction::Grab : ControlAction::Select, false,
                               grip ? "button_grip" : "button_trigger"));
      }
      if (controls.size() == 1)
        std::snprintf(line, sizeof(line), text().gesturePage.volumeOneHint, named({controls[0]}, "").c_str());
      else
        std::snprintf(line, sizeof(line), t.volumeHint, named({controls[0]}, "").c_str(),
                      named({controls[1]}, "").c_str());
      break;
    }
  }
  cairo_t* cr = h.canvas.cr();
  h.canvas.clear();
  SetupPages::drawPill(*this, cr, kHintW, kHintH, glyphs, line);
  h.canvas.upload();
  width = kHintW;
  height = kHintH;
  return h.canvas.texture();
}

void Menu::drawNotice(cairo_t* cr, double w, double h, NoticeIcon icon, const std::string& text,
                      double drawn[4]) {
  static_assert(kNoticeH[0] == kPillH && kNoticeShadow == kPillShadow, "the hints' pill");
  // The icon from the pill's left, then the text; at its right as the hints'.
  constexpr double kIconX = 28, kIconSize = 34, kIconGap = 16, kEnd = 38;
  constexpr int kSecondSize = kit::kRowSize;
  const size_t newline = text.find('\n');
  const bool two = newline != std::string::npos;
  const std::string first = text.substr(0, newline);
  const std::string second = two ? text.substr(newline + 1) : std::string();
  Icon shown = Icon::None;
  switch (icon) {
    case NoticeIcon::None: break;
    case NoticeIcon::Recenter: shown = Icon::Recenter; break;
    case NoticeIcon::Viewpoint: shown = Icon::FormatDome; break;
    case NoticeIcon::Passthrough: shown = Icon::Headset; break;
    case NoticeIcon::Card: shown = Icon::Card; break;
    case NoticeIcon::Usb: shown = Icon::Usb; break;
    case NoticeIcon::Drive: shown = Icon::Storage; break;
    case NoticeIcon::Warning: shown = Icon::Warning; break;
  }
  const double textX = shown != Icon::None ? kIconX + kIconSize + kIconGap : kEnd;
  const double textW = std::max(textWidth(first, kPillSize, two),
                                two ? textWidth(second, kSecondSize, false) : 0.0);
  const double pillH = kNoticeH[two];
  const double pillW = std::min({w - 2 * kNoticeShadow, double(kNoticeMaxW), textX + textW + kEnd});
  const double x = std::round((w - pillW) / 2), y = std::round((h - pillH) / 2);
  assert(MenuFinishes::reach(Finish::Pop) <= kNoticeShadow && h >= pillH + 2 * kNoticeShadow);
  drawFinish(cr, Finish::Pop, x, y, pillW, pillH, kPillH / 2.0);
  const Palette pal = palette();
  if (shown != Icon::None) {
    const Rgba& ink = icon == NoticeIcon::Warning ? pal.danger : pal.text;
    drawIcon(cr, shown, x + kIconX + kIconSize / 2, y + pillH / 2, kIconSize, ink.r, ink.g, ink.b);
  }
  // Two lines: the first in the semibold, the second dimmer and smaller.
  const double lineW = pillW - textX - kEnd + 4;
  if (!two) {
    drawText(cr, first, x + textX, y, lineW, pillH, kPillSize, false, false, pal.text.r, pal.text.g,
             pal.text.b);
  } else {
    drawText(cr, first, x + textX, y + 20, lineW, 44, kPillSize, true, false, pal.text.r,
             pal.text.g, pal.text.b);
    drawText(cr, second, x + textX, y + 64, lineW, 36, kSecondSize, false, false, pal.textDim.r,
             pal.textDim.g, pal.textDim.b);
  }
  const double reach = kNoticeShadow;
  drawn[0] = std::max(0.0, x - reach);
  drawn[1] = std::max(0.0, y - reach);
  drawn[2] = std::min(w, x + pillW + reach) - drawn[0];
  drawn[3] = std::min(h, y + pillH + reach) - drawn[1];
}

void Menu::drawRingNotice(cairo_t* cr, double w, double h, const RingNotice& ring, double drawn[4]) {
  // The ring in the pill's round start (its box kRingBox across, kRingIn from
  // the pill's left), the text kRingGap after it; the stars kStar wide.
  constexpr double kRingIn = 8, kRingBox = 72, kRingGap = 14, kTextGap = 12, kEnd = 40;
  constexpr double kRadius = 31, kStroke = 8;
  constexpr int kStar = 34, kStarGap = 2;
  // The travel, in degrees clockwise from the top.
  constexpr double kFrom = -135, kTo = 135;
  constexpr double kPi = 3.14159265358979323846;
  const Palette pal = palette();
  const bool stars = ring.kind == RingNotice::Kind::Stars;
  const bool position = ring.kind == RingNotice::Kind::Position;
  const std::string& name = position ? ring.number : ring.name;
  const std::string& second = position ? ring.after : ring.number;
  const double nameW = textWidth(name, kPillSize, position);
  const double secondW = stars    ? 5 * kStar + 4 * kStarGap
                         : second.empty() ? 0.0
                                          : textWidth(second, kPillSize, !position);
  const double textX = kRingIn + kRingBox + kRingGap;
  const double pillH = kNoticeH[0];
  const double pillW = std::min({w - 2 * kNoticeShadow, double(kNoticeMaxW),
                                 textX + nameW + (secondW > 0 ? kTextGap + secondW : 0) + kEnd});
  const double x = std::round((w - pillW) / 2), y = std::round((h - pillH) / 2);
  drawFinish(cr, Finish::Pop, x, y, pillW, pillH, kPillH / 2.0);

  const double cx = x + kRingIn + kRingBox / 2, cy = y + pillH / 2;
  const auto angle = [&](double along) { return kFrom + (kTo - kFrom) * std::clamp(along, 0.0, 1.0); };
  // Cairo's angles go clockwise from 3 o'clock, in radians.
  const auto rad = [](double degrees) { return (degrees - 90) * kPi / 180; };
  const auto arc = [&](double a, double b, const Rgba& ink, double alpha, cairo_line_cap_t cap) {
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, kRadius, rad(a), rad(b));
    cairo_set_line_width(cr, kStroke);
    cairo_set_line_cap(cr, cap);
    cairo_set_source_rgba(cr, ink.r, ink.g, ink.b, alpha);
    cairo_stroke(cr);
  };
  const auto tick = [&](double a, double alpha, double reach) {
    const double s = std::sin(a * kPi / 180), c = -std::cos(a * kPi / 180);
    cairo_new_path(cr);
    cairo_move_to(cr, cx + (kRadius - reach) * s, cy + (kRadius - reach) * c);
    cairo_line_to(cr, cx + (kRadius + reach) * s, cy + (kRadius + reach) * c);
    cairo_set_line_width(cr, 2.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgba(cr, 1, 1, 1, alpha);
    cairo_stroke(cr);
  };
  const Rgba white{1, 1, 1, 1};
  cairo_save(cr);
  if (stars) {
    // Ten half-star steps, lit gold to the rating.
    const double step = (kTo - kFrom) / 10;
    for (int i = 0; i < 10; ++i)
      arc(kFrom + i * step + 2.5, kFrom + (i + 1) * step - 2.5, i < ring.halves ? pal.gold : white,
          i < ring.halves ? 1.0 : 0.17, CAIRO_LINE_CAP_BUTT);
  } else {
    arc(kFrom, kTo, white, 0.17, CAIRO_LINE_CAP_ROUND);
    for (const double t : ring.ticks)
      if (t > 0) tick(angle(t), 0.35, 4.5);
    // At an end, brighter: the accent mixed with white.
    const Rgba fill = ring.limit ? Rgba{pal.accent.r * 0.55 + 0.45, pal.accent.g * 0.55 + 0.45,
                                        pal.accent.b * 0.55 + 0.45, 1}
                                 : pal.accent;
    const double lo = std::min(angle(ring.from), angle(ring.at));
    const double hi = std::max(angle(ring.from), angle(ring.at));
    if (hi - lo > 0.5) arc(lo, hi, fill, 1, CAIRO_LINE_CAP_ROUND);
    if (ring.centred) tick(angle(0.5), 0.85, 8);
    if (ring.mark >= 0 && !(ring.centred && std::abs(ring.mark - 0.5) < 1e-6)) tick(angle(ring.mark), 0.75, 8);
    if (ring.limit) {
      const double a = angle(ring.at) * kPi / 180;
      const double px = cx + kRadius * std::sin(a), py = cy - kRadius * std::cos(a);
      cairo_new_path(cr);
      cairo_arc(cr, px, py, 12, 0, 2 * kPi);
      cairo_set_source_rgba(cr, pal.accent.r, pal.accent.g, pal.accent.b, 0.38);
      cairo_fill(cr);
      cairo_arc(cr, px, py, 6, 0, 2 * kPi);
      cairo_set_source_rgb(cr, 1, 1, 1);
      cairo_fill(cr);
    }
  }
  cairo_restore(cr);

  // The text: Position's number first in the semibold, then dim what follows.
  double tx = x + textX;
  const double room = x + pillW - kEnd + 4 - tx;
  drawText(cr, name, tx, y, std::min(nameW + 4, room), pillH, kPillSize, position, false, pal.text.r,
           pal.text.g, pal.text.b);
  tx += nameW + kTextGap;
  if (stars) {
    for (int i = 0; i < 5; ++i) {
      const int fill = ring.halves >= 2 * (i + 1) ? 2 : ring.halves > 2 * i ? 1 : 0;
      cairo_set_source_surface(cr, starImage(kStar, fill, StarInk::Gold), onPixel(tx),
                               onPixel(cy - kStar / 2.0));
      cairo_paint(cr);
      tx += kStar + kStarGap;
    }
  } else if (!second.empty()) {
    const Rgba& ink = position ? pal.textDim : pal.text;
    drawText(cr, second, tx, y, std::max(0.0, x + pillW - kEnd + 4 - tx), pillH, kPillSize, !position,
             false, ink.r, ink.g, ink.b);
  }
  const double reach = kNoticeShadow;
  drawn[0] = std::max(0.0, x - reach);
  drawn[1] = std::max(0.0, y - reach);
  drawn[2] = std::min(w, x + pillW + reach) - drawn[0];
  drawn[3] = std::min(h, y + pillH + reach) - drawn[1];
}

bool Menu::pointingAtLongList(int& page) const {
  page = static_cast<int>(page_);
  if (setup_ || !listOpen() || !onListContent(hovered_)) return false;
  if (dialog_ == Dialog::WhatsNew) return true;
  if (groupedPage()) return pageScrollMax_ > 0;
  if (overSide()) return side_.max > 0;
  return listCount_ > visibleRows_;
}

bool Menu::pointingAtVolume() const {
  if (setup_ || hovered_ < 0 || hovered_ >= static_cast<int>(buttons_.size())) return false;
  const Button& b = buttons_[hovered_];
  return b.panel == kMain && (b.volumeSlider || b.icon == Icon::VolumeUp || b.icon == Icon::Mute);
}
