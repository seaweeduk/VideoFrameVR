#include "menu_parts.h"

#include "platform.h"

#include <algorithm>
#include <cstdio>

// Funscripts played on a device through Intiface (script_player.h): their
// section in Settings, Intiface's address being typed, and the strip's label
// (layoutMain() in menu_player.cpp draws it).

std::string Menu::scriptStatusText() const {
  const ScriptStrings& t = text().scripts;
  const ScriptStatus& s = view_.scriptStatus;
  char line[512];
  switch (s.link) {
    case ScriptLink::Off: return {};
    case ScriptLink::NoAddress: return t.noAddress;
    case ScriptLink::Connecting: return t.connecting;
    case ScriptLink::NoServer: return t.noServer;
    case ScriptLink::NoDevice: return t.noDevice;
    case ScriptLink::Ready:
      if (s.devices > 1) std::snprintf(line, sizeof(line), t.readyMore, s.device.c_str(), s.devices - 1);
      else std::snprintf(line, sizeof(line), t.ready, s.device.c_str());
      return line;
  }
  return {};
}

void Menu::showScriptSettings() {
  showPage(Page::Settings);
  settingsSection_ = SettingsSection::Scripts;
}

// Settings' Funscripts section: the switch; while on, where Intiface is and
// how far the way to a device has got, and beside it the timing and the part
// of the stroke used.
void Menu::layoutScriptSettings(PageLayout& page) {
  const ScriptStrings& t = text().scripts;
  const ScriptSettings& s = view_.scripts;
  GroupLayout scripts = page.group(0, t.group);
  scripts.toggle(t.use, s.enabled, command(MenuCommand::ToggleScripts)).tip(t.offHint);
  if (!s.enabled) {
    scripts.note(t.offHint);
    return;
  }
  scripts.labelsFor({t.address});
  const bool empty = s.address.empty();
  scripts
      .field(t.address, !empty ? s.address : kStandaloneHeadset ? t.notSet : t.thisComputer,
             [this] {
               openScriptAddress();
               return MenuCommand{};
             },
             empty)
      .tip(t.addressHint);
  Lamp lamp = Lamp::None;
  switch (view_.scriptStatus.link) {
    case ScriptLink::Ready: lamp = Lamp::Ok; break;
    case ScriptLink::Connecting: lamp = Lamp::Busy; break;
    case ScriptLink::NoServer:
    case ScriptLink::NoDevice: lamp = Lamp::Failed; break;
    case ScriptLink::Off:
    case ScriptLink::NoAddress: break;
  }
  scripts.status(lamp, scriptStatusText());

  // The device's motion: earlier or later than the script, and the part of
  // its stroke used. Each end pushes the other along to keep their gap
  // (main.cpp).
  GroupLayout motion = page.group(1, t.motion);
  motion.labelsFor({t.offset, t.rangeLow, t.rangeHigh});
  const auto ms = [](int v, bool tick) {
    char text[32];
    std::snprintf(text, sizeof(text), v ? "%+d%s" : "%d%s", v, tick ? "" : " ms");
    return std::string(text);
  };
  motion.value(t.offset, MenuCommand::SetScriptOffset, s.offsetMs, value_range::kScriptOffset.least,
               value_range::kScriptOffset.most, ms(s.offsetMs, false), value_range::kScriptOffset.step)
      .format(ms)
      .mark(0)
      .tip(t.offsetHint);
  motion.note(t.offsetHint);
  motion.value(t.rangeLow, MenuCommand::SetScriptRangeLow, s.rangeLow, 0, 100 - kScriptRangeGap,
               std::to_string(s.rangeLow) + " %")
      .format(withUnit(" %"))
      .tip(t.rangeHint);
  motion.value(t.rangeHigh, MenuCommand::SetScriptRangeHigh, s.rangeHigh, kScriptRangeGap, 100,
               std::to_string(s.rangeHigh) + " %")
      .format(withUnit(" %"))
      .tip(t.rangeHint);
  motion.note(t.rangeHint);
}

void Menu::openScriptAddress() {
  scriptAddress_ = {true, view_.scripts.address};
  dropdown_ = Dropdown::None;
  editing_ = true;
  dirty_ = true;
}

MenuCommand Menu::submitScriptAddress() {
  MenuCommand c;
  c.type = MenuCommand::SetScriptAddress;
  c.path = scriptAddress_.text;
  scriptAddress_ = {};
  editing_ = false;
  dirty_ = true;
  return c;
}

// The address field in a group alone on the page, the keyboard under it:
// what to type and what Intiface needs set for it, and Save.
void Menu::layoutScriptAddress() {
  const ScriptStrings& t = text().scripts;
  PageLayout page(*this, text().settings);
  GroupLayout group = page.group(0, t.group);
  group.labelsFor({t.address});
  Field field;
  field.text = scriptAddress_.text;
  if (!kStandaloneHeadset) field.example = t.thisComputer;
  field.typing = editing_;
  field.action = [this] {
    editing_ = true;
    return MenuCommand{};
  };
  group.field(t.address, field);
  group.note(kStandaloneHeadset ? std::string(t.addressHint)
                                : std::string(t.addressHint) + " " + t.addressLocal,
             false, 6);
  group.buttons({{t.save, "", [this] { return submitScriptAddress(); }, false, true},
                 {text().cancel, "", [this] {
                    scriptAddress_ = {};
                    editing_ = false;
                    dirty_ = true;
                    return MenuCommand{};
                  }}});
  if (editing_) layoutKeyboard();
}
