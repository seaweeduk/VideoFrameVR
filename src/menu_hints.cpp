// The one-line hints after first-run setup (hints.h): each line with the
// glyphs and names of the controls it is about, as the runtime binds them;
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
  if (!h.ready && !h.canvas.init(*uploader_, kHintW, kHintH, menu_geometry::kRasterScale))
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
      const auto sources = handSources(bound, ControlAction::Recenter, 1);
      glyphs.push_back(glyph(sources, ControlAction::Recenter, false, "button_thumbstick_click"));
      if (sources.empty() || controlOf(sources[0].input) == "thumbstick")
        std::snprintf(line, sizeof(line), "%s",
                      !sources.empty() && sources[0].hand == 0 ? t.recenterLeftHint : t.recenterRightHint);
      else
        std::snprintf(line, sizeof(line), t.recenterHint, named(sources, "").c_str());
      break;
    }
    case Hint::Volume:
    case Hint::Count: {
      const auto grip = handSources(bound, ControlAction::Grab, 1);
      const auto trigger = handSources(bound, ControlAction::Select, 1);
      glyphs.push_back(glyph(grip, ControlAction::Grab, false, "button_grip"));
      glyphs.push_back(glyph(trigger, ControlAction::Select, false, "button_trigger"));
      std::snprintf(line, sizeof(line), t.volumeHint, named(grip, t.grip).c_str(),
                    named(trigger, t.trigger).c_str());
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
