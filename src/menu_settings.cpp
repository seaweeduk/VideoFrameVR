#include "menu_parts.h"

#include "menu_controls.h"
#include "menu_stash.h"

#include <cmath>
#include <cstdio>

// Settings: a page in four sections, built from the menu's parts.
//   General: the menu and its colour, the view, playback, subtitles, Help.
//   Controls: what the controllers' buttons do (menu_controls.cpp).
//   Stash / XBVR: which media server is used, its server and folders, how
//   it syncs, and sending to it (menu_stash_settings.cpp).
//   Funscripts: played on a device through Intiface (menu_scripts.cpp).
// Video settings (menu_video_settings.cpp) and stereo focus (menu_focus.cpp)
// are tabs of the playback strip and show without the sources column. The
// page never scrolls (the debug fit check says where it would).

namespace {

// Subtitles' distance: centimetres, said in metres.
std::string metres(int cm, bool unit = true) {
  char text[32];
  std::snprintf(text, sizeof(text), unit ? "%g m" : "%g", cm / 100.0);
  return text;
}

// The remote control's state, and what to type into the other app.
std::string remoteControlText(const RemoteControlStrings& t, const RemoteControlStatus& s) {
  using Link = RemoteControlStatus::Link;
  char text[256];
  switch (s.link) {
    case Link::Off: return {};
    case Link::Starting: return t.starting;
    case Link::PortInUse:
      std::snprintf(text, sizeof(text), t.portInUse, deo::kPort);
      return text;
    case Link::Failed: return t.failed;
    case Link::Listening: break;
  }
  char apps[128];
  if (s.clients == 1) std::snprintf(apps, sizeof(apps), "%s", t.oneApp);
  else if (s.clients > 1) std::snprintf(apps, sizeof(apps), t.apps, s.clients);
  else std::snprintf(apps, sizeof(apps), "%s", t.noApp);
  if (s.address.empty()) std::snprintf(text, sizeof(text), t.port, deo::kPort);
  else std::snprintf(text, sizeof(text), t.listening, s.address.c_str(), deo::kPort);
  return std::string(text) + " · " + apps;
}

}  // namespace

void Menu::layoutSettings() {
  // Intiface's address being typed in: alone, over the keyboard. Before
  // Stash's panel, which takes any typing here for its own.
  if (settingsPages_.scriptAddress.open) {
    layoutScriptAddress();
    return;
  }
  // Stash's server being typed in (its fields over the keyboard), or its
  // ratings and plays being sent: alone.
  if (stashMenu_ && StashPages::layoutSettingsPanel(*this)) return;
#ifndef NDEBUG
  if (settingsPages_.settingsFitChecked != &text()) checkSettingsFit();
#endif
  layoutSettingsSection(settingsPages_.section);
}

void Menu::showStashSettings() {
  showPage(Page::Settings);
  settingsPages_.section = SettingsSection::Stash;
}

void Menu::layoutSettingsSection(SettingsSection section, const StashSettingsShown* stash) {
  const Strings& t = text();
  PageLayout page(*this, t.settings);
  // The servers' section is named for every kind, whichever is in use.
  // By SettingsSection, in its order.
  page.sections({t.general, t.controlsPage.section, mediaServerNames(), t.scripts.group},
                static_cast<int>(section),
                [this](int i) {
                  StashPages::leaveSettings(*this);
                  if (controls_) controls_->noticeShown = false;
                  settingsPages_.section = static_cast<SettingsSection>(i);
                  dropdown_ = Dropdown::None;
                  dirty_ = true;
                });
  if (section == SettingsSection::Controls) {
    layoutControls(page);
    return;
  }
  if (section == SettingsSection::Stash) {
    StashPages::layoutSettings(*this, page, stash ? *stash : StashPages::settingsShown(*this));
    return;
  }
  if (section == SettingsSection::Scripts) {
    layoutScriptSettings(page);
    return;
  }
#ifndef NDEBUG
  const bool checking = fitCase_ != nullptr;
#else
  constexpr bool checking = false;
#endif

  // The menu: its language, from a list, and the file thumbnails.
  {
    GroupLayout menu = page.group(0, t.groupMenu);
    menu.labelsFor({t.language});
    const bool open = dropdown_ == Dropdown::Language;
    Row language = menu.select(t.language, languageName(languageChoice()), [this] {
      dropdown_ = dropdown_ == Dropdown::Language ? Dropdown::None : Dropdown::Language;
      return MenuCommand{};
    }, open);
    language.tip(t.languageHint);
    if (open) {
      const Button opener = language.control();
      std::vector<Button> entries;
      for (int i = 0; i < languageCount(); ++i)
        entries.push_back(textButton(languageName(i), Icon::None, [this, i] {
          dropdown_ = Dropdown::None;
          return command(MenuCommand::SetLanguage, i)();
        }, i == languageChoice()));
      addDropdownBelow(opener, std::move(entries));
    }
    menu.toggle(t.thumbnails, view_.thumbnails, command(MenuCommand::ToggleThumbnails))
        .tip(t.thumbnailsHint);
  }

  // Appearance: the menu's colour, a preset or (Custom) a hue, brightness
  // and saturation of one's own, then the pointer's, a preset or a hue.
  // Hue is drawn as the colours to choose from.
  {
    const AccentStrings& at = t.accent;
    GroupLayout look = page.group(0, at.group);
    look.labelsFor({at.menu, at.pointer, at.hue, at.brightness, at.saturation});
    look.swatches(at.menu, {at.names, at.names + kAccentCustom + 1}, view_.accent.preset,
                  [](int i) { return command(MenuCommand::SetAccent, i)(); });
    if (view_.accent.preset == kAccentCustom) {
      const int hue = view_.accent.hue, light = view_.accent.brightness;
      const int saturation = view_.accent.saturation;
      // Around the circle: − from 0 goes to 359.
      look.value(at.hue, MenuCommand::SetAccentHue, hue, value_range::kAccentHue.least,
                 value_range::kAccentHue.most, std::to_string(hue) + "°", value_range::kAccentHue.step,
                 command(MenuCommand::SetAccentHue, (hue + 359) % 360),
                 command(MenuCommand::SetAccentHue, (hue + 1) % 360))
          .format(withUnit("°"));
      look.value(at.brightness, MenuCommand::SetAccentBrightness, light, value_range::kPercent.least,
                 value_range::kPercent.most, std::to_string(light) + " %")
          .format(withUnit(" %"))
          .mark(Accent{}.brightness)
          .tip(at.brightnessHint);
      look.value(at.saturation, MenuCommand::SetAccentSaturation, saturation, value_range::kPercent.least,
                 value_range::kPercent.most, std::to_string(saturation) + " %")
          .format(withUnit(" %"));
    }
    std::vector<int> pointers;
    for (int i = 0; i < kAccentPresets; ++i) {
      double c[3];
      pointerSrgb({i, 0}, c);
      pointers.push_back(static_cast<int>(std::lround(c[0] * 255)) << 16 |
                         static_cast<int>(std::lround(c[1] * 255)) << 8 | static_cast<int>(std::lround(c[2] * 255)));
    }
    look.swatches(at.pointer, {at.names, at.names + kAccentCustom + 1}, view_.pointerColour,
                  [](int i) { return command(MenuCommand::SetPointerColour, i)(); }, pointers);
    if (view_.pointerColour == kAccentCustom) {
      const int hue = view_.pointerHue;
      look.value(at.hue, MenuCommand::SetPointerHue, hue, value_range::kAccentHue.least,
                 value_range::kAccentHue.most, std::to_string(hue) + "°", value_range::kAccentHue.step,
                 command(MenuCommand::SetPointerHue, (hue + 359) % 360),
                 command(MenuCommand::SetPointerHue, (hue + 1) % 360))
          .format(withUnit("°"));
    }
  }

  // Remote control (deo_remote.h): the switch; while on, this device's
  // address and port, for the other app, and how many are connected.
  {
    const RemoteControlStrings& rt = t.remoteControl;
    GroupLayout remote = page.group(0, rt.group);
    remote.toggle(rt.use, view_.remoteControl, command(MenuCommand::ToggleRemoteControl)).tip(rt.hint);
    if (view_.remoteControl) {
      using Link = RemoteControlStatus::Link;
      const RemoteControlStatus& s = view_.remoteControlStatus;
      const Lamp lamp = s.link == Link::Listening   ? (s.clients > 0 ? Lamp::Ok : Lamp::None)
                        : s.link == Link::Starting  ? Lamp::Busy
                        : s.link == Link::Off       ? Lamp::None
                                                    : Lamp::Failed;
      remote.status(lamp, remoteControlText(rt, s));
    } else {
      remote.note(rt.hint);
    }
  }

  // The view: the night sky, and passthrough for videos made for it.
  {
    GroupLayout view = page.group(1, t.groupView);
    view.toggle(t.background, view_.background, command(MenuCommand::ToggleBackground));
    const AutoPassthroughStrings& pt = t.autoPassthrough;
    // Only where the room can show: the runtime blends the eye images over
    // it, or on a PC whose runtime doesn't, the room is drawn in key green
    // (its switch shows only there).
    const bool available = view_.passthroughAvailable;
    view.toggle(pt.setting, view_.autoPassthrough && available,
                command(MenuCommand::ToggleAutoPassthrough))
        .tip(available ? pt.hint : pt.unavailable)
        .disable(!available);
    if (!kStandaloneHeadset && !view_.opacityApplies)
      view.toggle(pt.keyGreen, view_.keyGreen, command(MenuCommand::ToggleKeyGreen))
          .tip(pt.keyGreenHint);
  }

  // Playback: resuming where videos were left, the performance readout,
  // controller vibration, and the drive offline downloads go to
  // (menu_offline.cpp), from a list.
  {
    GroupLayout playback = page.group(1, t.groupPlayback);
    playback.toggle(t.resume, view_.resume, command(MenuCommand::ToggleResume)).tip(t.resumeHint);
    if (kStandaloneHeadset)
      playback
          .toggle(t.decoderBusy.setting, view_.freeDecoder, command(MenuCommand::ToggleFreeDecoder))
          .tip(t.decoderBusy.settingHint);
    playback
        .toggle(t.showPerformance, view_.showPerformance,
                command(MenuCommand::ToggleShowPerformance))
        .tip(t.showPerformanceHint);
    playback.toggle(t.vibration, view_.vibration, command(MenuCommand::ToggleVibration))
        .tip(t.vibrationHint);
    const bool drives = offline_ && !offline_->drives().empty();
    if (drives || checking) {
      playback.labelsFor({t.offline.setting});
      const bool open = drives && dropdown_ == Dropdown::DownloadsTo;
      Row row = playback.select(t.offline.setting,
                                drives ? driveName(downloadRoot(), false) : std::string(),
                                [this] {
                                  dropdown_ = dropdown_ == Dropdown::DownloadsTo
                                                  ? Dropdown::None
                                                  : Dropdown::DownloadsTo;
                                  return MenuCommand{};
                                },
                                open);
      row.tip(t.offline.settingHint);
      if (open) {
        const Button opener = row.control();
        const std::string chosen = downloadRoot();
        std::vector<Button> entries;
        for (const Offline::Drive& d : offline_->drives())
          entries.push_back(textButton(driveName(d, false), Icon::None, [this, root = d.root] {
            dropdown_ = Dropdown::None;
            MenuCommand c;
            c.type = MenuCommand::SetDownloadsTo;
            c.path = root;
            return c;
          }, d.root == chosen));
        addDropdownBelow(opener, std::move(entries), true);
      }
    }
  }

  // The timeline's preview, and 3D videos' in 3D; on the Frame, what it
  // costs, said under it.
  {
    GroupLayout preview = page.group(2, t.seekPreview);
    preview.toggle(t.seekPreviewShow, view_.seekPreview, command(MenuCommand::ToggleSeekPreview))
        .tip(t.seekPreviewHint);
    if (kStandaloneHeadset) preview.note(t.seekPreviewHintFrame);
    preview.toggle(t.seekPreview3d, view_.seekPreview3d, command(MenuCommand::ToggleSeekPreview3d))
        .tip(t.seekPreview3dHint)
        .disable(!view_.seekPreview);
  }

  // Subtitles: floating (2D video's), their size, height and distance.
  {
    GroupLayout subtitles = page.group(2, t.subtitles);
    subtitles.labelsFor({t.subtitleSize, t.subtitleRaise, t.subtitleDistance});
    subtitles
        .toggle(t.subtitles3d, view_.floatingSubtitles,
                command(MenuCommand::ToggleFloatingSubtitles))
        .tip(t.subtitles3dHint);
    const int size = view_.subtitleSize, raise = view_.subtitleRaise;
    const int distance = view_.subtitleDistance;
    static const Settings defaults;
    subtitles
        .value(t.subtitleSize, MenuCommand::SetSubtitleSize, size, value_range::kSubtitleSize.least,
               value_range::kSubtitleSize.most, std::to_string(size) + " %",
               value_range::kSubtitleSize.step)
        .format(withUnit(" %"))
        .mark(defaults.subtitleSize);
    subtitles
        .value(t.subtitleRaise, MenuCommand::SetSubtitleRaise, raise,
               value_range::kSubtitleRaise.least, value_range::kSubtitleRaise.most,
               std::to_string(raise), value_range::kSubtitleRaise.step)
        .tip(t.tipSubtitleRaise);
    subtitles
        .value(t.subtitleDistance, MenuCommand::SetSubtitleDistance, distance,
               value_range::kSubtitleDistance.least, value_range::kSubtitleDistance.most,
               metres(distance), value_range::kSubtitleDistance.step)
        .format([](int v, bool tick) { return metres(v, !tick); })
        .mark(defaults.subtitleDistance);
  }
  layoutHelpGroup(page, 2);
}

// Help: What's new (if this build has notes for this device), and the logs
// and settings saved for a bug report, and how that went.
void Menu::layoutHelpGroup(PageLayout& page, int column) {
  using State = DiagnosticsExport::State;
  const DiagnosticsStrings& t = text().diagnostics;
  const State state = view_.diagnostics;
  GroupLayout help = page.group(column, t.group);
  static const bool notes = !allReleaseNotes().empty();
  std::vector<Action> actions;
  if (notes)
    actions.push_back({text().whatsNew.title, "", [this] {
                         showWhatsNew(allReleaseNotes());
                         return MenuCommand{};
                       }});
  actions.push_back({state == State::Saving ? t.saving : t.save, t.hint,
                     command(MenuCommand::SaveDiagnostics), state == State::Saving});
  help.buttons(actions);
  char saved[512];
  std::snprintf(saved, sizeof(saved), t.saved, view_.diagnosticsFile.c_str());
  const std::string line = state == State::Saved    ? saved
                           : state == State::Failed ? t.failed
                           : view_.crashedLastRun   ? t.crashedHint
                                                    : "";
  if (!line.empty()) help.note(line, true);
}

#ifndef NDEBUG
// Both sections with every row that can show at once, laid out and taken
// away again.
void Menu::checkSettingsFit() {
  settingsPages_.settingsFitChecked = &text();
  const MenuView saved = view_;
  const Dropdown dropdown = dropdown_;
  const PageHead head = pageHead_;
  const size_t buttons = buttons_.size(), groups = groups_.size();

  MenuView& v = view_;
  // General: a colour of one's own, the timeline preview, the downloads'
  // drive (fitCase_), where diagnostics were saved.
  v.pointerColour = kAccentCustom;
  v.accent.preset = kAccentCustom;
  v.diagnostics = DiagnosticsExport::State::Saved;
  v.diagnosticsFile = "videoframevr-diagnostics-20261116-214032.zip";
  v.remoteControl = true;
  v.remoteControlStatus = {RemoteControlStatus::Link::Listening, "192.168.100.200", 8};
  dropdown_ = Dropdown::None;
  fitCase_ = " (General, every row)";
  layoutSettingsSection(SettingsSection::General);
  // Stash: on and connected, every state and folder line at its longest,
  // named as one in use is (not every kind's name, as while none is).
  const std::string named = serverNameShown();
  setServerName(mediaServers().front()->name());
  const StashSettingsShown fullest = StashPages::fullestSettings();
  fitCase_ = " (Stash, every row)";
  layoutSettingsSection(SettingsSection::Stash, &fullest);
  setServerName(named);
  // Funscripts: on, an address, the longest state.
  v.scripts.enabled = true;
  v.scripts.address = "192.168.100.200:12345";
  v.scriptStatus.link = ScriptLink::NoDevice;
  fitCase_ = " (Funscripts, every row)";
  layoutSettingsSection(SettingsSection::Scripts);
  // Controls: every controller type as a list (its longest), with the
  // notice, on a page of its own so the one shown keeps its state.
  std::unique_ptr<ControlsPage> controls = std::move(controls_);
  controls_ = std::make_unique<ControlsPage>();
  for (int type = 0; type < kControllerTypes; ++type)
    for (bool grip : {false, true}) {
      v.controls.type = static_cast<ControllerType>(type);
      const LayoutControl& first = controllerLayout(v.controls.type).controls.front();
      controls_->type = v.controls.type;
      controls_->gripView = grip;
      controls_->noticeShown = true;
      controls_->notice = {{v.controls.type, first.hand, first.control, grip}, Job::Menu, Job::None, 0};
      fitCase_ = grip ? " (Controls, every type, with grip held)" : " (Controls, every type)";
      layoutSettingsSection(SettingsSection::Controls);
    }
  // ... and each type's gestures, as many as there can be, with a notice.
  for (int type = 0; type < kControllerTypes; ++type)
    for (const bool eitherFirst : {false, true}) {
      v.controls.type = static_cast<ControllerType>(type);
      const GestureMap gestures = fullestGestures(v.controls.type, eitherFirst);
      v.controls.gestures = &gestures;
      const std::vector<Gesture>& list = v.controls.gestures->list(v.controls.type);
      controls_->type = v.controls.type;
      controls_->gripView = false;
      controls_->gestureView = true;
      controls_->noticeShown = !list.empty();
      controls_->notice = {};
      if (!list.empty()) {
        controls_->notice.gesture = true;
        controls_->notice.changed = list.back();
        controls_->notice.wasValue = controls_->notice.nowValue = list.back().value;
      }
      fitCase_ = eitherFirst ? " (Controls, every type, gestures, Either first)" : " (Controls, every type, gestures)";
      layoutSettingsSection(SettingsSection::Controls);
    }
  controls_ = std::move(controls);
  fitCase_ = nullptr;

  buttons_.erase(buttons_.begin() + static_cast<std::ptrdiff_t>(buttons), buttons_.end());
  groups_.erase(groups_.begin() + static_cast<std::ptrdiff_t>(groups), groups_.end());
  pageHead_ = head;
  view_ = saved;
  dropdown_ = dropdown;
}
#endif

// The update dialog: a newer release of the managed GitHub install, and once
// the user updates, how that goes. The check itself never shows.
void Menu::layoutUpdateDialog(DialogLayout& dialog) {
  using State = ReleaseUpdate::State;
  using Ink = DialogLayout::Ink;
  const State state = view_.releaseUpdate;
  const ReleaseUpdateStrings& t = text().releaseUpdate;
  const std::string& version = view_.releaseVersion;
  dialog.title(t.notice);
  dialog.line(stashPrinted(state == State::Ready ? t.ready : t.offer, version));
  if (state == State::DownloadFailed)
    dialog.line(t.downloadFailed, Ink::Warn);
  else if (state == State::Downloading || state == State::Restarting)
    dialog.line(stashPrinted(state == State::Downloading ? t.downloading : t.restarting, version),
                Ink::Strong, 3, true);
  auto close = [this] {
    dialog_ = Dialog::None;
    return MenuCommand{};
  };
  const bool failed = state == State::DownloadFailed;
  if (state != State::Available && state != State::Downloading && state != State::Restarting &&
      !failed) {
    dialog.button(textButton(text().whatsNew.close, Icon::None, close, true));
    return;
  }
  const bool busy = state == State::Downloading || state == State::Restarting;
  Button later = textButton(failed ? text().whatsNew.close : t.notNow, Icon::None, close);
  later.disabled = busy;
  dialog.button(later);
  Button go = textButton(failed ? t.tryAgain : t.updateNow, Icon::None,
                         command(MenuCommand::DownloadReleaseUpdate), true);
  go.disabled = busy;
  dialog.button(go);
}

// The settings pages: they share the browser's Settings source.
bool Menu::settingsShown() const { return page_ == Page::Settings; }
