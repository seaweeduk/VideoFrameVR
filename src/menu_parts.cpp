#include "menu_parts.h"

#include <cassert>
#include <cstdio>
#include <set>

// The menu's parts (menu_parts.h), laid out from the kit's tokens
// (menu_style.h, kit::) and drawn by drawButtons() and drawGroups().

namespace {

constexpr double kPageL = Menu::kPageX + kit::kPanelInset + kit::kSpace;
constexpr double kPageR = Menu::kPageX + Menu::kPageWidth - kit::kPanelInset - kit::kSpace;
constexpr double kPageT = kit::kPanelInset + kit::kSpace;

}  // namespace

// ---------------------------------------------------------------- a row

Menu::Row& Menu::Row::tip(const std::string& text) {
  for (size_t i = first_; i < end_; ++i)
    if (menu_->buttons_[i].action) menu_->buttons_[i].tooltip = text;
  return *this;
}

Menu::Row& Menu::Row::disable(bool disabled) {
  if (!disabled) return *this;
  for (size_t i = first_; i < end_; ++i) menu_->buttons_[i].disabled = true;
  return *this;
}

Menu::Row& Menu::Row::mark(int value) {
  for (size_t i = first_; i < end_; ++i)
    if (menu_->buttons_[i].slider != MenuCommand::None) menu_->buttons_[i].sliderDefault = value;
  return *this;
}

Menu::Row& Menu::Row::format(std::function<std::string(int value, bool tick)> text) {
  for (size_t i = first_; i < end_; ++i)
    if (menu_->buttons_[i].magnify) menu_->buttons_[i].sliderFormat = text;
  return *this;
}

Menu::Row& Menu::Row::inner(Icon icon, const std::string& tooltip,
                            std::function<MenuCommand()> action, bool disabled) {
  for (size_t i = first_; i < end_; ++i) {
    Button& row = menu_->buttons_[i];
    if (!row.toggle) continue;
    constexpr double size = kit::kRowH;
    row.inner = size + kit::kSpaceTight;
    Button b = menu_->iconButton(icon, std::move(action));
    b.panel = kBrowser;
    b.bare = true;
    b.w = b.h = size;
    b.x = row.x + row.w - kit::kSpace - kit::kSwitchW - kit::kSpaceTight - size;
    b.y = row.y + (row.h - size) / 2;
    b.radius = kit::kCorner;
    b.tooltip = tooltip;
    b.disabled = disabled;
    menu_->buttons_.push_back(b);  // after the row, so it is found first (buttonAt())
    break;
  }
  return *this;
}

Menu::Row& Menu::Row::endButton(const std::string& label, std::function<MenuCommand()> action,
                                bool list, bool open) {
  if (first_ == end_) return *this;
  const double right = right_, top = menu_->buttons_[first_].y;
  Button b = menu_->textButton(label, Icon::None, std::move(action));
  b.panel = kBrowser;
  b.h = kit::kRowH;
  b.fontSize = Menu::kTextSize;
  b.w = menu_->textWidth(label, Menu::kTextSize, false) + 2 * 20 + (list ? 24 + 12 : 0);
  b.x = right - b.w;
  b.y = top;
  b.radius = kit::kCorner;
  b.select = b.alignLeft = list;
  b.latched = open;
  // What the row says stops before it, its field (or list button) 8 before.
  for (size_t i = first_; i < end_; ++i) {
    Button& part = menu_->buttons_[i];
    const double gap = part.staticText ? kit::kSpace : kit::kSpaceTight;
    if ((part.staticText || part.field || part.select) && part.x + part.w > b.x - gap)
      part.w = std::max(0.0, b.x - gap - part.x);
  }
  menu_->buttons_.push_back(b);
  end_ = menu_->buttons_.size();
  right_ = b.x - kit::kSpaceTight;
  return *this;
}

Menu::Row& Menu::Row::endButton(const Action& action) {
  endButton(action.label, action.action);
  Button& b = control();
  b.tooltip = action.tooltip;
  b.disabled = action.disabled;
  b.selected = action.primary;
  return *this;
}

Menu::Row& Menu::Row::option(int option) {
  for (size_t i = first_; i < end_; ++i)
    if (menu_->buttons_[i].slider != MenuCommand::None) menu_->buttons_[i].sliderOption = option;
  return *this;
}

Menu::Row& Menu::Row::logarithmic() {
  for (size_t i = first_; i < end_; ++i)
    if (menu_->buttons_[i].slider != MenuCommand::None) menu_->buttons_[i].sliderLog = true;
  return *this;
}

Menu::Button& Menu::Row::piece(int i) {
  for (size_t j = first_; j < end_; ++j)
    if (menu_->buttons_[j].piece && i-- == 0) return menu_->buttons_[j];
  assert(false && "no such piece");
  return menu_->buttons_[end_ - 1];
}

Menu::Button& Menu::Row::control() { return menu_->buttons_[end_ - 1]; }

// ---------------------------------------------------------------- a group's rows

Menu& Menu::GroupLayout::menu() const { return page_->menu_; }
double Menu::GroupLayout::x() const { return menu().groups_[group_].x + kit::kGroupPadX; }
double Menu::GroupLayout::w() const { return menu().groups_[group_].w - 2 * kit::kGroupPadX; }

Menu::GroupLayout& Menu::GroupLayout::labelsFor(const std::vector<std::string>& labels) {
  double widest = 0;
  for (const std::string& label : labels)
    widest = std::max(widest, menu().textWidth(label, kit::kRowSize, false));
  // 16 from the row's box to its text, 16 from it to the control.
  labelW_ = std::clamp(std::ceil(widest) + 2 * kit::kSpace + 2, 150.0, 300.0);
  return *this;
}

double Menu::GroupLayout::take(double h) {
  assert(page_->last_[column_] == group_ && "rows go into the last group of a column");
  if (!labelled_.empty() && labelled_.back().second == kOpen) labelled_.back().second = menu().buttons_.size();
  Group& g = menu().groups_[group_];
  const double y = g.y + g.h;
  g.h += h + kit::kSpaceTight;
  page_->next_[column_] = g.y + g.h + kit::kSpace;
  return y;
}

double Menu::GroupLayout::labelled(const std::string& label) {
  Menu& m = menu();
  constexpr int size = kit::kRowSize;
  const double textW = m.textWidth(label, size, false);
  // 16 from the row's box to its text; widened, 16 more to the control.
  if (textW > labelW_ - kit::kSpace) {
    const double want = std::ceil(textW) + 2 * kit::kSpace + 2;
    const double split = std::min(want, std::max(labelW_, w() - kControlMinW));
    if (split > labelW_) widen(split);
  }
  if (textW <= labelW_ - kit::kSpace) {
    const double y = take(kit::kRowH);
    labelled_.emplace_back(m.buttons_.size(), kOpen);
    m.addLabel(x() + kit::kSpace, y, labelW_ - kit::kSpace, kit::kRowH, label, size);
    return y;
  }
  // Still too long: two lines, the row as tall as they need.
  double lineW = 0, linesH = 0;
  m.twoLines(label, labelW_ - 2 * kit::kSpace, size, lineW, linesH);
  const double h = std::max(kit::kRowH, std::ceil(linesH) + 2 * kit::kSpaceTight);
  const double y = take(h);
  labelled_.emplace_back(m.buttons_.size(), kOpen);
  m.addLabel(x() + kit::kSpace, y + (h - linesH) / 2, lineW, linesH, label, size, false, 2);
  return y + (h - kit::kRowH) / 2;
}

// The parts after each labelled row's label, from where its control
// started: the − moves, the field, well, list button or text field gives up
// the room from its left; what is at the row's right end stays.
void Menu::GroupLayout::widen(double split) {
  Menu& m = menu();
  const double d = split - labelW_, from = x() + labelW_;
  for (const auto& [first, last] : labelled_) {
    const size_t end = last == kOpen ? m.buttons_.size() : last;
    for (size_t i = first; i < end; ++i) {
      Button& b = m.buttons_[i];
      if (b.popup) continue;
      if (i == first) {
        if (b.staticText && b.lines == 1) b.w += d;
      } else if (b.stepper == -1) {
        b.x += d;
      } else if ((b.slider != MenuCommand::None && b.magnify) || std::abs(b.x - from) < 0.5) {
        b.x += d;
        b.w -= d;
        if (b.well) m.placePieces(i);
      }
    }
  }
  labelW_ = split;
}

Menu::Row Menu::GroupLayout::toggle(const std::string& label, bool on,
                                    std::function<MenuCommand()> action) {
  Menu& m = menu();
  const size_t first = m.buttons_.size();
  // Its label before the switch (drawButtonContent()); over two lines in a
  // taller row where one isn't enough.
  const double room = w() - kit::kSwitchW - 3 * kit::kSpace;
  double lineW = 0, linesH = 0, h = kit::kRowH;
  const bool two = m.textWidth(label, kit::kRowSize, false) > room;
  if (two) {
    m.twoLines(label, room, kit::kRowSize, lineW, linesH);
    h = std::max(kit::kRowH, std::ceil(linesH) + 2 * kit::kSpaceTight);
  }
  Button& b = m.addSwitch(x(), take(h), w(), h, label, on, std::move(action));
  if (two) {
    b.lines = 2;
    b.wrapW = lineW;
  }
  return Row(m, first, x() + w());
}

Menu::Button& Menu::addSwitch(double x, double y, double w, double h, const std::string& label,
                              bool on, std::function<MenuCommand()> action) {
  Button b;
  b.panel = kBrowser;
  b.x = x;
  b.y = y;
  b.w = w;
  b.h = h;
  b.label = label;
  b.toggle = b.bare = true;
  b.on = on;
  b.fontSize = kit::kRowSize;
  b.action = std::move(action);
  buttons_.push_back(b);
  return buttons_.back();
}

double Menu::switchWidth(const std::string& label) const {
  return (label.empty() ? 0 : textWidth(label, kit::kRowSize, false) + kit::kSpace) + kit::kSwitchW +
         2 * kit::kSpace;
}

Menu::Row Menu::GroupLayout::choice(const std::string& label,
                                    const std::vector<std::string>& options, int chosen,
                                    std::function<MenuCommand(int)> choose) {
  Menu& m = menu();
  const size_t first = m.buttons_.size();
  const double y = label.empty() ? take(kit::kRowH) : labelled(label);
  m.addChoice(x(), y, w(), label.empty() ? 0 : labelW_, kit::kRowH, {}, options, chosen,
              std::move(choose));
  return Row(m, first, x() + w());
}

void Menu::addChoice(double x, double y, double w, double labelW, double h, const std::string& label,
                     const std::vector<std::string>& options, int chosen,
                     std::function<MenuCommand(int)> choose) {
  if (labelW > 0 && !label.empty())
    addLabel(x + kit::kSpace, y, labelW - kit::kSpace, h, label, kit::kRowSize);
  constexpr double in = kit::kSpacePiece;
  Button well;
  well.panel = kBrowser;
  well.well = true;
  well.x = x + labelW;
  well.y = y;
  well.w = w - labelW;
  well.h = h;
  well.radius = kit::kCorner;
  const size_t at = buttons_.size();
  buttons_.push_back(well);
  for (size_t i = 0; i < options.size(); ++i) {
    Button b = textButton(options[i], Icon::None,
                          [choose, i] { return choose(static_cast<int>(i)); },
                          static_cast<int>(i) == chosen);
    b.panel = kBrowser;
    b.piece = true;
    b.radius = kit::kPieceCorner;
    b.y = y + in;
    b.h = h - 2 * in;
    b.fontSize = kit::kWellSize;
    buttons_.push_back(b);
  }
  placePieces(at);
}

double Menu::pieceNeed(const Button& piece) const {
  // Its label, kSpaceTight from its ends as drawn (drawButtonContent()).
  return std::ceil(textWidth(piece.label, piece.fontSize, false)) + 2 * kit::kSpaceTight + 2;
}

// Equal pieces where every label fits one; else each as wide as its label
// needs and the rest of the well shared out evenly. Where even that doesn't
// fit, equal ones (their labels shortened, said in debug builds).
void Menu::placePieces(size_t well) {
  constexpr double in = kit::kSpacePiece;
  const Button& w = buttons_[well];
  size_t end = well + 1;
  while (end < buttons_.size() && buttons_[end].piece) ++end;
  const double count = static_cast<double>(end - well - 1);
  if (count == 0) return;
  const double room = w.w - 2 * in - in * (count - 1);
  double needs = 0;
  bool equal = true;
  for (size_t i = well + 1; i < end; ++i) {
    const double need = pieceNeed(buttons_[i]);
    needs += need;
    equal = equal && need <= room / count;
  }
  if (!equal && needs > room) {
    equal = true;
#ifndef NDEBUG
    std::fprintf(stderr, "[fit] a choice's %.0f pieces need %.0f px, the well has %.0f (%s ...)\n",
                 count, needs, room, buttons_[well + 1].label.c_str());
#endif
  }
  const double each = room / count, spare = (room - needs) / count;
  double x = w.x + in;
  for (size_t i = well + 1; i < end; ++i) {
    Button& b = buttons_[i];
    b.x = equal ? w.x + in + static_cast<double>(i - well - 1) * (each + in) : x;
    b.w = equal ? each : pieceNeed(b) + spare;
    x += b.w + in;
  }
}

Menu::Row Menu::GroupLayout::value(const std::string& label, MenuCommand::Type type, int now,
                                   int lo, int hi, const std::string& text, int step) {
  return value(label, type, now, lo, hi, text, step, command(type, std::max(lo, now - step)),
               command(type, std::min(hi, now + step)));
}

Menu::Row Menu::GroupLayout::value(const std::string& label, MenuCommand::Type type, int now,
                                   int lo, int hi, const std::string& text, int step,
                                   std::function<MenuCommand()> less,
                                   std::function<MenuCommand()> more) {
  Menu& m = menu();
  const size_t first = m.buttons_.size();
  const double y = label.empty() ? take(kit::kRowH) : labelled(label);
  m.addValue(x(), y, w(), labelW_, kit::kRowH, {}, type, now, lo, hi, text, step, std::move(less),
             std::move(more));
  return Row(m, first, x() + w());
}

void Menu::addValue(double x, double y, double w, double labelW, double h, const std::string& label,
                    MenuCommand::Type type, int now, int lo, int hi, const std::string& text, int step,
                    std::function<MenuCommand()> less, std::function<MenuCommand()> more) {
  if (!label.empty()) addLabel(x + kit::kSpace, y, labelW - kit::kSpace, h, label, kit::kRowSize);
  // − and + flush at the field's ends, 6 from it.
  const double stepW = h;
  constexpr double gap = 6;
  Button minus = textButton("−", Icon::None, std::move(less));
  Button plus = textButton("+", Icon::None, std::move(more));
  minus.x = x + labelW;
  plus.x = x + w - stepW;
  minus.stepper = -1;
  plus.stepper = 1;
  for (Button* b : {&minus, &plus}) {
    b->panel = kBrowser;
    b->y = y;
    b->w = b->h = stepW;
    b->bare = b->dimInk = true;
    b->fontSize = 30;
    b->radius = kit::kCorner;
    buttons_.push_back(*b);
  }
  // The field: dragged along its range (Menu::click(), drag()).
  Button field;
  field.panel = kBrowser;
  field.x = minus.x + stepW + gap;
  field.y = y;
  field.w = plus.x - gap - field.x;
  field.h = h;
  field.radius = kit::kCorner;
  field.slider = type;
  field.sliderValue = now;
  field.sliderMin = lo;
  field.sliderMax = hi;
  field.sliderStep = step;
  field.sliderText = text;
  field.magnify = true;
  field.action = [] { return MenuCommand{}; };  // clickable; click() reads the slider
  buttons_.push_back(field);
}

Menu::Row Menu::GroupLayout::tiles(int columns, double height, const std::vector<Tile>& tiles) {
  Menu& m = menu();
  const size_t first = m.buttons_.size();
  constexpr double in = kit::kSpacePiece;
  const int rows = (static_cast<int>(tiles.size()) + columns - 1) / columns;
  const double h = rows * height + (rows - 1) * in + 2 * in;
  Button well;
  well.panel = kBrowser;
  well.well = true;
  well.x = x();
  well.y = take(h);
  well.w = w();
  well.h = h;
  well.radius = kit::kCorner;
  m.buttons_.push_back(well);
  const double each = (well.w - 2 * in - (columns - 1) * in) / columns;
  for (size_t i = 0; i < tiles.size(); ++i) {
    const Tile& t = tiles[i];
    Button b;
    b.panel = kBrowser;
    b.piece = true;
    b.radius = kit::kPieceCorner;
    b.x = well.x + in + (i % columns) * (each + in);
    b.y = well.y + in + (i / columns) * (height + in);
    b.w = each;
    b.h = height;
    b.icon = t.icon;
    b.caption = t.caption;
    b.detail = t.sub;
    b.tooltip = t.tooltip;
    b.selected = t.chosen;
    b.key = t.key;
    b.action = t.action;
    m.buttons_.push_back(b);
  }
  return Row(m, first, x() + w());
}

Menu::Row Menu::GroupLayout::select(const std::string& label, const std::string& value,
                                    std::function<MenuCommand()> action, bool open) {
  Menu& m = menu();
  const size_t first = m.buttons_.size();
  const double y = labelled(label);
  Button b = m.textButton(value, Icon::None, std::move(action));
  b.panel = kBrowser;
  b.x = x() + labelW_;
  b.y = y;
  b.w = w() - labelW_;
  b.h = kit::kRowH;
  b.radius = kit::kCorner;
  b.select = b.alignLeft = true;
  b.latched = open;
  b.fontSize = kit::kWellSize;
  m.buttons_.push_back(b);
  return Row(m, first, x() + w());
}

Menu::Row Menu::GroupLayout::note(const std::string& text, bool said, int lines) {
  Menu& m = menu();
  const size_t first = m.buttons_.size();
  const double textW = w() - 2 * kit::kSpace;
  const int count = m.wrappedLines(text, textW, kit::kNoteSize, lines);
  const double h = count * kit::kNoteLine;
  Button& b = m.addLabel(x() + kit::kSpace, take(h), textW, h, text, kit::kNoteSize, said, count);
  b.faint = !said;
  return Row(m, first, x() + w());
}

Menu::Row Menu::GroupLayout::button(const std::string& label, std::function<MenuCommand()> action) {
  Menu& m = menu();
  const size_t first = m.buttons_.size();
  Button b = m.textButton(label, Icon::None, std::move(action));
  b.panel = kBrowser;
  b.x = x() + kit::kSpace;  // its edge under the rows' labels
  b.y = take(kit::kRowH);
  b.h = kit::kRowH;
  b.w = m.textWidth(label, Menu::kTextSize, false) + 2 * 20;
  b.radius = kit::kCorner;
  b.fontSize = Menu::kTextSize;
  m.buttons_.push_back(b);
  return Row(m, first, x() + w());
}

Menu::Row Menu::GroupLayout::buttons(const std::vector<Action>& actions) {
  Menu& m = menu();
  const size_t first = m.buttons_.size();
  double y = take(kit::kRowH);
  // The first one's edge under the rows' labels; one that doesn't fit after
  // another starts a row of its own there.
  const double left = x() + kit::kSpace;
  double bx = left;
  for (const Action& a : actions) {
    Button b = m.textButton(a.label, Icon::None, a.action, a.primary);
    b.w = m.textWidth(a.label, Menu::kTextSize, false) + 2 * 20;
    if (bx > left && bx + b.w > x() + w()) {
      y = take(kit::kRowH);
      bx = left;
    }
    b.panel = kBrowser;
    b.x = bx;
    b.y = y;
    b.h = kit::kRowH;
    b.radius = kit::kCorner;
    b.fontSize = Menu::kTextSize;
    b.tooltip = a.tooltip;
    b.disabled = a.disabled;
    m.buttons_.push_back(b);
    bx += b.w + kit::kSpaceTight;
  }
  return Row(m, first, x() + w());
}

Menu::Row Menu::GroupLayout::field(const std::string& label, const std::string& text,
                                   std::function<MenuCommand()> action, bool placeholder) {
  Menu& m = menu();
  const size_t first = m.buttons_.size();
  const double y = label.empty() ? take(kit::kRowH) : labelled(label);
  const double labelW = label.empty() ? 0 : labelW_;
  Button b = m.textButton(text, Icon::None, std::move(action));
  b.panel = kBrowser;
  b.field = true;
  b.placeholder = placeholder;
  b.x = x() + labelW;
  b.y = y;
  b.w = w() - labelW;
  b.h = kit::kRowH;
  b.radius = kit::kCorner;
  b.fontSize = kit::kWellSize;
  m.buttons_.push_back(b);
  return Row(m, first, x() + w());
}

Menu::Row Menu::GroupLayout::status(Lamp lamp, const std::string& text,
                                    const std::vector<Action>& actions) {
  Menu& m = menu();
  const size_t first = m.buttons_.size();
  constexpr int size = 23;
  constexpr double lampW = 24, line = kit::kNoteLine;
  // The actions' room at the right; what it says wraps in the rest.
  double right = x() + w();
  for (const Action& a : actions)
    right -= m.textWidth(a.label, Menu::kTextSize, false) + 2 * 20 + kit::kSpaceTight;
  if (!actions.empty()) right -= kit::kSpace - kit::kSpaceTight;
  const double textX = x() + kit::kSpace, textW = right - textX;
  const double wrapW = textW - (lamp != Lamp::None ? lampW : 0);
  const int lines = m.wrappedLines(text, wrapW, size, 6);
  const double h = std::max(kit::kRowH, lines * line + 2 * 11);
  const double y = take(h);
  Button& b = lines > 1 ? m.addLabel(textX, y + 11, textW, lines * line, text, size, true, lines)
                        : m.addLabel(textX, y, textW, kit::kRowH, text, size, true);
  b.lamp = static_cast<int>(lamp);
  Row row(m, first, x() + w());
  std::vector<Action> fromRight(actions.rbegin(), actions.rend());
  for (const Action& a : fromRight) row.endButton(a);
  return row;
}

Menu::Row Menu::GroupLayout::swatches(const std::vector<std::string>& names, int chosen,
                                      std::function<MenuCommand(int)> choose) {
  Menu& m = menu();
  const size_t first = m.buttons_.size();
  constexpr double in = kit::kSpacePiece;
  Button well;
  well.panel = kBrowser;
  well.well = true;
  well.x = x();
  well.y = take(kit::kRowH);
  well.w = w();
  well.h = kit::kRowH;
  well.radius = kit::kCorner;
  m.buttons_.push_back(well);
  const double count = static_cast<double>(names.size());
  const double each = (well.w - 2 * in - in * (count - 1)) / count;
  for (size_t i = 0; i < names.size(); ++i) {
    Button b;
    b.panel = kBrowser;
    b.piece = true;
    b.swatch = static_cast<int>(i);
    b.selected = static_cast<int>(i) == chosen;
    b.radius = kit::kPieceCorner;
    b.x = well.x + in + i * (each + in);
    b.y = well.y + in;
    b.w = each;
    b.h = kit::kRowH - 2 * in;
    b.tooltip = names[i];
    b.action = [choose, i] { return choose(static_cast<int>(i)); };
    m.buttons_.push_back(b);
  }
  return Row(m, first, x() + w());
}

// ---------------------------------------------------------------- the page

Menu::PageLayout::PageLayout(Menu& menu, const std::string& title, const std::string& about)
    : menu_(menu), title_(title), about_(about) {
  menu_.pageHead_ = {};
  for (double& next : next_) next += menu.pageTop_;
}

Menu::PageLayout::~PageLayout() {
  if (!headed_) header();
#ifndef NDEBUG
  // A settings page never scrolls: its groups end above the page's bottom.
  // Said for every column of the check with every row (Menu::fitCase_), else
  // once a run for each page and column.
  static std::set<std::string> said;
  for (int c = 0; c < 3; ++c)
    if (last_[c] != ~size_t{0} && bottom(c) > kBottom + 0.5 &&
        (menu_.fitCase_ || said.insert(title_ + char('1' + c)).second))
      std::fprintf(stderr, "[fit] %s doesn't fit: column %d ends %.0f px below the page%s\n",
                   title_.c_str(), c + 1, bottom(c) - kBottom,
                   menu_.fitCase_ ? menu_.fitCase_ : "");
#endif
}

double Menu::PageLayout::columnX(int column) { return kPageL + column * (kColumnW + kit::kSpace); }

void Menu::PageLayout::sections(const std::vector<std::string>& names, int chosen,
                                std::function<void(int)> pick) {
  sections_ = names;
  sectionsOff_.assign(names.size(), {});
  section_ = chosen;
  pick_ = std::move(pick);
}

void Menu::PageLayout::disableSection(int index, const std::string& tooltip) {
  sectionsOff_.at(static_cast<size_t>(index)) = tooltip;
}

void Menu::PageLayout::pageSwitch(const std::string& label, bool on,
                                  std::function<MenuCommand()> action, const std::string& tooltip) {
  switches_.push_back({label, tooltip, on, std::move(action)});
}

// The header row: the title, the sections' well, what the page is about,
// and its switches from the right end.
void Menu::PageLayout::header() {
  headed_ = true;
  Menu& m = menu_;
  constexpr double h = kit::kPageHeaderH, gap = kit::kSpaceEdge, in = kit::kSpacePiece;
  PageHead& head = m.pageHead_;
  const double top = m.pageTop_ + kPageT;
  head.title = title_;
  head.titleX = kPageL + kit::kSpace;
  double x = head.titleX + m.textWidth(title_, kit::kTitleSize, true) + gap;
  if (!sections_.empty()) {
    std::vector<double> widths;
    double total = 2 * in + in * (sections_.size() - 1);
    for (const std::string& name : sections_) {
      widths.push_back(m.textWidth(name, kit::kRowSize, false) + 2 * 22);
      total += widths.back();
    }
    Button well;
    well.panel = kBrowser;
    well.well = true;
    well.x = x;
    well.y = top;
    well.w = total;
    well.h = h;
    well.radius = kit::kCorner;
    m.buttons_.push_back(well);
    double px = x + in;
    for (size_t i = 0; i < sections_.size(); ++i) {
      Button b = m.textButton(sections_[i], Icon::None,
                              [pick = pick_, i] {
                                pick(static_cast<int>(i));
                                return MenuCommand{};
                              },
                              static_cast<int>(i) == section_);
      b.panel = kBrowser;
      b.piece = true;
      b.radius = kit::kPieceCorner;
      b.x = px;
      b.y = top + in;
      b.w = widths[i];
      b.h = h - 2 * in;
      b.fontSize = kit::kRowSize;
      b.disabled = !sectionsOff_[i].empty();
      b.tooltip = sectionsOff_[i];
      m.buttons_.push_back(b);
      px += widths[i] + in;
    }
    x += total + gap;
  }
  double right = kPageR;
  for (auto s = switches_.rbegin(); s != switches_.rend(); ++s) {
    Button b;
    b.panel = kBrowser;
    b.w = m.textWidth(s->label, kit::kWellSize, false) + kit::kSpace + kit::kSwitchW + 2 * kit::kSpace;
    b.x = right - b.w;
    b.y = top;
    b.h = h;
    b.label = s->label;
    b.toggle = b.bare = true;
    b.on = s->on;
    b.fontSize = kit::kWellSize;
    b.tooltip = s->tooltip;
    b.action = s->action;
    m.buttons_.push_back(b);
    right = b.x - gap;
  }
  if (!about_.empty() && right - x > 40) {
    head.about = about_;
    head.aboutX = x;
    head.aboutW = right - x;
  }
}

Menu::GroupLayout Menu::PageLayout::group(int column, const std::string& title,
                                          const Action& action) {
  if (!headed_) header();
  Menu& m = menu_;
  Group g{columnX(column), next_[column], kColumnW, kit::kGroupTitle, title};
  g.titleH = kit::kGroupTitle;
  m.groups_.push_back(g);
  const size_t index = m.groups_.size() - 1;
  last_[column] = index;
  next_[column] = g.y + g.h + kit::kSpace;
  titleAction(g, action);
  return GroupLayout(*this, column, index);
}

Menu::GroupLayout Menu::PageLayout::groupAt(int column, double x, double y, double w,
                                            const std::string& title, const Action& action) {
  if (!headed_) header();
  Group g{x, y, w, kit::kGroupTitle, title};
  g.titleH = kit::kGroupTitle;
  menu_.groups_.push_back(g);
  const size_t index = menu_.groups_.size() - 1;
  last_[column] = index;
  next_[column] = g.y + g.h + kit::kSpace;
  titleAction(g, action);
  return GroupLayout(*this, column, index);
}

// A small bare button at the title row's right end.
void Menu::PageLayout::titleAction(const Group& g, const Action& action) {
  if (!action.action) return;
  Menu& m = menu_;
  constexpr int size = kit::kGroupTitleSize;
  Button b = m.textButton(action.label, Icon::None, action.action);
  b.panel = kBrowser;
  b.bare = b.dimInk = true;
  b.fontSize = size;
  b.h = 36;
  b.w = m.textWidth(action.label, size, false) + 2 * 14;
  b.x = g.x + g.w - kit::kGroupPadX - b.w;
  b.y = g.y + (kit::kGroupTitle - b.h) / 2;
  b.radius = kit::kPieceCorner;
  b.tooltip = action.tooltip;
  b.disabled = action.disabled;
  m.buttons_.push_back(b);
}

// ---------------------------------------------------------------- the strip's parts

void Menu::StripLayout::beginTabs(double x) {
  firstTab_ = menu_.buttons_.size();
  tabsX_ = x;
  tabsEnd_ = x + kit::kSpacePiece;
}

double Menu::StripLayout::tabWidth(const std::string& label) const {
  return kit::kSpace + strip::kTabIcon + strip::kTabIconGap +
         menu_.textWidth(label, strip::kTabSize, false) + kit::kSpace;
}

Menu::Button& Menu::StripLayout::tab(const std::string& label, Icon icon, bool open,
                                     std::function<MenuCommand()> action) {
  Button b = menu_.textButton(label, icon, std::move(action), open);
  b.tab = b.piece = true;
  b.x = tabsEnd_;
  b.y = strip::kTabsY + kit::kSpacePiece;
  b.w = tabWidth(label);
  b.h = strip::kTabsH - 2 * kit::kSpacePiece;
  b.radius = kit::kPieceCorner;
  b.fontSize = strip::kTabSize;
  menu_.buttons_.push_back(b);
  tabsEnd_ += b.w + kit::kSpacePiece;
  return menu_.buttons_.back();
}

double Menu::StripLayout::endTabs() {
  Button well;
  well.well = true;
  well.x = tabsX_;
  well.y = strip::kTabsY;
  well.w = tabsEnd_ - tabsX_;
  well.h = strip::kTabsH;
  well.radius = kit::kCorner;
  menu_.buttons_.insert(menu_.buttons_.begin() + static_cast<std::ptrdiff_t>(firstTab_), well);
  return tabsEnd_;
}

Menu::Button& Menu::StripLayout::quit(double right, const std::string& label,
                                      std::function<MenuCommand()> action) {
  Button b = menu_.textButton(label, Icon::Power, std::move(action));
  b.tab = b.bare = b.dangerHover = true;
  b.w = tabWidth(label);
  b.x = right - b.w;
  b.y = strip::kTabsY;
  b.h = strip::kTabsH;
  b.radius = kit::kCorner;
  b.fontSize = strip::kTabSize;
  menu_.buttons_.push_back(b);
  return menu_.buttons_.back();
}

double Menu::StripLayout::chipWidth(const std::string& text) const {
  return menu_.textWidth(text, strip::kChipSize, false) + 2 * strip::kChipPad;
}

Menu::Button& Menu::StripLayout::chip(double x, const std::string& text, bool bad,
                                      std::function<MenuCommand()> action) {
  Button b;
  b.label = text;
  b.chip = bad ? 1 : 2;
  b.action = std::move(action);
  b.x = x;
  b.y = strip::kTabsY + (strip::kTabsH - strip::kChipH) / 2;
  b.w = chipWidth(text);
  b.h = strip::kChipH;
  b.radius = strip::kChipCorner;
  b.fontSize = strip::kChipSize;
  menu_.buttons_.push_back(b);
  return menu_.buttons_.back();
}

Menu::Button& Menu::StripLayout::round(double x, Icon icon, const std::string& tooltip,
                                       std::function<MenuCommand()> action, bool on) {
  Button& b = menu_.addRound(icon, tooltip, std::move(action), x, strip::kRowY, strip::kButton);
  b.bare = true;
  b.latched = on;
  b.iconSize = strip::kButtonIcon;
  return b;
}

Menu::Button& Menu::StripLayout::play(double x, Icon icon, const std::string& tooltip,
                                      std::function<MenuCommand()> action) {
  Button& b = menu_.addRound(icon, tooltip, std::move(action), x, strip::kRowY, strip::kButton);
  b.w = kPlayW;
  b.radius = kit::kCorner;
  b.primary = true;
  b.iconSize = strip::kPlayIcon;
  return b;
}

Menu::Button& Menu::StripLayout::stars(double x, const std::string& path, float rating) {
  menu_.buttons_.push_back(
      menu_.ratingButton(path, rating, kMain, x, strip::kRowY, strip::kStarCell, strip::kButton));
  return menu_.buttons_.back();
}

Menu::Button& Menu::StripLayout::volume(double x, const std::string& tooltip) {
  Button b;
  b.x = x;
  b.y = strip::kRowY;
  b.w = strip::kVolumeW;
  b.h = strip::kButton;
  b.volumeSlider = true;
  b.tooltip = tooltip;
  b.action = [] { return MenuCommand{}; };  // handled in click()/drag()
  menu_.buttons_.push_back(b);
  return menu_.buttons_.back();
}

void Menu::StripLayout::list(const Button& opener, const std::vector<Item>& items) {
  const int count = static_cast<int>(items.size());
  if (count == 0) return;
  constexpr double pad = kit::kPopPad, gap = kit::kSpacePiece, h = strip::kListItemH;
  constexpr double iconW = strip::kTabIcon + 14, subGap = 24, dot = 24;
  const int columns = (count + strip::kListRows - 1) / strip::kListRows;
  const int rows = std::min(count, strip::kListRows);
  double w = 0;
  for (const Item& item : items)
    w = std::max(w, kit::kSpace + (item.icon != Icon::None ? iconW : 0) +
                        menu_.textWidth(item.label, strip::kListSize, false) +
                        (item.sub.empty() ? 0 : subGap + menu_.textWidth(item.sub, strip::kListSubSize, false)) +
                        (item.chosen ? dot : 0) + kit::kSpace);
  const double room = (strip::kRight - strip::kLeft - 2 * pad - (columns - 1) * gap) / columns;
  w = std::clamp(std::ceil(w), std::min(240.0, room), std::min(560.0, room));
  const double total = columns * w + (columns - 1) * gap;
  const double x0 = std::clamp(opener.x + opener.w / 2 - total / 2, strip::kLeft + pad,
                               strip::kRight - pad - total);
  const double bottom = strip::kRowY - strip::kListAbove - pad;  // the items'
  for (int i = 0; i < count; ++i) {
    const Item& item = items[i];
    Button e;
    e.popup = e.listItem = true;
    e.icon = item.icon;
    e.label = item.label;
    e.detail = item.sub;
    e.tooltip = item.tooltip;
    e.selected = item.chosen;
    e.action = item.action;
    e.radius = kit::kCorner;
    e.fontSize = strip::kListSize;
    e.x = x0 + (i / strip::kListRows) * (w + gap);
    e.y = bottom - (rows - i % strip::kListRows) * (h + gap) + gap;
    e.w = w;
    e.h = h;
    menu_.buttons_.push_back(e);
  }
}

// ---------------------------------------------------------------- fields

size_t Menu::addField(double x, double y, double w, const Field& f) {
  Button b;
  b.panel = kBrowser;
  b.label = f.text;
  b.example = f.example;
  b.field = true;
  b.on = f.typing;  // the accent ring
  b.caret = f.typing ? caret() : -1;
  b.action = f.action;
  b.tooltip = f.tooltip;
  b.x = x;
  b.y = y;
  b.w = w;
  b.h = kit::kRowH;
  b.radius = kit::kCorner;
  b.fontSize = kit::kWellSize;
  const size_t at = buttons_.size();
  if (f.inner.empty()) {
    buttons_.push_back(b);
    return at;
  }
  // Its own button, flush in the well's right end until pointed (a piece's
  // corners), its text dim.
  constexpr double in = kit::kSpacePiece;
  constexpr int size = kit::kGroupTitleSize;
  Button inner = textButton(f.inner, Icon::None, f.innerAction);
  inner.panel = kBrowser;
  inner.bare = inner.dimInk = true;
  inner.fontSize = size;
  inner.w = textWidth(f.inner, size, false) + 2 * kit::kSpace;
  inner.h = kit::kRowH - 2 * in;
  inner.x = x + w - in - inner.w;
  inner.y = y + in;
  inner.radius = kit::kPieceCorner;
  b.inner = inner.w + in;
  buttons_.push_back(b);
  buttons_.push_back(inner);  // after the field, so it is found first (buttonAt())
  return at;
}

Menu::Row Menu::GroupLayout::field(const std::string& label, const Field& f) {
  Menu& m = menu();
  const size_t first = m.buttons_.size();
  const double y = label.empty() ? take(kit::kRowH) : labelled(label);
  const double labelW = label.empty() ? 0 : labelW_;
  m.addField(x() + labelW, y, w() - labelW, f);
  return Row(m, first, x() + w());
}

Menu::GroupLayout::Box Menu::GroupLayout::room(double h) {
  const double y = take(h);
  return {x(), y, w(), h};
}

Menu::PageLayout::PageLayout(Menu& menu, UnderHeader, const std::string& name)
    : menu_(menu), title_(name) {
  headed_ = true;
  for (double& next : next_) next += menu.pageTop_;
}

// ---------------------------------------------------------------- dialogs

Menu::DialogLayout::DialogLayout(Menu& menu, double width) : menu_(menu), w_(width) {}

void Menu::DialogLayout::title(const std::string& text, Icon icon) {
  const Palette& pal = menu_.palette();
  const Rgba warn = hexColour(0xff8a85);
  DialogShown::Text t{text, kPad, y_, w_ - 2 * kPad, kTitleH, kTitleSize, true,
                      {pal.text.r, pal.text.g, pal.text.b, 1}, 1, false, icon};
  if (icon != Icon::None) {  // its ink is the icon's; the title's is white
    t.ink[0] = warn.r, t.ink[1] = warn.g, t.ink[2] = warn.b;
  }
  texts_.push_back(t);
  y_ += kTitleH + kTitleGap;
}

void Menu::DialogLayout::line(const std::string& text, Ink ink, int lines, bool bold) {
  if (text.empty()) return;
  const Palette& pal = menu_.palette();
  const Rgba k = ink == Ink::Warn ? hexColour(0xff8a85) : ink == Ink::Strong ? pal.text : pal.textDim;
  bold = bold || ink == Ink::Warn;
  const double w = w_ - 2 * kPad;
  const int count = menu_.wrappedLines(text, w, kLineSize, lines, bold);
  texts_.push_back({text, kPad, y_, w, count * kLineH, kLineSize, bold, {k.r, k.g, k.b, 1}, count,
                    false, Icon::None});
  y_ += count * kLineH + kLineGap;
}

void Menu::DialogLayout::key(const std::string& label, double labelW, const std::string& key) {
  const Palette& pal = menu_.palette();
  const double w = w_ - 2 * kPad - labelW;
  const double ink[4] = {pal.text.r, pal.text.g, pal.text.b, 1};
  const double h = std::max(kLineH, menu_.drawMono(nullptr, key, 0, 0, w, kKeySize, ink) + 6);
  texts_.push_back({label, kPad, y_, labelW, kLineH, kLineSize, false,
                    {pal.textDim.r, pal.textDim.g, pal.textDim.b, 1}, 1, false, Icon::None});
  texts_.push_back({key, kPad + labelW, y_ + 3, w, h - 3, kKeySize, false,
                    {ink[0], ink[1], ink[2], 1}, 0, true, Icon::None});
  y_ += h + kLineGap;
}

void Menu::DialogLayout::room(double h, std::function<void(double, double, double)> place) {
  rooms_.emplace_back(y_, std::move(place));
  y_ += h + kLineGap;
}

void Menu::DialogLayout::button(Button b) { buttons_.push_back(std::move(b)); }

void Menu::DialogLayout::beside(Button b) { beside_.push_back(std::move(b)); }

Menu::DialogLayout::~DialogLayout() {
  Menu& m = menu_;
  const double h = y_ - kLineGap + kButtonsGap + kButtonH + kEdge;
  // Centred on the page's panel (a page alone's is shorter); where it is
  // too short, the dialog sits as far from its bottom as from its top, the
  // panel's top rising above it (Menu::alonePanelTop()).
  const double top = m.pageTop_;
  double y = std::round(top + (kBrowserHeight - top - h) / 2);
  if (y < top + kit::kSpaceWide)
    y = std::max(std::round((kBrowserHeight - h) / 2), kBrowserHeight - kit::kSpaceWide - h);
  const double x = kPageCenterX - w_ / 2;
  DialogShown& shown = m.dialogShown_;
  shown = {x, y, w_, h, std::move(texts_)};
  for (DialogShown::Text& t : shown.texts) {
    t.x += x;
    t.y += y;
  }
  for (const auto& [top, place] : rooms_)
    if (place) place(x + kPad, y + top, w_ - 2 * kPad);
  // The buttons from the right, the last rightmost.
  const double by = y + h - kEdge - kButtonH;
  double right = x + w_ - kEdge;
  for (auto b = buttons_.rbegin(); b != buttons_.rend(); ++b) {
    b->panel = kBrowser;
    b->popup = b->raised = b->dialog = true;
    b->fontSize = kLineSize;
    b->w = m.textWidth(b->label, kLineSize, false) + 2 * kButtonPad +
           (b->icon != Icon::None ? kButtonIcon + kButtonIconGap : 0);
    b->x = right - b->w;
    b->y = by;
    b->h = kButtonH;
    b->radius = kit::kCorner;
    right = b->x - kit::kSpaceTight;
  }
  for (Button& b : beside_) {
    b.panel = kBrowser;
    b.popup = true;
    b.x += x;
    b.y = by + (kButtonH - b.h) / 2;
    m.buttons_.push_back(std::move(b));
  }
  for (Button& b : buttons_) m.buttons_.push_back(std::move(b));
}
