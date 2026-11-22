#include "menu_stash.h"
#include "menu_style.h"
#include "stash.h"
#include "stash_catalog.h"

#include <algorithm>
#include <cstdlib>

// Where Stash's videos are here, so that nobody has to type the folder
// mappings: Stash looks for each of its library folders once per sync while
// Settings shows, and the Settings group offers what it found, a row per
// folder; after a server is first connected, a folder found in one place
// only is used without asking. Choose… opens the file browser to pick a
// folder's place here, which is looked in at once, or to stop using one.

namespace {

// Both give the same place here for each of the files.
bool sameMapping(const StashSettings& s, const StashFolderFinder::Mapping& m,
                 const std::vector<StashFolderFinder::File>& files) {
  StashSettings other;
  other.folders = {{m.pathFrom, m.pathTo}};
  for (const StashFolderFinder::File& f : files)
    if (s.localPath(f.path) != other.localPath(f.path)) return false;
  return true;
}

// The mapping a row of Stash's folders goes by: its own; for scenes in none
// of Stash's folders (or a copy without them), its first file's, else the
// first.
const StashFolderMap* mappingOf(const StashSettings& s, const std::string& folder,
                                const std::vector<StashFolderFinder::File>& samples = {}) {
  if (!folder.empty()) return s.mappingFor(folder);
  if (!samples.empty())
    if (const StashFolderMap* map = s.mappingFor(samples.front().path)) return map;
  return s.folders.empty() ? nullptr : &s.folders.front();
}

}  // namespace

void Menu::findStashFolders(bool on, std::vector<std::string> places) {
  StashFolders& f = stashFolders_;
  f.automatic = on;
  f.places = std::move(places);
}

void Menu::updateStashFolders() {
  StashFolders& f = stashFolders_;
  if (!stash_) return;
  // Not while a folder is chosen: its check would be replaced.
  if ((page_ == Page::Settings || f.automatic) && !f.picking) stash_->lookForFolders(f.places);
  // A server newly connected (in setup, or saved in Settings): what its
  // first search found is saved as it is, once setup has ended (saving
  // starts Stash anew). Only that one: later searches offer what they find.
  if (f.automatic && !setup_ && !f.picking && stash_->foldersLookedThrough() &&
      !stash_->folderFinder().running()) {
    f.automatic = false;
    const std::vector<StashFolderMap> found = stash_->foundFolders();
    if (found != view_.stash->folders) command_ = stashFolders(found);
  }
  if (stash_->folderSearches() == f.searches) return;
  f.searches = stash_->folderSearches();
  if (f.checking && stash_->folderCheck()) {
    f.checking = false;
    const auto& found = stash_->folderFinder().foundFor(f.folder);
    if (!found.empty()) {
      command_ = stashFolders(stash_->foldersWith(f.folder, {found.front().pathFrom,
                                                             found.front().pathTo}));
      f.picking = false;
      showStashSettings();
    } else {
      f.notThere = true;
    }
  }
  dirty_ = true;
}

MenuCommand Menu::stashFolders(const std::vector<StashFolderMap>& folders) const {
  MenuCommand c;
  c.type = MenuCommand::SetStashFolders;
  for (const StashFolderMap& m : folders) c.path += m.from + "\n" + m.to + "\n";
  return c;
}

// The browser, where Stash's folder is meant to be, else where it was.
void Menu::chooseStashFolder(const std::string& folder) {
  stashFolders_.automatic = false;  // chosen by hand from here on
  stashFolders_.picking = true;
  stashFolders_.notThere = false;
  stashFolders_.folder = folder;
  std::vector<std::string> fallbacks;
  if (!directory().empty()) fallbacks.push_back(directory());
  fallbacks.push_back(std::getenv("HOME") ? std::getenv("HOME") : "/");
  const StashFolderMap* map = mappingOf(*view_.stash, folder);
  std::string dir = map ? (folder.empty() ? map->to : stashMapped(*map, folder)) : std::string();
  if (dir.empty() || dir.find('/') == std::string::npos) {
    dir = fallbacks.front();
    fallbacks.erase(fallbacks.begin());
  }
  files_.kind = FileKind::Video;
  openDirectory(dir, fallbacks);
  showPage(Page::Files);
}

// The browser's heading while choosing: Cancel, Play from Stash (while the
// folder has a place here: no more) and Use this folder where the pin is;
// `right` is where the location must end.
double Menu::layoutStashFolderChoice() {
  const StashFoldersStrings& t = text().stashFolders;
  double right = kPageX + kPageWidth - kPagePad;
  auto add = [&](const char* label, bool primary, std::function<MenuCommand()> action) {
    Button b = textButton(label, Icon::None, std::move(action), primary);
    b.panel = kBrowser;
    b.x = right - b.w;
    b.y = kBrowserRowY;
    b.h = kBrowserRowH;
    b.fontSize = kTextSize;
    right = b.x - kGap;
    buttons_.push_back(b);
  };
  const bool checking = stashFolders_.checking;
  add(checking ? t.checking : t.useFolder, true, [this] {
    if (!stash_ || !StashPages::catalog(*this)) return MenuCommand{};
    stashFolders_.checking = true;
    stashFolders_.notThere = false;
    stash_->lookForFolder(stashFolders_.folder, directory());
    dirty_ = true;
    return MenuCommand{};
  });
  buttons_.back().disabled = checking || page_ != Page::Files;
  if (stash_ && stash_->folderMapping(stashFolders_.folder))
    add(t.playFromServer, false, [this] {
      const MenuCommand c = stash_ ? stashFolders(stash_->foldersWithout(stashFolders_.folder))
                                   : MenuCommand{};
      showStashSettings();
      return c;
    });
  add(strings().cancel, false, [this] {
    showStashSettings();  // which stops choosing
    return MenuCommand{};
  });
  return right - 16;
}

// The Folders rows, once a sync has listed Stash's library folders: one per
// folder with scenes, named above its field when there are several; the
// field says where its videos play from here, or that they play from Stash,
// with Change… or Choose…; under it what isn't as chosen, and the other
// places its videos were found in, with Use.
void StashPages::shownFolders(Menu& m, StashSettingsShown& shown) {
  const StashFoldersStrings& tf = strings().stashFolders;
  const StashSettings& s = *m.view_.stash;
  const StashCatalog* c = catalog(m);
  if (!c) return;
  const std::vector<StashCatalogFolder>& rows = c->folders();
  const bool several = rows.size() > 1;
  const StashFolderFinder& finder = m.stash_->folderFinder();
  const bool looking = finder.running() && !m.stashFolders_.checking;
  for (const StashCatalogFolder& row : rows) {
    const std::string& folder = row.folder;
    const StashFolderMap* map = mappingOf(s, folder, row.samples);
    std::string from = folder, here = folder.empty() ? "" : s.localPath(folder);
    if (map && folder.empty()) {
      here = map->to;
      from = map->from;
    }
    const bool foundHere = row.listed * 2 >= row.scenes;
    const bool streamed = row.streamed > 0;
    const std::string shownHere = here.empty() ? "" : m.shownPlace(here);
    StashSettingsShown::Folder& f = shown.folders.emplace_back();
    f.folder = folder;
    if (several) f.name = folder.empty() ? tf.otherFiles : from;
    if (foundHere) f.here = shownHere;
    if (streamed) f.fromServer = tf.fromServer;
    f.tooltip = (from.empty() ? "" : from + "\n") + (f.here.empty() ? "" : f.here + "\n") +
                tf.tipChoose;

    // Places, not mappings: /a → /b and /a/c → /b/c are the same one.
    std::vector<const StashFolderFinder::Mapping*> places;
    const int half = static_cast<int>(row.samples.size() + 1) / 2;
    const std::vector<StashFolderFinder::Mapping> none;
    for (const StashFolderFinder::Mapping& found : !looking ? finder.foundFor(folder) : none) {
      if (found.found < half || sameMapping(s, found, row.samples)) continue;
      bool known = false;
      for (const StashFolderFinder::Mapping* p : places) {
        StashSettings other;
        other.folders = {{p->pathFrom, p->pathTo}};
        known |= sameMapping(other, found, row.samples);
      }
      // One for each of several folders, so the rows fit the page (Choose…
      // reaches any other).
      if (!known && places.size() < (several ? 1 : 3)) places.push_back(&found);
    }
    // What isn't as its field says: some of its scenes play from Stash;
    // its videos aren't where they were chosen to be; or they are nowhere,
    // and Stash can't play them either.
    if (foundHere) {
      if (streamed) f.lines.push_back(tf.folderRest);
    } else if (map && !shownHere.empty()) {
      f.lines.push_back(stashPrinted(tf.notIn, shownHere.c_str()));
    } else if (!streamed && places.empty() && !looking) {
      f.lines.push_back(tf.folderNotHere);
    }
    for (const StashFolderFinder::Mapping* p : places) {
      const bool first = p == places.front() && !foundHere;
      const std::string place = m.shownPlace(stashMapped({p->pathFrom, p->pathTo},
                                                         folder.empty() ? p->pathFrom : folder));
      f.uses.push_back({stashPrinted(first ? tf.foundIn : tf.alsoIn, place.c_str()),
                        stashPrinted(tf.tipUse, place.c_str()), folder,
                        {p->pathFrom, p->pathTo}, first});
    }
  }
  if (looking && !shown.folders.empty()) {
    const int jobs = finder.jobs();
    shown.looking = jobs > 1 ? stashPrinted(tf.lookingFolders, finder.done() + 1, jobs)
                             : std::string(tf.looking);
  }
}

// Stash's scenes are there but none is listed (none here, none its stream
// can play): why, and Choose… for the
// first folder; true if shown, `y` then below it.
bool StashPages::layoutNotHere(Menu& m, double x, double& y, double w) {
  const StashCatalog* c = catalog(m);
  if (!c || c->stats().stashScenes == 0 || !c->scenes().empty()) return false;
  const StashFoldersStrings& tf = strings().stashFolders;
  const std::string folder = c->folders().empty() ? std::string() : c->folders().front().folder;
  m.addLabel(x, y, w, 0, tf.notFound, 27, true, 2);
  y += 2 * 40 + 8;
  Button choose = m.textButton(tf.choose, Menu::Icon::None, [&m, folder] {
    m.chooseStashFolder(folder);
    return MenuCommand{};
  }, true);
  choose.panel = Menu::kBrowser;
  choose.x = x;
  choose.y = y;
  choose.h = 56;
  choose.fontSize = Menu::kTextSize;
  choose.tooltip = tf.tipChoose;
  m.buttons_.push_back(choose);
  y += 56 + 16;
  return true;
}
