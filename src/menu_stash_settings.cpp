#include "menu_style.h"

#include "menu_stash.h"
#include "stash.h"

// Stash in the navigation column and on the Settings page: its section
// (Home, Scenes, Performers, Studios and the pinned lists), its settings
// group, and the server's fields typed in with the keyboard.

std::string stashAgo(int64_t when) {
  const StashStrings& t = strings().stash;
  const long seconds = static_cast<long>(std::time(nullptr) - when);
  if (seconds < 60) return t.justNow;
  if (seconds < 3600) return stashPrinted(t.minutesAgo, static_cast<int>(seconds / 60));
  if (seconds < 86400) return stashPrinted(t.hoursAgo, static_cast<int>(seconds / 3600));
  return stashPrinted(t.daysAgo, static_cast<int>(seconds / 86400));
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

std::string stashJoined(const std::vector<std::string>& parts, const char* between) {
  std::string out;
  for (const std::string& p : parts)
    if (!p.empty()) out += (out.empty() ? "" : between) + p;
  return out;
}

// Under Library: Stash, its pages, then the pinned lists as far
// as there is room, the last row then opening all of them.
void StashPages::layoutNavigation(Menu& m, double x, double w, double y, double bottom) {
  StashMenu& st = state(m);
  const StashStrings& t = strings().stash;
  const bool scenes = m.page_ == Menu::Page::StashScenes;
  const int pin = scenes && st.full == StashFull::None ? pinOfList(m) : -1;
  auto row = [&](const std::string& label, StashIcon icon, bool chosen, double top, double h,
                 int size, double indent, std::function<void()> open) {
    Button b;
    b.panel = Menu::kBrowser;
    b.label = label;
    b.nav = true;
    b.on = chosen;
    b.x = x + indent;
    b.y = top;
    b.w = w - indent;
    b.h = h;
    b.fontSize = size;
    b.radius = 14;
    b.stashLook = static_cast<int>(StashLook::Nav);
    b.stashIcon = static_cast<int>(icon);
    b.action = [open] {
      open();
      return MenuCommand{};
    };
    if (m.textWidth(label, size, false) > w - indent - 110) b.tooltip = label;
    m.buttons_.push_back(b);
  };
  auto place = [&](const std::string& label, StashIcon icon, bool chosen,
                   std::function<void()> open) {
    row(label, icon, chosen, y, kPlaceRowH, Menu::kTextSize, 24, std::move(open));
    y += kPlaceRowH + 2;
  };
  const Menu::Page page = m.page_;
  row(t.group, StashIcon::Stash, false, y, kNavRowH, 27, 0, [&m] { showHome(m); });
  y += kNavRowH + 4;
  place(t.navHome, StashIcon::Home, page == Menu::Page::StashHome, [&m] { showHome(m); });
  place(t.navScenes, StashIcon::Scenes, scenes && pin < 0 && st.full != StashFull::Pins,
        [&m] { showScenes(m); });
  place(t.navPerformers, StashIcon::Performers,
        page == Menu::Page::StashPerformers || page == Menu::Page::StashPerformer,
        [&m] { showPerformers(m); });
  place(t.navStudios, StashIcon::Studios,
        page == Menu::Page::StashStudios || page == Menu::Page::StashStudio,
        [&m] { showStudios(m); });
  const int fit = std::max(0, static_cast<int>((bottom - y) / (kPlaceRowH + 2)));
  const int count = static_cast<int>(st.pins.size());
  const int shown = count <= fit ? count : std::max(0, fit - 1);
  for (int i = 0; i < shown; ++i)
    place(st.pins[i].name, StashIcon::Pin, pin == i, [&m, i] { openPin(m, i); });
  if (shown < count && fit > 0)
    place(stashPrinted(t.allPins, count), StashIcon::Pin, scenes && st.full == StashFull::Pins,
          [&m] { showFull(m, StashFull::Pins); });
}

// After first-run setup's Not now, with Stash not set up: one "Use Stash?"
// card above Settings, its box ending at `bottom`. Connect Stash (or its
// title) turns Stash on and opens Settings → Stash at the server's
// address, the keyboard up; × hides it. Either way it is gone for good.
void StashPages::layoutCard(Menu& m, double x, double w, double bottom) {
  const SetupStrings& t = strings().setup;
  constexpr double h = 124;
  const double y = bottom - h;
  m.stashCardY_ = y;
  auto connect = [&m] {
    if (!m.stashMenu_) attach(m);
    m.showPage(Menu::Page::Settings);
    editServer(m, 0);
    return command(MenuCommand::StashCard, 1)();
  };
  Button title;
  title.panel = Menu::kBrowser;
  title.label = t.useStash;
  title.nav = true;
  title.x = x;
  title.y = y + 6;
  title.w = w - kPlaceRowH - 8;
  title.h = kPlaceRowH;
  title.fontSize = Menu::kTextSize;
  title.radius = 14;
  title.stashLook = static_cast<int>(StashLook::Nav);
  title.stashIcon = static_cast<int>(StashIcon::Stash);
  title.action = connect;
  m.buttons_.push_back(title);
  Button& close = m.addRound(Menu::Icon::Close, t.dontAsk, command(MenuCommand::StashCard, 0),
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

// Opens the server's fields with the keyboard, at `field` (the API key's
// opens at the user name while there is no key: signing in gets it).
void StashPages::editServer(Menu& m, int field, const std::string& folder) {
  StashMenu& st = state(m);
  const StashSettings& now = m.view_.stash;
  st.draftFolder = now.folders.empty() ? -1 : 0;
  if (!folder.empty()) {
    const StashFolderMap* map = now.mappingFor(folder);
    st.draftFolder = map ? static_cast<int>(map - now.folders.data()) : -1;
  }
  const StashFolderMap draft = st.draftFolder >= 0 ? now.folders[st.draftFolder]
                                                   : StashFolderMap{folder, ""};
  st.draft = {now.url, now.apiKey, draft.from, draft.to, "", ""};
  st.draftUnverified = now.unverified() ? now.unverifiedHost : std::string();
  st.showPassword = false;
  st.signingIn = {};
  st.signInError.clear();
  st.field = field == 1 && now.apiKey.empty() ? kStashUser : field;
  st.typing = StashMenu::Typing::Server;
  m.editing_ = true;
  m.pageScroll_ = 0;
}

// The Settings page's Stash group, atop the third column: the switch; while
// on, the server (tapped, its fields open with the keyboard), the two
// display switches and the male performers', what the last sync found, and
// Sync now.
void StashPages::layoutSettingsGroup(Menu& m, double top) {
  const StashStrings& t = strings().stash;
  const StashSettings& s = m.view_.stash;
  const double x = columnX(2) + kGroupPad, w = kColumnW - 2 * kGroupPad;
  double y = firstRow(top);
  m.addToggleRow(x, y, w, t.use, s.enabled, command(MenuCommand::ToggleStash)).tooltip = t.tipUse;
  y += kSettingStep;
  if (!s.enabled) {
    m.addLabel(x + 12, y, w - 12, 0, t.offHint, kHintSize, true, 2);
    y += hintHeight(2) + 6;
    m.addGroup(columnX(2), top, kColumnW, y - top, t.group);
    return;
  }
  if (!m.stashMenu_) attach(m);
  constexpr double labelW = 130;
  auto field = [&](const char* label, const std::string& value, int which, double right = 0) {
    m.addLabel(x + 12, y, labelW - 12, kSettingRowH, label);
    Button b = m.textButton(value, Menu::Icon::None, [&m, which] {
      editServer(m, which);
      return MenuCommand{};
    });
    b.panel = Menu::kBrowser;
    b.x = x + labelW;
    b.y = y;
    b.w = w - labelW - right;
    b.h = kSettingRowH;
    b.field = true;
    b.radius = 10;
    b.fontSize = 24;
    b.stashLook = static_cast<int>(StashLook::Field);
    b.tooltip = which == 1 ? t.tipFields[1] : value + "\n" + t.tipFields[which];
    m.buttons_.push_back(b);
    y += kSettingStep;
  };
  field(t.server, s.url, 0);
  field(t.apiKey, s.apiKey.empty() ? t.apiKeyNone : t.apiKeySet, 1);
  y = layoutFolders(m, x, y, w, labelW);
  if (!m.stash_) {
    m.addLabel(x + 12, y, w - 12, 0, t.setupHint, kHintSize, true, 2);
    y += hintHeight(2) + 6;
    m.addGroup(columnX(2), top, kColumnW, y - top, t.group);
    return;
  }
  m.addToggleRow(x, y, w, t.covers, s.covers, command(MenuCommand::ToggleStashCovers));
  y += kSettingStep;
  m.addToggleRow(x, y, w, t.titles, s.titles, command(MenuCommand::ToggleStashTitles));
  y += kSettingStep;
  m.addToggleRow(x, y, w, t.malePerformers, s.malePerformers,
                 command(MenuCommand::ToggleStashMalePerformers));
  y += kSettingStep;

  // Write-back: the switch, the backup it needs first, and while on what
  // waits or was sent, and the one-off send.
  const StashWriteBackStrings& wb = strings().stashWriteBack;
  m.addToggleRow(x, y, w, wb.toggle, s.writeBack, command(MenuCommand::ToggleStashWriteBack))
      .tooltip = wb.tipToggle;
  y += kSettingStep;
  m.addLabel(x + 12, y, w - 12, 0, wb.hint, kHintSize, true, 5);
  y += hintHeight(5) + 6;
  if (s.writeBack) {
    std::string line;
    bool ok = true;
    if (const StashWriteBack* writeBack = m.stash_->writeBack()) {
      const StashWriteBackStatus& a = writeBack->status();
      if (a.waitingRatings || a.waitingPlays)
        line = stashPrinted(wb.waiting, a.waitingRatings, a.waitingPlays);
      else if (a.sentRatings || a.sentPlays)
        line = stashPrinted(wb.sent, a.sentRatings, a.sentPlays);
      if (!a.error.empty()) {
        line += (line.empty() ? "" : " · ") + a.error;
        ok = false;
      }
    }
    if (!line.empty()) {
      m.addLabel(x + 12, y, w - 12, 0, line, kHintSize, ok, 2);
      y += hintHeight(2);
    }
    Button send = m.textButton(wb.sendAll, Menu::Icon::None, [&m] {
      StashMenu& st = state(m);
      st.sendOpen = true;
      st.sendList = false;
      m.pageScroll_ = 0;
      m.stash_->planSend();
      return MenuCommand{};
    });
    send.panel = Menu::kBrowser;
    send.x = x + w - send.w;
    send.y = y + 4;
    send.h = kSettingRowH;
    send.fontSize = Menu::kTextSize;
    send.tooltip = wb.tipSendAll;
    m.buttons_.push_back(send);
    y += kSettingRowH + 12;
  }

  // What the copy in use holds, and how the last sync went.
  const StashStatus& status = m.stash_->status();
  const StashCatalog* c = catalog(m);
  std::string lines;
  if (c) {
    const StashCatalogStats& stats = c->stats();
    lines = stats.streamed > 0
                ? stashPrinted(t.statusStreamed, stashNumber(stats.stashScenes),
                               stashNumber(stats.listed), stashNumber(stats.streamed),
                               stashNumber(stats.libraryMatched), stashNumber(stats.libraryVideos))
                : stashPrinted(t.status, stashNumber(stats.stashScenes), stashNumber(stats.listed),
                               stashNumber(stats.libraryMatched), stashNumber(stats.libraryVideos));
  }
  std::string state;
  if (status.syncing) {
    state = stashSyncStep(status);
  } else if (!status.error.empty()) {
    state = status.error;
  } else if (status.lastSync > 0 || (c && c->syncedAt() > 0)) {
    state = stashPrinted(t.synced, stashAgo(status.lastSync > 0 ? status.lastSync : c->syncedAt()),
                         c ? c->version() : status.version);
  } else {
    state = c ? t.notSynced : t.loadingCopy;
  }
  if (!lines.empty()) {
    m.addLabel(x + 12, y, w - 12, 0, lines, kHintSize, true, 2);
    y += hintHeight(2);
  }
  Button sync = m.textButton(t.syncNow, Menu::Icon::None, command(MenuCommand::SyncStash));
  sync.panel = Menu::kBrowser;
  sync.x = x + w - sync.w;
  sync.y = y + 4;
  sync.h = kSettingRowH;
  sync.fontSize = Menu::kTextSize;
  sync.disabled = status.syncing;
  sync.tooltip = t.tipSync;
  m.buttons_.push_back(sync);
  // Its certificate isn't trusted: in place of the reason, a warning that
  // says so in a few words (its tooltip the rest) and asks whether to
  // connect anyway.
  if (status.problem == StashProblem::Certificate && !status.syncing) {
    const StashSetupStrings& ts = strings().stashSetup;
    Button warning = m.textButton(ts.untrustedStatus, Menu::Icon::None, [&m] {
      m.openDialog(Menu::Dialog::StashCertificate, stashHost(m.view_.stash.url));
      return MenuCommand{};
    });
    warning.panel = Menu::kBrowser;
    warning.stashLook = static_cast<int>(StashLook::Warning);
    warning.x = x;
    warning.y = y + 4;
    warning.w = std::min(m.textWidth(ts.untrustedStatus, Menu::kTextSize, false) + 82, sync.x - kGap - x);
    warning.h = kSettingRowH;
    warning.fontSize = Menu::kTextSize;
    warning.tooltip = ts.untrustedTip;
    m.buttons_.push_back(warning);
    y += kSettingRowH + 18;
    m.addGroup(columnX(2), top, kColumnW, y - top, t.group);
    return;
  }
  double stateW = w - sync.w - 28;
  // Stash has a login the app hasn't signed in to: Sign in, beside it.
  if (!status.syncing && (status.problem == StashProblem::NeedsSignIn ||
                          status.problem == StashProblem::KeyRefused)) {
    Button signIn = m.textButton(strings().network.signIn, Menu::Icon::None, [&m] {
      editServer(m, kStashUser);
      return MenuCommand{};
    }, true);
    signIn.panel = Menu::kBrowser;
    signIn.x = sync.x - kGap - signIn.w;
    signIn.y = y + 4;
    signIn.h = kSettingRowH;
    signIn.fontSize = Menu::kTextSize;
    m.buttons_.push_back(signIn);
    stateW -= signIn.w + kGap;
  }
  // Some reasons take a few lines (what to change in Stash).
  const int stateLines =
      status.error.empty()
          ? 2
          : std::clamp(static_cast<int>(m.textWidth(state, kHintSize, false) / stateW) + 2, 2, 6);
  m.addLabel(x + 12, y + 4, stateW, 0, state, kHintSize, status.error.empty(), stateLines);
  y += std::max(kSettingRowH + 8, hintHeight(stateLines)) + 10;
  if (!c && status.syncing) {
    m.addLabel(x + 12, y, w - 12, 0, strings().stashSetup.firstSync, kHintSize, true, 3);
    y += hintHeight(3) + 6;
  }
  m.addGroup(columnX(2), top, kColumnW, y - top, t.group);
}

// The server's fields, the one typed into outlined blue: the server, the
// user name and password to sign in with, the API key, and the folders.
// Save and sync keeps them (and syncs), signing in first with a password
// typed; Cancel or Hide leaves them as they were. The keyboard takes the
// bottom of the page.
void StashPages::layoutServerEditor(Menu& m) {
  StashMenu& st = state(m);
  const StashStrings& t = strings().stash;
  const NetworkStrings& tn = strings().network;
  const double x0 = columnX(0), w = 3 * kColumnW + 2 * kGap;
  const double x = x0 + kGroupPad, inner = w - 2 * kGroupPad;
  double y = firstRow(kPagePad);
  constexpr double labelW = 380, h = 56, step = 66, eyeW = 120;
  const bool signing = st.signingIn.valid();
  for (const int i : {0, kStashUser, kStashPassword, 1, 2, 3}) {
    const bool password = i == kStashPassword;
    m.addLabel(x + 12, y, labelW - 12, h, i < 4 ? t.fieldNames[i] : tn.fields[i - 3]);
    const bool active = st.field == i;
    std::string shown = st.draft[i];
    if (password && !st.showPassword) {
      shown.clear();
      for (const char c : st.draft[i])
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) shown += "•";
    }
    // The server's address, before anything is typed: an example.
    const bool hint = i == 0 && shown.empty() && !active;
    Button b = m.textButton(hint ? std::string(strings().stashSetup.serverHint)
                                 : shown,
                            Menu::Icon::None, [&m, i] {
                              state(m).field = i;
                              return MenuCommand{};
                            });
    b.placeholder = hint;
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
    if (i < 4) b.tooltip = t.tipFields[i];
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
  m.addLabel(x + 12, y, inner - 12, 0, strings().stashSetup.signInHint, kHintSize, true, 2);
  y += hintHeight(2);
  m.addLabel(x + 12, y, inner - 12, 0, t.editHint, kHintSize, true, 1);
  y += hintHeight(1);
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
    const StashSetupStrings& ts = strings().stashSetup;
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

void StashPages::drawCertificateDialog(Menu& m, cairo_t* cr, double textX, double boxY) {
  const StashSetupStrings& t = strings().stashSetup;
  const double textW = kDialogW - 100, dim[4] = {0.75, 0.75, 0.8, 1}, red[4] = {1.0, 0.45, 0.4, 1};
  const std::string title = stashPrinted(t.untrustedTitle, m.dialogPath_);
  const double titleW = std::min(m.textWidth(title, 38, true), textW - 60);
  const double iconX = Menu::kPageCenterX - (titleW + 60) / 2;
  Menu::drawIcon(cr, Menu::Icon::Warning, iconX + 22, boxY + 60, 44, red[0], red[1], red[2]);
  m.drawText(cr, title, iconX + 60, boxY + 30, titleW + 4, 60, 38, true, false, 1, 1, 1);
  m.drawTextLines(cr, t.untrusted, textX, boxY + 110, textW, 26, 3, dim, true);
  const int lines = m.wrappedLines(t.untrusted, textW, 26, 3);
  m.drawTextLines(cr, t.untrustedRisk, textX, boxY + 130 + lines * 26 * 1.36, textW, 26, 2, red, true, 0, true);
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
      line(t.rules, true, 2);
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
