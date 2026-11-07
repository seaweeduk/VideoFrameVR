#include "menu_style.h"

#include "platform.h"

#include <pango/pangocairo.h>

// What's new: the release notes (whats_new.h) in a dialog over the page,
// after an update or from Settings → Help. Its icon, title and notes hang
// off one left edge; it is as tall as its notes, up to kMaxH, and beyond
// that they scroll beside their own scroll bar. Its switch and Close are
// along the bottom.

namespace {

constexpr double kW = 1400, kMaxH = 1000;
constexpr double kPad = 64;  // the left edge: the icon, the title, the notes and the switch
constexpr double kTop = 48, kIcon = 84;
// Beside the icon and centred on it, the title over the line under it.
constexpr double kTitleH = 42, kSubtitleGap = 6, kSubtitleH = 31.2;
constexpr double kNotesY = kTop + kIcon + 36;
constexpr double kFullW = kW - 2 * kPad;
constexpr double kBar = 20;  // the scroll bar, in its gutter right of the notes
constexpr double kScrollingW = kFullW - kBar - 20;
constexpr double kButtonH = 80, kButtonW = 240;  // Close
constexpr double kFooter = 24 + 24 + kButtonH + 44;
constexpr double kMaxNotesH = kMaxH - kNotesY - kFooter;
constexpr double kFadeH = 60;  // the notes fade out above their bottom while more follow
// A release: its version and date, then its sections, each with its name in
// a column on the left and its lines beside it, bulleted.
constexpr int kVersionSize = 32, kDateSize = 22, kNoteSize = 26;
constexpr double kNoteLineH = kNoteSize * 1.35;  // a line of the notes, whatever the font's own
constexpr double kDateGap = 16, kVersionGap = 6;
constexpr double kReleaseGap = 26, kReleaseRulePad = 22;
constexpr double kSectionPad = 12, kLineGap = 4;
constexpr double kNameW = 150, kBulletW = 26;
constexpr double kRuleH = 1.5;
constexpr int kMaxLines = 8;
constexpr double kNoteInk[4] = {0.91, 0.91, 0.933, 1};
constexpr double kBox[3] = {0.1, 0.1, 0.13};  // the dialogs' fill (Menu::drawDialog())

// A note's lines kNoteLineH apart from y, each centred in its line.
void drawNote(cairo_t* cr, PangoLayout* layout, double x, double y) {
  PangoLayoutIter* line = pango_layout_get_iter(layout);
  do {
    PangoRectangle logical;
    pango_layout_iter_get_line_extents(line, nullptr, &logical);
    const double h = logical.height / double(PANGO_SCALE);
    const double baseline =
        (pango_layout_iter_get_baseline(line) - logical.y) / double(PANGO_SCALE);
    cairo_move_to(cr, x, onPixel(y + (kNoteLineH - h) / 2) + baseline);
    pango_cairo_show_layout_line(cr, pango_layout_iter_get_line_readonly(line));
    y += kNoteLineH;
  } while (pango_layout_iter_next_line(line));
  pango_layout_iter_free(line);
}

}  // namespace

void Menu::showWhatsNew(std::vector<ReleaseNotes> releases, const std::string& from,
                        const std::string& to) {
  whatsNew_ = {};
  whatsNew_.releases = std::move(releases);
  whatsNew_.from = from;
  whatsNew_.to = to;
  openDialog(Dialog::WhatsNew, "");
}

// The notes from their top, `width` wide, measured as drawWhatsNew() draws
// them.
void Menu::layoutWhatsNewNotes(double width) {
  using Piece = WhatsNewPiece;
  WhatsNewDialog& d = whatsNew_;
  d.pieces.clear();
  d.width = width;
  cairo_t* cr = canvases_[kMain].cr();
  const double versionH = [&] {
    int w = 0, h = 0;
    pango_layout_get_pixel_size(textLayout(cr, "0", kVersionSize, true, -1, 0, false), &w, &h);
    return static_cast<double>(h);
  }();
  const double lineW = width - kNameW - kBulletW;
  double y = 0;
  for (size_t r = 0; r < d.releases.size(); ++r) {
    const ReleaseNotes& release = d.releases[r];
    if (r > 0) {
      y += kReleaseGap;
      d.pieces.push_back({Piece::ReleaseRule, "", "", y, kRuleH});
      y += kRuleH + kReleaseRulePad;
    }
    d.pieces.push_back({Piece::Version, release.version, release.date, y, versionH});
    y += versionH + kVersionGap;
    for (size_t s = 0; s < release.sections.size(); ++s) {
      const ReleaseNotes::Section& section = release.sections[s];
      if (s > 0) {
        d.pieces.push_back({Piece::SectionRule, "", "", y, kRuleH});
        y += kRuleH;
      }
      y += kSectionPad;
      d.pieces.push_back({Piece::Section, section.name, "", y + kLineGap, kNoteLineH});
      double lines = y + kLineGap;
      for (size_t i = 0; i < section.lines.size(); ++i) {
        if (i > 0) lines += kLineGap;
        PangoLayout* layout =
            textLayout(cr, section.lines[i], kNoteSize, false, lineW, kMaxLines, false);
        const double h = kNoteLineH * pango_layout_get_line_count(layout);
        d.pieces.push_back({Piece::Line, section.lines[i], "", lines, h});
        lines += h;
      }
      y = std::max(y + kLineGap + kNoteLineH, lines) + kLineGap + kSectionPad;
    }
  }
  d.height = std::ceil(y);
}

Menu::WhatsNewBox Menu::whatsNewBox() const {
  const double notesH = std::min(whatsNew_.height, kMaxNotesH);
  const double h = kNotesY + notesH + kFooter;
  return {kPageCenterX - kW / 2, std::round((kBrowserHeight - h) / 2), h, notesH};
}

void Menu::layoutWhatsNew() {
  const WhatsNewStrings& t = text().whatsNew;
  // The notes at the full width while they fit; narrower, beside their
  // scroll bar, once they don't.
  WhatsNewDialog& d = whatsNew_;
  if (d.width <= 0) {
    layoutWhatsNewNotes(kFullW);
    if (d.height > kMaxNotesH) layoutWhatsNewNotes(kScrollingW);
  }
  d.max = std::max(0.0, d.height - kMaxNotesH);
  d.scroll = std::clamp(d.scroll, 0.0, d.max);
  const WhatsNewBox box = whatsNewBox();
  const double footY = box.y + kNotesY + box.notesH + 48;
  // The switch's label on the left edge (a switch's label is 12 into it).
  Button show;
  show.label = t.showAfterUpdates;
  show.toggle = show.bare = true;
  show.on = view_.showWhatsNew;
  show.x = box.x + kPad - 12;
  show.y = footY + (kButtonH - kSettingRowH) / 2;
  show.w = std::max(412.0, textWidth(show.label, kTextSize, false) + 112);
  show.h = kSettingRowH;
  show.action = command(MenuCommand::ToggleShowWhatsNew);
  Button close = textButton(t.close, Icon::None, [this] {
    dialog_ = Dialog::None;
    return MenuCommand{};
  }, true);
  close.x = box.x + kW - kPad - kButtonW;
  close.y = footY;
  close.w = kButtonW;
  close.h = kButtonH;
  close.fontSize = 27;
  for (Button* b : {&show, &close}) {
    b->panel = kBrowser;
    b->popup = true;
    buttons_.push_back(*b);
  }
  if (d.max <= 0) return;
  Button bar;
  bar.panel = kBrowser;
  bar.popup = true;
  bar.x = box.x + kW - kPad - kBar;
  bar.y = box.y + kNotesY;
  bar.w = kBar;
  bar.h = box.notesH;
  bar.radius = kBar / 2;
  bar.bare = true;
  bar.scrollbar = true;
  bar.action = [] { return MenuCommand{}; };  // handled in click()/drag()
  buttons_.push_back(bar);
}

void Menu::drawWhatsNew(cairo_t* cr) {
  using Piece = WhatsNewPiece;
  const WhatsNewStrings& t = text().whatsNew;
  const Palette& pal = palette();
  const WhatsNewBox box = whatsNewBox();
  const WhatsNewDialog& d = whatsNew_;
  roundedRect(cr, box.x, box.y, kW, box.h, 30);
  cairo_set_source_rgb(cr, kBox[0], kBox[1], kBox[2]);
  cairo_fill(cr);

  // The icon, and beside it the title over the line under it.
  drawBrand(cr, box.x + kPad, box.y + kTop, kIcon);
  const double textX = box.x + kPad + kIcon + 26, textW = kW - kPad - (textX - box.x);
  const double titleY = box.y + kTop + (kIcon - kTitleH - kSubtitleGap - kSubtitleH) / 2;
  drawText(cr, t.title, textX, titleY, textW, kTitleH, 40, true, false, pal.text.r, pal.text.g,
           pal.text.b);
  std::string subtitle = kAppTitle;
  if (d.releases.size() > 1 && !d.from.empty()) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), t.updatedFrom, d.from.c_str(), d.to.c_str());
    subtitle = buf;
  }
  drawText(cr, subtitle, textX, titleY + kTitleH + kSubtitleGap, textW, kSubtitleH, kNoteSize,
           false, false, pal.textDim.r, pal.textDim.g, pal.textDim.b);

  // The notes, cut off at their edges.
  const double x = box.x + kPad, top = box.y + kNotesY, w = d.width;
  const double lineW = w - kNameW - kBulletW;
  const Rgba& dim = pal.textDim;
  cairo_save(cr);
  cairo_rectangle(cr, x, top, w, box.notesH);
  cairo_clip(cr);
  for (const Piece& p : d.pieces) {
    const double y = top + p.y - d.scroll;
    if (y + p.h < top - 4 || y > top + box.notesH + 4) continue;
    switch (p.kind) {
      case Piece::ReleaseRule:
      case Piece::SectionRule:
        cairo_rectangle(cr, x, onPixel(y), w, kRuleH);
        if (p.kind == Piece::ReleaseRule)
          setColor(cr, pal.edge, 0.45);
        else
          cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
        cairo_fill(cr);
        break;
      case Piece::Version: {  // the date on the version's baseline
        PangoLayout* version = textLayout(cr, p.text, kVersionSize, true, -1, 0, false);
        PangoLayout* date = textLayout(cr, p.date, kDateSize, false, -1, 0, false);
        int versionW = 0, versionH = 0;
        pango_layout_get_pixel_size(version, &versionW, &versionH);
        const double baseline = pango_layout_get_baseline(version) / double(PANGO_SCALE);
        setColor(cr, pal.textAccent);
        cairo_move_to(cr, x, onPixel(y));
        pango_cairo_show_layout(cr, version);
        setColor(cr, pal.textDim);
        const double dateBaseline = pango_layout_get_baseline(date) / double(PANGO_SCALE);
        cairo_move_to(cr, x + versionW + kDateGap, onPixel(y + baseline - dateBaseline));
        pango_cairo_show_layout(cr, date);
        break;
      }
      case Piece::Section:
        drawText(cr, p.text, x, y, kNameW, p.h, kNoteSize, true, false, dim.r, dim.g, dim.b);
        break;
      case Piece::Line:
        drawText(cr, "•", x + kNameW + 4, y, kBulletW, kNoteLineH, kNoteSize, false, false, dim.r,
                 dim.g, dim.b);
        cairo_set_source_rgba(cr, kNoteInk[0], kNoteInk[1], kNoteInk[2], kNoteInk[3]);
        drawNote(cr, textLayout(cr, p.text, kNoteSize, false, lineW, kMaxLines, false),
                 x + kNameW + kBulletW, y);
        break;
    }
  }
  if (d.scroll < d.max) {
    const double fadeY = top + box.notesH - kFadeH;
    cairo_pattern_t* fade = cairo_pattern_create_linear(0, fadeY, 0, fadeY + kFadeH);
    cairo_pattern_add_color_stop_rgba(fade, 0, kBox[0], kBox[1], kBox[2], 0);
    cairo_pattern_add_color_stop_rgba(fade, 1, kBox[0], kBox[1], kBox[2], 1);
    cairo_rectangle(cr, x, fadeY, w, kFadeH);
    cairo_set_source(cr, fade);
    cairo_fill(cr);
    cairo_pattern_destroy(fade);
  }
  cairo_restore(cr);

  // The footer's line, across.
  cairo_rectangle(cr, x, top + box.notesH + 24, kFullW, kRuleH);
  cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
  cairo_fill(cr);
}

// By whole texture pixels. The notes and their scroll bar are drawn again,
// in whole rows of the browser (a dialog alone is drawn on cleared rows).
void Menu::scrollWhatsNew(double pixels) {
  WhatsNewDialog& d = whatsNew_;
  const double position = std::clamp(onPixel(d.scroll + pixels), 0.0, d.max);
  if (position == d.scroll) return;
  d.scroll = position;
  hoverQuiet_ = true;
  const WhatsNewBox box = whatsNewBox();
  browserChanged(box.y + kNotesY - 4, box.y + kNotesY + box.notesH + 4);
}

bool Menu::overWhatsNewNotes() const {
  if (dialog_ != Dialog::WhatsNew || pointerPanel_ != kBrowser) return false;
  const WhatsNewBox box = whatsNewBox();
  const double x = box.x + kPad, y = box.y + kNotesY;
  return pointerX_ >= x && pointerX_ <= x + whatsNew_.width && pointerY_ >= y &&
         pointerY_ <= y + box.notesH;
}
