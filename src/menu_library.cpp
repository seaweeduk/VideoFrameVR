#include "menu_style.h"

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
  return places_.ready() && std::none_of(places_.places().begin(), places_.places().end(),
                                         [](const Places::Place& p) { return p.pinned; });
}

void Menu::showLibrary() {
  loadLibrary();
  showPage(Page::Library);
  scrollToPlace(libraryPlace_, libraryPaths(), gallery_ ? kGalleryColumns : 1);
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
  return libraryCatalog_.owner_before(catalog) || catalog.owner_before(libraryCatalog_);
}

// After the files in the pins: Stash's scenes with none of them, each where
// it plays from (stashSceneLocation()), added when it was added to Stash. A
// file in the pins is a scene's by its path, else by the oshash the library
// knows for it, else by its name and size (the same file through a mount
// and SMB or SFTP, or with Stash's folder not mapped to it).
void Menu::addLibraryScenes() {
  const auto& catalog = stashCatalog();
  libraryCatalog_ = catalog;
  if (!catalog) return;
  const std::vector<StashScene>& scenes = catalog->scenes();
  std::vector<char> pinned(scenes.size());
  std::unordered_map<std::string, int> byFile;
  auto fileKey = [](const std::string& name, uint64_t size) { return name + '/' + std::to_string(size); };
  for (const LibraryItem& item : libraryItems_) {
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
    libraryItems_.push_back(std::move(item));
  }
}

void Menu::loadLibrary() {
  libraryItems_.clear();
  if (index_)
    for (const auto& [path, file] : index_->files()) libraryItems_.push_back(libraryItem(path, file));
  addLibraryScenes();
  if (library_) libraryGeneration_ = library_->generation();
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
  const double top = scrollOffset_ + scrollFraction_;
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
  libraryItems_.erase(std::remove_if(libraryItems_.begin(), libraryItems_.end(),
                                     [&](const LibraryItem& item) {
                                       return item.scene ? scenes : drop.count(item.path) > 0;
                                     }),
                      libraryItems_.end());
  const size_t kept = libraryItems_.size();
  for (const std::string& path : changed)
    libraryItems_.push_back(libraryItem(path, index_->files().at(path)));
  if (scenes) addLibraryScenes();
  placeLibraryItems();
  const auto added = libraryItems_.begin() + static_cast<long>(kept);
  for (auto it = added; it != libraryItems_.end(); ++it) {
    if (stashCovers_) it->title = stashCovers_->title(it->entry.path);
    it->listed = libraryShows(*it);
  }
  auto before_ = [this](const LibraryItem& a, const LibraryItem& b) { return libraryBefore(a, b); };
  std::stable_sort(added, libraryItems_.end(), before_);
  std::inplace_merge(libraryItems_.begin(), libraryItems_.begin() + static_cast<long>(kept),
                     libraryItems_.end(), before_);
  libraryListStale_ = true;

  if (anchored) {
    const std::vector<ListItem>& after = libraryListItems();
    for (int i = 0; i < static_cast<int>(after.size()); ++i)
      if (after[i].thumbPath == anchor) {
        const double position = std::max(0.0, i / columns - rowsFromTop);
        scrollOffset_ = static_cast<int>(std::floor(position));
        scrollFraction_ = position - scrollOffset_;
        break;
      }
  }
  dirty_ = true;
}

// Ratings and plays changed: shown at once, in the same order (as in a folder).
// By id: a path listed may since have become another video's, or none.
void Menu::refreshLibraryRecords() {
  libraryGeneration_ = library_->generation();
  libraryListStale_ = true;
  for (LibraryItem& item : libraryItems_) {
    item.record = item.id ? library_->video(item.id) : library_->record(item.path);
    item.id = item.record.id;
  }
}

// Each video shows its file; away from home, one on a server that doesn't
// answer shows its copy on this device instead, if it has one, else Stash's
// stream if that serves the file (a scene's, pinned or not) and answers,
// else none. True if that changed anything.
bool Menu::placeLibraryItems() {
  libraryListStale_ = true;
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
  for (LibraryItem& item : libraryItems_) {
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

// In librarySort_: by name, or by the field with what has none of it last;
// ties by name. The name is the one shown: Stash's title, if it replaces
// the file's.
bool Menu::libraryBefore(const LibraryItem& a, const LibraryItem& b) const {
  const int field = fieldOf(librarySort_);
  const bool down = librarySort_ == kFieldOrders[field].down;
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
  switch (libraryShow_) {
    case 1: return item.record.playCount == 0;
    case 2: return item.record.playCount > 0;
    case 3: return item.record.rating > 0;
  }
  return true;
}

void Menu::sortLibrary() {
  libraryListStale_ = true;
  for (LibraryItem& item : libraryItems_) {
    if (stashCovers_) item.title = stashCovers_->title(item.entry.path);
    item.listed = libraryShows(item);
  }
  std::stable_sort(libraryItems_.begin(), libraryItems_.end(),
                   [this](const LibraryItem& a, const LibraryItem& b) { return libraryBefore(a, b); });
}

int Menu::libraryMatchCount() const {
  int n = 0;
  for (const LibraryItem& item : libraryItems_)
    n += !item.entry.path.empty() && item.listed && matchesFilter(item.entry, libraryFilter_);
  return n;
}

const std::vector<Menu::ListItem>& Menu::libraryListItems() {
  if (!libraryListStale_ && libraryListPlaying_ == view_.currentFile) return libraryList_;
  libraryListStale_ = false;
  libraryListPlaying_ = view_.currentFile;
  std::vector<ListItem>& items = libraryList_;
  items.clear();
  std::fill(std::begin(libraryCounts_), std::end(libraryCounts_), 0);
  const bool played = fieldOf(librarySort_) == kByPlayed;
  for (const LibraryItem& library : libraryItems_) {
    const FolderListing::Entry& e = library.entry;
    if (e.path.empty() || !matchesFilter(e, libraryFilter_)) continue;
    const VideoRecord& r = library.record;
    ++libraryCounts_[0];
    ++libraryCounts_[r.playCount > 0 ? 2 : 1];
    libraryCounts_[3] += r.rating > 0;
    if (!library.listed) continue;
    ListItem item{e.name, e.path == view_.currentFile, nullptr};
    item.action = [this, path = e.path] {
      // Previous and Next go through the list as it shows.
      playList_.clear();
      for (const ListItem& shown : libraryList_) playList_.push_back(shown.thumbPath);
      playAt_ = path;
      return openVideo(path);
    };
    item.thumbPath = e.path;
    item.ratingPath = e.path;
    item.ratingVideo = library.id;
    item.rating = r.rating;
    if (r.playCount > 0) item.plays = std::to_string(r.playCount);
    item.size = sizeText(e.size);
    // Sorted by when they were played, the date shown is that; else when added.
    item.date = dateText(played ? r.lastPlayedAt : library.addedAt);
    items.push_back(std::move(item));
  }
  return items;
}

// All · Not played · Played · Rated with how many the search finds, on the
// tool row from x (as Stash's All / VR / 2D).
void Menu::layoutLibraryShow(double& x) {
  const Strings& t = text();
  libraryListItems();  // the counts
  for (int i = 0; i < 4; ++i) {
    const std::string label = std::string(t.libraryShows[i]) + "  " + stashNumber(libraryCounts_[i]);
    Button b = textButton(label, Icon::None,
                          [this, i] {
                            libraryShow_ = i;
                            sortLibrary();
                            scrollOffset_ = 0;
                            scrollFraction_ = 0;
                            dirty_ = true;
                            return MenuCommand{};
                          },
                          libraryShow_ == i);
    // Never a choice that shows nothing.
    b.disabled = libraryCounts_[i] == 0 && libraryShow_ != i;
    b.panel = kBrowser;
    b.x = x;
    b.y = kBrowserToolsY + toolsShift();
    b.h = kBrowserToolsH;
    b.w = textWidth(label, kTextSize, false) + 36;
    b.radius = kBrowserToolsH / 2;
    buttons_.push_back(b);
    x += b.w + 6;
  }
  x += 10;
}

namespace {

constexpr double kBannerH = 64, kSyncLineH = 56, kBandGap = 12;
// The page with nothing to list: its icon's centre, the title and the
// text under it, then its buttons.
constexpr double kCentreY = Menu::kBrowserHeight * 0.33;
constexpr double kCentreTextY = kCentreY + 120, kCentreTextLines = 3;
constexpr double kCentreButtonsY = Menu::kBrowserHeight * 0.58;
constexpr double kAmber[3] = {1, 190 / 255.0, 120 / 255.0};

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
  if (state != libraryFirstSync_) {
    dirty_ = true;
  } else if (words != libraryFirstSyncText_) {
    const LibraryBands b = libraryBands();
    if (b.sync >= 0)
      browserChanged(b.sync, b.sync + kSyncLineH, kPageX, kPageX + kPageWidth);
    else
      browserChanged(kCentreTextY - 4, kCentreButtonsY - 4, kPageX, kPageX + kPageWidth);
  }
  libraryFirstSync_ = state;
  libraryFirstSyncText_ = std::move(words);
}

// "You're all set" with the quality preset in effect, a link to Video
// settings and ×; the first sync's line with Try again once it failed.
void Menu::layoutLibraryBands(const LibraryBands& bands) {
  const Strings& t = text();
  const double x = kPageX + kPagePad, right = kPageX + kPageWidth - kPagePad;
  if (bands.banner >= 0) {
    const std::string words = allSetText();
    Button link = textButton(t.videoSettings, Icon::None, [this] {
      showPage(Page::VideoSettings);
      return MenuCommand{};
    });
    link.panel = kBrowser;
    link.bare = true;
    link.on = true;
    link.fontSize = 26;
    link.x = x + 60 + textWidth(words + " " + t.setup.changeItIn, 26, false) + 4;
    link.y = bands.banner + 4;
    link.w = textWidth(t.videoSettings, 26, false) + 24;
    link.h = kBannerH - 8;
    buttons_.push_back(link);
    Button& close = addRound(Icon::Close, t.setup.dismiss, command(MenuCommand::DismissSetupBanner),
                             right - kBannerH + 4, bands.banner + 4, kBannerH - 8);
    close.panel = kBrowser;
    close.bare = true;
  }
  if (bands.sync >= 0 && firstSync() == FirstSync::Failed) {
    Button again = textButton(t.setup.tryAgain, Icon::None, command(MenuCommand::SyncStash), true);
    again.panel = kBrowser;
    again.fontSize = 24;
    again.w = textWidth(t.setup.tryAgain, 24, false) + 60;
    again.x = right - 8 - again.w;
    again.y = bands.sync + 6;
    again.h = kSyncLineH - 12;
    buttons_.push_back(again);
  }
}

// Nothing pinned: how to pin a folder; with Stash's first sync under way,
// that the scenes are coming; once it failed, why, Try again and Stash
// settings.
void Menu::layoutLibraryCentre() {
  const Strings& t = text();
  listCount_ = 0;
  std::vector<Button> row;
  if (firstSync() == FirstSync::Failed) {
    row.push_back(textButton(t.setup.tryAgain, Icon::None, command(MenuCommand::SyncStash), true));
    row.push_back(textButton(t.setup.stashSettings, Icon::None, [this] {
      showPage(Page::Settings);
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

void Menu::drawLibraryBands(cairo_t* cr, const LibraryBands& bands) {
  const Palette& pal = palette();
  const double x = kPageX + kPagePad, w = kPageWidth - 2 * kPagePad;
  if (bands.banner >= 0) {
    const double y = bands.banner;
    roundedRect(cr, x, y, w, kBannerH, 16);
    setColor(cr, pal.accent, 0.12);
    cairo_fill_preserve(cr);
    setColor(cr, pal.edge, 0.9);
    cairo_set_line_width(cr, 2);
    cairo_stroke(cr);
    drawIcon(cr, Icon::Check, x + 32, y + kBannerH / 2, 32, pal.textAccent.r, pal.textAccent.g,
             pal.textAccent.b);
    drawText(cr, allSetText() + " " + text().setup.changeItIn, x + 60, y, w - 140, kBannerH, 26,
             false, false, pal.text.r, pal.text.g, pal.text.b);
  }
  if (bands.sync >= 0) {
    const double y = bands.sync;
    const bool failed = firstSync() == FirstSync::Failed;
    roundedRect(cr, x, y, w, kSyncLineH, 16);
    setColor(cr, pal.field, 0.35);
    cairo_fill(cr);
    if (failed)
      drawIcon(cr, Icon::Warning, x + 32, y + kSyncLineH / 2, 30, kAmber[0], kAmber[1], kAmber[2]);
    else
      drawIcon(cr, Icon::Refresh, x + 32, y + kSyncLineH / 2, 30, pal.textAccent.r,
               pal.textAccent.g, pal.textAccent.b);
    drawText(cr, firstSyncText(true), x + 60, y, w - (failed ? 300 : 80), kSyncLineH, 24, false,
             false, pal.text.r, pal.text.g, pal.text.b);
  }
}

void Menu::drawLibraryCentre(cairo_t* cr) {
  const Strings& t = text();
  const Palette& pal = palette();
  const double dim[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a};
  const double w = kPageWidth - 2 * kPagePad - 200;
  const FirstSync state = firstSync();
  std::string title = t.libraryNoPinsTitle, words;
  if (state == FirstSync::Running) {
    drawIcon(cr, Icon::Refresh, kPageCenterX, kCentreY, 64, pal.textAccent.r, pal.textAccent.g,
             pal.textAccent.b);
    title = t.setup.gettingScenes;
    words = firstSyncText(false) + "\n" + t.setup.gettingScenesText;
  } else if (state == FirstSync::Failed) {
    drawIcon(cr, Icon::Warning, kPageCenterX, kCentreY, 64, kAmber[0], kAmber[1], kAmber[2]);
    title = t.setup.couldntGetScenes;
    words = firstSyncText(false);
  } else {
    drawIcon(cr, Icon::Pin, kPageCenterX, kCentreY, 56, dim[0], dim[1], dim[2]);
    // Stash on but no catalog yet: its scenes are to come.
    words = stashCovers_ && !stashCatalog() ? t.libraryNoPinsStash : t.libraryNoPinsText;
  }
  drawText(cr, title, kPageX + kPagePad, kCentreY + 50, kPageWidth - 2 * kPagePad, 60, 34, true,
           true, pal.text.r, pal.text.g, pal.text.b);
  drawTextLines(cr, words, kPageCenterX - w / 2, kCentreTextY, w, 27, kCentreTextLines, dim, true);
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
  const double listX = kPageX + kPagePad;
  listRight_ = kPageX + kPageWidth - kPagePad + 6;
  listShowsFolder_ = false;
  listColumns_ = false;
  const LibraryBands bands = libraryBands();
  layoutLibraryBands(bands);
  if (libraryPinHelp()) {
    layoutLibraryCentre();
    return;
  }
  const LibraryIndex::State s = index_ ? index_->state() : LibraryIndex::State{};
  const bool open = index_ && index_->videoOpen();
  Button& again = addRound(Icon::Refresh,
                           open ? t.tipLookWaits : s.looking ? t.tipLookingNow : t.tipLookAgain,
                           [this] {
                             if (index_) index_->lookAgain();
                             return MenuCommand{};
                           },
                           kPageX + kPageWidth - kPagePad - kBrowserRowH, kBrowserRowY, kBrowserRowH);
  again.panel = kBrowser;
  again.bare = true;
  again.on = s.looking;
  const bool editing = editingFilter();
  listBottom_ = editing ? kKeyboardY - 16 : kListBottom;
  if (gallery_) {
    listTop_ = kGridTop + bands.shift;
    addGallery(libraryListItems(), listX, editing ? kEditingGalleryRows : kGalleryRows);
  } else {
    listTop_ = kListTop + bands.shift;
    listColumns_ = true;
    addList(libraryListItems(), listX, kFileRowH,
            static_cast<int>((listBottom_ - listTop_) / kFileRowH));
    layoutListHeader(listX, listRight_ - listX - 30);
  }
  if (editing) layoutKeyboard();
  layoutFileTools();
}

void Menu::drawLibraryHeading(cairo_t* cr) {
  const Strings& t = text();
  const Palette& pal = palette();
  const double x = kPageX + kPagePad + 8, right = kPageX + kPageWidth - kPagePad;
  drawText(cr, t.library, x, kBrowserRowY, 400, kBrowserRowH, 30, true, false, pal.text.r,
           pal.text.g, pal.text.b);
  const double dim[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a};
  drawLibraryBands(cr, libraryBands());
  if (libraryPinHelp()) {
    drawLibraryCentre(cr);
    return;
  }
  int total = 0;
  for (const LibraryItem& item : libraryItems_) total += !item.entry.path.empty();
  std::string count;
  if (libraryFilter_.empty() && libraryShow_ == 0)
    count = stashPrinted(index_ && index_->state().firstLook ? t.libraryCountSoFar
                         : total == 1                         ? t.libraryCountOne
                                                              : t.libraryCount,
                         total);
  else
    count = stashPrinted(t.libraryMatches, libraryMatchCount(), total);
  const double titleW = textWidth(t.library, 30, true);
  drawText(cr, count, x + titleW + 36, kBrowserRowY, 800, kBrowserRowH, 24, false, false, dim[0],
           dim[1], dim[2], dim[3]);
  const std::string status = libraryStatus();
  const double w = textWidth(status, 24, false);
  drawText(cr, status, right - kBrowserRowH - 16 - w, kBrowserRowY, w + 4, kBrowserRowH, 24, false,
           false, dim[0], dim[1], dim[2], dim[3]);
}
