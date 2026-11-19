#include "menu_parts.h"

#include "menu_stash.h"
#include "stash.h"

// The media server in the navigation column and on the Settings page: its
// section (Home, Scenes, Performers, Studios and the pinned lists),
// Settings' section for it (which kind is used, its server, its folders
// here, sending to it), and the server's fields typed in with the keyboard.

std::string stashAgo(int64_t when) {
  const StashStrings& t = strings().stash;
  const long seconds = static_cast<long>(std::time(nullptr) - when);
  if (seconds < 60) return t.justNow;
  if (seconds < 3600) return stashPrinted(t.minutesAgo, static_cast<int>(seconds / 60));
  if (seconds < 86400) return stashPrinted(t.hoursAgo, static_cast<int>(seconds / 3600));
  const int days = static_cast<int>(seconds / 86400);
  return days == 1 ? std::string(t.oneDayAgo) : stashPrinted(t.daysAgo, days);
}

std::string stashSyncStep(const StashStatus& status) {
  const StashSetupStrings& t = strings().stashSetup;
  switch (status.step) {
    case StashStatus::Step::Saving: return t.saving;
    case StashStatus::Step::Building: return t.building;
    case StashStatus::Step::Fetching: break;
  }
  if (status.received == 0) return t.connecting;
  if (status.received < 100000) return t.downloadingStart;
  return stashPrinted(t.downloading, stashPrinted("%.1f", status.received / 1e6));
}

void StashPages::drawWaiting(const Menu& m, cairo_t* cr, double x, double y, double w) {
  const StashStatus& status = m.stash_->status();
  const Palette& pal = m.palette();
  const bool failed = !status.error.empty();
  const std::string line = status.problem == StashProblem::TooOld ? status.error
                           : failed                               ? strings().stash.noCatalog
                           : status.syncing                       ? stashSyncStep(status)
                                                                  : strings().stash.loading;
  m.drawText(cr, line, x, y, w, kFileRowH, 27, false, false, pal.textDim.r, pal.textDim.g,
             pal.textDim.b, pal.textDim.a);
  if (failed || !status.syncing) return;
  const double ink[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a};
  m.drawTextLines(cr, strings().stashSetup.firstSync, x, y + kFileRowH, w, 22, 2, ink, false);
}

std::string stashNumber(long n) {
  std::string digits = std::to_string(n < 0 ? -n : n), out;
  for (size_t i = 0; i < digits.size(); ++i) {
    if (i && (digits.size() - i) % 3 == 0) out += ',';
    out += digits[i];
  }
  return n < 0 ? "-" + out : out;
}

std::string stashPlays(int plays) {
  const StashStrings& t = strings().stash;
  if (plays == 1) return t.onePlay;
  return plays > 1 ? stashPrinted(t.playsCount, stashNumber(plays)) : "";
}

std::string stashScenes(long scenes) {
  const StashStrings& t = strings().stash;
  return scenes == 1 ? std::string(t.oneScene) : stashPrinted(t.count, stashNumber(scenes));
}

std::string stashJoined(const std::vector<std::string>& parts, const char* between) {
  std::string out;
  for (const std::string& p : parts)
    if (!p.empty()) out += (out.empty() ? "" : between) + p;
  return out;
}

// After Library and Offline: Stash, a section as they are, its pages
// and then the pinned lists as places under it, as far as there is room,
// the last row then opening all of them. A label cut off shows in full in
// its tooltip.
void StashPages::layoutNavigation(Menu& m, double y, double bottom) {
  StashMenu& st = state(m);
  const StashStrings& t = strings().stash;
  const bool scenes = m.page_ == Menu::Page::StashScenes;
  const bool allPins = scenes && st.full == StashFull::Pins;
  const int pin = allPins ? -1 : shownPin(m);
  auto row = [&](const std::string& label, Menu::Icon icon, bool chosen, bool place,
                 std::function<void()> open) {
    Button& b = m.navRow(label, icon, chosen, y, place, std::move(open));
    if (m.textWidth(label, b.fontSize, false) > m.navLabelWidth(b)) b.tooltip = label;
    y += b.h + kit::kSpacePiece;
  };
  const Menu::Page page = m.page_;
  row(t.group, Menu::Icon::Layers, false, false, [&m] { showHome(m); });
  row(t.navHome, Menu::Icon::Home, page == Menu::Page::StashHome, true, [&m] { showHome(m); });
  row(t.navScenes, Menu::Icon::Film, scenes && !allPins && !st.ownScenes, true,
      [&m] { showOwnScenes(m); });
  row(t.navPerformers, Menu::Icon::People,
      page == Menu::Page::StashPerformers || page == Menu::Page::StashPerformer, true,
      [&m] { showPerformers(m); });
  row(t.navStudios, Menu::Icon::Building,
      page == Menu::Page::StashStudios || page == Menu::Page::StashStudio, true,
      [&m] { showStudios(m); });
  // The last needs no gap after it.
  const int fit =
      std::max(0, static_cast<int>((bottom - y + kit::kSpacePiece) / (kPlaceRowH + kit::kSpacePiece)));
  const int count = static_cast<int>(st.pins.size());
  const int shown = count <= fit ? count : std::max(0, fit - 1);
  for (int i = 0; i < shown; ++i)
    row(st.pins[i].name, Menu::Icon::Pin, pin == i, true, [&m, i] { openPin(m, i); });
  if (shown < count && fit > 0)
    row(stashPrinted(t.allPins, count), Menu::Icon::Pin, allPins || pin >= shown, true,
        [&m] { showFull(m, StashFull::Pins); });
}

// After first-run setup's Not now, with no server set up: one "Use Stash?"
// card above Settings, its box ending at `bottom`. Connect Stash (or its
// title) opens Settings where a server is chosen; × hides it. Either way
// it is gone for good.
void StashPages::layoutCard(Menu& m, double x, double w, double bottom) {
  const SetupStrings& t = strings().setup;
  constexpr double h = 124;
  const double y = bottom - h;
  m.stashCardY_ = y;
  auto connect = [&m] {
    m.showStashSettings();
    return command(MenuCommand::StashCard)();
  };
  // Its title as a section of the navigation, a place's height.
  Button& title = m.navRow(t.useStash, Menu::Icon::Layers, false, y + 6, false, nullptr);
  title.w = w - kPlaceRowH - 8;
  title.h = kPlaceRowH;
  title.action = connect;
  Button& close = m.addRound(Menu::Icon::Close, t.dontAsk, command(MenuCommand::StashCard),
                             x + w - kPlaceRowH - 4, y + 6, kPlaceRowH);
  close.panel = Menu::kBrowser;
  close.bare = true;
  Button go = m.textButton(t.connectStash, Menu::Icon::None, connect, true);
  go.panel = Menu::kBrowser;
  go.x = x + 20;
  go.y = y + 6 + kPlaceRowH + 10;
  go.h = kPlaceRowH;
  go.fontSize = 24;
  m.buttons_.push_back(go);
}

// Opens a kind's server fields with the keyboard, at `field` (the API key's
// opens at the user name while there is no key: signing in gets it). A kind
// that isn't the one in use starts with none, and stays unused until its
// fields are saved.
void StashPages::editServer(Menu& m, int field, ServerKind kind) {
  if (!m.stashMenu_) attach(m);
  StashMenu& st = state(m);
  const StashSettings& now = m.view_.stash;
  const bool inUse = now.kind == kind;
  st.draft = {inUse ? now.url : std::string(), inUse ? now.apiKey : std::string(), "", ""};
  st.draftUnverified = inUse && now.unverified() ? now.unverifiedHost : std::string();
  st.draftKind = kind;
  st.showPassword = false;
  st.signingIn = {};
  st.signInError.clear();
  st.foundOther.clear();
  st.field = field == 1 && st.draft[1].empty() ? kStashUser : field;
  // A server without a login has only its address.
  if (!mediaServer(kind).abilities().signIn) st.field = 0;
  st.typing = StashMenu::Typing::Server;
  m.editing_ = true;
  m.pageScroll_ = 0;
  m.settingsSection_ = Menu::SettingsSection::Stash;  // where Save and Cancel go back to
}

void StashPages::editServer(Menu& m, int field) { editServer(m, field, m.view_.stash.kind); }

// The choice at the top of the section: no server (-1), or a kind (its
// place in mediaServers()). One with a server set up is used at once, its
// setup as it was left; one without opens its fields, and is used once they
// are saved.
MenuCommand StashPages::chooseServer(Menu& m, int index) {
  leaveSettings(m);
  if (index < 0) return command(MenuCommand::UseServer, -1)();
  const ServerKind kind = mediaServers()[index]->kind();
  const StashSettings& now = m.view_.stash;
  if (now.enabled && now.kind == kind) return {};
  if (m.view_.serversSetUp >> static_cast<int>(kind) & 1)
    return command(MenuCommand::UseServer, static_cast<int>(kind))();
  editServer(m, 0, kind);
  return {};
}

// What Settings' section shows: which kind is used; with one, its server,
// and once it runs how it syncs, what it shows, its folders here and
// sending to it.
StashSettingsShown StashPages::settingsShown(Menu& m) {
  const StashStrings& t = strings().stash;
  const StashSettings& s = m.view_.stash;
  StashSettingsShown shown;
  shown.enabled = s.enabled;
  shown.abilities = mediaServer(s.kind).abilities();
  if (m.stashMenu_ && !state(m).foundOther.empty())
    shown.foundOther = stashPrinted(strings().stashSetup.foundOther, state(m).foundOther);
  if (!s.enabled) return shown;
  const auto& kinds = mediaServers();
  for (size_t i = 0; i < kinds.size(); ++i)
    if (kinds[i]->kind() == s.kind) shown.chosen = static_cast<int>(i) + 1;
  if (!m.stashMenu_) attach(m);
  shown.url = s.url;
  shown.apiKey = s.apiKey.empty() ? t.apiKeyNone : t.apiKeySet;
  if (!m.stash_) return shown;
  shown.connected = true;
  shownFolders(m, shown);
  shown.covers = s.covers;
  shown.titles = s.titles;
  shown.malePerformers = s.malePerformers;

  // What the copy in use holds, and how the last sync went.
  const StashStatus& status = m.stash_->status();
  const StashCatalog* c = catalog(m);
  if (c) {
    const StashCatalogStats& stats = c->stats();
    shown.counts =
        stats.streamed > 0
            ? stashPrinted(t.statusStreamed, stashNumber(stats.stashScenes), stashNumber(stats.listed),
                           stashNumber(stats.streamed), stashNumber(stats.libraryMatched),
                           stashNumber(stats.libraryVideos))
            : stashPrinted(t.status, stashNumber(stats.stashScenes), stashNumber(stats.listed),
                           stashNumber(stats.libraryMatched), stashNumber(stats.libraryVideos));
  }
  const bool synced = status.lastSync > 0 || (c && c->syncedAt() > 0);
  shown.syncing = status.syncing;
  // Its certificate isn't trusted: said in a few words (Sync now's tooltip
  // the rest), and Sync now asks whether to connect anyway.
  shown.certificate = status.problem == StashProblem::Certificate && !status.syncing;
  // Stash has a login the app hasn't signed in to: Sign in.
  shown.signIn = !status.syncing && (status.problem == StashProblem::NeedsSignIn ||
                                     status.problem == StashProblem::KeyRefused);
  if (status.syncing) {
    shown.state = stashSyncStep(status);
    shown.lamp = static_cast<int>(Lamp::Busy);
  } else if (shown.certificate) {
    shown.state = strings().stashSetup.untrustedStatus;
    shown.lamp = static_cast<int>(Lamp::Failed);
  } else if (!status.error.empty()) {
    shown.state = status.error;
    shown.lamp = static_cast<int>(Lamp::Failed);
  } else if (synced) {
    shown.state = stashPrinted(t.synced, stashAgo(status.lastSync > 0 ? status.lastSync : c->syncedAt()),
                               c ? c->version() : status.version);
    shown.lamp = static_cast<int>(Lamp::Ok);
  } else {
    shown.state = c ? t.notSynced : t.loadingCopy;
  }
  shown.firstSync = !c && status.syncing;

  // Write-back: what waits or was sent, and what went wrong.
  shown.writeBack = s.writeBack;
  if (const StashWriteBack* writeBack = s.writeBack ? m.stash_->writeBack() : nullptr) {
    const StashWriteBackStrings& wb = strings().stashWriteBack;
    const StashWriteBackStatus& a = writeBack->status();
    if (a.waitingRatings || a.waitingPlays)
      shown.writeBackLine = stashPrinted(wb.waiting, a.waitingRatings, a.waitingPlays);
    else if (a.sentRatings || a.sentPlays)
      shown.writeBackLine = stashPrinted(wb.sent, a.sentRatings, a.sentPlays);
    if (!a.error.empty()) shown.writeBackLine += (shown.writeBackLine.empty() ? "" : " · ") + a.error;
  }
  return shown;
}

// Settings' section: which kind of server is used (or none) and, with one,
// its server, how it syncs and Sync now, what it shows; its folders here;
// and sending to it.
void StashPages::layoutSettings(Menu& m, Menu::PageLayout& page, const StashSettingsShown& shown) {
  using Action = Menu::Action;
  const StashStrings& t = strings().stash;
  const StashSetupStrings& ts = strings().stashSetup;
  Menu::GroupLayout stash = page.group(0, mediaServerNames());
  std::vector<std::string> kinds{strings().off};
  for (const MediaServer* server : mediaServers()) kinds.push_back(server->name());
  stash.choice({}, kinds, shown.chosen, [&m](int i) { return chooseServer(m, i - 1); })
      .tip(t.tipUse);
  if (!shown.foundOther.empty()) stash.note(shown.foundOther, true);
  if (!shown.enabled) {
    stash.note(t.offHint);
    return;
  }
  auto edit = [&m](int field) {
    return [&m, field] {
      editServer(m, field);
      return MenuCommand{};
    };
  };
  // The server's fields, tapped: they open with the keyboard.
  // A server without a login has no key to show.
  const bool keyed = shown.abilities.signIn;
  const StashStrings& own = stashOwnStrings().stash;  // the key is Stash's
  if (keyed) stash.labelsFor({t.server, own.apiKey});
  else stash.labelsFor({t.server});
  const bool noServer = shown.url.empty();
  stash.field(t.server, noServer ? std::string(ts.serverHint) : shown.url, edit(0), noServer)
      .tip(noServer ? std::string(t.tipFields[0]) : shown.url + "\n" + t.tipFields[0]);
  if (keyed) stash.field(own.apiKey, shown.apiKey, edit(1)).tip(own.tipFields[1]);
  if (!shown.connected) {
    stash.note(t.setupHint);
    return;
  }
  std::vector<Action> actions;
  if (shown.signIn && keyed)
    actions.push_back({strings().network.signIn, "", edit(kStashUser), false, true});
  Action sync{t.syncNow, t.tipSync, command(MenuCommand::SyncStash), shown.syncing};
  if (shown.certificate) {
    sync.tooltip = ts.untrustedTip;
    sync.action = [&m] {
      m.openDialog(Menu::Dialog::StashCertificate, stashHost(m.view_.stash.url));
      return MenuCommand{};
    };
  }
  actions.push_back(sync);
  stash.status(static_cast<Lamp>(shown.lamp), shown.state, actions);
  if (!shown.counts.empty()) stash.note(shown.counts, true);
  if (shown.firstSync) stash.note(ts.firstSync, true);
  stash.toggle(t.covers, shown.covers, command(MenuCommand::ToggleStashCovers));
  stash.toggle(t.titles, shown.titles, command(MenuCommand::ToggleStashTitles));
  stash.toggle(t.malePerformers, shown.malePerformers,
               command(MenuCommand::ToggleStashMalePerformers));

  // Its library folders, once a sync has listed them: what having one here
  // is for, then each one's place here (or that it plays from the server)
  // with Choose…, and the other places its videos were found in, with Use.
  if (!shown.folders.empty()) {
    const StashFoldersStrings& tf = strings().stashFolders;
    Menu::GroupLayout folders = page.group(1, t.folders);
    folders.note(t.foldersHint, false, 3);
    for (const StashSettingsShown::Folder& f : shown.folders) {
      if (!f.name.empty()) folders.note(f.name, true, 1);
      auto choose = [&m, folder = f.folder] {
        m.chooseStashFolder(folder);
        return MenuCommand{};
      };
      const bool here = !f.here.empty();
      folders.field({}, here ? f.here : f.fromServer, choose, !here)
          .tip(f.tooltip)
          .endButton({here ? tf.change : tf.choose, tf.tipChoose, choose});
      for (const std::string& line : f.lines) folders.note(line, true);
      for (const StashSettingsShown::Use& use : f.uses)
        folders.status(Lamp::None, use.text,
                       {{tf.use, use.tooltip,
                         [&m, folder = use.folder, map = use.map] {
                           return m.stashFolders(m.stash_->foldersWith(folder, map));
                         },
                         false, use.first}});
    }
    if (!shown.looking.empty()) folders.note(shown.looking, true);
  }

  // Sending ratings and plays to Stash: the switch, the backup it needs
  // first, and while on what waits or was sent, and the one-off send.
  const StashWriteBackStrings& wb = strings().stashWriteBack;
  Menu::GroupLayout send = page.group(2, wb.group);
  const bool basic = shown.abilities.ratingsAndWatchedOnly();
  send.toggle(basic ? wb.toggleBasic : wb.toggle, shown.writeBack,
              command(MenuCommand::ToggleStashWriteBack))
      .tip(basic ? wb.tipToggleBasic : wb.tipToggle);
  send.note(basic ? wb.hintBasic : wb.hint, false, 6);
  if (!shown.writeBack) return;
  if (!shown.writeBackLine.empty()) send.note(shown.writeBackLine, true);
  send.buttons({{wb.sendAll, wb.tipSendAll, [&m] {
                   StashMenu& st = state(m);
                   st.sendOpen = true;
                   st.sendList = false;
                   m.pageScroll_ = 0;
                   m.stash_->planSend();
                   return MenuCommand{};
                 }}});
}

#ifndef NDEBUG
// Every row the section can have at once, each at its longest: connected,
// signing in after a long reason, two folders each with its line and
// another place, the search under way, sending on with an error.
StashSettingsShown StashPages::fullestSettings() {
  const StashStrings& t = strings().stash;
  const StashFoldersStrings& tf = strings().stashFolders;
  const StashWriteBackStrings& wb = strings().stashWriteBack;
  StashSettingsShown s;
  s.enabled = s.connected = true;
  s.chosen = 1;
  s.abilities.signIn = s.abilities.editTags = s.abilities.deleteFiles = s.abilities.playTimes = true;
  s.url = "https://server.example.com:9999";
  s.apiKey = t.apiKeySet;
  s.foundOther = stashPrinted(strings().stashSetup.foundOther, "Stash");
  for (const char* folder : {"/media/raid/vr", "/media/raid/vr-new"}) {
    StashSettingsShown::Folder& f = s.folders.emplace_back();
    f.folder = f.name = folder;
    f.here = "SanDisk 512/vr";
    f.fromServer = tf.fromServer;
    f.lines = {tf.folderRest};
    f.uses.push_back({stashPrinted(tf.alsoIn, "Videos/Stash/vr-new"), "", folder, {}, false});
  }
  s.looking = stashPrinted(tf.lookingFolders, 2, 2);
  s.lamp = static_cast<int>(Lamp::Failed);
  s.state = strings().stashSetup.publicRefused;
  s.counts = stashPrinted(t.statusStreamed, stashNumber(34540), stashNumber(31910),
                          stashNumber(2630), stashNumber(11870), stashNumber(12040));
  s.signIn = s.firstSync = true;
  s.writeBack = true;
  s.writeBackLine = stashPrinted(wb.waiting, 40, 120) + " · " + strings().stashSetup.refused;
  return s;
}
#endif

// A kind's server fields, the one typed into outlined blue: the server,
// and where it may have a login the user name and password to sign in
// with, and the API key. Save and sync keeps them (and syncs), signing in
// first with a password typed; Cancel or Hide leaves everything as it was.
// The keyboard takes the bottom of the page.
void StashPages::layoutServerEditor(Menu& m) {
  StashMenu& st = state(m);
  // The kind the fields are for names them, whichever is in use.
  const Strings& text = stringsNamed(mediaServer(st.draftKind).name());
  const StashStrings& t = text.stash;
  const NetworkStrings& tn = strings().network;
  const double x0 = columnX(0), w = 3 * kColumnW + 2 * kGap;
  const double x = x0 + kGroupPad, inner = w - 2 * kGroupPad;
  double y = firstRow(kPagePad);
  constexpr double labelW = 380, h = 56, step = 66, eyeW = 120;
  const bool signing = st.signingIn.valid();
  // One without a login has no user name, password or key to type.
  const bool keyed = mediaServer(st.draftKind).abilities().signIn;
  for (const int i : {0, kStashUser, kStashPassword, 1}) {
    if (!keyed && i != 0) continue;
    const bool password = i == kStashPassword;
    // The key is Stash's own.
    const StashStrings& named = i == 1 ? stashOwnStrings().stash : t;
    m.addLabel(x + 12, y, labelW - 12, h, i < 2 ? named.fieldNames[i] : tn.fields[i - 1]);
    const bool active = st.field == i;
    std::string shown = st.draft[i];
    if (password && !st.showPassword) {
      shown.clear();
      for (const char c : st.draft[i])
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) shown += "•";
    }
    Button b = m.textButton(shown, Menu::Icon::None, [&m, i] {
      state(m).field = i;
      return MenuCommand{};
    });
    // The server's address, before anything is typed: an example.
    if (i == 0) b.example = text.stashSetup.serverHint;
    b.panel = Menu::kBrowser;
    b.x = x + labelW;
    b.y = y;
    b.w = inner - labelW - (password ? eyeW + kGap : 0);
    b.h = h;
    b.field = true;
    b.on = active;
    b.caret = active ? m.caret() : -1;
    b.radius = 12;
    b.fontSize = Menu::kTextSize;
    b.stashLook = static_cast<int>(StashLook::Field);
    if (i < 2) b.tooltip = named.tipFields[i];
    m.buttons_.push_back(b);
    if (password) {
      Button eye = m.textButton(st.showPassword ? strings().hide : tn.show, Menu::Icon::None, [&m] {
        state(m).showPassword = !state(m).showPassword;
        return MenuCommand{};
      });
      eye.panel = Menu::kBrowser;
      eye.x = x + inner - eyeW;
      eye.y = y;
      eye.w = eyeW;
      eye.h = h;
      m.buttons_.push_back(eye);
    }
    y += step;
  }
  if (keyed) {
    m.addLabel(x + 12, y, inner - 12, 0, stashOwnStrings().stashSetup.signInHint, kHintSize, true, 2);
    y += hintHeight(2);
  }
  if (!st.signInError.empty()) {
    m.addLabel(x + 12, y, inner - 12, 0, st.signInError, kHintSize, false, 1);
    y += hintHeight(1);
  }
  y += 8;
  const char* waiting = st.draft[kStashPassword].empty() ? strings().setup.connecting : tn.signingIn;
  Button save = m.textButton(signing ? waiting : t.save, Menu::Icon::None,
                             [&m] { return saveServer(m); }, true);
  save.panel = Menu::kBrowser;
  save.x = x + inner - save.w;
  save.y = y;
  save.h = h;
  save.disabled = signing;
  m.buttons_.push_back(save);
  Button cancel = m.textButton(strings().cancel, Menu::Icon::None, [&m] {
    stopEditing(m);
    return MenuCommand{};
  });
  cancel.panel = Menu::kBrowser;
  cancel.x = save.x - kGap - cancel.w;
  cancel.y = y;
  cancel.h = h;
  m.buttons_.push_back(cancel);
  // One accepted unverified: Verify certificate, with a warning sign (its
  // tooltip says which), which Save then checks.
  if (draftUnverified(m)) {
    const StashSetupStrings& ts = text.stashSetup;
    Button verify = m.textButton(ts.verify, Menu::Icon::None, [&m] {
      state(m).draftUnverified.clear();
      m.dirty_ = true;
      return MenuCommand{};
    });
    verify.panel = Menu::kBrowser;
    verify.stashLook = static_cast<int>(StashLook::Warning);
    verify.x = x;
    verify.y = y;
    verify.w = std::min(m.textWidth(ts.verify, Menu::kTextSize, false) + 82, cancel.x - kGap - x);
    verify.h = h;
    verify.tooltip = stashPrinted(ts.unverified, st.draftUnverified);
    m.buttons_.push_back(verify);
  }
  y += h + 16;
  m.addGroup(x0, kPagePad, w, y - kPagePad, t.editTitle);
  m.layoutKeyboard();
}

// Like an SFTP server's changed key: Don't connect is the one selected.
// Connecting anyway saves the server's fields as they are (or as typed, in
// the editor) with its certificate accepted unverified, and syncs.
void StashPages::layoutCertificateDialog(Menu& m, std::vector<Menu::Button>& row) {
  row.push_back(m.textButton(strings().network.dontConnect, Menu::Icon::None, [&m] {
    m.dialog_ = Menu::Dialog::None;
    return MenuCommand{};
  }, true));
  Menu::Button connect = m.textButton(strings().stashSetup.connectUnverified, Menu::Icon::None, [&m] {
    const std::string host = m.dialogPath_;
    m.dialog_ = Menu::Dialog::None;
    const bool editing = m.editing_;
    if (!editing) editServer(m, 0);
    state(m).draftUnverified = host;
    return editing ? saveServer(m) : serverCommand(m);
  });
  connect.danger = true;
  row.push_back(connect);
}

bool StashPages::layoutSettingsPanel(Menu& m) {
  if (m.editing_) {
    layoutServerEditor(m);
    return true;
  }
  if (!m.stash_ || !m.stashMenu_ || !state(m).sendOpen || !m.stash_->writeBack()) return false;
  layoutSendPanel(m);
  return true;
}

// "Send my ratings and plays": what it would send (counts, what is left
// out and why), Dry run listing it scene by scene, Send, and Close; then
// what it sent. The page scrolls when the list is long.
void StashPages::layoutSendPanel(Menu& m) {
  StashMenu& st = state(m);
  const StashWriteBackStrings& t = strings().stashWriteBack;
  const StashSendPlan& plan = m.stash_->writeBack()->sendPlan();
  using State = StashSendPlan::State;
  const double x0 = columnX(0), w = 3 * kColumnW + 2 * kGap;
  const double x = x0 + kGroupPad, inner = w - 2 * kGroupPad;
  double y = firstRow(kPagePad);
  auto line = [&](const std::string& text, bool dim, int lines) {
    m.addLabel(x + 12, y, inner - 12, 0, text, kHintSize, dim, lines);
    y += hintHeight(lines);
  };
  const bool sendable = plan.state == State::Ready && (plan.scenes > 0 || plan.waiting > 0);
  switch (plan.state) {
    case State::None:
    case State::Checking: line(t.checking, false, 1); break;
    case State::Ready:
      if (plan.scenes > 0)
        line(stashPrinted(t.ready, stashNumber(plan.ratings), stashNumber(plan.plays),
                          stashNumber(plan.scenes)),
             false, 1);
      else
        line(t.nothing, false, 1);
      if (plan.found > 0) line(stashPrinted(t.found, stashNumber(plan.found)), true, 1);
      if (plan.waiting > 0) line(stashPrinted(t.waitingToo, stashNumber(plan.waiting)), true, 1);
      line(mediaServer(m.view_.stash.kind).abilities().ratingsAndWatchedOnly() ? t.rulesBasic
                                                                                : t.rules,
           true, 2);
      if (sendable) line(t.backup, true, 1);
      break;
    case State::Sending: line(t.sending, false, 1); break;
    case State::Done:
      line(stashPrinted(t.done, stashNumber(plan.sentRatings), stashNumber(plan.sentPlays)), false,
           1);
      break;
    case State::Failed: line(stashPrinted(t.failed, plan.error), false, 2); break;
  }
  y += 8;

  constexpr double h = 56;
  double right = x + inner;
  auto add = [&](Button b) {
    b.panel = Menu::kBrowser;
    b.x = right - b.w;
    b.y = y;
    b.h = h;
    right = b.x - kGap;
    m.buttons_.push_back(b);
  };
  if (sendable)
    add(m.textButton(t.send, Menu::Icon::None, [&m] {
      m.stash_->sendPlanned();
      state(m).sendList = false;
      return MenuCommand{};
    }, true));
  add(m.textButton(plan.state == State::Ready ? strings().cancel : t.close, Menu::Icon::None, [&m] {
    state(m).sendOpen = false;
    m.stash_->closeSend();
    m.dirty_ = true;
    return MenuCommand{};
  }));
  const bool listed = plan.state == State::Ready && plan.scenes > 0;
  if (listed) {
    add(m.textButton(st.sendList ? t.hideList : t.dryRun, Menu::Icon::None, [&m] {
      state(m).sendList = !state(m).sendList;
      m.dirty_ = true;
      return MenuCommand{};
    }, st.sendList));
    m.buttons_.back().tooltip = t.tipDryRun;
  }
  y += h + 16;

  // The dry run: each scene with what would be sent to it.
  if (listed && st.sendList) {
    constexpr int kMaxListed = 150;
    int shown = 0, left = 0;
    for (const StashSendPlan::Item& item : plan.items) {
      if (item.stars <= 0 && item.plays <= 0) continue;
      if (shown == kMaxListed) {
        ++left;
        continue;
      }
      std::string what;
      if (item.stars > 0)
        what = stashPrinted(t.rating, stashPrinted("%g", item.stars),
                            item.stashRating100 < 0
                                ? std::string(t.none)
                                : stashPrinted("★ %g", item.stashRating100 / 20.0));
      if (item.plays > 0)
        what = stashJoined({what, item.plays == 1 ? t.play : stashPrinted(t.plays, item.plays)});
      m.addLabel(x + 12, y, inner * 0.62 - 12, 34, item.name, kHintSize, false, 1);
      m.addLabel(x + inner * 0.62, y, inner * 0.38, 34, what, kHintSize, true, 1);
      y += 36;
      ++shown;
    }
    if (left > 0) line(stashPrinted(t.more, stashNumber(left)), true, 1);
    y += 8;
  }
  m.addGroup(x0, kPagePad, w, y - kPagePad, t.title);
}
