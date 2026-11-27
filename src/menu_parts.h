#pragma once

// The menu's parts (the menu design's kit): a settings page is a header row
// and, under it, three columns of groups; a group is a title row (with an
// action at its right, such as Reset) and rows of parts under it. Each part
// lays itself out from the tokens in menu_style.h and is drawn in its
// finish; a page's code says which parts it has, in order:
//
//   PageLayout page(*this, t.videoSettings, about);
//   page.sections({t.display, t.thisVideo}, section, pick);
//   GroupLayout picture = page.group(1, t.picture, {t.reset, tip, reset});
//   picture.labelsFor({t.brightness, t.zoom});
//   picture.value(t.brightness, MenuCommand::SetBrightness, now, -100, 100);
//   picture.toggle(t.toneLook, on, command(MenuCommand::ToggleToneLook)).tip(hint);
//
// Groups stack down their column and close themselves as rows are added.

#include "menu_style.h"

#include <string>
#include <vector>

// A row's parts as laid out (a range of Menu::buttons_), to finish them: a
// tooltip on its controls, greyed out.
class Menu::Row {
 public:
  // Its parts are buttons_[first..] (to the end), its box ends at `right`.
  Row(Menu& menu, size_t first, double right)
      : menu_(&menu), first_(first), end_(menu.buttons_.size()), right_(right) {}
  // The tooltip of each of its controls (what can be pointed at).
  Row& tip(const std::string& text);
  // Greyed out and not clickable (its tooltip still shows).
  Row& disable(bool disabled = true);
  // A value row's default, as a tick on its field's bottom edge.
  Row& mark(int value);
  // A value row's value as text, for the magnified scale (Button::sliderFormat).
  Row& format(std::function<std::string(int value, bool tick)> text);
  // A switch row's own small button, just left of its switch (Read again).
  Row& inner(Icon icon, const std::string& tooltip, std::function<MenuCommand()> action,
             bool disabled = false);
  // A raised button at the row's right end, what the row says or its field
  // cut off before it; `list`: it opens a list (a caret at its end), `open`:
  // the list is open. Another goes left of it, 8 apart.
  Row& endButton(const std::string& label, std::function<MenuCommand()> action, bool list = false,
                 bool open = false);
  Row& endButton(const Action& action);
  // A value row's command's option (MenuCommand::option): one command for
  // several values (stereo focus's).
  Row& option(int option);
  // A value row whose range is laid out logarithmically along its field.
  Row& logarithmic();
  // A choice's (or tiles') ith piece, for what only it has.
  Button& piece(int i);
  // The row's control (its last part).
  Button& control();

 private:
  Menu* menu_;
  size_t first_, end_;
  double right_;
};

// A picture tile in a tiles row: its icon, caption and second line.
struct Menu::Tile {
  Icon icon = Icon::None;
  std::string caption, sub, tooltip;
  bool chosen = false;
  std::function<MenuCommand()> action;
  bool key = false;  // a click redraws only what it changes (Button::key)
};

// An action's button: a group's at the right of its title row (Reset), a
// row's at its end (Sync now), a row of buttons. `primary`: filled in the
// accent, the one to take.
struct Menu::Action {
  std::string label, tooltip;
  std::function<MenuCommand()> action;
  bool disabled = false;
  bool primary = false;
};

// A text field (the kit's): a sunk well with its text, or a faint example
// while it is empty; typed into, an accent ring and the caret. Its own
// button (Show) sits inside its right end.
struct Menu::Field {
  std::string text;     // what it shows (a password: its dots)
  std::string example;  // faint, while it is empty
  bool typing = false;  // typed into: the ring, the caret (Menu::caret())
  std::function<MenuCommand()> action;  // pressed: typing into it
  std::string inner;                    // its own button's label, if it has one
  std::function<MenuCommand()> innerAction;
  std::string tooltip;
};

// A connection's state on a status row: its lamp.
enum class Lamp { None, Ok, Busy, Failed };

// A value row's text (Row::format()): the number and its unit; on
// Magnify's scale, the number.
inline std::function<std::string(int, bool)> withUnit(const char* unit) {
  return [unit = std::string(unit)](int v, bool tick) {
    return tick ? std::to_string(v) : std::to_string(v) + unit;
  };
}

class Menu::GroupLayout {
 public:
  // The rows' labels (value, choice, list button, field) as wide as the
  // widest of these needs, so their controls line up; else 200. A label
  // that needs more widens them all as it is added (its control keeps at
  // least kControlMinW), or where that isn't enough goes over two lines.
  GroupLayout& labelsFor(const std::vector<std::string>& labels);
  // A switch: the whole row is the target, the switch at its right.
  Row toggle(const std::string& label, bool on, std::function<MenuCommand()> action);
  // Two to five exclusive values: the label, then a well with the chosen
  // one raised in it. No label: the well is the row.
  Row choice(const std::string& label, const std::vector<std::string>& options, int chosen,
             std::function<MenuCommand(int)> choose);
  // A number: the label, − and +, and a sunk field filled to it with `text`
  // (the number if empty) at its right end, from the middle when it goes
  // both ways. − and + step it by `step`; the field is dragged along its
  // range (Menu::click(), drag()).
  Row value(const std::string& label, MenuCommand::Type type, int now, int lo, int hi,
            const std::string& text = "", int step = 1);
  // ... with − and + of its own (a value whose steps aren't even).
  Row value(const std::string& label, MenuCommand::Type type, int now, int lo, int hi,
            const std::string& text, int step, std::function<MenuCommand()> less,
            std::function<MenuCommand()> more);
  // Exclusive choices with a picture or a second line, `columns` across,
  // each `height` high, in a well; the chosen one raised, its icon in the accent.
  Row tiles(int columns, double height, const std::vector<Tile>& tiles);
  // A choice from a longer list: the label, then a raised button with the
  // value and a caret that opens it (`open`: it is open).
  Row select(const std::string& label, const std::string& value,
             std::function<MenuCommand()> action, bool open = false);
  // A note under a row, as many lines as it takes (at most `lines`); `said`:
  // a result or a state rather than advice, dim instead of faint.
  Row note(const std::string& text, bool said = false, int lines = 4);
  // A raised button at the row's left (Save as …).
  Row button(const std::string& label, std::function<MenuCommand()> action);
  // Raised buttons from the row's left, 8 apart (What's new, Save diagnostics).
  Row buttons(const std::vector<Action>& actions);
  // Text entry: the label (none: the field is the row), then a sunk well
  // with the text, or a faint example (`placeholder`); pressing it types
  // into it (`action`). Its own button at its end: Row::endButton().
  Row field(const std::string& label, const std::string& text, std::function<MenuCommand()> action,
            bool placeholder = false);
  Row field(const std::string& label, const Field& field);
  // Room for what the page draws itself (a key in its box): its box, the
  // row's width, `h` high.
  struct Box {
    double x, y, w, h;
  };
  Box room(double h);
  // A row's width, across the group's inside.
  double rowWidth() const { return w(); }
  // A state: its lamp (or none) and what it says, over more lines where it
  // needs them (up to six: what to change in Stash); its actions at the row's right end (Sync now), the last
  // rightmost.
  Row status(Lamp lamp, const std::string& text, const std::vector<Action>& actions = {});
  // A colour to choose: the accent's colours (or `colours`, 0xRRGGBB, for
  // the presets; Custom shows its hues) as dots in a well, the chosen one raised. A
  // label before the well, as a choice's; none: the well is the row.
  Row swatches(const std::string& label, const std::vector<std::string>& names, int chosen,
               std::function<MenuCommand(int)> choose, const std::vector<int>& colours = {});

 private:
  friend class Menu::PageLayout;
  GroupLayout(PageLayout& page, int column, size_t group)
      : page_(&page), column_(column), group_(group), labelW_(kLabelW) {}
  static constexpr double kLabelW = 200;  // a row's label, before its control
  // The narrowest a labelled row's control gets: a value's − and + and a
  // field 140 wide (about what labelsFor()'s widest, 300, leaves it).
  static constexpr double kControlMinW = 2 * kit::kRowH + 2 * 6 + 140;
  // The next row's top, its box then `h` high: the group grows under it.
  double take(double h);
  // A labelled row: its label added (the split widened for it, or over two
  // lines in a taller row) and the row taken; where its control goes.
  double labelled(const std::string& label);
  // The labelled rows so far laid out again for a wider split.
  void widen(double split);
  Menu& menu() const;
  double x() const;
  double w() const;
  PageLayout* page_;
  int column_;
  size_t group_;
  double labelW_;
  // The labelled rows' parts (Menu::buttons_), from their label; the last
  // one's end is open (kOpen) until the next row is taken.
  static constexpr size_t kOpen = static_cast<size_t>(-1);
  std::vector<std::pair<size_t, size_t>> labelled_;
};

class Menu::PageLayout {
 public:
  // A page alone on the browser's page: its title and what it is about
  // (dim, after the sections; may be empty).
  PageLayout(Menu& menu, const std::string& title, const std::string& about = "");
  // Groups under a header the page lays out itself (a content page's,
  // Menu::contentTitle()); `name` only says which page in the fit check.
  struct UnderHeader {};
  PageLayout(Menu& menu, UnderHeader, const std::string& name);
  ~PageLayout();
  PageLayout(const PageLayout&) = delete;
  PageLayout& operator=(const PageLayout&) = delete;

  // The sections in the header's well, `chosen` raised; picking one calls `pick`.
  void sections(const std::vector<std::string>& names, int chosen, std::function<void(int)> pick);
  // ... one of them greyed out, its tooltip saying why.
  void disableSection(int index, const std::string& tooltip);
  // A switch for the whole page at the header's right end (Advanced).
  void pageSwitch(const std::string& label, bool on, std::function<MenuCommand()> action,
                  const std::string& tooltip);
  // A new group at the bottom of a column (0..2), 16 under the one before.
  GroupLayout group(int column, const std::string& title, const Action& action = {});
  // A group at x, y, w on a panel whose code places its parts itself
  // (setup's), the last of column `column` (0..2) for the rows added to it.
  GroupLayout groupAt(int column, double x, double y, double w, const std::string& title,
                      const Action& action = {});
  // Every column's bottom so far, for what is laid out under them.
  double bottom(int column) const { return next_[column] - kit::kSpace; }

  // Where the columns are, and how low their groups may reach (the page
  // never scrolls: kit guide, "Pages").
  static double columnX(int column);
  static constexpr double kTop = kit::kPanelInset + kit::kSpace + kit::kPageHeaderH + kit::kSpace;
  static constexpr double kBottom = Menu::kBrowserHeight - kit::kPanelInset - kit::kSpace;
  static constexpr double kColumnW =
      (Menu::kPageWidth - 2 * (kit::kPanelInset + kit::kSpace) - 2 * kit::kSpace) / 3;
  // A note's lines, as wide as they wrap (GroupLayout::note()).
  static constexpr double kNoteW = kColumnW - 2 * kit::kGroupPadX - 2 * kit::kSpace;

 private:
  friend class Menu::GroupLayout;
  void header();
  void titleAction(const Group& g, const Action& action);
  Menu& menu_;
  std::string title_, about_;
  std::vector<std::string> sections_;
  std::vector<std::string> sectionsOff_;  // a greyed-out section's tooltip, else empty
  int section_ = -1;
  std::function<void(int)> pick_;
  struct Switch {
    std::string label, tooltip;
    bool on = false;
    std::function<MenuCommand()> action;
  };
  std::vector<Switch> switches_;
  bool headed_ = false;
  double next_[3] = {kTop, kTop, kTop};  // where each column's next group goes
  size_t last_[3] = {~size_t{0}, ~size_t{0}, ~size_t{0}};  // its last group (Menu::groups_)
};

// The playback strip's parts (on Menu::kMain, rows and sizes in menu_style.h,
// namespace strip). Each is placed at x on its row; the strip's code says
// which it has, left to right:
//
//   StripLayout strip(*this);
//   strip.beginTabs(x);
//   strip.tab(t.videoSettings, Icon::FormatDome, open, show);
//   x = strip.endTabs() + kit::kSpace;
//   strip.round(x, Icon::Recenter, t.recenter, command(MenuCommand::Recenter));
//   strip.list(opener, items);
class Menu::StripLayout {
 public:
  explicit StripLayout(Menu& menu) : menu_(menu) {}

  // The tabs' well from x on the top row: tab() adds each, its icon and
  // label, the open one raised; endTabs() closes the well and returns its
  // right end.
  void beginTabs(double x);
  Button& tab(const std::string& label, Icon icon, bool open, std::function<MenuCommand()> action);
  double endTabs();
  // A tab's width.
  double tabWidth(const std::string& label) const;
  // Quit: bare, its icon and label, red when pointed, ending at `right`.
  Button& quit(double right, const std::string& label, std::function<MenuCommand()> action);
  // A label on the top row from x: red (`bad`, only its tooltip) or in the
  // accent (a note: the way to what it says).
  Button& chip(double x, const std::string& text, bool bad, std::function<MenuCommand()> action);
  double chipWidth(const std::string& text) const;
  // The controls' row. A round button, bare until pointed; `on`: latched
  // (a mode that is on, its list open).
  Button& round(double x, Icon icon, const std::string& tooltip,
                std::function<MenuCommand()> action, bool on = false);
  // Play: wide, filled in the accent.
  Button& play(double x, Icon icon, const std::string& tooltip, std::function<MenuCommand()> action);
  static constexpr double kPlayW = kit::kPlayW;
  // The playing video's rating: five stars.
  Button& stars(double x, const std::string& path, float rating);
  static constexpr double kStarsW = 5 * strip::kStarCell;
  // The volume's slider from x, its number after it (Menu::drawVolumeSlider()).
  Button& volume(double x, const std::string& tooltip);
  // An item of a list (list()): its icon (optional), label, a second text
  // at its right end (dim), chosen.
  struct Item {
    Icon icon = Icon::None;
    std::string label, sub, tooltip;
    bool chosen = false;
    std::function<MenuCommand()> action;
  };
  // A list open above its opener (a button of the controls' row), in the
  // pop finish, the chosen item with a dot; in columns if it is long.
  void list(const Button& opener, const std::vector<Item>& items);

 private:
  Menu& menu_;
  size_t firstTab_ = 0;
  double tabsX_ = 0, tabsEnd_ = 0;
};

// A dialog (the kit's): the menu dims behind a raised box centred on the
// page, as tall as what it says. Its title and lines kPad from its edges,
// then its buttons in its bottom-right corner, kEdge from its edges, 8
// apart; the one that confirms is last, filled (Button::selected: in the
// accent; ::danger: red, it destroys), the others raised. A dialog's code
// says what it has, top to bottom:
//
//   DialogLayout dialog(*this, DialogLayout::kNarrow);
//   dialog.title(t.quitTitle);
//   dialog.button(textButton(t.cancel, Icon::None, close));
//   dialog.button(quit);  // danger
//
// It is placed as it goes out of scope (Menu::dialogShown_, drawDialog()).
class Menu::DialogLayout {
 public:
  static constexpr double kWide = 1000, kNarrow = 760;  // a short question's
  static constexpr double kPad = kit::kDialogPad, kEdge = kit::kDialogEdge;
  static constexpr double kTitleH = 48, kTitleGap = 12, kLineH = 36, kLineGap = kit::kDialogLineGap;
  static constexpr double kButtonsGap = kit::kDialogButtonsGap, kButtonH = kit::kDialogButtonH;
  static constexpr double kButtonPad = 24, kButtonIcon = 26, kButtonIconGap = 12;
  static constexpr int kTitleSize = 34, kLineSize = 25, kKeySize = 24;
  // What a line is: dim (what it says), strong (the name of what it is
  // about), a warning (red, bold).
  enum class Ink { Dim, Strong, Warn };

  explicit DialogLayout(Menu& menu, double width = kWide);
  ~DialogLayout();
  DialogLayout(const DialogLayout&) = delete;
  DialogLayout& operator=(const DialogLayout&) = delete;

  // Its title; `icon` before it, red (a warning).
  void title(const std::string& text, Icon icon = Icon::None);
  // A line, over as many as it takes up to `lines`.
  void line(const std::string& text, Ink ink = Ink::Dim, int lines = 3, bool bold = false);
  // A key (or a fingerprint) in the monospace font, its label before it.
  void key(const std::string& label, double labelW, const std::string& key);
  // Room `h` high across its inside for parts of its own; `place` puts them
  // there (x, y, w) once the box is placed.
  void room(double h, std::function<void(double x, double y, double w)> place = nullptr);
  // A button, after those before it (the last confirms).
  void button(Button b);
  // A part at the left end of the buttons' row (What's new's switch), its x
  // from the box's left edge, centred on the row.
  void beside(Button b);
  // A button for beside(), `x` from the box's left edge, raised as the
  // dialog's own are.
  Button besideButton(const char* label, std::function<MenuCommand()> action,
                      double x = kEdge) const;

 private:
  Menu& menu_;
  double w_, y_ = kPad;
  std::vector<DialogShown::Text> texts_;
  std::vector<std::pair<double, std::function<void(double, double, double)>>> rooms_;
  std::vector<Button> buttons_;
  std::vector<Button> beside_;
};
