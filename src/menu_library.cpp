#include "menu_style.h"

#include "menu_list_parts.h"

#include "file_util.h"
#include "menu_stash.h"  // stashAgo, stashNumber, stashPrinted
#include "menu_stash_covers.h"
#include "stash.h"
#include "stash_stream.h"

#include <filesystem>
#include <unordered_set>

namespace fs = std::filesystem;

// The library view: every video in the pinned folders and their subfolders,
// played or not, with the same list and gallery, live search and sorting as
// a folder; it starts on Date added, newest first. It is made from the
// library index's memory (library_index.h), so it opens at once. What a
// look finds comes in batches as it goes, the row pointed at (else the top
// one) staying where it is. While Stash is on, its scenes are in it too,
// once each: one with a file in the pins is that file's row.

bool Menu::noPins() const {
  return navigation_.places.ready() && std::none_of(navigation_.places.places().begin(), navigation_.places.places().end(),
                                         [](const Places::Place& p) { return p.pinned; });
}

void Menu::showLibrary() {
  loadLibrary();
  showPage(Page::Library);
  scrollToPlace(libraryPage_.place, libraryPaths(), gallery_ ? kGalleryColumns : 1);
}

std::vector<std::string> Menu::libraryPaths() {
  std::vector<std::string> paths;
  for (const ListItem& item : libraryListItems()) paths.push_back(item.thumbPath);
  return paths;
}

Menu::LibraryItem Menu::libraryItem(const std::string& path, const LibraryIndex::File& file) const {
  LibraryItem item;
  item.path = path;
  item.entry.directory = false;
  item.entry.size = file.size;
  item.entry.modified = file.mtime;
  item.addedAt = file.addedAt;
  if (library_) {
    item.record = library_->record(path);
    item.id = item.record.id;
    // A video the library had before the index found it keeps its date.
    const int64_t added = item.id ? library_->addedAt(item.id) : 0;
    if (added > 0 && added <= file.foundAt) item.addedAt = added;
  }
  return item;
}

const std::shared_ptr<const StashCatalog>& Menu::stashCatalog() const {
  static const std::shared_ptr<const StashCatalog> none;
  return stashCovers_ ? stashCovers_->stash().catalog() : none;
}

// Whether the scenes listed are from another catalog than Stash's now.
bool Menu::libraryCatalogChanged() const {
  const auto& catalog = stashCatalog();
  return libraryPage_.catalog.owner_before(catalog) || catalog.owner_before(libraryPage_.catalog);
}

// After the files in the pins: Stash's scenes with none of them, each where
// it plays from (stashSceneLocation()), added when it was added to Stash. A
// file in the pins is a scene's by its path, else by the oshash the library
// knows for it, else by its name and size (the same file through a mount
// and SMB or SFTP, or with Stash's folder not mapped to it).
void Menu::addLibraryScenes() {
  const auto& catalog = stashCatalog();
  libraryPage_.catalog = catalog;
  if (!catalog) return;
  const std::vector<StashScene>& scenes = catalog->scenes();
  std::vector<char> pinned(scenes.size());
  std::unordered_map<std::string, int> byFile;
  auto fileKey = [](const std::string& name, uint64_t size) { return name + '/' + std::to_string(size); };
  for (const LibraryItem& item : libraryPage_.items) {
    int i = catalog->sceneByPath(item.path);
    if (i < 0 && library_) {
      const std::string oshash = library_->oshashOf(item.path);
      if (!oshash.empty()) i = catalog->sceneByOshash(oshash);
    }
    if (i < 0) {
      if (byFile.empty())
        for (size_t k = 0; k < scenes.size(); ++k)
          byFile.emplace(fileKey(scenes[k].fileName(), scenes[k].size), static_cast<int>(k));
      const auto it = byFile.find(fileKey(fs::path(item.path).filename().string(), item.entry.size));
      if (it != byFile.end()) i = it->second;
    }
    if (i >= 0) pinned[i] = 1;
  }
  for (size_t i = 0; i < scenes.size(); ++i) {
    if (pinned[i] || stashCovers_->stash().hidden(scenes[i].id)) continue;
    const StashScene& s = scenes[i];
    LibraryItem item;
    item.path = stashSceneLocation(s);
    item.scene = true;
    item.oshash = s.oshash;
    item.entry.directory = false;
    item.entry.size = s.size;
    item.entry.modified = s.modified;
    item.addedAt = s.createdAt;
    if (library_) {
      item.record = library_->record(item.path);
      item.id = item.record.id;
    }
    libraryPage_.items.push_back(std::move(item));
  }
}

void Menu::loadLibrary() {
  libraryPage_.items.clear();
  if (index_)
    for (const auto& [path, file] : index_->files()) libraryPage_.items.push_back(libraryItem(path, file));
  addLibraryScenes();
  if (library_) libraryPage_.generation = library_->generation();
  placeLibraryItems();
  sortLibrary();
}

// Files a look (or a folder listing) found new, changed or gone, and
// Stash's scenes again (one may be among them, or the catalog changed).
// New ones go in their place in the order; the row pointed at, else the top
// one, stays where it is on the screen.
void Menu::libraryFilesChanged(const std::vector<std::string>& changed,
                               const std::vector<std::string>& removed) {
  // The library's records of new files, and moved ones found by their size.
  if (library_ && !changed.empty()) {
    std::vector<FolderListing::Entry> entries;
    for (const std::string& path : changed) {
      const LibraryIndex::File& f = index_->files().at(path);
      entries.push_back({fs::path(path).filename().string(), path, false, f.mtime, f.size});
    }
    library_->lookup(entries);
  }
  if (page_ != Page::Library) return;  // showLibrary() reads them all
  const std::vector<ListItem>& before = libraryListItems();
  const int columns = gallery_ ? kGalleryColumns : 1;
  const double top = list_.offset + list_.fraction;
  std::string anchor;
  if (hovered_ >= 0 && hovered_ < static_cast<int>(buttons_.size()) &&
      buttons_[hovered_].panel == kBrowser && buttons_[hovered_].scrolls) {
    const Button& b = buttons_[hovered_];
    anchor = !b.ratingPath.empty() ? b.ratingPath : b.offlinePath;
  }
  double rowsFromTop = 0;
  bool anchored = false;
  for (int i = 0; i < static_cast<int>(before.size()); ++i) {
    const double row = i / columns;
    if (anchor.empty() ? row >= top - 1e-6 : before[i].thumbPath == anchor) {
      anchor = before[i].thumbPath;
      rowsFromTop = row - top;
      anchored = true;
      break;
    }
  }

  const bool scenes =
      libraryCatalogChanged() || (stashCatalog() && (!changed.empty() || !removed.empty()));
  std::unordered_set<std::string> drop(removed.begin(), removed.end());
  drop.insert(changed.begin(), changed.end());
  libraryPage_.items.erase(std::remove_if(libraryPage_.items.begin(), libraryPage_.items.end(),
                                     [&](const LibraryItem& item) {
                                       return item.scene ? scenes : drop.count(item.path) > 0;
                                     }),
                      libraryPage_.items.end());
  const size_t kept = libraryPage_.items.size();
  for (const std::string& path : changed)
    libraryPage_.items.push_back(libraryItem(path, index_->files().at(path)));
  if (scenes) addLibraryScenes();
  placeLibraryItems();
  const auto added = libraryPage_.items.begin() + static_cast<long>(kept);
  for (auto it = added; it != libraryPage_.items.end(); ++it) {
    if (stashCovers_) it->title = stashCovers_->title(it->entry.path);
    it->listed = libraryShows(*it);
  }
  auto before_ = [this](const LibraryItem& a, const LibraryItem& b) { return libraryBefore(a, b); };
  std::stable_sort(added, libraryPage_.items.end(), before_);
  std::inplace_merge(libraryPage_.items.begin(), libraryPage_.items.begin() + static_cast<long>(kept),
                     libraryPage_.items.end(), before_);
  libraryPage_.listStale = true;

  if (anchored) {
    const std::vector<ListItem>& after = libraryListItems();
    for (int i = 0; i < static_cast<int>(after.size()); ++i)
      if (after[i].thumbPath == anchor) {
        const double position = std::max(0.0, i / columns - rowsFromTop);
        list_.offset = static_cast<int>(std::floor(position));
        list_.fraction = position - list_.offset;
        break;
      }
  }
  dirty_ = true;
}

// Ratings and plays changed: shown at once, in the same order (as in a folder).
// By id: a path listed may since have become another video's, or none.
void Menu::refreshLibraryRecords() {
  libraryPage_.generation = library_->generation();
  libraryPage_.listStale = true;
  for (LibraryItem& item : libraryPage_.items) {
    item.record = item.id ? library_->video(item.id) : library_->record(item.path);
    item.id = item.record.id;
  }
}

// Each video shows its file; away from home, one on a server that doesn't
// answer shows its copy on this device instead, if it has one, else Stash's
// stream if that serves the file (a scene's, pinned or not) and answers,
// else none. True if that changed anything.
bool Menu::placeLibraryItems() {
  libraryPage_.listStale = true;
  bool changed = false;
  auto stream = [this](const LibraryItem& item) {
    const auto& catalog = stashCatalog();
    if (!catalog || isStashScene(item.path)) return std::string();
    std::string oshash = item.oshash.empty() && library_ ? library_->oshashOf(item.path) : item.oshash;
    int i = catalog->sceneByPath(item.path);
    if (i >= 0 && oshash.empty() && catalog->scenes()[i].path == item.path)
      oshash = catalog->scenes()[i].oshash;
    if (i < 0 && !oshash.empty()) i = catalog->sceneByOshash(oshash);
    const std::string at = i >= 0 ? stashStreamOf(catalog->scenes()[i], oshash) : std::string();
    return offline_->serverDown(at) ? std::string() : at;
  };
  for (LibraryItem& item : libraryPage_.items) {
    std::string shown = item.path;
    if (offline_ && offline_->serverDown(shown)) {
      shown = offline_->copyOf(shown, item.oshash);
      if (shown.empty()) shown = stream(item);
    }
    if (shown == item.entry.path) continue;
    item.entry.path = shown;
    item.entry.name = shown.empty() ? "" : fs::path(shown).filename().string();
    changed = true;
  }
  return changed;
}

// In libraryPage_.sort: by name, or by the field with what has none of it last;
// ties by name. The name is the one shown: Stash's title, if it replaces
// the file's.
bool Menu::libraryBefore(const LibraryItem& a, const LibraryItem& b) const {
  const int field = fieldOf(libraryPage_.sort);
  const bool down = libraryPage_.sort == kFieldOrders[field].down;
  if (field != kByName) {
    auto key = [field](const LibraryItem& item) {
      return field == kBySize ? static_cast<double>(item.entry.size)
                              : recordKey(field, item.record, item.addedAt);
    };
    const double ka = key(a), kb = key(b);
    if ((ka > 0) != (kb > 0)) return ka > 0;
    if (ka != kb) return down ? ka > kb : ka < kb;
    return fileNameLess(a.name(), b.name());
  }
  return down ? fileNameLess(b.name(), a.name()) : fileNameLess(a.name(), b.name());
}

bool Menu::libraryShows(const LibraryItem& item) const {
  switch (libraryPage_.show) {
    case 1: return item.record.playCount == 0;
    case 2: return item.record.playCount > 0;
    case 3: return item.record.rating > 0;
  }
  return true;
}

void Menu::sortLibrary() {
  libraryPage_.listStale = true;
  for (LibraryItem& item : libraryPage_.items) {
    if (stashCovers_) item.title = stashCovers_->title(item.entry.path);
    item.listed = libraryShows(item);
  }
  std::stable_sort(libraryPage_.items.begin(), libraryPage_.items.end(),
                   [this](const LibraryItem& a, const LibraryItem& b) { return libraryBefore(a, b); });
}

int Menu::libraryMatchCount() const {
  int n = 0;
  for (const LibraryItem& item : libraryPage_.items)
    n += !item.entry.path.empty() && item.listed && matchesFilter(item.entry, libraryPage_.filter);
  return n;
}

const std::vector<Menu::ListItem>& Menu::libraryListItems() {
  if (!libraryPage_.listStale && libraryPage_.listPlaying == view_.currentFile) return libraryPage_.list;
  libraryPage_.listStale = false;
  libraryPage_.listPlaying = view_.currentFile;
  std::vector<ListItem>& items = libraryPage_.list;
  items.clear();
  std::fill(std::begin(libraryPage_.counts), std::end(libraryPage_.counts), 0);
  const bool played = fieldOf(libraryPage_.sort) == kByPlayed;
  for (const LibraryItem& library : libraryPage_.items) {
    const FolderListing::Entry& e = library.entry;
    if (e.path.empty() || !matchesFilter(e, libraryPage_.filter)) continue;
    const VideoRecord& r = library.record;
    ++libraryPage_.counts[0];
    ++libraryPage_.counts[r.playCount > 0 ? 2 : 1];
    libraryPage_.counts[3] += r.rating > 0;
    if (!library.listed) continue;
    ListItem item{e.name, e.path == view_.currentFile, nullptr};
    item.action = [this, path = e.path] {
      // Previous and Next go through the list as it shows.
      played_.list.clear();
      for (const ListItem& shown : libraryPage_.list) played_.list.push_back(shown.thumbPath);
      played_.at = path;
      return openVideo(path);
    };
    item.thumbPath = e.path;
    item.ratingPath = e.path;
    item.ratingVideo = library.id;
    item.rating = r.rating;
    if (r.playCount > 0) item.plays = stashNumber(r.playCount);
    if (r.durationSeconds > 0) item.length = static_cast<int>(r.durationSeconds);
    item.size = sizeText(e.size);
    // Sorted by when they were played, the date shown is that; else when added.
    item.date = dayText(played ? r.lastPlayedAt : library.addedAt);
    items.push_back(std::move(item));
  }
  return items;
}

// All · Not played · Played · Rated, each with how many the search finds:
// a well on the tools row from x, the one shown raised.
void Menu::layoutLibraryShow(double& x) {
  const Strings& t = text();
  libraryListItems();  // the counts
  std::vector<ToolPiece> pieces;
  for (int i = 0; i < 4; ++i) {
    ToolPiece p;
    p.label = t.libraryShows[i];
    p.count = stashNumber(libraryPage_.counts[i]);
    p.chosen = libraryPage_.show == i;
    p.disabled = libraryPage_.counts[i] == 0 && libraryPage_.show != i;  // never a choice that shows nothing
    p.action = [this, i] {
      libraryPage_.show = i;
      sortLibrary();
      list_.offset = 0;
      list_.fraction = 0;
      dirty_ = true;
      return MenuCommand{};
    };
    pieces.push_back(std::move(p));
  }
  // The search after them keeps at least its example's width (its text after
  // its icon, as fieldTextBox() has it, up to toolsJumps()). Where it
  // wouldn't, the pieces' sides narrow; where that isn't enough, the
  // filters not chosen keep their counts in their tooltips.
  const double searchRight = kContentRight - 2 * kBrowserToolsH - kit::kSpaceTight - kColumnGap;
  const double need = std::ceil(textWidth(t.tipSearchLibrary, kit::kWellSize, false)) + 2 * kit::kSpace + 26 + 12;
  auto over = [&] { return x + toolsWellWidth(pieces) + kColumnGap + need - searchRight; };
  auto narrow = [&] {
    if (over() <= 0) return;
    const double pad = std::max(kit::kSpaceTight + 2, kButtonPadX - std::ceil(over() / (2 * pieces.size())));
    for (ToolPiece& p : pieces) p.padX = pad;
  };
  narrow();
  if (over() > 0) {
    for (ToolPiece& p : pieces) {
      p.padX = -1;
      if (p.chosen) continue;
      p.tooltip = p.label + " · " + p.count;
      p.count.clear();
    }
    narrow();
  }
  x = toolsWell(x, kBrowserToolsY + toolsShift(), pieces) + kColumnGap;
}

namespace {

// The bands under the Library's header (the banner after setup, the first
// sync's line), as high as its rows, kit::kSpace apart.
constexpr double kBannerH = kBrowserRowH, kSyncLineH = kBrowserRowH, kBandGap = kit::kSpace;
// The page with nothing to list: its icon's centre, the title and the
// text under it, then its buttons.
constexpr double kCentreY = Menu::kBrowserHeight * 0.33;
constexpr double kCentreTextY = kCentreY + 120, kCentreTextLines = 3;
constexpr double kCentreButtonsY = Menu::kBrowserHeight * 0.58;
// A button inside a band, kit::kSpacePiece in from its edges.
constexpr double kBandButtonH = kBrowserRowH - 2 * kit::kSpacePiece;
// A band's text, after its icon (kit::kSpace in, 28 wide).
constexpr double kBandTextX = kit::kSpace + 28 + 12;

}  // namespace

Menu::FirstSync Menu::firstSync() const {
  if (!stash_ || stash_->catalog()) return FirstSync::None;
  const StashStatus& s = stash_->status();
  if (s.syncing) return FirstSync::Running;
  return s.error.empty() ? FirstSync::None : FirstSync::Failed;
}

// Running: its step, as Settings → Stash words it. Failed: the sync's own
// error, but plain words for Stash no longer answering; `line` as the line
// above the pins says it.
std::string Menu::firstSyncText(bool line) const {
  const StashStatus& s = stash_->status();
  const SetupStrings& t = text().setup;
  if (s.syncing) return line ? stashPrinted(t.gettingScenesLine, stashSyncStep(s)) : stashSyncStep(s);
  const StashSetupStrings& e = text().stashSetup;
  const bool stopped = s.error == e.refused || s.error == e.timeout || s.error == e.unreachable;
  if (line) return stashPrinted(t.couldntGetLine, stopped ? t.stoppedAnsweringShort : s.error);
  return stopped ? t.stoppedAnswering : s.error;
}

Menu::LibraryBands Menu::libraryBands() const {
  LibraryBands b;
  double y = kBrowserToolsY;
  if (view_.setupBanner) {
    b.banner = y;
    y += kBannerH + kBandGap;
  }
  if (libraryPinHelp()) return b;  // the page says it, and has no tools
  if (firstSync() != FirstSync::None) {
    b.sync = y;
    y += kSyncLineH + kBandGap;
  }
  b.shift = y - kBrowserToolsY;
  return b;
}

// The first sync's state and words as drawn: a new state redraws the page,
// a new step only its line.
void Menu::updateLibraryFirstSync() {
  const FirstSync state = firstSync();
  std::string words = state == FirstSync::None ? "" : firstSyncText(!libraryPinHelp());
  if (state != libraryPage_.firstSync) {
    dirty_ = true;
  } else if (words != libraryPage_.firstSyncText) {
    const LibraryBands b = libraryBands();
    if (b.sync >= 0)
      browserChanged(b.sync, b.sync + kSyncLineH, kPageX, kPageX + kPageWidth);
    else
      browserChanged(kCentreTextY - 4, kCentreButtonsY - 4, kPageX, kPageX + kPageWidth);
  }
  libraryPage_.firstSync = state;
  libraryPage_.firstSyncText = std::move(words);
}

// "You're all set" with the quality preset in effect, a link to Video
// settings and ×; the first sync's line with Try again once it failed.
void Menu::layoutLibraryBands(const LibraryBands& bands) {
  const Strings& t = text();
  constexpr double in = kit::kSpacePiece;
  if (bands.banner >= 0) {
    // Leave a gap before the button itself, not just before its label.
    const std::string words = allSetText() + " " + t.setup.changeItIn;
    Button link = textButton(t.videoSettings, Icon::None, [this] {
      showPage(Page::VideoSettings);
      return MenuCommand{};
    });
    link.panel = kBrowser;
    link.bare = true;
    link.on = true;
    link.fontSize = kit::kRowSize;
    link.w = textWidth(t.videoSettings, kit::kRowSize, false) + 2 * kit::kSpaceTight;
    link.x = kContentLeft + kBandTextX + textWidth(words, kit::kRowSize, false) + kit::kSpaceTight;
    link.y = bands.banner + in;
    link.h = kBandButtonH;
    link.radius = kit::kPieceCorner;
    buttons_.push_back(link);
    Button& close = addRound(Icon::Close, t.setup.dismiss, command(MenuCommand::DismissSetupBanner),
                             kContentRight - in - kBandButtonH, bands.banner + in, kBandButtonH);
    close.panel = kBrowser;
    close.bare = true;
    close.iconSize = 26;
  }
  if (bands.sync >= 0 && firstSync() == FirstSync::Failed) {
    Button again = textButton(t.setup.tryAgain, Icon::None, command(MenuCommand::SyncStash), true);
    again.panel = kBrowser;
    again.fontSize = kit::kWellSize;
    again.w = textWidth(t.setup.tryAgain, kit::kWellSize, false) + 2 * kButtonPadX;
    again.x = kContentRight - in - again.w;
    again.y = bands.sync + in;
    again.h = kBandButtonH;
    again.radius = kit::kPieceCorner;
    buttons_.push_back(again);
  }
}

// Nothing pinned: how to pin a folder; with Stash's first sync under way,
// that the scenes are coming; once it failed, why, Try again and Stash
// settings.
void Menu::layoutLibraryCentre() {
  const Strings& t = text();
  list_.count = 0;
  std::vector<Button> row;
  if (firstSync() == FirstSync::Failed) {
    row.push_back(textButton(t.setup.tryAgain, Icon::None, command(MenuCommand::SyncStash), true));
    row.push_back(textButton(t.setup.stashSettings, Icon::None, [this] {
      showStashSettings();
      return MenuCommand{};
    }));
  } else {
    row.push_back(textButton(t.openFileExplorer, Icon::Folder, [this] {
      showFileBrowser();
      return MenuCommand{};
    }, true));
  }
  double w = -16;
  for (const Button& b : row) w += b.w + 16;
  double x = kPageCenterX - w / 2;
  for (Button& b : row) {
    b.panel = kBrowser;
    b.x = x;
    b.y = kCentreButtonsY;
    b.h = kBrowserToolsH;
    x += b.w + 16;
    buttons_.push_back(b);
  }
}

std::string Menu::allSetText() const {
  const SetupStrings& t = text().setup;
  const PerformancePreset preset = view_.performancePreset;
  if (preset == PerformancePreset::High && kStandaloneHeadset) return t.allSetFrame;
  if (preset == PerformancePreset::Balanced && !kStandaloneHeadset) return t.allSetDesktop;
  const PresetStrings& p = text().presets;
  return stashPrinted(t.allSet, preset == PerformancePreset::Custom ? p.custom
                                                                    : p.names[static_cast<int>(preset)]);
}

// The banner: in the accent's soft colour, its tick in the accent; the
// first sync's line: a well, its state's icon.
void Menu::drawLibraryBands(cairo_t* cr, const LibraryBands& bands) {
  const Palette& pal = palette();
  const double x = kContentLeft, w = kContentRight - kContentLeft;
  constexpr double icon = 28;
  if (bands.banner >= 0) {
    const double y = bands.banner;
    roundedRect(cr, x, y, w, kBannerH, kit::kCorner);
    setColor(cr, pal.accentSoft);
    cairo_fill(cr);
    drawIcon(cr, Icon::Check, x + kit::kSpace + icon / 2, y + kBannerH / 2, icon, pal.textAccent.r,
             pal.textAccent.g, pal.textAccent.b);
    drawText(cr, allSetText() + " " + text().setup.changeItIn, x + kBandTextX, y,
             w - kBandTextX - kBannerH, kBannerH, kit::kRowSize, false, false, pal.text.r, pal.text.g,
             pal.text.b);
  }
  if (bands.sync >= 0) {
    const double y = bands.sync;
    const bool failed = firstSync() == FirstSync::Failed;
    drawFinish(cr, Finish::Sunk, x, y, w, kSyncLineH, kit::kCorner);
    const Rgba& k = failed ? pal.gold : pal.textAccent;
    drawIcon(cr, failed ? Icon::Warning : Icon::Refresh, x + kit::kSpace + icon / 2, y + kSyncLineH / 2,
             icon, k.r, k.g, k.b);
    double end = x + w - kit::kSpace;  // before Try again (layoutLibraryBands())
    if (failed)
      end -= kit::kSpacePiece + textWidth(text().setup.tryAgain, kit::kWellSize, false) + 2 * kButtonPadX;
    drawText(cr, firstSyncText(true), x + kBandTextX, y, end - x - kBandTextX, kSyncLineH,
             kit::kWellSize, false, false, pal.text.r, pal.text.g, pal.text.b);
  }
}

void Menu::drawLibraryCentre(cairo_t* cr) {
  const Strings& t = text();
  const Palette& pal = palette();
  const double dim[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a};
  const double w = kContentRight - kContentLeft - 200;
  const FirstSync state = firstSync();
  std::string title = t.libraryNoPinsTitle, words;
  if (state == FirstSync::Running) {
    drawIcon(cr, Icon::Refresh, kPageCenterX, kCentreY, 64, pal.textAccent.r, pal.textAccent.g,
             pal.textAccent.b);
    title = t.setup.gettingScenes;
    words = firstSyncText(false) + "\n" + t.setup.gettingScenesText;
  } else if (state == FirstSync::Failed) {
    drawIcon(cr, Icon::Warning, kPageCenterX, kCentreY, 64, pal.gold.r, pal.gold.g, pal.gold.b);
    title = t.setup.couldntGetScenes;
    words = firstSyncText(false);
  } else {
    drawIcon(cr, Icon::Pin, kPageCenterX, kCentreY, 56, dim[0], dim[1], dim[2]);
    // Stash on but no catalog yet: its scenes are to come.
    words = stashCovers_ && !stashCatalog() ? t.libraryNoPinsStash : t.libraryNoPinsText;
  }
  drawText(cr, title, kContentLeft, kCentreY + 50, kContentRight - kContentLeft, 60, kit::kTitleSize,
           true, true, pal.text.r, pal.text.g, pal.text.b);
  drawTextLines(cr, words, kPageCenterX - w / 2, kCentreTextY, w, kit::kRowSize, kCentreTextLines, dim,
                true);
}

// What the heading says of the looks for new videos.
std::string Menu::libraryStatus() const {
  if (!index_) return "";
  const Strings& t = text();
  const LibraryIndex::State s = index_->state();
  if (s.looking && index_->videoOpen()) return t.libraryLookWaits;
  if (s.looking)
    return s.percent >= 0 ? stashPrinted(t.libraryLooking, s.percent) : t.libraryLookingUnknown;
  return s.lookedAt > 0 ? stashPrinted(t.libraryUpToDate, stashAgo(s.lookedAt)) : "";
}

// The page: "Library", the count, the looks' status and Look again; list or
// thumbnails, the order, what to show and the search; then the list with its
// headings, or the thumbnails. With nothing to list, how to pin a folder.
void Menu::layoutLibraryPage() {
  const Strings& t = text();
  contentEdges();
  listShowsFolder_ = false;
  listColumns_ = false;
  const LibraryBands bands = libraryBands();
  double right = kContentRight;
  if (libraryPinHelp()) {
    contentTitle(t.library, "", right);
    layoutLibraryBands(bands);
    layoutLibraryCentre();
    return;
  }
  const LibraryIndex::State s = index_ ? index_->state() : LibraryIndex::State{};
  const bool open = index_ && index_->videoOpen();
  headerRound(right, Icon::Refresh, open ? t.tipLookWaits : s.looking ? t.tipLookingNow : t.tipLookAgain,
              [this] {
                if (index_) index_->lookAgain();
                return MenuCommand{};
              }).on = s.looking;
  // How many: all of them, or what the search and Show leave of them.
  libraryListItems();  // the counts
  int total = 0;
  for (const LibraryItem& item : libraryPage_.items) total += !item.entry.path.empty();
  const std::string count =
      libraryPage_.filter.empty() && libraryPage_.show == 0
          ? stashPrinted(s.firstLook ? t.libraryCountSoFar : total == 1 ? t.libraryCountOne : t.libraryCount,
                         stashNumber(total))
          : stashPrinted(t.libraryMatches, stashNumber(libraryPage_.counts[libraryPage_.show]), stashNumber(total));
  contentTitle(t.library, count, right, libraryStatus());
  layoutLibraryBands(bands);
  const bool editing = editingFilter();
  list_.bottom = editing ? kKeyboardY - 16 : kListBottom;
  if (gallery_) {
    list_.top = kGridTop + bands.shift;
    addGallery(libraryListItems(), list_.left, editing ? kEditingGalleryRows : kGalleryRows);
  } else {
    list_.top = kListTop + bands.shift;
    listColumns_ = true;
    addList(libraryListItems(), list_.left, kFileRowH,
            static_cast<int>((list_.bottom - list_.top) / kFileRowH));
    if (list_.count > 0) layoutListHeader(list_.left, list_.right - list_.left - kScrollbarRoom);
  }
  if (editing) layoutKeyboard();
  layoutFileTools();
}

void Menu::drawLibraryHeading(cairo_t* cr) {
  drawPageHead(cr);
  drawLibraryBands(cr, libraryBands());
  if (libraryPinHelp()) drawLibraryCentre(cr);
}
