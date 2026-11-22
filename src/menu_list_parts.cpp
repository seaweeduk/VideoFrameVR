#include "menu_list_parts.h"

#include "menu_style.h"

#include "curved_panel.h"

#include <pango/pangocairo.h>

// The navigation column's and the content pages' parts that draw
// themselves (ListLook).

Menu::Icon Menu::placeIcon(Places::Kind kind) {
  switch (kind) {
    case Places::Kind::Home: return Icon::Home;
    case Places::Kind::Videos: return Icon::Video;
    case Places::Kind::Card: return Icon::Card;
    case Places::Kind::Usb: return Icon::Usb;
    case Places::Kind::Drive: return Icon::Storage;
    case Places::Kind::Folder: break;
  }
  return Icon::Folder;
}

namespace {

constexpr double kNavIcon = 28, kPlaceIcon = 26, kNavIconGap = 14;
// The list's fades' texture (listFade()): its width, and each edge's rows.
constexpr int kFadeW = 4, kFadeRows = 40;

double navIconSize(int listLook) {
  return listLook == static_cast<int>(ListLook::NavPlace) ? kPlaceIcon : kNavIcon;
}

}  // namespace

double Menu::navLabelWidth(const Button& b) const {
  const double textX = b.x + kit::kSpace + navIconSize(b.listLook) + kNavIconGap;
  double right = b.x + b.w - (b.inner > 0 ? b.inner : kit::kSpace);
  if (!b.badge.empty()) right -= 30 + kit::kSpace;
  return right - textX;
}

void Menu::drawListPart(cairo_t* cr, const Button& b, const double ink[4]) const {
  const Palette& pal = palette();
  switch (static_cast<ListLook>(b.listLook)) {
    case ListLook::NavSection:
    case ListLook::NavPlace: {
      // Its icon (dim, in the accent while chosen), then the label; room
      // at its end for a button of its own (`inner`) or its badge.
      const double iconSize = navIconSize(b.listLook), cy = b.y + b.h / 2;
      double icon[4];
      iconInk(b, ink, icon);
      const double* text = ink;
      const double dim[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a};
      if (b.crossed) icon[0] = dim[0], icon[1] = dim[1], icon[2] = dim[2], text = dim;
      const double ix = b.x + kit::kSpace + iconSize / 2;
      drawIcon(cr, b.icon, ix, cy, iconSize, icon[0], icon[1], icon[2]);
      if (b.crossed) {  // a server that doesn't answer: its icon struck through
        const double r = iconSize * 0.55;
        cairo_move_to(cr, ix - r, cy - r);
        cairo_line_to(cr, ix + r, cy + r);
        cairo_set_source_rgba(cr, dim[0], dim[1], dim[2], 1);
        cairo_set_line_width(cr, 3);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_stroke(cr);
      }
      const double textX = b.x + kit::kSpace + iconSize + kNavIconGap;
      drawText(cr, b.label, textX, b.y, navLabelWidth(b), b.h, b.fontSize, false, false, text[0],
               text[1], text[2]);
      return;
    }
    case ListLook::PlacePin: {
      // Faint in the accent: it shows the place is pinned; pointed, it lights.
      drawIcon(cr, b.icon, b.x + b.w / 2, b.y + b.h / 2, b.iconSize, pal.textAccent.r,
               pal.textAccent.g, pal.textAccent.b, 0.55);
      return;
    }
    case ListLook::PagerDots: {
      constexpr double dot = 8, gap = 10;
      const int n = std::max(1, b.sliderMax);
      double cx = b.x + (b.w - n * dot - (n - 1) * gap) / 2 + dot / 2;
      for (int i = 0; i < n; ++i, cx += dot + gap) {
        cairo_arc(cr, cx, b.y + b.h / 2, dot / 2, 0, 2 * M_PI);
        if (i == b.sliderValue)
          setColor(cr, pal.accent);
        else
          cairo_set_source_rgba(cr, 1, 1, 1, 0.22);
        cairo_fill(cr);
      }
      return;
    }
    case ListLook::Heading: {
      // Its label, and the sorted one's direction after it.
      constexpr int size = 22;
      constexpr double arrow = 20;
      const double x = b.x + 12, w = textWidth(b.label, size, true);
      drawText(cr, b.label, x, b.y, w + 2, b.h, size, true, false, ink[0], ink[1], ink[2]);
      if (b.icon != Icon::None)
        drawIcon(cr, b.icon, x + w + 8 + arrow / 2, b.y + b.h / 2, arrow, ink[0], ink[1], ink[2]);
      return;
    }
    case ListLook::Search: {
      // A well: its icon, the text (drawFieldText()), what it finds before
      // Clear; an accent ring inside its edge while typed into.
      if (b.on) {
        roundedRect(cr, b.x + 1, b.y + 1, b.w - 2, b.h - 2, std::max(0.0, b.radius - 1));
        setColor(cr, pal.accent);
        cairo_set_line_width(cr, 2);
        cairo_stroke(cr);
      }
      drawIcon(cr, b.icon, b.x + kit::kSpace + 13, b.y + b.h / 2, 26, pal.textDim.r, pal.textDim.g,
               pal.textDim.b);
      drawFieldText(cr, b, ink);
      if (!b.detail.empty()) {
        const double w = textWidth(b.detail, 22, false), x = b.x + b.w - kit::kSpace - b.inner + 12;
        drawText(cr, b.detail, x, b.y, w + 2, b.h, 22, false, false, pal.textDim.r,
                 pal.textDim.g, pal.textDim.b);
      }
      return;
    }
    case ListLook::Section: {  // its text where the rows' names start (listPicture())
      constexpr double h = kFileRowH - kRowGap - 2 * kRowPictureIn;
      const double w = view_.thumbnails || stashCoversShown() ? std::floor(h * 16 / 9) : h;
      const double x = b.x + kRowPictureIn + w + kRowNameGap;
      drawText(cr, b.label, x, b.y, b.x + b.w - x, b.h, kit::kGroupTitleSize, true, false,
               pal.textDim.r, pal.textDim.g, pal.textDim.b);
      return;
    }
    case ListLook::Counted: {
      // Centred together: the label, then its count dim after it.
      const double labelW = textWidth(b.label, kit::kWellSize, false);
      const double countW = b.detail.empty() ? 0 : textWidth(b.detail, kCountSize, false);
      const double gap = countW > 0 ? kit::kSpaceTight : 0;
      const double x = b.x + (b.w - labelW - gap - countW) / 2;
      drawText(cr, b.label, x, b.y, labelW + 2, b.h, kit::kWellSize, false, false, ink[0], ink[1],
               ink[2]);
      if (countW > 0)
        drawText(cr, b.detail, x + labelW + gap, b.y, countW + 2, b.h, kCountSize, false, false,
                 pal.textDim.r, pal.textDim.g, pal.textDim.b);
      return;
    }
    case ListLook::Badge: {  // white on a dark pill
      constexpr int size = 20;
      roundedRect(cr, b.x, b.y, b.w, b.h, b.h / 2);
      cairo_set_source_rgba(cr, 0, 0, 0, 0.62);
      cairo_fill(cr);
      drawText(cr, b.label, b.x, b.y, b.w, b.h, size, true, true, 1, 1, 1);
      return;
    }
    case ListLook::RowStars:  // drawRating()'s
    case ListLook::Overlay:   // its stashLook's
    case ListLook::None:
      return;
  }
}

void Menu::contentEdges() {
  listLeft_ = kContentLeft;
  listRight_ = kContentRight + kListRightPad;
}

Menu::Button& Menu::headerRound(double& right, Icon icon, const std::string& tooltip,
                                std::function<MenuCommand()> action) {
  Button& b = addRound(icon, tooltip, std::move(action), right - kBrowserRowH, kBrowserRowY, kBrowserRowH);
  b.panel = kBrowser;
  b.bare = true;
  b.iconSize = 28;
  right = b.x - kit::kSpaceTight;
  return b;
}

Menu::Button& Menu::headerButton(double& right, const std::string& label,
                                 std::function<MenuCommand()> action, bool primary) {
  Button b = textButton(label, Icon::None, std::move(action), primary);
  b.panel = kBrowser;
  b.fontSize = kit::kRowSize;
  b.w = textWidth(label, kit::kRowSize, false) + 2 * kButtonPadX;
  b.x = right - b.w;
  b.y = kBrowserRowY;
  b.h = kBrowserRowH;
  b.radius = kit::kCorner;
  buttons_.push_back(b);
  right = b.x - kit::kSpaceTight;
  return buttons_.back();
}

// Bare text, as a page's title (kit::kSpace in from the boxes' edge, 32
// from the panel's); what it says after it, the state ending kSpaceEdge
// before the actions.
void Menu::contentTitle(const std::string& title, const std::string& about, double right,
                        const std::string& status) {
  PageHead& head = pageHead_;
  head = {};
  head.title = title;
  head.titleX = kContentLeft + kit::kSpace;
  head.aboutX = head.titleX + textWidth(title, kit::kTitleSize, true) + kit::kSpaceEdge;
  head.statusRight = right + kit::kSpaceTight - kit::kSpaceEdge;
  head.status = status;
  double aboutRight = head.statusRight;
  if (!status.empty()) aboutRight -= textWidth(status, 23, false) + kit::kSpaceEdge;
  if (!about.empty() && aboutRight - head.aboutX > 40) {
    head.about = about;
    head.aboutW = aboutRight - head.aboutX;
  }
}

double Menu::toolsWell(double x, double y, const std::vector<ToolPiece>& pieces) {
  return toolsWell(x, y, pieces, kBrowserToolsH);
}

double Menu::pieceWidth(const ToolPiece& p) const {
  if (p.icon != Icon::None) return kViewPieceW;
  double w = textWidth(p.label, kit::kWellSize, false) + 2 * (p.padX < 0 ? kButtonPadX : p.padX);
  if (!p.count.empty()) w += kit::kSpaceTight + textWidth(p.count, kCountSize, false);
  return w;
}

double Menu::toolsWellWidth(const std::vector<ToolPiece>& pieces) const {
  double w = kit::kSpacePiece;
  for (const ToolPiece& p : pieces) w += pieceWidth(p) + kit::kSpacePiece;
  return w;
}

Menu::Button Menu::cardBadge(const Button& card, const std::string& label) const {
  double x, y, w, h;
  listPicture(card, x, y, w, h);
  Button b;
  b.panel = card.panel;
  b.label = label;
  b.listLook = static_cast<int>(ListLook::Badge);
  b.x = x + kit::kSpaceTight;
  b.y = y + kit::kSpaceTight;
  b.w = textWidth(label, 20, true) + 2 * 12;
  b.h = 34;
  b.scrolls = card.scrolls;
  b.side = card.side;
  return b;
}

double Menu::toolsWell(double x, double y, const std::vector<ToolPiece>& pieces, double h) {
  constexpr double in = kit::kSpacePiece;
  std::vector<double> widths;
  double w = in;
  for (const ToolPiece& p : pieces) {
    widths.push_back(pieceWidth(p));
    w += widths.back() + in;
  }
  Button well;
  well.panel = kBrowser;
  well.well = true;
  well.x = x;
  well.y = y;
  well.w = w;
  well.h = h;
  well.radius = kit::kCorner;
  buttons_.push_back(well);
  double px = x + in;
  for (size_t i = 0; i < pieces.size(); ++i) {
    const ToolPiece& p = pieces[i];
    Button b = p.icon != Icon::None ? iconButton(p.icon, p.action, p.chosen)
                                    : textButton(p.label, Icon::None, p.action, p.chosen);
    b.panel = kBrowser;
    b.piece = true;
    b.radius = kit::kPieceCorner;
    b.x = px;
    b.y = y + in;
    b.w = widths[i];
    b.h = h - 2 * in;
    b.tooltip = p.tooltip;
    b.disabled = p.disabled;
    if (p.icon != Icon::None) {
      b.iconSize = 28;
    } else {
      b.listLook = static_cast<int>(ListLook::Counted);
      b.detail = p.count;
    }
    buttons_.push_back(b);
    px += widths[i] + in;
  }
  return x + w;
}

double Menu::toolsSort(double x, double y, const std::vector<SortField>& fields, int chosen, int up,
                       const std::string& upTip, std::function<MenuCommand()> turn) {
  const Strings& t = text();
  constexpr double h = kBrowserToolsH;
  Button& label = addLabel(x, y, textWidth(t.sortBy, kit::kRowSize, false) + 4, h, t.sortBy,
                           kit::kRowSize, true);
  label.dim = true;
  x += label.w + kit::kSpace;
  const bool open = dropdown_ == Dropdown::Sort;
  Button value = textButton(fields[chosen].label, Icon::None, [this] {
    dropdown_ = dropdown_ == Dropdown::Sort ? Dropdown::None : Dropdown::Sort;
    return MenuCommand{};
  });
  value.panel = kBrowser;
  value.select = true;
  value.latched = open;
  value.x = x;
  value.y = y;
  value.h = h;
  value.fontSize = kit::kWellSize;
  value.w = textWidth(value.label, kit::kWellSize, false) + 2 * kit::kSpace + 24 + 12;
  value.radius = kit::kCorner;
  buttons_.push_back(value);
  x += value.w;
  if (up >= 0) {
    x += kit::kSpaceTight;
    Button& direction = addRound(up ? Icon::ArrowUp : Icon::ArrowDown, upTip, std::move(turn), x, y, h);
    direction.panel = kBrowser;
    direction.bare = true;
    direction.iconSize = 26;
    x += h;
  }
  if (open) {
    // The fields stacked under it, the chosen one with a dot; their box
    // (popupBox()) kPopPad around them, kit::kSpaceTight under the button.
    constexpr double kPopPad = 12;
    double w = 300 - 2 * kPopPad;
    for (const SortField& f : fields) w = std::max(w, textWidth(f.label, kit::kRowSize, false) + 72);
    for (size_t i = 0; i < fields.size(); ++i) {
      const auto choose = fields[i].choose;
      Button e = textButton(fields[i].label, Icon::None,
                            [this, choose] {
                              dropdown_ = Dropdown::None;
                              return choose();
                            },
                            static_cast<int>(i) == chosen);
      e.panel = kBrowser;
      e.popup = true;
      e.listItem = true;
      e.fontSize = kit::kRowSize;
      e.radius = kit::kCorner;
      e.x = value.x + kPopPad;
      e.y = y + h + kit::kSpaceTight + kPopPad + i * (strip::kListItemH + kit::kSpacePiece);
      e.w = w;
      e.h = strip::kListItemH;
      buttons_.push_back(e);
    }
  }
  return x + kColumnGap;
}

double Menu::toolsJumps(double y, std::function<MenuCommand()> top, std::function<MenuCommand()> end) {
  const Strings& t = text();
  constexpr double h = kBrowserToolsH;
  const double position = scrollOffset_ + scrollFraction_;
  const bool atTop = position <= 0, atEnd = position >= std::max(0, listCount_ - visibleRows_);
  double right = kContentRight - h;
  Button& last = addRound(Icon::JumpBottom, t.tipBottom, std::move(end), right, y, h);
  last.panel = kBrowser;
  last.bare = true;
  last.iconSize = 28;
  last.disabled = atEnd;
  right -= h + kit::kSpaceTight;
  Button& first = addRound(Icon::JumpTop, t.tipTop, std::move(top), right, y, h);
  first.panel = kBrowser;
  first.bare = true;
  first.iconSize = 28;
  first.disabled = atTop;
  return right - kColumnGap;
}

void Menu::toolsSearch(double x, double y, double right, const std::string& words,
                       const std::string& hint, bool typing, std::function<MenuCommand()> type,
                       std::function<MenuCommand()> clear, const std::string& count) {
  const Strings& t = text();
  constexpr double h = kBrowserToolsH;
  Button search = textButton(words, Icon::Search, std::move(type), false);
  search.panel = kBrowser;
  search.listLook = static_cast<int>(ListLook::Search);
  search.x = x;
  search.y = y;
  search.w = right - x;
  search.h = h;
  search.fontSize = kit::kWellSize;
  search.field = true;
  search.on = typing;  // an accent ring while typing
  search.caret = typing ? caret() : -1;
  search.example = hint;  // while empty, typed into or not
  search.tooltip = hint;
  search.radius = kit::kCorner;
  if (words.empty()) {
    buttons_.push_back(search);
    return;
  }
  Button wipe = textButton(t.clear, Icon::None, std::move(clear));
  wipe.panel = kBrowser;
  wipe.bare = true;
  wipe.dimInk = true;
  wipe.fontSize = 22;
  wipe.w = textWidth(t.clear, 22, false) + 2 * kit::kSpace;
  wipe.x = right - kit::kSpacePiece - wipe.w;
  wipe.y = y + kit::kSpacePiece;
  wipe.h = h - 2 * kit::kSpacePiece;
  wipe.radius = kit::kPieceCorner;
  // Its text ends before Clear, and before how many it finds.
  search.inner = search.x + search.w - kit::kSpace - (wipe.x - kit::kSpacePiece);
  if (!count.empty()) {
    search.detail = count;
    search.inner += textWidth(count, 22, false) + 12;
  }
  buttons_.push_back(search);
  buttons_.push_back(wipe);
}

void Menu::listHeading(const std::string& label, double x, double w, bool rightAligned, double y,
                       bool sorted, bool up, const std::string& tooltip,
                       std::function<MenuCommand()> sort) {
  Button b;
  b.label = label;
  b.action = std::move(sort);
  b.panel = kBrowser;
  b.listLook = static_cast<int>(ListLook::Heading);
  b.bare = true;
  b.on = sorted;
  b.dimInk = !sorted;
  b.icon = !sorted ? Icon::None : up ? Icon::ArrowUp : Icon::ArrowDown;
  b.w = textWidth(b.label, 22, true) + 24 + (sorted ? 8 + 20 : 0);
  b.x = rightAligned ? x + w + 12 - b.w : x - 12;
  b.y = y;
  b.h = kListHeaderH;
  b.radius = kit::kPieceCorner;
  b.tooltip = tooltip;
  buttons_.push_back(b);
}

double Menu::headingsApart(const std::string& left, double leftX, bool leftEnds,
                           const std::string& right, double rightX, bool rightEnds,
                           bool rightSorts) const {
  constexpr double arrow = 8 + 20;  // a sorted heading's, after its label (drawListPart())
  const double leftEnd = leftEnds ? leftX : leftX + textWidth(left, 22, true);
  const double rightStart = rightEnds ? rightX - textWidth(right, 22, true) : rightX;
  // A left-aligned heading's arrow reaches right, a right-aligned one's
  // label moves left for its arrow: the gap loses the one that does.
  const double arrowIn = std::max(leftEnds ? 0.0 : arrow, rightEnds && rightSorts ? arrow : 0.0);
  return std::max(0.0, std::ceil(kColumnGap - (rightStart - leftEnd - arrowIn)));
}

// Length and size as wide as their longest values; the date as its days,
// or what its widest heading needs with its arrow, reaching into the row's
// end. Rating's heading starts at its column, Plays', Length's and Size's
// end at theirs, the date's starts at its column (layoutListHeader()).
const ListFit& Menu::listFit() const {
  if (listFit_.strings == &text()) return listFit_.fit;
  const Strings& t = text();
  constexpr int size = 24;  // the rows' columns (drawButtonContent())
  ListFit& f = listFit_.fit;
  f = {};
  f.length = std::ceil(textWidth("0:00:00", size, false)) + 2;
  for (const char* most : {"888.8 GB", "888 MB", "888 KB"})
    f.size = std::max(f.size, std::ceil(textWidth(most, size, false)) + 2);
  f.date = std::ceil(textWidth(dayText(1767139200), size, false)) + 2;
  for (const int field : {kByDate, kByAdded, kByPlayed})
    f.date = std::max(f.date, std::ceil(textWidth(t.sortFields[field], 22, true)) + 8 + 20 - kit::kSpace);
  const ListColumns c = listColumns(0, f);
  f.apart = {headingsApart(t.sortFields[kByRating], c.rating + 2, false, t.sortFields[kByPlays],
                           c.plays + kPlaysColumnW, true),
             headingsApart(t.sortFields[kByPlays], c.plays + kPlaysColumnW, true, t.lengthColumn,
                           c.length + f.length, true, false),
             headingsApart(t.lengthColumn, c.length + f.length, true, t.sortFields[kBySize],
                           c.size + f.size, true),
             headingsApart(t.sortFields[kBySize], c.size + f.size, true, t.sortFields[kByDate],
                           c.date, false)};
  listFit_.strings = &t;
  return f;
}

void Menu::drawEmptyPage(cairo_t* cr, Icon icon, const std::string& heading,
                         const std::string& hint) const {
  // The icon, the heading (two lines at most) and the hint (three), centred
  // together; the hint no wider than a comfortable line, and over more than
  // one as narrow as keeps as many (its lines about as long).
  constexpr double kIcon = 72, kIconGap = 20, kHintGap = 8, kHintMaxW = 880;
  constexpr int kHeadingSize = 30, kHintSize = 24, kHeadingLines = 2, kHintLines = 3;
  const Palette& pal = palette();
  const double x0 = listLeft_ + kit::kSpace, x1 = listRight_ - kScrollbarRoom - kit::kSpace;
  const double cx = (x0 + x1) / 2, w = x1 - x0;
  double hintW = std::min(w, kHintMaxW);
  if (!hint.empty()) {
    const int lines = wrappedLines(hint, hintW, kHintSize, kHintLines + 1);
    if (lines > 1 && lines <= kHintLines) hintW = narrowest(hint, hintW, kHintSize, lines);
  }
  cairo_t* measure = canvases_[kMain].cr();
  int lw = 0, headingH = 0, hintH = 0;
  pango_layout_get_pixel_size(textLayout(measure, heading, kHeadingSize, true, w, kHeadingLines, true),
                              &lw, &headingH);
  if (!hint.empty())
    pango_layout_get_pixel_size(textLayout(measure, hint, kHintSize, false, hintW, kHintLines, true), &lw,
                                &hintH);
  const double h = kIcon + kIconGap + headingH + (hint.empty() ? 0 : kHintGap + hintH);
  const double top = std::round(listTop_ + std::max(0.0, (listBottom_ - listTop_ - h) / 2));
  drawIcon(cr, icon, cx, top + kIcon / 2, kIcon, pal.faint.r, pal.faint.g, pal.faint.b);
  const double ink[4] = {pal.text.r, pal.text.g, pal.text.b, pal.text.a};
  const double y = top + kIcon + kIconGap;
  drawTextLines(cr, heading, x0, y, w, kHeadingSize, kHeadingLines, ink, true, 0, true);
  if (hint.empty()) return;
  const double dim[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a};
  drawTextLines(cr, hint, cx - hintW / 2, y + headingH + kHintGap, hintW, kHintSize, kHintLines, dim, true);
}

void Menu::listPicture(const Button& b, double& x, double& y, double& w, double& h) const {
  if (b.card) {
    x = b.x + kCardIn;
    y = b.y + kCardIn;
    w = b.w - 2 * kCardIn;
    h = std::round(w * 9 / 16);
    return;
  }
  x = b.x + kRowPictureIn;
  y = b.y + kRowPictureIn;
  h = b.h - 2 * kRowPictureIn;
  w = view_.thumbnails || stashCoversShown() ? std::floor(h * 16 / 9) : h;
}

void Menu::drawPlayMark(cairo_t* cr, double cx, double cy) const {
  const Rgba& k = palette().textAccent;
  drawIcon(cr, Icon::Play, cx, cy, kPlayMarkSize * 0.8, k.r, k.g, k.b);
}

// The list's top and bottom kListFadeH, where more of it is above or below:
// its rows fade into the panel there, and the fade covers the panel just
// past them too (kListFadePast). The fade is its own texture, drawn by
// main.cpp over the browser where the list's edges are, so scrolling (which
// moves the list's pixels on the GPU, shiftList()) draws nothing more. Not
// where the browser has something over the list (a list, a dialog, a
// tooltip), which it would fade too.
bool Menu::listFade(int edge, const CurvedPanel& on, CurvedPanel& part, const Texture*& texture) {
  // While a page is drawn in bands, the texture shown is the one before it:
  // its fades as they were.
  ListFadePart& shown = listFadeShown_[edge];
  if (browserRow_ >= 0) {
    if (!shown.on || heldFade() > 0) return false;
    texture = listFade_.texture();
    if (!texture) return false;
    placeListFade(edge, on, shown, part);
    return true;
  }
  shown.on = false;
  if (page_ != Page::Files && page_ != Page::Library && page_ != Page::Offline && !networkList() &&
      !stashListPage())
    return false;
  if (!panelShown(kBrowser) || listCount_ <= visibleRows_ || smbForm_.kind != SmbForm::Kind::None ||
      dialog_ != Dialog::None || heldFade() > 0)
    return false;
  const double position = scrollOffset_ + scrollFraction_;
  if (edge == 0 ? position <= 1e-6 : position >= listCount_ - visibleRows_ - 1e-6) return false;
  // Its rows: clear kListFadeH inside the list, opaque where the list's rows
  // are cut (listTop_ - 2 and listBottom_, as shiftList() moves them) and
  // kListFadePast past it.
  auto rows = [this](int e, double& from, double& to) {
    from = e == 0 ? listTop_ - 2 - kListFadePast : listBottom_ - kListFadeH;
    to = e == 0 ? listTop_ + kListFadeH : listBottom_ + kListFadePast;
  };
  const double x0 = listLeft_ - kListShadowX, x1 = listRight_ - kListRightCut;
  double from, y0, y1;
  rows(edge, from, y1);
  y0 = from;
  // Not over the pointed control above the list (a column heading, which
  // the panel's shader lights up to its box's edge): only up to its box.
  // It is where the eye is, sharp, where the list's edge carries least.
  if (edge == 0 && hovered_ >= 0 && hovered_ < static_cast<int>(buttons_.size())) {
    const Button& b = buttons_[hovered_];
    if (b.panel == kBrowser && !b.scrolls && b.x < x1 && b.x + b.w > x0 && b.y + b.h > y0 &&
        b.y < listTop_ - 2)
      y0 = std::min(b.y + b.h, listTop_ - 2);
  }
  auto meets = [&](const double box[4]) {
    return box[0] < x1 && box[0] + box[2] > x0 && box[1] < y1 && box[1] + box[3] > y0;
  };
  double box[4];
  if (covered(kBrowser, box) && meets(box)) return false;
  if (tooltip_ >= 0 && tooltip_ < static_cast<int>(buttons_.size()) &&
      buttons_[tooltip_].panel == kBrowser && meets(tooltipBox_))
    return false;

  // The texture: kFadeRows each, the top's opaque at its top, the bottom's
  // at its bottom (and below, for its filtering), in the panel's colour
  // there (flat, scrollBand(), then its gradient past the list's bottom);
  // drawn again when the list's edges move.
  static_assert(2 + kListFadeH + kListFadePast < kFadeRows, "the list's fades fit their rows");
  if (!listFade_.cr() && !initCanvas(listFade_, kFadeW, 2 * kFadeRows, menu_geometry::kRasterScale))
    return false;
  if (listFadeAt_[0] != listTop_ || listFadeAt_[1] != listBottom_) {
    listFadeAt_[0] = listTop_;
    listFadeAt_[1] = listBottom_;
    cairo_t* cr = listFade_.cr();
    listFade_.clear();
    for (int e = 0; e < 2; ++e) {
      double start, end;
      rows(e, start, end);
      const double a = e == 0 ? listTop_ - 2 : start;
      const double b = e == 0 ? end : listBottom_;
      cairo_save(cr);
      cairo_translate(cr, 0, e * kFadeRows - start);
      cairo_rectangle(cr, 0, start, kFadeW, kFadeRows);
      cairo_clip(cr);
      MenuFinishes::fillPanel(cr, 0, start, kFadeW, start + kFadeRows, kit::kPanelInset,
                              kBrowserHeight - kit::kPanelInset, scrollBand());
      cairo_pattern_t* g = cairo_pattern_create_linear(0, a, 0, b);
      cairo_pattern_add_color_stop_rgba(g, 0, 0, 0, 0, e == 0 ? 1 : 0);
      cairo_pattern_add_color_stop_rgba(g, 1, 0, 0, 0, e == 0 ? 0 : 1);
      cairo_set_operator(cr, CAIRO_OPERATOR_DEST_IN);
      cairo_set_source(cr, g);
      cairo_paint(cr);
      cairo_pattern_destroy(g);
      cairo_restore(cr);
    }
    listFade_.upload();
  }
  texture = listFade_.texture();
  if (!texture) return false;
  shown = {true, x0, x1, y0, y1, from};
  placeListFade(edge, on, shown, part);
  return true;
}

// Its part of the panel: across from x0 to x1, its rows from y0 to y1.
void Menu::placeListFade(int edge, const CurvedPanel& on, const ListFadePart& f, CurvedPanel& part) const {
  const double x0 = f.x0, x1 = f.x1, y0 = f.y0, y1 = f.y1, from = f.from;
  const double v0 = (edge * kFadeRows + y0 - from) / (2 * kFadeRows);
  const double v1 = v0 + (y1 - y0) / (2 * kFadeRows);
  const double w = on.right - on.left, h = on.top - on.bottom;
  const double top = on.top - h * y0 / kBrowserHeight, bottom = on.top - h * y1 / kBrowserHeight;
  const double height = (top - bottom) / (v1 - v0);
  part = on;
  part.left = static_cast<float>(on.left + w * x0 / kBrowserWidth);
  part.right = static_cast<float>(on.left + w * x1 / kBrowserWidth);
  part.top = static_cast<float>(top + v0 * height);
  part.bottom = static_cast<float>(top + v0 * height - height);
  part.u0 = 0;
  part.u1 = 1;
  part.v0 = static_cast<float>(v0);
  part.v1 = static_cast<float>(v1);
}

void Menu::drawLinesIn(cairo_t* cr, const std::string& text, double x, double y, double w, int size,
                       int lines, double lineH, const double rgba[4], bool fileName) const {
  double cx0, cy0, cx1, cy1;
  cairo_clip_extents(cr, &cx0, &cy0, &cx1, &cy1);
  if (y >= cy1 || y + lines * lineH <= cy0 || x >= cx1 || x + w <= cx0) return;
  PangoLayout* layout = textLayout(cr, text, size, false, w, lines, false, fileName);
  // The font's own line height, centred in each box (as CSS's line-height).
  PangoRectangle logical;
  PangoLayoutLine* first = pango_layout_get_line_readonly(layout, 0);
  if (!first) return;
  pango_layout_line_get_pixel_extents(first, nullptr, &logical);
  const double above = -logical.y;  // its baseline below its top
  cairo_set_source_rgba(cr, rgba[0], rgba[1], rgba[2], rgba[3]);
  const int count = std::min(lines, pango_layout_get_line_count(layout));
  for (int i = 0; i < count; ++i) {
    cairo_move_to(cr, x, onPixel(y + i * lineH + (lineH - logical.height) / 2 + above));
    pango_cairo_show_layout_line(cr, pango_layout_get_line_readonly(layout, i));
  }
}
