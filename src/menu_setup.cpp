#include "menu_setup.h"

#include "menu_style.h"
#include "platform.h"
#include "media_client.h"

#include <cassert>
#include <pango/pangocairo.h>

namespace {

using Step = SetupMenu::Step;

using setup::kW;
using setup::kH;
constexpr int kSteps = 3;                  // with dots: Controls, Add folders, Stash
// The cues' canvases, in layout pixels.
constexpr int kArrowSize = 112, kLabelW = 240, kLabelH = 36, kHintW = 1100 + 2 * kPillShadow, kHintH = kPillHeight;
constexpr int kMirrorW = 1280, kMirrorH = 720;

// The app's name, its second half in the icon's blue to green, centred in w at y.
void drawWordmark(cairo_t* cr, double x, double y, double w, int size) {
  PangoLayout* layout = pango_cairo_create_layout(cr);
  PangoFontDescription* font = pango_font_description_from_string(kFontBold);
  pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
  pango_layout_set_font_description(layout, font);
  pango_font_description_free(font);
  int firstW = 0, allW = 0, h = 0;
  pango_layout_set_text(layout, kAppTitle, 10);  // "VideoFrame"
  pango_layout_get_pixel_size(layout, &firstW, &h);
  pango_layout_set_text(layout, kAppTitle, -1);
  pango_layout_get_pixel_size(layout, &allW, &h);
  const double left = x + (w - allW) / 2;
  cairo_save(cr);
  cairo_rectangle(cr, left - 4, y, firstW + 4, h);
  cairo_clip(cr);
  cairo_set_source_rgb(cr, 0xee / 255.0, 0xf2 / 255.0, 0xf8 / 255.0);
  cairo_move_to(cr, left, y);
  pango_cairo_show_layout(cr, layout);
  cairo_restore(cr);
  cairo_save(cr);
  cairo_rectangle(cr, left + firstW, y, allW - firstW + 8, h);
  cairo_clip(cr);
  cairo_pattern_t* blend = cairo_pattern_create_linear(left + firstW, 0, left + allW, 0);
  cairo_pattern_add_color_stop_rgb(blend, 0, 0x8a / 255.0, 0xb8 / 255.0, 1.0);
  cairo_pattern_add_color_stop_rgb(blend, 1, 0x6f / 255.0, 0xf2 / 255.0, 0xc0 / 255.0);
  cairo_set_source(cr, blend);
  cairo_move_to(cr, left, y);
  pango_cairo_show_layout(cr, layout);
  cairo_pattern_destroy(blend);
  cairo_restore(cr);
  g_object_unref(layout);
}

// SteamVR's glyphs' light grey.
constexpr double kGlyphInk = 0xf1 / 255.0;
constexpr int kGlyph = kSetupGlyph;

// A controller's button as SteamVR's glyphs draw it: a thin ring, its letter
// drawn in it by the caller.
void glyphRing(cairo_t* cr, double cx, double cy) {
  cairo_arc(cr, cx, cy, kGlyph * 29.0 / 75, 0, 2 * M_PI);
  cairo_set_source_rgb(cr, kGlyphInk, kGlyphInk, kGlyphInk);
  cairo_set_line_width(cr, kGlyph * 3.0 / 75);
  cairo_stroke(cr);
}

}  // namespace

bool Menu::setupShowsControls() const { return setup_ && setup_->step == Step::Controls; }

bool Menu::startSetup(bool holdWrites, const std::vector<std::string>& places) {
  // The strip drawn once now: its first drawing takes longer than a frame
  // (17 ms on the Frame), and would come when setup ends.
  redraw();
  setup_ = std::make_unique<SetupMenu>();
  constexpr float s = menu_geometry::kRasterScale;
  SetupMenu& m = *setup_;
  m.holdWrites = holdWrites;
  m.places = places;
  // The desktop window's card is a third of these pixels, at its own size.
  // Scrolling (Choose a folder's list moves on the GPU): a spare image.
  if (!initCanvas(m.canvas, kSetupWidth, kSetupHeight + kSetupKeyboardHeight, s, true) ||
      !initCanvas(m.arrow, kArrowSize, kArrowSize, s) ||
      !initCanvas(m.label, kLabelW, kLabelH, s) || !initCanvas(m.hint, kHintW, kHintH, s) ||
      (!kStandaloneHeadset && !initCanvas(m.mirror, kMirrorW, kMirrorH)))
    return false;
  SetupPages::loadControls(*this);  // Welcome's trigger glyph
  dropdown_ = Dropdown::None;
  // Welcome, the cues and the intro's layers drawn before any frame: each
  // takes longer than one.
  dirty_ = true;
  SetupPages::redraw(*this);
  SetupPages::drawCues(*this);
  setupIntroPiece(IntroLayer::Backdrop);
  return true;
}

void Menu::endSetup(bool finished) {
  if (finished && setup_) {
    // Each pin goes first: the last tick's first, so they keep the order
    // they were shown in after Add.
    for (auto it = setup_->places.rbegin(); it != setup_->places.rend(); ++it)
      if (!navigation_.places.pinned(*it)) navigation_.places.togglePin(*it);
    pinsChanged();
    for (const std::string& root : setup_->logins) services_.rememberLogin(root);
  }
  if (setup_ && setup_->stash.client) setup_->stash.client->cancel();
  setup_.reset();
  network_.form = {};
  editing_ = false;
  dropdown_ = Dropdown::None;
  hovered_ = tooltip_ = pointerPanel_ = -1;
  dirty_ = true;
}

// Welcome's intro: the icon's parts in cells side by side (a gap between
// them, for the image's smaller levels), the name under them in a cell at
// least kNameW wide.
namespace {
constexpr int kBrandSize = 230, kBrandX = (Menu::kSetupWidth - kBrandSize) / 2, kBrandY = 64;
constexpr int kNameW = 700, kNameH = 160, kNameY = 318, kNameSize = 104, kCellGap = 16;
constexpr int kPieces = 5;  // tile, blue, green, lens, play
constexpr int kPiecesW = kPieces * (kBrandSize + kCellGap) - kCellGap;
constexpr int kPiecesH = kBrandSize + kCellGap + kNameH;
}  // namespace

bool Menu::setupIntroDue() const {
  return setup_ && setup_->intro.due && setup_->step == Step::Welcome;
}

void Menu::startSetupIntro() {
  if (setup_) setup_->intro.due = false;
}

void Menu::dropSetupIntro() {
  if (!setup_) return;
  setup_->intro.backdrop.reset();
  setup_->intro.pieces.reset();
  setup_->intro.black.reset();
}

Menu::IntroPiece Menu::setupIntroPiece(IntroLayer layer) {
  IntroPiece p;
  if (!setup_) return p;
  SetupMenu::Intro& in = setup_->intro;
  if (!in.backdrop) {
    // The panel with nothing on it (at layout resolution: the page drawn
    // over it hides it from 3.7 s), the pieces, and black.
    in.backdrop = std::make_unique<CairoCanvas>();
    in.pieces = std::make_unique<CairoCanvas>();
    in.black = std::make_unique<CairoCanvas>();
    // The name's cell: the whole word with a gap at either end, an even
    // width so that it is centred on whole pixels as the page centres it.
    const int word = static_cast<int>(std::ceil(textWidth(kAppTitle, kNameSize, true)));
    in.nameW = std::max(kNameW, (word + 2 * kCellGap + 1) / 2 * 2);
    if (!initCanvas(*in.backdrop, kSetupWidth, kSetupHeight) ||
        !initCanvas(*in.pieces, std::max(kPiecesW, in.nameW), kPiecesH, menu_geometry::kRasterScale) ||
        !initCanvas(*in.black, 4, 4)) {
      dropSetupIntro();
      return p;
    }
    MenuFinishes::panel(in.backdrop->cr(), 0, 0, kSetupWidth, kSetupHeight, kit::kPanelInset,
                        kit::kPanelCorner);
    in.backdrop->upload();
    cairo_t* cr = in.pieces->cr();
    for (int i = 0; i < kPieces; ++i)
      drawBrand(cr, i * (kBrandSize + kCellGap), 0, kBrandSize, 1 << i);
    drawWordmark(cr, 0, kBrandSize + kCellGap, in.nameW, kNameSize);
    in.pieces->upload();
    cairo_set_source_rgb(in.black->cr(), 0, 0, 0);
    cairo_paint(in.black->cr());
    in.black->upload();
  }
  if (layer == IntroLayer::Backdrop) {
    p.texture = in.backdrop->texture();
    p.w = kSetupWidth, p.h = kSetupHeight;
  } else if (layer == IntroLayer::Black) {
    p.texture = in.black->texture();
  } else {
    p.texture = in.pieces->texture();
    const bool name = layer == IntroLayer::Name;
    const int i = static_cast<int>(layer) - static_cast<int>(IntroLayer::Tile);
    const float cellX = name ? 0 : i * (kBrandSize + kCellGap), cellY = name ? kBrandSize + kCellGap : 0;
    const float piecesW = in.pieces->width();
    p.w = name ? in.nameW : kBrandSize, p.h = name ? kNameH : kBrandSize;
    p.u0 = cellX / piecesW, p.u1 = (cellX + p.w) / piecesW;
    p.v0 = cellY / kPiecesH, p.v1 = (cellY + p.h) / kPiecesH;
    p.x = name ? (kSetupWidth - in.nameW) / 2 : kBrandX;
    p.y = name ? kNameY : kBrandY;
  }
  return p;
}

void Menu::setSetupController(bool tracked) {
  if (!setup_ || setup_->controller == tracked) return;
  setup_->controller = tracked;
  if (setup_->step == Step::Welcome) dirty_ = true;
}

bool Menu::setupTyping() const { return setup_ && editingFilter(); }

bool Menu::setupKeyboardShown() const { return setup_ && setup_->rowsShown > kSetupHeight; }

bool Menu::setupScrolls() const { return setup_ && SetupPages::scrolls(*this); }

const Texture* Menu::setupCue(SetupCue cue, int& width, int& height) {
  if (!setup_->cuesDrawn) SetupPages::drawCues(*this);
  SetupMenu& s = *setup_;
  switch (cue) {
    case SetupCue::Arrow: width = height = kArrowSize; return s.arrow.texture();
    case SetupCue::Label: width = kLabelW, height = kLabelH; return s.label.texture();
    case SetupCue::Hint: width = kHintW, height = kHintH; return s.hint.texture();
    case SetupCue::Mirror: width = kMirrorW, height = kMirrorH; return s.mirror.texture();
  }
  return nullptr;
}

void SetupPages::redraw(Menu& m) {
  SetupMenu& s = *m.setup_;
  CairoCanvas& canvas = s.canvas;
  const bool all = m.dirty_;
  // Parts changed wait until a new page is shown; a whole page, once one is
  // shown, goes to the spare image (drawBand()), next frame if the image
  // shown still waits for one.
  if (!all && s.row >= 0) return;
  if (all && canvas.texture() && !canvas.beginUpdate()) return;
  m.dirty_ = false;
  std::vector<SetupMenu::Box> damage;
  damage.swap(s.damage);
  m.keyboard_.shown = false;
  layout(m);
  // The parts (menu_parts.h) put theirs on the browser: all of these are on setup's panel.
  for (Menu::Button& b : m.buttons_) b.panel = Menu::kSetup;
  if (!m.keyboard_.shown) m.keyboard_.popup = {};  // gone with the keyboard
  // No tooltips (the keyboard's): one appearing would redraw it all.
  for (Menu::Button& b : m.buttons_) b.tooltip.clear();
  m.hovered_ = m.pointerPanel_ >= 0 ? m.buttonAt(m.pointerPanel_, m.pointerX_, m.pointerY_) : -1;
  // What moved under a still pointer (a list scrolled) isn't entered
  // (Menu::feelHover()).
  m.hoverSpot_ = {};
  if (m.hovered_ >= 0) {
    const Menu::Button& b = m.buttons_[m.hovered_];
    m.hoverSpot_ = {b.panel, b.x, b.y, b.w, b.h};
  }
  m.tooltip_ = -1;
  cairo_t* cr = canvas.cr();
  if (all) {  // the rows shown: the keyboard's only while typing
    s.row = 0;
    s.rowsEnd = Menu::kSetupHeight + (m.setupTyping() ? Menu::kSetupKeyboardHeight : 0);
    if (s.folders) s.folders->drawnScroll = s.folders->scroll;
    // The first page, before any frame (Menu::startSetup()): at once.
    if (!canvas.texture()) drawBand(m, s.rowsEnd);
    return;
  }
  // At most a band's pixels a frame, as drawBand() draws; the rest next frame.
  int budget = Menu::kSetupWidth * 128;
  for (SetupMenu::Box d : damage) {
    if (budget <= 0) {
      s.damage.push_back(d);
      continue;
    }
    const int rows = std::max(8, budget / (d.x1 - d.x0));
    if (d.y1 - d.y0 > rows) {
      s.damage.push_back({d.x0, d.y0 + rows, d.x1, d.y1});
      d.y1 = d.y0 + rows;
    }
    budget -= (d.x1 - d.x0) * (d.y1 - d.y0);
    cairo_save(cr);
    cairo_rectangle(cr, d.x0, d.y0, d.x1 - d.x0, d.y1 - d.y0);
    cairo_clip(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    draw(m, cr);
    cairo_restore(cr);
    canvas.upload(d.y0, d.y1, d.x0, d.x1);
  }
}

void SetupPages::drawBand(Menu& m, int rows) {
  SetupMenu& s = *m.setup_;
  CairoCanvas& canvas = s.canvas;
  const int y0 = s.row, y1 = std::min(s.row + rows, s.rowsEnd);
  canvas.clear(y0, y1);
  cairo_t* cr = canvas.cr();
  cairo_save(cr);
  cairo_rectangle(cr, 0, y0, kW, y1 - y0);
  cairo_clip(cr);
  // Every band at the list's scroll the page started with; scrolled
  // meanwhile, the list moves once the page shows (update()).
  SetupFolders* f = s.folders.get();
  const double scroll = f ? std::exchange(f->scroll, f->drawnScroll) : 0;
  draw(m, cr);
  if (f) f->scroll = scroll;
  cairo_restore(cr);
  canvas.upload(y0, y1);
  s.row = y1;
  if (y1 < s.rowsEnd) return;
  canvas.finishUpdate();
  s.row = -1;
  s.rowsShown = s.rowsEnd;
}

void SetupPages::damaged(Menu& m, double y0, double y1, double x0, double x1) {
  const int bottom = Menu::kSetupHeight + Menu::kSetupKeyboardHeight;
  const SetupMenu::Box box{std::max(0, static_cast<int>(std::floor(x0))),
                           std::max(0, static_cast<int>(std::floor(y0))),
                           std::min(Menu::kSetupWidth, static_cast<int>(std::ceil(x1))),
                           std::min(bottom, static_cast<int>(std::ceil(y1)))};
  if (box.x1 <= box.x0 || box.y1 <= box.y0) return;
  for (const SetupMenu::Box& d : m.setup_->damage)  // within one already to draw
    if (d.x0 <= box.x0 && d.y0 <= box.y0 && d.x1 >= box.x1 && d.y1 >= box.y1) return;
  m.setup_->damage.push_back(box);
}

void SetupPages::layout(Menu& m) {
  using Button = Menu::Button;
  using Icon = Menu::Icon;
  SetupMenu& s = *m.setup_;
  const SetupStrings& t = m.text().setup;
  m.buttons_.clear();
  m.groups_.clear();
  m.pageScrollMax_ = 0;  // drawGroups() mustn't cut its groups off as a scrolled page's
  m.listColumns_ = false;  // its list rows have no columns
  m.pageHead_ = {};
  auto go = [&m](Step step) {
    return [&m, step] {
      setStep(m, step);
      return MenuCommand{};
    };
  };

  if (s.step == Step::Welcome) {
    // The language shown (the system's until chosen) as a list button at
    // the top right, its list under it.
    const std::string shown = shownLanguageCode();
    int current = 1;
    for (int i = 1; i < languageCount(); ++i)
      if (shown == languageCode(i)) current = i;
    const bool open = m.dropdown_ == Menu::Dropdown::Language;
    Button language = m.textButton(languageName(current), Icon::None, [&m] {
      m.dropdown_ = m.dropdown_ == Menu::Dropdown::Language ? Menu::Dropdown::None
                                                            : Menu::Dropdown::Language;
      return MenuCommand{};
    });
    language.part = PartKind::Select;
    language.alignLeft = true;
    language.latched = open;
    language.fontSize = kit::kWellSize;
    language.radius = kit::kCorner;
    const double languageW = m.textWidth(language.label, kit::kWellSize, false) + 2 * 20 + 24 + 12;
    const Button opener = add(m, language, setup::kRight - languageW, setup::kLeft, languageW, kit::kRowH);
    // Get started: the one way on, filled, larger than the footer's.
    constexpr double startW = 400, startH = 80;
    Button start = m.textButton(t.getStarted, Icon::None, go(Step::Controls), true);
    start.fontSize = 32;
    start.radius = kit::kCorner;
    start.disabled = !s.controller;
    add(m, start, (setup::kW - startW) / 2, 548, startW, startH);
    const double laterW = m.textWidth(t.setUpLater, kit::kWellSize, false) + 2 * kit::kSpace;
    addLater(m, (setup::kW - laterW) / 2, setup::kFooterY);
    if (open) {
      std::vector<Menu::Action> items;
      for (int i = 1; i < languageCount(); ++i)
        items.push_back({languageName(i), "", [&m, i] {
                           m.dropdown_ = Menu::Dropdown::None;
                           return command(MenuCommand::SetLanguage, i)();
                         }});
      listBelow(m, opener, items, current - 1);
    }
    return;
  }

  if (s.step == Step::Folders) {
    layoutFolders(m);
    return;
  }
  if (s.step == Step::Stash) {
    layoutStash(m);
    return;
  }
  layoutControls(m);
  addLater(m, 0, setup::kLeft);
  const Step back = static_cast<Step>(static_cast<int>(s.step) - 1);
  Menu::Action next{t.next, "", go(static_cast<Step>(static_cast<int>(s.step) + 1))};
  next.primary = true;
  addFooter(m, SetupPages::back(m, go(back)), Icon::Arrow, {next});
}

Menu::Button& SetupPages::add(Menu& m, Menu::Button b, double x, double y, double w, double h) {
  b.panel = Menu::kSetup;
  b.x = x, b.y = y, b.w = w, b.h = h;
  m.buttons_.push_back(std::move(b));
  return m.buttons_.back();
}

// Defaults, no Stash, nothing pinned. x 0: at the panel's top right.
Menu::Button& SetupPages::addLater(Menu& m, double x, double y) {
  const SetupStrings& t = m.text().setup;
  Menu::Button later = m.textButton(t.setUpLater, Menu::Icon::None, command(MenuCommand::EndSetup, 0));
  later.bare = later.dimInk = true;
  later.fontSize = kit::kWellSize;
  later.radius = kit::kCorner;
  const double w = m.textWidth(t.setUpLater, kit::kWellSize, false) + 2 * kit::kSpace;
  return add(m, later, x > 0 ? x : setup::kRight - w, y, w, setup::kFooterH);
}

double SetupPages::footerWidth(Menu& m, const std::string& label, bool icon) {
  // An icon before the label (Button's): 35 in, three quarters of kIconSize, 20 to the text.
  const double text = m.textWidth(label, kit::kRowSize, false);
  return icon ? 35 + kIconSize * 0.75 + 20 + text + 35 : std::max(160.0, text + 2 * setup::kButtonPad);
}

Menu::Action SetupPages::back(Menu& m, std::function<MenuCommand()> action) {
  return {m.text().setup.back, "", std::move(action)};
}

void SetupPages::addFooter(Menu& m, const Menu::Action& left, Menu::Icon icon,
                           const std::vector<Menu::Action>& actions, bool raised) {
  if (left.action) {
    Menu::Button b = m.textButton(left.label, icon, left.action);
    b.bare = !raised;
    b.fontSize = kit::kRowSize;
    b.radius = kit::kCorner;
    b.disabled = left.disabled;
    add(m, b, setup::kLeft, setup::kFooterY, footerWidth(m, left.label, icon != Menu::Icon::None),
        setup::kFooterH);
  }
  double right = setup::kRight;
  for (auto a = actions.rbegin(); a != actions.rend(); ++a) {
    Menu::Button b = m.textButton(a->label, Menu::Icon::None, a->action, a->primary);
    b.fontSize = kit::kRowSize;
    b.radius = kit::kCorner;
    b.disabled = a->disabled;
    b.tooltip = a->tooltip;
    const double w = footerWidth(m, a->label);
    add(m, b, right - w, setup::kFooterY, w, setup::kFooterH);
    right -= w + kit::kSpaceTight;
  }
}

void SetupPages::listBelow(Menu& m, const Menu::Button& opener, const std::vector<Menu::Action>& items,
                           int chosen) {
  constexpr double h = strip::kListItemH, gap = kit::kSpacePiece;
  double w = opener.w;
  for (const Menu::Action& a : items)
    w = std::max(w, m.textWidth(a.label, strip::kListSize, false) + 2 * kit::kSpace + 24);
  const double x = opener.x + opener.w - w;
  double y = opener.y + opener.h + kit::kSpaceTight + kit::kPopPad;
  for (size_t i = 0; i < items.size(); ++i) {
    Menu::Button e;
    e.popup = true;
    e.part = PartKind::ListItem;
    e.label = items[i].label;
    e.tooltip = items[i].tooltip;
    e.selected = static_cast<int>(i) == chosen;
    e.action = items[i].action;
    e.radius = kit::kCorner;
    e.fontSize = strip::kListSize;
    add(m, e, x, y, w, h);
    y += h + gap;
  }
}

void SetupPages::draw(Menu& m, cairo_t* cr) {
  const SetupMenu& s = *m.setup_;
  const SetupStrings& t = m.text().setup;
  const Palette pal = m.palette();
  // One colour behind a list that scrolls (Add folders' tree, what was
  // added): its rows move on the GPU (scrollTo()).
  MenuFinishes::Flat flat;
  const SetupFolders* f = s.folders.get();
  if (s.step == Step::Folders && f && (f->page == SetupFolders::Page::Added || !f->form)) flat = {f->top - 2, f->bottom};
  MenuFinishes::panel(cr, 0, 0, kW, kH, kit::kPanelInset, kit::kPanelCorner, flat);
  const Rgba& ink = pal.text;
  const Rgba& dim = pal.textDim;
  if (s.step == Step::Welcome) {
    // As the intro's pieces end up (setupIntroPiece()).
    drawBrand(cr, kBrandX, kBrandY, kBrandSize);
    drawWordmark(cr, 0, kNameY, kW, kNameSize);
    m.drawText(cr, t.welcome, 0, 456, kW, 50, 32, false, true, ink.r, ink.g, ink.b);
    // A line with its mark before it, centred: the trigger's SteamVR glyph,
    // else one drawn like it.
    auto markedLine = [&](Menu::Icon icon, bool glyph, const char* text, double y, double h, int size,
                          const Rgba& c) {
      const double mark = glyph ? kGlyph : 40, gap = glyph ? 14 : 16;
      const double textW = m.textWidth(text, size, false);
      const double x = (kW - mark - gap - textW) / 2;
      const double markInk = glyph ? kGlyphInk : 1.0;
      if (!glyph || !drawGlyph(m, cr, "button_trigger", x + mark / 2, y + h / 2))
        Menu::drawIcon(cr, icon, x + mark / 2, y + h / 2, mark, markInk, markInk, markInk);
      m.drawText(cr, text, x + mark + gap, y, textW + 4, h, size, false, false, c.r, c.g, c.b);
    };
    if (s.controller) {
      markedLine(Menu::Icon::Trigger, true, t.pointAndPull, 660, 60, 26, dim);
    } else {
      markedLine(Menu::Icon::Controller, false, t.turnOnController, 656, 50, 28, ink);
      m.drawText(cr, kStandaloneHeadset ? t.pressSteamButton : t.holdPowerButton, 0, 706, kW, 40,
                 24, false, true, dim.r, dim.g, dim.b);
    }
  } else if (s.step == Step::Folders) {
    drawFolders(m, cr);
  } else if (s.step == Step::Stash) {
    drawStash(m, cr);
  } else {
    drawControls(m, cr);
  }
  m.drawGroups(cr);
  m.drawButtons(cr, Menu::kSetup);
  m.drawPopups(cr, Menu::kSetup);
}

bool SetupPages::shows(cairo_t* cr, double x, double y, double w, double h) {
  constexpr double kReach = 16;
  double x0, y0, x1, y1;
  cairo_clip_extents(cr, &x0, &y0, &x1, &y1);
  return x - kReach < x1 && x + w + kReach > x0 && y - kReach < y1 && y + h + kReach > y0;
}

void SetupPages::drawHead(Menu& m, cairo_t* cr, const std::string& title, const std::string& line,
                          double width) {
  using namespace setup;
  if (!shows(cr, kTextLeft, kTitleY, std::max(width, kW - 2 * kTextLeft), kTop - kTitleY)) return;
  const Palette pal = m.palette();
  const double x = kTextLeft;
  // The title stops before Set up later at the top right.
  m.drawText(cr, title, x, kTitleY, kW - kTextLeft - x - 240, kTitleH, kTitleSize, true, false, pal.text.r,
             pal.text.g, pal.text.b);
  const double lineInk[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a};
  m.drawTextLines(cr, line, kTextLeft, kLineY, std::min(width, kW - 2 * kTextLeft), kLineSize, 2, lineInk,
                  false);
}

// Under the panel while it types, a panel of its own.
void SetupPages::drawKeyboardPanel(Menu& m, cairo_t* cr) {
  if (!m.keyboard_.shown) return;
  const double* k = m.keyboard_.rect;
  MenuFinishes::panel(cr, k[0], k[1], k[2], k[3], 0, kit::kGroupCorner);
}

// The dots: this step's long, those done in the accent.
void SetupPages::drawDots(Menu& m, cairo_t* cr) {
  const Palette pal = m.palette();
  const int at = static_cast<int>(m.setup_->step) - 1;
  double x = (kW - (kSteps - 1) * 30 - 34) / 2;
  for (int i = 0; i < kSteps; ++i) {
    const double w = i == at ? 34 : 16;
    roundedRect(cr, x, setup::kFooterY + (setup::kFooterH - 16) / 2, w, 16, 8);
    if (i <= at)
      setColor(cr, pal.accent, i == at ? 1.0 : 0.6);
    else
      cairo_set_source_rgba(cr, 1, 1, 1, 0.22);
    cairo_fill(cr);
    x += w + 14;
  }
}

void SetupPages::setStep(Menu& m, Step step) {
  SetupMenu& s = *m.setup_;
  if (step == s.step) return;
  if (s.step == Step::Stash) {  // a request under way is dropped
    stopAsking(m);
    m.editing_ = false;
  }
  s.stash.page = SetupStash::Page::Ask;
  if (s.step == Step::Folders) {  // its tree stays as it was for Back
    m.network_.form = {};
    m.editing_ = false;
  }
  s.step = step;
  if (step == Step::Folders) {
    if (!s.folders)
      startFolders(m);
    else  // a sign-in shows again
      showPlace(m, s.folders->shown);
  }
}

void SetupPages::drawPill(Menu& m, cairo_t* cr, double w, double h,
                          const std::vector<PillGlyph>& glyphs, const std::string& line) {
  constexpr double kStart = 20, kGap = 20, kEnd = 38, kBetween = 10;
  double glyphsW = 0;
  for (const PillGlyph& g : glyphs)
    if (g.image || !g.letter.empty() || g.trigger) glyphsW += (glyphsW > 0 ? kBetween : 0) + kGlyph;
  const double textX = glyphsW > 0 ? kStart + glyphsW + kGap : kEnd;
  // In the kit's pop surface, its shadow inside the canvas.
  const double pillW = std::min(w - 2 * kPillShadow, textX + m.textWidth(line, kPillSize, false) + kEnd);
  const double x = (w - pillW) / 2, y = (h - kPillH) / 2;
  assert(MenuFinishes::reach(Finish::Pop) <= kPillShadow && h >= kPillH + 2 * kPillShadow);
  m.drawFinish(cr, Finish::Pop, x, y, pillW, kPillH, kPillH / 2);
  const Rgba& ink = m.palette().text;
  double gx = x + kStart;
  for (const PillGlyph& g : glyphs) {
    const double cx = gx + kGlyph / 2.0, cy = y + kPillH / 2;
    if (g.image) {
      constexpr double s = menu_geometry::kRasterScale;
      cairo_save(cr);
      cairo_translate(cr, std::round((cx - kGlyph / 2.0) * s) / s, std::round((cy - kGlyph / 2.0) * s) / s);
      cairo_scale(cr, 1 / s, 1 / s);
      cairo_set_source_surface(cr, g.image, 0, 0);
      cairo_paint(cr);
      cairo_restore(cr);
    } else if (!g.letter.empty()) {
      glyphRing(cr, cx, cy);
      m.drawText(cr, g.letter, gx, y, kGlyph, kPillH, 24, true, true, kGlyphInk, kGlyphInk, kGlyphInk);
    } else if (g.trigger) {
      Menu::drawIcon(cr, Menu::Icon::Trigger, cx, cy, kGlyph, kGlyphInk, kGlyphInk, kGlyphInk);
    } else {
      continue;
    }
    gx += kGlyph + kBetween;
  }
  m.drawText(cr, line, x + textX, y, pillW - textX - kEnd + 4, kPillH, kPillSize, false, false, ink.r,
             ink.g, ink.b);
}

SetupPages::PillGlyph SetupPages::pillGlyph(const ControllerArt* art,
                                            const std::vector<ControllerBindings::Source>& sources,
                                            ControlAction action, bool upDown, const std::string& fallback) {
  PillGlyph g;
  const std::string name = sources.empty() ? fallback : controlGlyph(sources[0].input, action, upDown);
  if (art && art->glyphs.count(name)) {
    g.image = art->glyphs.at(name).get();
  } else if (name.size() == 8 && name.rfind("button_", 0) == 0) {
    g.letter = std::string(1, name[7] - 'a' + 'A');
  } else {
    g.trigger = name == "button_trigger";
  }
  return g;
}

void SetupPages::drawCues(Menu& m) {
  SetupMenu& s = *m.setup_;
  const SetupStrings& t = m.text().setup;
  const Palette pal = m.palette();
  s.cuesDrawn = true;

  // A dark disc ringed and lit in the accent, a chevron pointing right in it.
  cairo_t* cr = s.arrow.cr();
  s.arrow.clear();
  const double c = kArrowSize / 2.0;
  for (const auto& [width, alpha] : {std::pair{14.0, 0.12}, {8.0, 0.25}}) {
    cairo_arc(cr, c, c, 48 - width / 2, 0, 2 * M_PI);
    setColor(cr, pal.accent, alpha);
    cairo_set_line_width(cr, width);
    cairo_stroke(cr);
  }
  cairo_arc(cr, c, c, 48, 0, 2 * M_PI);
  cairo_set_source_rgba(cr, 6 / 255.0, 8 / 255.0, 12 / 255.0, 0.85);
  cairo_fill(cr);
  cairo_arc(cr, c, c, 47, 0, 2 * M_PI);
  setColor(cr, pal.accent, 0.8);
  cairo_set_line_width(cr, 2);
  cairo_stroke(cr);
  Menu::drawIcon(cr, Menu::Icon::Chevron, c + 3, c, 54);
  s.arrow.upload();

  cr = s.label.cr();
  s.label.clear();
  m.drawText(cr, t.cue, 0, 0, kLabelW, kLabelH, 24, false, true, pal.textDim.r, pal.textDim.g,
             pal.textDim.b);
  s.label.upload();

  // The menu's button brings it, as the hints name it (Menu::drawHint()).
  cr = s.hint.cr();
  s.hint.clear();
  const ControllerArt* art = s.controls.art.get();
  const auto sources =
      handSources(art ? art->bindings : s.controls.bindings, ControlAction::Menu, 1);
  char line[512];
  std::snprintf(line, sizeof(line), t.bringHere,
                sources.empty() ? t.buttonB : controlNames({sources[0]}, m.text().controls).c_str());
  drawPill(m, cr, kHintW, kHintH, {pillGlyph(art, sources, ControlAction::Menu, false, "button_b")},
           line);
  s.hint.upload();

  if (kStandaloneHeadset) return;
  // The desktop window: the icon, then what to do, on the window's dark.
  cr = s.mirror.cr();
  cairo_rectangle(cr, 0, 0, kMirrorW, kMirrorH);
  cairo_set_source_rgb(cr, 0x1a / 255.0, 0x1c / 255.0, 0x22 / 255.0);
  cairo_fill(cr);
  drawBrand(cr, (kMirrorW - 120) / 2.0, 170, 120);
  m.drawText(cr, t.putOnHeadset, 0, 330, kMirrorW, 70, 50, true, true, 1, 1, 1);
  m.drawText(cr, t.startsWhenWorn, 0, 410, kMirrorW, 50, 36, false, true, pal.textDim.r,
             pal.textDim.g, pal.textDim.b);
  s.mirror.upload();
}
