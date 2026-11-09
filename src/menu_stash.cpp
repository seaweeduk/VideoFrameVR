#include "menu_style.h"

#include "file_util.h"
#include "menu_stash.h"
#include "stash.h"
#include "stash_client.h"

#include <algorithm>
#include <ctime>
#include <thread>

// What the core menu calls for Stash: setting it, the frame's update, and
// the text its keyboard types (menu.h's thin hooks).

const StashCatalog* StashPages::catalog(const Menu& m) {
  return m.stash_ ? m.stash_->catalog().get() : nullptr;
}

// Menu::setStash() (menu_stash_covers.cpp): Stash on, or anew.
void StashPages::attach(Menu& m) {
  if (!m.stashMenu_) m.stashMenu_ = std::make_unique<StashMenu>();
  StashMenu& st = state(m);
  // A new Stash: its catalogs are counted anew, so whatever a page made from
  // the last one's is made again (the choices and positions stay).
  st.catalogGeneration = ~0ull;
  st.libraryGeneration = ~0ull;
  st.stale = true;
  st.fullStale = true;
  st.shownCatalog = st.shownLibrary = ~0ull;
  StashPerformersState& ps = st.performers;
  ps.catalogGeneration = ps.libraryGeneration = ~0ull;
  ps.statsCatalog = ps.statsLibrary = ~0ull;
  ps.stale = true;
  StashSceneTab& d = st.sceneTab;
  d.similarGeneration = d.countGeneration = ~0ull;
  d.shownCatalog = d.shownLibrary = ~0ull;
  staleHome(m);
  staleStudios(m);
  if (!st.pinsLoaded && !st.pendingPins.valid()) {
    auto done = std::make_shared<std::promise<std::vector<StashPin>>>();
    st.pendingPins = done->get_future();
    queueConfigWork([done] { done->set_value(loadStashPins()); });
  }
}

void StashPages::leavePage(Menu& m) {
  if (m.page_ == Menu::Page::StashScenes)
    leaveScenes(m);
  else if (m.page_ == Menu::Page::StashPerformers)
    leavePerformers(m);
  else if (m.page_ == Menu::Page::StashPerformer)
    leavePerformer(m);
  else if (m.page_ == Menu::Page::StashStudios)
    leaveStudios(m);
  else if (m.page_ == Menu::Page::StashStudio)
    leaveStudio(m);
  else if (m.page_ == Menu::Page::StashSceneDetails)
    leaveSceneDetails(m);
  else if (m.page_ == Menu::Page::StashHome)
    leaveHome(m);
}

bool StashPages::showAgain(Menu& m, Menu::Page page) {
  switch (page) {
    case Menu::Page::StashHome: showHome(m); return true;
    case Menu::Page::StashScenes: showScenes(m); return true;
    case Menu::Page::StashPerformers: showPerformers(m); return true;
    case Menu::Page::StashPerformer: showPerformer(m, state(m).performer); return true;
    case Menu::Page::StashStudios: showStudios(m); return true;
    case Menu::Page::StashStudio: showStudio(m, state(m).studio); return true;
    default: return false;
  }
}

void StashPages::layoutPage(Menu& m) {
  switch (m.page_) {
    case Menu::Page::StashHome: layoutHome(m); break;
    case Menu::Page::StashScenes: layoutScenes(m); break;
    case Menu::Page::StashPerformers: layoutPerformers(m); break;
    case Menu::Page::StashPerformer: layoutPerformer(m); break;
    case Menu::Page::StashStudios: layoutStudios(m); break;
    case Menu::Page::StashStudio: layoutStudio(m); break;
    case Menu::Page::StashSceneDetails: layoutSceneDetails(m); break;
    default: break;
  }
}

void StashPages::drawHeading(const Menu& m, cairo_t* cr) {
  switch (m.page_) {
    case Menu::Page::StashHome: drawHomeHeading(m, cr); break;
    case Menu::Page::StashScenes: drawScenesHeading(m, cr); break;
    case Menu::Page::StashPerformers: drawPerformersHeading(m, cr); break;
    case Menu::Page::StashPerformer: drawPerformerHeading(m, cr); break;
    case Menu::Page::StashStudios: drawStudiosHeading(m, cr); break;
    case Menu::Page::StashStudio: drawStudioHeading(m, cr); break;
    case Menu::Page::StashSceneDetails: drawSceneDetailsHeading(m, cr); break;
    default: break;
  }
}

// Each frame: redraws for a new catalog, a sync's state, changed settings
// or the library's changed ratings and plays, where they show.
void StashPages::update(Menu& m) {
  StashMenu& st = state(m);
  if (st.pendingPins.valid() &&
      st.pendingPins.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    st.pins = st.pendingPins.get();
    st.pinsLoaded = true;
    st.stale = st.fullStale = true;
    staleHome(m);
    m.dirty_ = true;
  }
  const StashSettings& s = m.view_.stash;
  const bool scenes = m.stashListPage(), settings = m.page_ == Menu::Page::Settings;
  const bool performer = m.page_ == Menu::Page::StashPerformer;
  if (!st.settingsTaken) {
    st.settingsTaken = true;
    st.list.format = s.format;
    st.list.sort = s.scenesSort;
    st.list.descending = s.scenesDescending;
    st.seed = static_cast<uint64_t>(std::time(nullptr));
    st.performers.sort = s.performersSort;
    st.performers.descending = s.performersDescending;
    st.stale = true;
  }
  if (s.malePerformers != st.settings.malePerformers) st.stale = true;
  if (s.sections != st.settings.sections || s.hiddenSections != st.settings.hiddenSections ||
      s.subgroups != st.settings.subgroups || s.hiddenSubgroups != st.settings.hiddenSubgroups) {
    st.stale = true;
    if (scenes) m.dirty_ = true;
  }
  if (s.malePerformers != st.settings.malePerformers || s.titles != st.settings.titles ||
      s.covers != st.settings.covers) {
    if (scenes) m.dirty_ = true;
  }
  if (s.writeBack != st.settings.writeBack && m.page_ == Menu::Page::StashSceneDetails)
    m.dirty_ = true;  // Edit tags
  st.settings = s;
  const StashStatus& now = m.stash_->status();
  if (now.syncing != st.status.syncing || now.synced != st.status.synced ||
      now.lastSync != st.status.lastSync || now.error != st.status.error ||
      now.scenes != st.status.scenes || now.libraryMatched != st.status.libraryMatched ||
      now.step != st.status.step || now.received != st.status.received) {
    st.status = now;
    // ... and the pages waiting for a first copy say what the sync does.
    if (settings || (m.stashPage() && !catalog(m))) m.dirty_ = true;
  }
  const long minute = static_cast<long>(std::time(nullptr) / 60);  // "synced 2 min ago"
  if (minute != st.minute) {
    st.minute = minute;
    if (settings) m.dirty_ = true;
  }
  if (m.stash_->generation() != st.shownCatalog) {
    st.shownCatalog = m.stash_->generation();
    if (scenes || settings || m.page_ == Menu::Page::StashStudio) m.dirty_ = true;
  }
  if (m.library_ && m.library_->generation() != st.shownLibrary) {
    st.shownLibrary = m.library_->generation();
    if (m.page_ == Menu::Page::StashPerformers || m.page_ == Menu::Page::StashStudios ||
        m.page_ == Menu::Page::StashStudio)
      m.dirty_ = true;  // plays and ratings: the cards, their order, a studio's band and the rail
    else if (performer)  // ... and her history beside her scenes
      m.browserChanged(std::min(m.listTop_, st.performers.profileTop) - 4, m.listBottom_ + 4);
    else if (scenes)
      m.browserChanged(m.listTop_ - 4, m.listBottom_ + 4);  // ratings and plays
  }
  // Write-back's state, shown in the Settings group and the send panel.
  if (st.sendOpen && !settings) {
    st.sendOpen = false;
    m.stash_->closeSend();
  }
  std::string writeBack;
  if (const StashWriteBack* w = m.stash_->writeBack()) {
    const StashWriteBackStatus& a = w->status();
    const StashSendPlan& p = w->sendPlan();
    writeBack = std::to_string(a.waitingRatings) + ' ' + std::to_string(a.waitingPlays) + ' ' +
                std::to_string(a.sentRatings) + ' ' + std::to_string(a.sentPlays) + ' ' + a.error +
                '|' + std::to_string(static_cast<int>(p.state)) + ' ' + p.error;
  }
  if (writeBack != st.shownWriteBack) {
    st.shownWriteBack = writeBack;
    if (settings) m.dirty_ = true;
  }
  updateSceneDetails(m);
  // Her portrait and her co-stars' beside the list, as they arrive.
  if (performer && st.performers.waiting && m.stash_->imagesArrived())
    m.browserChanged(st.performers.profileTop - 4, st.performers.profileBottom + 4);
  // A studio's logo above its scenes, once it is there.
  if (m.page_ == Menu::Page::StashStudio && st.bandPending && m.stash_->imagesArrived()) {
    st.bandPending = false;
    m.browserChanged(0, m.listTop_);
  }
}

const std::string& Menu::stashText() const {
  static const std::string none;
  if (!stashMenu_) return none;
  const StashMenu& st = *stashMenu_;
  switch (st.typing) {
    case StashMenu::Typing::Search:
      return st.list.text;
    case StashMenu::Typing::FullSearch:
      return st.fullSearch;
    case StashMenu::Typing::PinName:
      return st.renaming >= 0 && st.renaming < static_cast<int>(st.pins.size())
                 ? st.pins[st.renaming].name
                 : none;
    case StashMenu::Typing::Server:
      return st.draft[st.field];
    case StashMenu::Typing::Performers:
      return st.performers.text;
    case StashMenu::Typing::Studios:
      return st.studioSearch;
    case StashMenu::Typing::Tags:
      return st.sceneTab.search;
  }
  return none;
}

void StashPages::setText(Menu& m, const std::string& text) {
  StashMenu& st = state(m);
  switch (st.typing) {
    case StashMenu::Typing::Search:
      if (text == st.list.text) return;
      st.list.text = text;
      st.stale = true;
      if (st.full == StashFull::None) {
        m.scrollOffset_ = 0;
        m.scrollFraction_ = 0;
      }
      break;
    case StashMenu::Typing::FullSearch:
      st.fullSearch = text;
      st.fullStale = true;
      m.scrollOffset_ = 0;
      m.scrollFraction_ = 0;
      break;
    case StashMenu::Typing::PinName:
      if (st.renaming >= 0 && st.renaming < static_cast<int>(st.pins.size()))
        st.pins[st.renaming].name = text;
      break;
    case StashMenu::Typing::Server: {
      const bool unverified = draftUnverified(m);
      st.draft[st.field] = text;
      m.typedChanged();
      if (draftUnverified(m) != unverified) m.dirty_ = true;  // Verify certificate comes or goes
      return;
    }
    case StashMenu::Typing::Performers:
      if (text == st.performers.text) return;
      st.performers.text = text;
      st.performers.stale = true;
      m.scrollOffset_ = 0;
      m.scrollFraction_ = 0;
      break;
    case StashMenu::Typing::Studios:
      if (text == st.studioSearch) return;
      st.studioSearch = text;
      m.scrollOffset_ = 0;
      m.scrollFraction_ = 0;
      break;
    case StashMenu::Typing::Tags:
      if (text == st.sceneTab.search) return;
      st.sceneTab.search = text;
      m.side_.position = 0;
      break;
  }
  m.dirty_ = true;
}

// The server's fields as SetStashServer takes them: the URL, the API key and
// the folders, one per line (not the user name and password); the editor
// closes.
MenuCommand StashPages::serverCommand(Menu& m) {
  StashMenu& st = state(m);
  MenuCommand c;
  c.type = MenuCommand::SetStashServer;
  // The mapping edited in place, removed when its Stash folder is emptied,
  // or added.
  std::vector<StashFolderMap> folders = m.view_.stash.folders;
  const StashFolderMap edited{st.draft[2], st.draft[3]};
  if (st.draftFolder >= 0 && st.draftFolder < static_cast<int>(folders.size())) {
    if (edited.from.empty())
      folders.erase(folders.begin() + st.draftFolder);
    else
      folders[st.draftFolder] = edited;
  } else if (!edited.from.empty()) {
    folders.push_back(edited);
  }
  c.path = st.draft[0] + "\n" + st.draft[1];
  for (const StashFolderMap& f : folders) c.path += "\n" + f.from + "\n" + f.to;
  c.option = draftUnverified(m) ? 1 : 0;
  st.draft[kStashPassword].clear();
  st.signInError.clear();
  m.editing_ = false;
  m.dirty_ = true;
  return c;
}

bool StashPages::draftUnverified(Menu& m) {
  const StashMenu& st = state(m);
  return !st.draftUnverified.empty() && !st.draft[0].empty() &&
         stashHost(st.draft[0]) == st.draftUnverified;
}

// Enter (or Done): the search keeps its words, a pinned list its new name,
// the server's fields are saved (from the user name, on to the password).
MenuCommand StashPages::submitText(Menu& m) {
  StashMenu& st = state(m);
  if (m.page_ == Menu::Page::Settings) {
    if (st.field == kStashUser) {
      st.field = kStashPassword;
      m.dirty_ = true;
      return {};
    }
    return saveServer(m);
  }
  stopEditing(m);
  return {};
}

// Save and sync: a new address is looked for as setup's Connect does
// (https, http, Stash's port) and saved as it answered, else as typed; with
// a password typed, Stash is signed in to there and its key saved with the
// rest (updateSignIn()). A certificate that isn't trusted saves nothing: a
// dialog says so, and offers to connect without verifying it. Verifying it
// again is looked for the same way.
MenuCommand StashPages::saveServer(Menu& m) {
  StashMenu& st = state(m);
  if (st.signingIn.valid()) return {};
  const bool unverified = draftUnverified(m);
  const bool moved = st.draft[0] != m.view_.stash.url || unverified != m.view_.stash.unverified();
  if (st.draft[kStashPassword].empty() && !moved) return serverCommand(m);
  auto done = std::make_shared<std::promise<StashMenu::SignIn>>();
  st.signingIn = done->get_future();
  st.signInError.clear();
  // Its own thread, as the SMB sign-in: a server that doesn't answer
  // mustn't hold up anything else, and Cancel just drops the result.
  std::thread([done, moved, unverified, url = st.draft[0], user = st.draft[kStashUser],
               password = st.draft[kStashPassword]] {
    StashMenu::SignIn r;
    r.host = stashHost(url);
    if (moved) {
      StashClient found(url, std::string(), unverified);
      const StashClient::Answer a = found.check();
      if (a == StashClient::Answer::Open || a == StashClient::Answer::NeedsSignIn) r.url = found.server();
      r.untrusted = a == StashClient::Answer::Failed && found.problem() == StashProblem::Certificate;
    }
    r.ok = !r.untrusted;
    if (r.ok && !password.empty()) {
      StashClient client(r.url.empty() ? url : r.url, std::string(), unverified);
      r.ok = client.signIn(user, password, r.apiKey, r.refused);
      r.error = client.error();
      r.untrusted = !r.ok && client.problem() == StashProblem::Certificate;
    }
    done->set_value(std::move(r));
  }).detach();
  m.dirty_ = true;
  return {};
}

// A sign-in finished: its key is saved with the other fields, or the editor
// says why not.
void StashPages::updateSignIn(Menu& m) {
  StashMenu& st = state(m);
  if (!st.signingIn.valid() ||
      st.signingIn.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    return;
  const StashMenu::SignIn r = st.signingIn.get();
  m.dirty_ = true;
  if (!r.url.empty()) st.draft[0] = r.url;
  if (r.untrusted) {
    m.openDialog(Menu::Dialog::StashCertificate, r.host);
    return;
  }
  if (!r.ok) {
    st.signInError = r.refused ? strings().stashSetup.wrongLogin : r.error;
    return;
  }
  // Signed in: empty, Stash has no login and a key would be refused.
  if (!st.draft[kStashPassword].empty()) st.draft[1] = r.apiKey;
  m.command_ = serverCommand(m);
}

// Escape (or Hide): the same, but the server's fields are left as they were.
void StashPages::stopEditing(Menu& m) {
  StashMenu& st = state(m);
  if (st.typing == StashMenu::Typing::Server) {
    st.signingIn = {};
    st.draft[kStashPassword].clear();
    st.signInError.clear();
  }
  if (st.typing == StashMenu::Typing::PinName && st.renaming >= 0) {
    if (st.renaming < static_cast<int>(st.pins.size()) && st.pins[st.renaming].name.empty())
      st.pins[st.renaming].name = stashListName(st.pins[st.renaming].list, catalog(m));
    saveStashPins(st.pins);
    st.renaming = -1;
  }
  m.editing_ = false;
  m.dirty_ = true;
}
