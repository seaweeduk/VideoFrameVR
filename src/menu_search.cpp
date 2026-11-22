#include "menu_style.h"

#include "menu_setup.h"
#include "menu_stash.h"

#include <glib.h>

#include <algorithm>
#include <cstdio>
#include <iterator>

// Typing (the on-screen keyboard) into the folder's, the library's and
// Stash's search fields.

namespace {

// The letters' layouts, one for each language the menu speaks that needs
// its own (others type on English). The rows hold as many keys as
// English's, so what addresses and paths need gives way to the letters;
// it is all on the symbols too.
struct KeyLayout {
  const char* code;  // as settings.conf keeps it
  const char* key;   // the layout key's label
  const char* name;  // native, in the list of layouts
  std::vector<std::string> rows[3];
};

const KeyLayout kLayouts[] = {
    {"en", "EN", "English",
     {{"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "/", ":"},
      {"a", "s", "d", "f", "g", "h", "j", "k", "l", "@", "_"},
      {"z", "x", "c", "v", "b", "n", "m", ",", ".", "'"}}},
    // QWERTZ; ß is held on s, as on phones.
    {"de", "DE", "Deutsch",
     {{"q", "w", "e", "r", "t", "z", "u", "i", "o", "p", "ü", "/"},
      {"a", "s", "d", "f", "g", "h", "j", "k", "l", "ö", "ä"},
      {"y", "x", "c", "v", "b", "n", "m", "@", ".", ":"}}},
    // AZERTY; ç and ù are held on c and u.
    {"fr", "FR", "Français",
     {{"a", "z", "e", "r", "t", "y", "u", "i", "o", "p", "/", ":"},
      {"q", "s", "d", "f", "g", "h", "j", "k", "l", "m", "@"},
      {"w", "x", "c", "v", "b", "n", "é", "è", "à", "."}}},
    {"es", "ES", "Español",
     {{"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "/", ":"},
      {"a", "s", "d", "f", "g", "h", "j", "k", "l", "ñ", "@"},
      {"z", "x", "c", "v", "b", "n", "m", ",", ".", "'"}}},
    {"pt", "PT", "Português",
     {{"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "/", ":"},
      {"a", "s", "d", "f", "g", "h", "j", "k", "l", "ç", "@"},
      {"z", "x", "c", "v", "b", "n", "m", ",", ".", "'"}}},
};

int layoutIndex(const std::string& code) {
  for (size_t i = 0; i < std::size(kLayouts); ++i)
    if (code == kLayouts[i].code) return static_cast<int>(i);
  return -1;
}

// The layout of the language shown; English for those typed on it.
int languageLayout() {
  const std::string language = shownLanguageCode();
  const int i = layoutIndex(language.substr(0, 2));
  return i < 0 ? 0 : i;
}

// A letter held shows these (precomposed, as typed on other keyboards: SMB
// hashes a password's exact characters).
const std::vector<std::string>& accentsOf(const std::string& letter) {
  static const std::vector<std::pair<std::string, std::vector<std::string>>> kAccents = {
      {"a", {"á", "à", "â", "ä", "ã", "å", "æ"}},
      {"c", {"ç", "ć", "č"}},
      {"e", {"é", "è", "ê", "ë", "ę", "ė", "ē"}},
      {"i", {"í", "ì", "î", "ï", "ī"}},
      {"l", {"ł"}},
      {"n", {"ñ", "ń"}},
      {"o", {"ó", "ò", "ô", "ö", "õ", "ø", "œ"}},
      {"s", {"ß", "ś", "š"}},
      {"u", {"ú", "ù", "û", "ü", "ū"}},
      {"y", {"ý", "ÿ"}},
      {"z", {"ž", "ź", "ż"}},
  };
  static const std::vector<std::string> kNone;
  for (const auto& [base, accents] : kAccents)
    if (base == letter) return accents;
  return kNone;
}

std::string lowerKey(const std::string& key) {
  gchar* down = g_utf8_strdown(key.c_str(), -1);
  std::string lower = down ? down : key;
  g_free(down);
  return lower;
}

// One lower-case letter as a capital; anything else as it is.
std::string upperKey(const std::string& key) {
  if (g_utf8_strlen(key.c_str(), -1) != 1) return key;
  const gunichar c = g_utf8_get_char(key.c_str());
  if (!g_unichar_islower(c)) return key;
  char out[6];
  return std::string(out, g_unichar_to_utf8(g_unichar_toupper(c), out));
}

constexpr double kHoldSeconds = 0.4;  // a key held this long shows its popup

}  // namespace

void Menu::typeText(const std::string& utf8) {
  if (!textInputActive() || utf8.empty()) return;
  std::string text = filterText();
  const size_t at = cursorAt();
  text.insert(at, utf8);
  setFilter(text);
  setCursor(at + utf8.size());
}

void Menu::backspace() {
  if (!textInputActive()) return;
  std::string text = filterText();
  const size_t end = cursorAt();
  if (end == 0) return;
  // Remove one whole UTF-8 character.
  size_t start = end - 1;
  while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) --start;
  text.erase(start, end - start);
  setFilter(text);
  setCursor(start);
}

size_t Menu::cursorAt() const {
  const std::string& text = filterText();
  return &text == cursor_.text && text == cursor_.was ? std::min(cursor_.at, text.size()) : text.size();
}

void Menu::setCursor(size_t at) {
  const std::string& text = filterText();
  at = std::min(at, text.size());
  while (at > 0 && at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) --at;
  cursor_ = {&text, text, at};
}

int Menu::caret() const {
  return static_cast<int>(g_utf8_strlen(filterText().c_str(), static_cast<gssize>(cursorAt())));
}

// Only the field moves its caret (a pinned list's name is the heading).
void Menu::moveCursor(int chars) {
  if (!textInputActive()) return;
  const char* text = filterText().c_str();
  const char* p = text + cursorAt();
  for (; chars < 0 && p > text; ++chars) p = g_utf8_prev_char(p);
  for (; chars > 0 && *p; --chars) p = g_utf8_next_char(p);
  setCursor(static_cast<size_t>(p - text));
  if (std::any_of(buttons_.begin(), buttons_.end(), [](const Button& b) { return b.caret >= 0; }))
    typedChanged();
  else
    dirty_ = true;
}

const std::string& Menu::filterText() const {
  switch (textTarget()) {
    case TextTarget::Setup:
      return *setupText();
    case TextTarget::SmbForm:
      return network_.form.text[network_.form.field];
    case TextTarget::PlaceName:
      return navigation_.placeRename.text;
    case TextTarget::PresetName:
      return settingsPages_.presetName.text;
    case TextTarget::ScriptAddress:
      return settingsPages_.scriptAddress.text;
    case TextTarget::Stash:
      return stashText();
    case TextTarget::Search:
      break;
  }
  return page_ == Page::Library ? libraryPage_.filter : files_.filter;
}

void Menu::setFilter(const std::string& text) {
  switch (textTarget()) {
    case TextTarget::Setup:
      SetupPages::setText(*this, text);
      return;
    case TextTarget::SmbForm:
      typedIntoSmbForm(text);
      return;
    case TextTarget::PlaceName:
      navigation_.placeRename.text = text;
      typedChanged();
      return;
    case TextTarget::ScriptAddress:
      settingsPages_.scriptAddress.text = text.substr(0, 200);
      typedChanged();
      return;
    case TextTarget::PresetName: {
      // Whether Save turns into Replace, or is refused, shows with each key.
      // At most 40 characters.
      if (!g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) return;
      const glong chars = std::min<glong>(40, g_utf8_strlen(text.c_str(), -1));
      const PresetNameCheck before = checkPresetName();
      settingsPages_.presetName.text = text.substr(0, g_utf8_offset_to_pointer(text.c_str(), chars) - text.c_str());
      if (checkPresetName() == before)
        typedChanged();
      else
        browserChanged(0, kKeyboardY - 12);
      return;
    }
    case TextTarget::Stash:
      StashPages::setText(*this, text);
      return;
    case TextTarget::Search:
      break;
  }
  // The folder's (or the library's) search words. Its results start over at
  // the very top.
  std::string& filter = page_ == Page::Library ? libraryPage_.filter : files_.filter;
  if (text == filter) return;
  filter = text;
  libraryPage_.listStale = true;
  list_.offset = 0;
  list_.fraction = 0;
  dirty_ = true;
}

// Enter (or Done). Setup's panel decides for all its fields.
MenuCommand Menu::submitText() {
  if (setup_) return SetupPages::submit(*this);
  switch (textTarget()) {
    case TextTarget::SmbForm:
      return submitSmbForm();
    case TextTarget::PlaceName:
      if (navigation_.placeRename.path.empty()) break;
      savePlaceName();
      return {};
    case TextTarget::PresetName:
      return submitPresetName();
    case TextTarget::ScriptAddress:
      return submitScriptAddress();
    case TextTarget::Stash:
      if (editingFilter()) return StashPages::submitText(*this);
      break;
    case TextTarget::Setup:
    case TextTarget::Search:
      break;
  }
  stopEditing();
  return {};
}

void Menu::stopEditing() {
  if (!editingFilter()) return;
  if (textTarget() == TextTarget::Stash) {
    StashPages::stopEditing(*this);
  } else {
    // The keyboard goes; the words and what they found stay.
    editing_ = false;
    dirty_ = true;
  }
}

void Menu::keysChanged() {
  browserChanged(keyboard_.rect[1] - 4, keyboard_.rect[1] + keyboard_.rect[3] + 4, keyboard_.rect[0] - 4,
                 keyboard_.rect[0] + keyboard_.rect[2] + 4);
}

void Menu::typedChanged() {
  for (const Button& b : buttons_)
    if (b.panel != kMain && b.field && b.on) browserChanged(b.y - 4, b.y + b.h + 4, b.x - 4, b.x + b.w + 4);
}

// Called every frame while a key is lit: it stays so while the trigger is
// held on it, and long enough to be seen after a quick click. Backspace held
// repeats, as fast as what it changes is drawn. A letter held shows its
// accents; the layout key switches layouts when let go, held it lists them.
void Menu::keyHeld(bool held) {
  KeyDown& k = keyboard_.keyDown;
  const double now = steadySeconds();
  const bool over = pointerPanel_ == (setup_ ? kSetup : kBrowser) && pointerX_ >= k.x && pointerX_ <= k.x + k.w &&
                    pointerY_ >= k.y && pointerY_ <= k.y + kKeyH;
  const bool on = held && over;
  if (on && k.repeat && now >= k.next && browserRow_ < 0) {
    keyboard_.keyDown.next = now + 0.07;
    k.repeat();
  }
  if (k.hold != KeyDown::Hold::None && !k.done) {
    if (on && now - k.at >= kHoldSeconds) {
      k.done = true;
      openKeyPopup(k.hold == KeyDown::Hold::Accents, true);
    } else if (!held && k.hold == KeyDown::Hold::Layouts) {
      k.done = true;
      if (over) {
        if (otherLayout() == 0)
          openKeyPopup(false, false);
        else
          queueCommand(useLayouts(activeLayout() == 0 ? otherLayout() : 0, otherLayout()));
      }
    } else if (!on) {
      k.done = true;  // moved off before the popup came
    }
  }
  if (on || now - k.at < 0.12) return;
  keyChanged(k.x, k.y, k.w);
  keyboard_.keyDown = {};
}

int Menu::activeLayout() const {
  const int i = layoutIndex(keyboard_.layout);
  return i < 0 ? languageLayout() : i;
}

int Menu::otherLayout() const {
  const int i = layoutIndex(keyboard_.other);
  return i < 0 ? languageLayout() : i;
}

// Both are kept from now on, whatever the menu's language.
MenuCommand Menu::useLayouts(int active, int other) {
  keyboard_.layout = kLayouts[active].code;
  keyboard_.other = kLayouts[other].code;
  keysChanged();
  MenuCommand c;
  c.type = MenuCommand::SetKeyboardLayout;
  c.path = keyboard_.layout + "\n" + keyboard_.other;
  return c;
}

// Over the key pressed (keyboard_.keyDown): the letter's accents, in its case, or
// every layout. Opened by holding, it lasts until let go, over one of its
// keys to type it (drag()); opened by a tap, until a click.
void Menu::openKeyPopup(bool accents, bool dragging) {
  const KeyDown& k = keyboard_.keyDown;
  KeyPopup p;
  p.open = true;
  p.accents = accents;
  p.x = k.x + k.w / 2;
  p.y = k.y;
  p.dragging = dragging;
  if (accents) {
    const std::string lower = lowerKey(k.letter);
    for (const std::string& a : accentsOf(lower)) p.keys.push_back(lower != k.letter ? upperKey(a) : a);
    p.before = k.before;
    p.after = filterText();
    p.at = k.cursor;
    if (p.keys.empty() || p.after != std::string(p.before).insert(p.at, k.letter)) return;
  } else {
    for (const KeyLayout& l : kLayouts) p.keys.push_back(l.code);
  }
  keyboard_.popup = p;
  keysChanged();
}

void Menu::closeKeyPopup() {
  if (!keyboard_.popup.open) return;
  keyboard_.popup = {};
  keysChanged();
}

// An accent replaces the letter it came from (unless the text has changed
// since, as from the desktop keyboard), the cursor after it; a layout is used.
MenuCommand Menu::pickFromKeyPopup(size_t index) {
  const KeyPopup p = keyboard_.popup;
  closeKeyPopup();
  if (index >= p.keys.size()) return {};
  if (p.accents) {
    if (filterText() == p.after) {
      setFilter(std::string(p.before).insert(p.at, p.keys[index]));
      setCursor(p.at + p.keys[index].size());
    }
    return {};
  }
  const int chosen = layoutIndex(p.keys[index]);
  return useLayouts(chosen, chosen == 0 ? otherLayout() : chosen);
}

// The on-screen keyboard (the kit's): raised keys in a sunk well at the
// bottom of the browser's page (it never covers the fields or results above
// it). The number row with Backspace, the letters with what addresses and
// paths need (/ : @ _ . -) in the layout's letters (kLayouts), Done at the
// right, Shift, a key to all the other symbols and back, then Hide, Clear,
// the layout key, Space, ← and → (the text cursor) and Paste. Letters are
// large; the other keys say what they do, dim; Done is filled in the accent
// and Shift latched while it is on. A key pressed is drawn pushed in
// (keyboard_.keyDown, drawnPressed()).
void Menu::layoutKeyboard(int panel, double cx, double top, double keyW) {
  const Strings& t = text();
  static const std::vector<std::string> kNumbers = {"1", "2", "3", "4", "5", "6",
                                                    "7", "8", "9", "0", "-"};
  // All of ASCII's punctuation, for user names and passwords; / and : stay put.
  static const std::vector<std::string> kSymbols[3] = {
      {"!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "/", ":"},
      {"+", "=", "_", "\\", ";", "\"", "'", "<", ">", "?", "~"},
      {"[", "]", "{", "}", "|", "`", ",", ".", "-", "€"}};
  constexpr int kLetterSize = 30, kKeySize = 23, kDoneSize = 24;
  const KeyLayout& layout = kLayouts[activeLayout()];
  const std::vector<std::string>* kRows[4] = {&kNumbers, &layout.rows[0], &layout.rows[1], &layout.rows[2]};
  if (keyboard_.symbols)
    for (int r = 0; r < 3; ++r) kRows[r + 1] = &kSymbols[r];
  if (top < 0) top = kKeyboardY + kKeyboardPadY;
  constexpr double gap = kit::kSpaceTight;
  const double unit = keyW + gap;
  const double width = 13 * unit - gap;  // twelve and a half keys, Backspace included
  const double x0 = cx - width / 2;
  // On the page, a well kKeyboardPadY around the keys; on a panel of its
  // own (setup's), that panel, kit::kSpace around them.
  const double padX = panel == kBrowser ? kKeyboardPadY : kit::kSpace;
  keyboard_.rect[0] = x0 - padX;
  keyboard_.rect[1] = top - padX;
  keyboard_.rect[2] = width + 2 * padX;
  keyboard_.rect[3] = 4 * kKeyStep + kKeyH + 2 * padX;
  // Its action redraws what it changes (keyChanged()); `fn`: a key that
  // isn't a letter, its label dim.
  auto key = [&](const std::string& label, double x, int row, double w,
                 std::function<MenuCommand()> action, bool fn,
                 std::function<void()> repeat = nullptr) {
    const double y = top + row * kKeyStep;
    Button b = textButton(label, Icon::None, nullptr);
    b.action = [this, x, y, w, repeat, action = std::move(action)] {
      const KeyDown& k = keyboard_.keyDown;  // the key before comes back up
      if (k.at >= 0) keyChanged(k.x, k.y, k.w);
      const double now = steadySeconds();
      keyboard_.keyDown = {x, y, w, now, now + 0.5, repeat};
      keyChanged(x, y, w);
      return action();
    };
    b.key = b.keyCap = true;
    b.panel = panel;
    b.x = x;
    b.y = y;
    b.w = w;
    b.h = kKeyH;
    b.radius = kit::kPieceCorner;
    b.fontSize = fn ? kKeySize : kLetterSize;
    b.dimInk = fn;
    buttons_.push_back(b);
  };
  auto letter = [&](const std::string& k, double x, int row, double w) {
    const std::string label = keyboard_.shift ? upperKey(k) : k;
    key(label, x, row, w, [this, label] {
      const std::string before = filterText();
      const size_t at = cursorAt();
      typeText(label);
      if (!accentsOf(lowerKey(label)).empty()) {
        keyboard_.keyDown.hold = KeyDown::Hold::Accents;
        keyboard_.keyDown.letter = label;
        keyboard_.keyDown.before = before;
        keyboard_.keyDown.cursor = at;
      }
      if (keyboard_.shift && !keyboard_.capsLock) {  // one capital, like a phone keyboard
        keyboard_.shift = false;
        keysChanged();
      }
      return MenuCommand{};
    }, false);
  };
  // The rows are staggered like a real keyboard's; the second's last key
  // reaches its right edge.
  const double indent[] = {0, unit * 0.5, unit * 0.75, unit * 1.5};
  for (int r = 0; r < 4; ++r)
    for (size_t i = 0; i < kRows[r]->size(); ++i) {
      const double x = x0 + indent[r] + i * unit;
      letter((*kRows[r])[i], x, r, r == 1 && i + 1 == kRows[r]->size() ? x0 + width - x : keyW);
    }
  key(t.backspace, x0 + 11 * unit, 0, 2 * unit - gap, [this] {
    backspace();
    return MenuCommand{};
  }, true, [this] { backspace(); });
  // Done: the folder search keeps its words and hides the keyboard; Stash's
  // fields are kept (submitText()).
  key(t.done, x0 + indent[2] + 11 * unit, 2, width - indent[2] - 11 * unit,
      [this] { return submitText(); }, false);
  buttons_.back().key = false;
  buttons_.back().primary = true;
  buttons_.back().fontSize = kDoneSize;
  // Shift: one capital; tapped twice, caps lock until tapped again.
  key(keyboard_.capsLock ? t.keyboard.capsLock : t.shift, x0, 3, indent[3] - gap, [this] {
    const double now = steadySeconds();
    if (keyboard_.capsLock) {
      keyboard_.shift = keyboard_.capsLock = false;
    } else if (keyboard_.shift && now - keyboard_.shiftAt < 0.5) {
      keyboard_.capsLock = true;
    } else {
      keyboard_.shift = !keyboard_.shift;
    }
    keyboard_.shiftAt = now;
    keysChanged();
    return MenuCommand{};
  }, true);
  buttons_.back().latched = keyboard_.shift || keyboard_.capsLock;
  buttons_.back().disabled = keyboard_.symbols;
  buttons_.back().tooltip = t.keyboard.tipShift;
  key(keyboard_.symbols ? t.letterKeys : t.symbolKeys, x0 + indent[3] + 10 * unit, 3,
      width - indent[3] - 10 * unit, [this] {
        keyboard_.symbols = !keyboard_.symbols;
        keyboard_.shift = keyboard_.capsLock = false;
        keysChanged();
        return MenuCommand{};
      }, true);
  // Bottom row: Hide, Clear, the layout key, Space, the arrows and Paste (the
  // desktop clipboard, e.g. a name copied on the PC).
  const double sideW = 2 * unit - gap;
  key(t.hide, x0, 4, sideW, [this] {
    stopEditing();
    return MenuCommand{};
  }, true);
  buttons_.back().key = false;
  buttons_.back().tooltip = stashMenu_ && stashMenu_->typing == StashMenu::Typing::Server
                                ? t.tipHideServerKeyboard : t.tipHideKeyboard;
  key(t.clear, x0 + 2 * unit, 4, sideW, [this] {
    setFilter("");
    return MenuCommand{};
  }, true);
  // The layout key: what it does is decided when let go or held (keyHeld()).
  key(layout.key, x0 + 4 * unit, 4, keyW, [this] {
    keyboard_.keyDown.hold = KeyDown::Hold::Layouts;
    return MenuCommand{};
  }, true);
  if (otherLayout() == 0) {
    buttons_.back().tooltip = t.keyboard.tipLayouts;
  } else {
    char tip[256];
    std::snprintf(tip, sizeof(tip), t.keyboard.tipLayout,
                  kLayouts[activeLayout() == 0 ? otherLayout() : 0].name);
    buttons_.back().tooltip = tip;
  }
  key(t.space, x0 + 5 * unit, 4, 4 * unit - gap, [this] {
    typeText(" ");
    return MenuCommand{};
  }, true);
  for (const int step : {-1, 1}) {
    key("", x0 + (step < 0 ? 9 : 10) * unit, 4, keyW, [this, step] {
      moveCursor(step);
      return MenuCommand{};
    }, true, [this, step] { moveCursor(step); });
    buttons_.back().icon = step < 0 ? Icon::Arrow : Icon::ArrowRight;
    buttons_.back().iconSize = 26;
    buttons_.back().tooltip = step < 0 ? t.keyboard.tipLeft : t.keyboard.tipRight;
  }
  key(t.paste, x0 + 11 * unit, 4, sideW, command(MenuCommand::Paste), true);
  // The popup's row covers the one above the key it came from, within the
  // keyboard; drawPopups() puts its pane behind it.
  if (keyboard_.popup.open) {
    const KeyPopup& p = keyboard_.popup;
    const double w = p.accents ? keyW : 2 * unit - gap;
    const double total = static_cast<double>(p.keys.size()) * (w + gap) - gap;
    const double left = std::clamp(p.x - total / 2, x0, x0 + width - total);
    for (size_t i = 0; i < p.keys.size(); ++i) {
      const int chosen = p.accents ? -1 : layoutIndex(p.keys[i]);
      Button b = textButton(p.accents ? p.keys[i] : kLayouts[chosen].name, Icon::None, nullptr,
                            chosen >= 0 && chosen == activeLayout());
      b.action = [this, i] { return pickFromKeyPopup(i); };
      b.key = true;
      b.popup = true;
      b.panel = panel;
      b.x = left + static_cast<double>(i) * (w + gap);
      b.y = p.y - kKeyStep;
      b.w = w;
      b.h = kKeyH;
      b.radius = kit::kPieceCorner;
      b.fontSize = p.accents ? kLetterSize : kit::kRowSize;
      buttons_.push_back(b);
    }
  }
  keyboard_.shown = true;
}

void Menu::drawKeyboard(cairo_t* cr) const {
  if (!keyboard_.shown) return;
  drawFinish(cr, Finish::Sunk, keyboard_.rect[0], keyboard_.rect[1], keyboard_.rect[2],
             keyboard_.rect[3], kit::kGroupCorner);
}

// Its box and the shadow its finish at rest casts (Done's is the widest),
// which goes as it is pushed in.
void Menu::keyChanged(double x, double y, double w) {
  static const double reach =
      std::max({MenuFinishes::reach(Finish::Raised), MenuFinishes::reach(Finish::Primary),
                MenuFinishes::reach(Finish::Latched), MenuFinishes::reach(Finish::Pressed)}) + 2;
  browserChanged(y - reach, y + kKeyH + reach, x - reach, x + w + reach);
}
