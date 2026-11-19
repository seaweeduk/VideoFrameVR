// First-run setup's Your controllers (docs/alpha/first-run.md, step 2):
// SteamVR's drawings of both controllers, every control the app uses
// labelled beside them, lit while held. Without drawings, the same labels
// as a list.

#include "menu_setup.h"

#include "menu_style.h"

#include <pango/pangocairo.h>

namespace {

using Step = SetupMenu::Step;
using Label = SetupControls::Label;
using Source = ControllerBindings::Source;

constexpr double kW = Menu::kSetupWidth;
constexpr double kTop = setup::kTop, kBottom = setup::kBottom;  // under the head, over the footer
// With the drawings: a column of labels for each controller outside them,
// one for what either does between them.
constexpr double kLabelsX[3] = {setup::kTextLeft, 544, 1068}, kLabelsW[3] = {252, 272, 252};
constexpr double kDrawingX[2] = {302, 828}, kDrawingW = 230, kDrawingTop = kTop + kit::kSpace,
                 kDrawingH = kBottom - kit::kSpace - kDrawingTop;
// Without: the columns side by side, each under its heading.
constexpr double kListW = 400, kListGap = 40, kRowsTop = kTop + 6 + kit::kGroupTitle + kit::kSpaceTight;
// A label: the glyph, its title beside it, the line under the title.
constexpr double kTextX = kSetupGlyph + 12, kTitleH = 38, kLabelGap = 22;
constexpr int kTitleSize = 28, kLineSize = 23, kLineLines = 4;
// A held control's ring, and how far its glow reaches.
constexpr double kRingR = 16, kRing = kRingR + 11;

struct Role {
  ControlAction action;
  int hand;     // -1 either
  bool upDown;  // the stick up or down, else sideways
  const char* ControllerStrings::*title;
  const char* ControllerStrings::*line;  // printf: the names; none: the names alone
};
const Role kRoles[] = {
    {ControlAction::Select, -1, false, &ControllerStrings::choose, &ControllerStrings::chooseLine},
    {ControlAction::Grab, -1, false, &ControllerStrings::move, &ControllerStrings::moveLine},
    {ControlAction::Menu, -1, false, &ControllerStrings::menu, nullptr},
    {ControlAction::PlayPause, -1, false, &ControllerStrings::playPause, nullptr},
    {ControlAction::Recenter, -1, false, &ControllerStrings::recenter, nullptr},
    {ControlAction::Stick, 0, false, &ControllerStrings::turn, &ControllerStrings::sideways},
    {ControlAction::Stick, 1, false, &ControllerStrings::seek, &ControllerStrings::sideways},
    {ControlAction::Stick, -1, true, &ControllerStrings::scroll, &ControllerStrings::upDown},
};

// A line starting with a control's name, as a sentence: "Trigger or bumper".
std::string capitalized(const std::string& line) {
  if (line.empty() || !g_utf8_validate(line.c_str(), -1, nullptr)) return line;
  char first[6];
  const int n = g_unichar_to_utf8(g_unichar_toupper(g_utf8_get_char(line.c_str())), first);
  return std::string(first, n) + (g_utf8_next_char(line.c_str()));
}

void paintImage(cairo_t* cr, cairo_surface_t* image, double x, double y, double alpha = 1) {
  constexpr double s = menu_geometry::kRasterScale;
  cairo_save(cr);
  cairo_translate(cr, std::round(x * s) / s, std::round(y * s) / s);
  cairo_scale(cr, 1 / s, 1 / s);
  cairo_set_source_surface(cr, image, 0, 0);
  cairo_paint_with_alpha(cr, alpha);
  cairo_restore(cr);
}

// Where a label's lines start toward x: from its side facing it, level with
// its title.
void anchor(const Label& l, double x, double& ax, double& ay) {
  ax = x < l.x ? l.x - 6 : l.x + l.w + 6;
  ay = l.y + kTitleH / 2;
}

// Stacks a column's labels (in the order of their wanted heights) without
// overlaps, each run of touching ones placed where its labels are on
// average as far above as below where they want to be, within the page.
void stack(const std::vector<Label*>& column) {
  struct Run {
    size_t first, end;
    double top, height, wanted;  // wanted: the sum of the tops each label would give the run
  };
  std::vector<Run> runs;
  for (size_t i = 0; i < column.size(); ++i) {
    const double top = column[i]->want - kTitleH / 2;
    runs.push_back({i, i + 1, top, column[i]->h, top});
    while (true) {
      Run& r = runs.back();
      r.top = std::max(kTop, std::min(r.wanted / (r.end - r.first), kBottom - r.height));
      if (runs.size() < 2) break;
      Run& before = runs[runs.size() - 2];
      if (before.top + before.height + kLabelGap <= r.top) break;
      const double offset = before.height + kLabelGap;  // r's labels sit this much lower in it
      before.wanted += r.wanted - offset * (r.end - r.first);
      before.height = offset + r.height;
      before.end = r.end;
      runs.pop_back();
    }
  }
  for (const Run& r : runs) {
    double y = r.top;
    for (size_t i = r.first; i < r.end; ++i) {
      column[i]->y = y;
      y += column[i]->h + kLabelGap;
    }
  }
}

}  // namespace

void Menu::setSetupControls(const ControllerBindings& bindings) {
  if (!setup_ || bindings == setup_->controls.bindings) return;
  setup_->controls.bindings = bindings;
  SetupPages::loadControls(*this);
}

void Menu::setSetupHeld(uint32_t held) {
  if (!setup_ || setup_->controls.held == held) return;
  SetupControls& c = setup_->controls;
  const uint32_t was = c.held;
  c.held = held;
  if (setup_->step != Step::Controls || dirty_) return;
  // Only what changes is drawn again: a label's title, and the rings on a
  // hand's controls, all of them in one box.
  for (const Label& l : c.labels) {
    if (SetupPages::lit(l, held) != SetupPages::lit(l, was))
      SetupPages::damaged(*this, l.y, l.y + kTitleH, l.x + kTextX, l.x + l.w);
    for (int h = 0; h < 2; ++h) {
      if (!((held ^ was) & heldBit(h, l.control))) continue;
      double box[4] = {1e9, 1e9, -1e9, -1e9};
      for (const SetupControls::Point& p : l.points)
        if (p.hand == h) {
          box[0] = std::min(box[0], p.x - kRing), box[1] = std::min(box[1], p.y - kRing);
          box[2] = std::max(box[2], p.x + kRing), box[3] = std::max(box[3], p.y + kRing);
        }
      if (box[0] < box[2]) SetupPages::damaged(*this, box[1], box[3], box[0], box[2]);
    }
  }
}

void SetupPages::loadControls(Menu& m) {
  SetupControls& c = m.setup_->controls;
  c.loading.start([bindings = c.bindings, reuse = c.art] {
    return loadControllerArt(bindings, kDrawingW, kDrawingH, kSetupGlyph,
                             menu_geometry::kRasterScale, reuse);
  });
}

void SetupPages::updateControls(Menu& m) {
  SetupMenu& s = *m.setup_;
  std::shared_ptr<const ControllerArt> art;
  if (!s.controls.loading.take(art)) return;
  const bool trigger = s.controls.art && s.controls.art->glyphs.count("button_trigger");
  s.controls.art = art;
  s.cuesDrawn = false;  // the button the cue names
  if (s.step == Step::Controls || (s.step == Step::Welcome && s.controller && !trigger))
    m.dirty_ = true;
}

bool SetupPages::lit(const Label& label, uint32_t held) {
  for (int h = 0; h < 2; ++h)
    if ((label.hands >> h & 1) && (held & heldBit(h, label.control))) return true;
  return false;
}

bool SetupPages::drawGlyph(Menu& m, cairo_t* cr, const std::string& name, double cx, double cy) {
  const SetupControls& c = m.setup_->controls;
  if (!c.art || !c.art->glyphs.count(name)) return false;
  paintImage(cr, c.art->glyphs.at(name).get(), cx - kSetupGlyph / 2.0, cy - kSetupGlyph / 2.0);
  return true;
}

void SetupPages::layoutControls(Menu& m) {
  SetupControls& c = m.setup_->controls;
  c.labels.clear();
  if (!c.art) return;  // until the first is read
  const ControllerArt& art = *c.art;
  const ControllerStrings& t = m.text().controls;
  c.picture = art.hands[0].image && art.hands[1].image;
  for (int h = 0; h < 2 && c.picture; ++h) {
    c.drawX[h] = kDrawingX[h] + (kDrawingW - art.hands[h].width) / 2;
    c.drawY[h] = kDrawingTop + (kDrawingH - art.hands[h].height) / 2;
  }
  // Each role: one label for both hands where they are bound alike, else one
  // for each hand bound.
  for (const Role& role : kRoles) {
    std::vector<Source> hands[2];
    for (const Source& s : art.bindings.sources[static_cast<int>(role.action)])
      if (role.hand < 0 || s.hand == role.hand) hands[s.hand].push_back(s);
    auto inputs = [](const std::vector<Source>& sources) {
      std::set<std::string> out;
      for (const Source& s : sources) out.insert(controlOf(s.input));
      return out;
    };
    const bool alike = !hands[0].empty() && !hands[1].empty() && inputs(hands[0]) == inputs(hands[1]);
    for (int h = alike ? 1 : 0; h < 2; ++h) {
      if (hands[h].empty()) continue;
      Label l;
      l.hands = alike ? 3 : 1 << h;
      l.control = role.upDown ? kStickUpDown : static_cast<int>(role.action);
      l.column = alike ? 1 : 2 * h;
      sortSources(hands[h]);
      for (const Source& s : hands[h]) {
        l.glyph = controlGlyph(s.input, role.action, role.upDown);
        if (art.glyphs.count(l.glyph)) break;
      }
      l.title = t.*role.title;
      const std::string names = controlNames(hands[h], t);
      if (role.line) {
        char line[512];
        std::snprintf(line, sizeof(line), t.*role.line, names.c_str());
        l.line = capitalized(line);
      } else {
        l.line = capitalized(names);
      }
      // Its points: each hand's controls it is bound to, on that hand's drawing.
      for (int p = 0; p < 2 && c.picture; ++p) {
        if (!(l.hands >> p & 1)) continue;
        const std::vector<Source>& on = alike ? hands[p] : hands[h];
        for (const Source& s : on) {
          const auto at = art.hands[p].points.find(controlOf(s.input));
          if (at == art.hands[p].points.end()) continue;
          const SetupControls::Point point{p, c.drawX[p] + at->second[0], c.drawY[p] + at->second[1]};
          if (std::none_of(l.points.begin(), l.points.end(), [&](const SetupControls::Point& q) {
                return q.hand == point.hand && q.x == point.x && q.y == point.y;
              }))
            l.points.push_back(point);
        }
      }
      l.lit = lit(l, c.held);
      c.labels.push_back(std::move(l));
    }
  }

  // The columns: beside the drawings, or side by side as a list (those with
  // labels, centred).
  int used[3] = {0, 0, 0};
  for (const Label& l : c.labels) used[l.column] = 1;
  const int columns = used[0] + used[1] + used[2];
  double listX = (kW - columns * kListW - (columns - 1) * kListGap) / 2;
  cairo_t* cr = m.setup_->canvas.cr();
  for (int column = 0; column < 3; ++column) {
    if (!used[column]) continue;
    const double x = c.picture ? kLabelsX[column] : listX;
    const double w = c.picture ? kLabelsW[column] : kListW;
    listX += kListW + kListGap;
    std::vector<Label*> stacked;
    double rowY = kRowsTop;
    for (Label& l : c.labels) {
      if (l.column != column) continue;
      l.x = x, l.w = w;
      int lineW = 0, lineH = 0;
      PangoLayout* layout = m.textLayout(cr, l.line, kLineSize, false, w - kTextX, kLineLines, false);
      pango_layout_get_pixel_size(layout, &lineW, &lineH);
      l.h = std::max<double>(kSetupGlyph, kTitleH + lineH);
      if (!c.picture) {  // in the list, one under another
        l.y = rowY;
        rowY += l.h + kLabelGap;
        continue;
      }
      // Beside its points; without any, last.
      l.want = l.points.empty() ? 1e9 : 0;
      for (const SetupControls::Point& p : l.points) l.want += p.y / l.points.size();
      stacked.push_back(&l);
    }
    std::stable_sort(stacked.begin(), stacked.end(),
                     [](const Label* a, const Label* b) { return a->want < b->want; });
    stack(stacked);
  }
  // Never under the footer: what doesn't fit (rebound to many controls) is
  // left out.
  c.labels.erase(std::remove_if(c.labels.begin(), c.labels.end(),
                                [](const Label& l) { return l.y + l.h > kBottom; }),
                 c.labels.end());
}

void SetupPages::drawControls(Menu& m, cairo_t* cr) {
  const SetupStrings& st = m.text().setup;
  const ControllerStrings& t = m.text().controls;
  const Palette pal = m.palette();
  drawHead(m, cr, st.controlsTitle, st.controlsText);
  drawDots(m, cr);
  const SetupControls& c = m.setup_->controls;
  if (!c.art) return;
  if (c.labels.empty()) {  // no controller is on
    m.drawText(cr, st.turnOnControllers, 0, (kTop + kBottom) / 2 - 25, kW, 50, 28, false, true, pal.text.r, pal.text.g,
               pal.text.b);
    return;
  }

  if (c.picture) {
    for (int h = 0; h < 2; ++h) paintImage(cr, c.art->hands[h].image.get(), c.drawX[h], c.drawY[h]);
    // Lines from the labels to their controls.
    for (const Label& l : c.labels)
      for (const SetupControls::Point& p : l.points) {
        double ax, ay;
        anchor(l, p.x, ax, ay);
        cairo_move_to(cr, ax, ay);
        cairo_line_to(cr, p.x, p.y);
      }
    setColor(cr, pal.accent, 0.75);
    cairo_set_line_width(cr, 2.5);
    cairo_stroke(cr);
    // A held control: a ring glowing round it, drawn once (its wide strokes
    // take a millisecond each).
    SetupControls& drawn = m.setup_->controls;
    const std::array<double, 3> accent{pal.accent.r, pal.accent.g, pal.accent.b};
    if (!drawn.ring || drawn.ringAccent != accent) {
      constexpr double s = menu_geometry::kRasterScale;
      const int size = static_cast<int>(2 * kRing * s);
      drawn.ring.reset(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size),
                       cairo_surface_destroy);
      drawn.ringAccent = accent;
      cairo_t* rc = cairo_create(drawn.ring.get());
      cairo_scale(rc, s, s);
      for (const auto& [width, alpha] : {std::pair{10.0, 0.15}, {5.0, 0.3}}) {
        cairo_arc(rc, kRing, kRing, kRingR + width / 2, 0, 2 * M_PI);
        setColor(rc, pal.accent, alpha);
        cairo_set_line_width(rc, width);
        cairo_stroke(rc);
      }
      cairo_arc(rc, kRing, kRing, kRingR, 0, 2 * M_PI);
      setColor(rc, pal.accent, 0.35);
      cairo_fill_preserve(rc);
      setColor(rc, pal.accent);
      cairo_set_line_width(rc, 3);
      cairo_stroke(rc);
      cairo_destroy(rc);
    }
    for (const Label& l : c.labels)
      for (const SetupControls::Point& p : l.points)
        if (c.held & heldBit(p.hand, l.control))
          paintImage(cr, drawn.ring.get(), p.x - kRing, p.y - kRing);
    for (const Label& l : c.labels)
      for (const SetupControls::Point& p : l.points) {
        cairo_arc(cr, p.x, p.y, 10, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 6 / 255.0, 8 / 255.0, 12 / 255.0, 0.9);
        cairo_fill(cr);
        cairo_arc(cr, p.x, p.y, 7, 0, 2 * M_PI);
        setColor(cr, pal.accent);
        cairo_fill(cr);
      }
  } else {
    int used[3] = {0, 0, 0};
    double x[3] = {0, 0, 0};
    for (const Label& l : c.labels) used[l.column] = 1, x[l.column] = l.x;
    const char* headings[3] = {t.left, t.either, t.right};
    for (int column = 0; column < 3; ++column)
      if (used[column])
        m.drawText(cr, headings[column], x[column], kTop + 6, kListW, kit::kGroupTitle, kit::kGroupTitleSize,
                   true, false, pal.textDim.r, pal.textDim.g, pal.textDim.b);
  }

  const double dim[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a};
  for (const Label& l : c.labels) {
    drawGlyph(m, cr, l.glyph, l.x + kSetupGlyph / 2.0, l.y + kTitleH / 2);
    const Rgba& ink = l.lit ? pal.textAccent : pal.text;
    m.drawText(cr, l.title, l.x + kTextX, l.y, l.w - kTextX, kTitleH, kTitleSize, true, false, ink.r,
               ink.g, ink.b);
    m.drawTextLines(cr, l.line, l.x + kTextX, l.y + kTitleH, l.w - kTextX, kLineSize, kLineLines, dim,
                    false);
  }
}
