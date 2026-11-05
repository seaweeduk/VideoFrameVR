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

// The folder's (or the library's) search words. Its results start over at
// the very top.
void Menu::setFilter(const std::string& text) {
  if (setup_ && setupText()) {
    SetupPages::setText(*this, text);
    return;
  }
  if (smbForm_.kind != SmbForm::Kind::None) {
    typedIntoSmbForm(text);
    return;
  }
  if (page_ == Page::Places) {
    placeRename_.text = text;
    typedChanged();
    return;
  }
  if (page_ == Page::VideoSettings && presetName_.open) {
    // Whether Save turns into Replace, or is refused, shows with each key.
    // At most 40 characters.
    if (!g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) return;
    const glong chars = std::min<glong>(40, g_utf8_strlen(text.c_str(), -1));
    const PresetNameCheck before = checkPresetName();
    presetName_.text = text.substr(0, g_utf8_offset_to_pointer(text.c_str(), chars) - text.c_str());
    if (checkPresetName() == before)
      typedChanged();
    else
      browserChanged(0, kKeyboardY - 12);
    return;
  }
  if (stashTextPage()) {
    StashPages::setText(*this, text);
    return;
  }
  std::string& filter = page_ == Page::Library ? libraryFilter_ : fileFilter_;
  if (text == filter) return;
  filter = text;
  libraryListStale_ = true;
  scrollOffset_ = 0;
  scrollFraction_ = 0;
  dirty_ = true;
}

MenuCommand Menu::submitText() {
  if (setup_) return SetupPages::submit(*this);
  if (smbForm_.kind != SmbForm::Kind::None) return submitSmbForm();
  if (page_ == Page::Places && !placeRename_.path.empty()) {
    savePlaceName();
    return {};
  }
  if (page_ == Page::VideoSettings && presetName_.open) return submitPresetName();
  if (editingFilter() && stashTextPage()) return StashPages::submitText(*this);
  if (editingFilter()) stopEditing();
  return {};
}

void Menu::stopEditing() {
  if (editingFilter() && stashTextPage()) {
    StashPages::stopEditing(*this);
  } else if (editingFilter()) {
    // The keyboard goes; the words and what they found stay.
    editing_ = false;
    dirty_ = true;
  }
}

void Menu::keysChanged() {
  browserChanged(keyboardRect_[1] - 4, keyboardRect_[1] + keyboardRect_[3] + 4, keyboardRect_[0] - 4,
                 keyboardRect_[0] + keyboardRect_[2] + 4);
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
  KeyDown& k = keyDown_;
  const double now = steadySeconds();
  const bool over = pointerPanel_ == (setup_ ? kSetup : kBrowser) && pointerX_ >= k.x && pointerX_ <= k.x + k.w &&
                    pointerY_ >= k.y && pointerY_ <= k.y + kKeyH;
  const bool on = held && over;
  if (on && k.repeat && now >= k.next && browserRow_ < 0) {
    keyDown_.next = now + 0.07;
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
          command_ = useLayouts(activeLayout() == 0 ? otherLayout() : 0, otherLayout());
      }
    } else if (!on) {
      k.done = true;  // moved off before the popup came
    }
  }
  if (on || now - k.at < 0.12) return;
  browserChanged(k.y - 4, k.y + kKeyH + 4, k.x - 4, k.x + k.w + 4);
  keyDown_ = {};
}

int Menu::activeLayout() const {
  const int i = layoutIndex(keyboardLayout_);
  return i < 0 ? languageLayout() : i;
}

int Menu::otherLayout() const {
  const int i = layoutIndex(keyboardOther_);
  return i < 0 ? languageLayout() : i;
}

// Both are kept from now on, whatever the menu's language.
MenuCommand Menu::useLayouts(int active, int other) {
  keyboardLayout_ = kLayouts[active].code;
  keyboardOther_ = kLayouts[other].code;
  keysChanged();
  MenuCommand c;
  c.type = MenuCommand::SetKeyboardLayout;
  c.path = keyboardLayout_ + "\n" + keyboardOther_;
  return c;
}

// Over the key pressed (keyDown_): the letter's accents, in its case, or
// every layout. Opened by holding, it lasts until let go, over one of its
// keys to type it (drag()); opened by a tap, until a click.
void Menu::openKeyPopup(bool accents, bool dragging) {
  const KeyDown& k = keyDown_;
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
  keyPopup_ = p;
  keysChanged();
}

void Menu::closeKeyPopup() {
  if (!keyPopup_.open) return;
  keyPopup_ = {};
  keysChanged();
}

// An accent replaces the letter it came from (unless the text has changed
// since, as from the desktop keyboard), the cursor after it; a layout is used.
MenuCommand Menu::pickFromKeyPopup(size_t index) {
  const KeyPopup p = keyPopup_;
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

// The on-screen keyboard, in its own panel at the bottom of the
// browser's page (it never covers the fields or results above it): the
// number row with Backspace, the letters with what addresses and paths need
// (/ : @ _ . -) in the layout's letters (kLayouts), Enter at the right,
// Shift, a key to all the other symbols and back, then Hide, Clear, the
// layout key, Space, ← and → (the text cursor) and Paste.
void Menu::layoutKeyboard(int panel, double cx, double top, double keyW) {
  const Strings& t = text();
  static const std::vector<std::string> kNumbers = {"1", "2", "3", "4", "5", "6",
                                                    "7", "8", "9", "0", "-"};
  // All of ASCII's punctuation, for user names and passwords; / and : stay put.
  static const std::vector<std::string> kSymbols[3] = {
      {"!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "/", ":"},
      {"+", "=", "_", "\\", ";", "\"", "'", "<", ">", "?", "~"},
      {"[", "]", "{", "}", "|", "`", ",", ".", "-", "€"}};
  const KeyLayout& layout = kLayouts[activeLayout()];
  const std::vector<std::string>* kRows[4] = {&kNumbers, &layout.rows[0], &layout.rows[1], &layout.rows[2]};
  if (symbols_)
    for (int r = 0; r < 3; ++r) kRows[r + 1] = &kSymbols[r];
  if (top < 0) top = kKeyboardY;
  constexpr double gap = 10;
  const double unit = keyW + gap;
  const double width = 13 * unit - gap;  // twelve and a half keys, Backspace included
  const double x0 = cx - width / 2;
  keyboardRect_[0] = x0 - 20;
  keyboardRect_[1] = top - kKeyboardPadY;
  keyboardRect_[2] = width + 40;
  keyboardRect_[3] = 4 * kKeyStep + kKeyH + 2 * kKeyboardPadY;
  // A key lights up while pressed (keyHeld()); its action redraws what it changes.
  auto key = [&](const std::string& label, double x, int row, double w,
                 std::function<MenuCommand()> action, bool chosen = false,
                 std::function<void()> repeat = nullptr) {
    const double y = top + row * kKeyStep;
    const bool down = keyDown_.at >= 0 && keyDown_.x == x && keyDown_.y == y;
    Button b = textButton(label, Icon::None, nullptr, chosen || down);
    b.action = [this, x, y, w, repeat, action = std::move(action)] {
      const KeyDown& k = keyDown_;  // the key before goes out
      if (k.at >= 0) browserChanged(k.y - 4, k.y + kKeyH + 4, k.x - 4, k.x + k.w + 4);
      const double now = steadySeconds();
      keyDown_ = {x, y, w, now, now + 0.5, repeat};
      browserChanged(y - 4, y + kKeyH + 4, x - 4, x + w + 4);
      return action();
    };
    b.key = true;
    b.panel = panel;
    b.x = x;
    b.y = y;
    b.w = w;
    b.h = kKeyH;
    b.radius = 10;
    b.fontSize = 28;
    buttons_.push_back(b);
  };
  auto letter = [&](const std::string& k, double x, int row, double w) {
    const std::string label = shift_ ? upperKey(k) : k;
    key(label, x, row, w, [this, label] {
      const std::string before = filterText();
      const size_t at = cursorAt();
      typeText(label);
      if (!accentsOf(lowerKey(label)).empty()) {
        keyDown_.hold = KeyDown::Hold::Accents;
        keyDown_.letter = label;
        keyDown_.before = before;
        keyDown_.cursor = at;
      }
      if (shift_ && !capsLock_) {  // one capital, like a phone keyboard
        shift_ = false;
        keysChanged();
      }
      return MenuCommand{};
    });
    buttons_.back().fontSize = 34;  // larger, so that . , ' and : read
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
  }, false, [this] { backspace(); });
  // Enter: the folder search keeps its words and hides the keyboard;
  // Stash's fields are kept (submitText()).
  key(t.done, x0 + indent[2] + 11 * unit, 2, width - indent[2] - 11 * unit,
      [this] { return submitText(); }, true);
  buttons_.back().key = false;
  // Shift: one capital; tapped twice, caps lock until tapped again.
  key(capsLock_ ? t.keyboard.capsLock : t.shift, x0, 3, indent[3] - gap, [this] {
    const double now = steadySeconds();
    if (capsLock_) {
      shift_ = capsLock_ = false;
    } else if (shift_ && now - shiftAt_ < 0.5) {
      capsLock_ = true;
    } else {
      shift_ = !shift_;
    }
    shiftAt_ = now;
    keysChanged();
    return MenuCommand{};
  }, shift_);
  buttons_.back().disabled = symbols_;
  buttons_.back().tooltip = t.keyboard.tipShift;
  key(symbols_ ? t.letterKeys : t.symbolKeys, x0 + indent[3] + 10 * unit, 3,
      width - indent[3] - 10 * unit, [this] {
        symbols_ = !symbols_;
        shift_ = capsLock_ = false;
        keysChanged();
        return MenuCommand{};
      });
  // Bottom row: Hide, Clear, the layout key, Space, the arrows and Paste (the
  // desktop clipboard, e.g. a name copied on the PC).
  const double sideW = 2 * unit - gap;
  key(t.hide, x0, 4, sideW, [this] {
    stopEditing();
    return MenuCommand{};
  });
  buttons_.back().key = false;
  buttons_.back().tooltip = stashMenu_ && stashMenu_->typing == StashMenu::Typing::Server
                                ? t.tipHideServerKeyboard : t.tipHideKeyboard;
  key(t.clear, x0 + 2 * unit, 4, sideW, [this] {
    setFilter("");
    return MenuCommand{};
  });
  // The layout key: what it does is decided when let go or held (keyHeld()).
  key(layout.key, x0 + 4 * unit, 4, keyW, [this] {
    keyDown_.hold = KeyDown::Hold::Layouts;
    return MenuCommand{};
  });
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
  });
  for (const int step : {-1, 1}) {
    key("", x0 + (step < 0 ? 9 : 10) * unit, 4, keyW, [this, step] {
      moveCursor(step);
      return MenuCommand{};
    }, false, [this, step] { moveCursor(step); });
    buttons_.back().icon = step < 0 ? Icon::Arrow : Icon::ArrowRight;
    buttons_.back().tooltip = step < 0 ? t.keyboard.tipLeft : t.keyboard.tipRight;
  }
  key(t.paste, x0 + 11 * unit, 4, sideW, command(MenuCommand::Paste));
  // The popup's row covers the one above the key it came from, within the
  // keyboard; drawPopups() puts its pane behind it.
  if (keyPopup_.open) {
    const KeyPopup& p = keyPopup_;
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
      b.radius = 10;
      b.fontSize = p.accents ? 34 : 28;
      buttons_.push_back(b);
    }
  }
  keyboardShown_ = true;
}
