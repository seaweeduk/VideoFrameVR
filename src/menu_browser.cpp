#include "menu_style.h"

#include "file_util.h"
#include "menu_stash_covers.h"
#include "thumbnailer.h"
#include "video_memory.h"

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <numeric>
#include <set>
#include <system_error>

namespace fs = std::filesystem;

// The file browser: the folder's contents, list and gallery.

void Menu::showFileBrowser() { openFiles(FileKind::Video); }

void Menu::openFiles(FileKind kind) {
  refreshPlaces();
  const bool returning = page_ != Page::Main && !directory().empty();
  fileKind_ = kind;
  // Start where the last session left off, else next to the current video;
  // later reopen where we left off.
  std::string dir = directory();
  std::vector<std::string> fallbacks;
  if (dir.empty()) {
    if (!startDirectory_.empty()) fallbacks.push_back(startDirectory_);
    if (remote::isRemote(view_.currentFile))
      fallbacks.push_back(remote::parent(view_.currentFile));
    else if (!view_.currentFile.empty() && view_.currentFile.find("://") == std::string::npos)
      fallbacks.push_back(fs::path(view_.currentFile).parent_path().string());
    fallbacks.push_back(std::getenv("HOME") ? std::getenv("HOME") : "/");
    dir = fallbacks.front();
    fallbacks.erase(fallbacks.begin());
  }
  openDirectory(dir, fallbacks);
  showPage(Page::Files);
  if (returning) return;
  // Back from playing: the browser is where it was, with the playing file
  // (maybe a later one, by next) in view.
  if (listing_.reading())
    revealPath_ = view_.currentFile;
  else
    scrollIntoView(view_.currentFile);
}

void Menu::rememberFolderView() {
  if (folders_ && !directory().empty())
    folders_->set(directory(), {static_cast<int>(sortMode_), gallery_});
}

// Reads the folder and returns to where the browser was in it this session
// (its search and the item in view), else to its top.
void Menu::openDirectory(const std::string& dir, const std::vector<std::string>& fallbacks) {
  if (page_ == Page::Files) saveBrowsing();
  if (dir != directory()) {
    editing_ = false;
    smbForm_ = {};
    stashFolders_.notThere = false;
  }
  // The folder's own order, if remembered (else as before). List or gallery
  // is one choice for all folders, kept in the settings.
  FolderView view;
  if (folders_ && folders_->get(dir, view) && view.sort >= 0 &&
      view.sort < static_cast<int>(SortMode::kCount)) {
    sortMode_ = static_cast<SortMode>(view.sort);
    sortDescending_ = sortMode_ == kFieldOrders[fieldOf(sortMode_)].down;
  }
  const bool sameFolder = dir == directory();
  listing_.read(dir, fileKind_ == FileKind::Subtitle, sortMode_, fallbacks);
  lookUpRecords();
  arrangeByRecords();
  const auto found = browsing_.find(dir);
  returnTo_ = found != browsing_.end() && fileKind_ == FileKind::Video ? found->second : Browsing{};
  fileFilter_ = returnTo_.filter;
  revealPath_.clear();
  if (!listing_.reading()) {
    scrollToItem(returnTo_.anchor, returnTo_.rowsFromTop, returnTo_.index);
  } else if (!sameFolder) {
    scrollOffset_ = 0;  // the place is restored when the folder has been read
    scrollFraction_ = 0;
  }
  dirty_ = true;
}

// A folder read in the background has arrived: back to the place in it.
void Menu::folderRead() {
  if (page_ != Page::Files) return;
  scrollToItem(returnTo_.anchor, returnTo_.rowsFromTop, returnTo_.index);
  if (!revealPath_.empty()) scrollIntoView(revealPath_);
  revealPath_.clear();
  dirty_ = true;
}

// Also what makes the library notice moved and changed files in the folder.
void Menu::lookUpRecords() {
  if (!library_) return;
  records_ = library_->lookup(listing_.entries());
  recordsGeneration_ = library_->generation();
}

// Only the rows or cards shown whose record changed are redrawn: redrawing
// the whole list hides the highlight and ignores clicks until it's done, and
// a folder's videos found at a new place change one after another.
void Menu::recordsChanged(const std::vector<VideoRecord>& before) {
  const std::vector<FolderListing::Entry>& entries = listing_.entries();
  if (before.size() != records_.size() || records_.size() != entries.size()) {
    browserChanged(listTop_ - 4, listBottom_ + 4);
    return;
  }
  for (size_t i = 0; i < records_.size(); ++i) {
    const VideoRecord &a = before[i], &b = records_[i];
    if (a.rating == b.rating && a.playCount == b.playCount && a.lastPlayedAt == b.lastPlayedAt)
      continue;
    for (const Button& part : buttons_) {
      if (part.panel != kBrowser || (part.ratingPath != entries[i].path &&
                                     part.thumbPath != entries[i].path))
        continue;
      // The row or card it is on.
      const double x = part.x + part.w / 2, y = part.y + part.h / 2;
      for (const Button& c : buttons_)
        if (c.panel == kBrowser && c.scrolls && x >= c.x && x <= c.x + c.w && y >= c.y &&
            y <= c.y + c.h)
          browserChanged(c.y - 4, c.y + c.h + 4, c.x - 4, c.x + c.w + 4);
    }
  }
}

// By rating, plays or last played, folders first (by name), then the files;
// those without (unrated, never played) after the others either way, and
// ties by name. Only when the folder is read or the order chosen: a rating
// changed meanwhile doesn't move its row away from the pointer.
void Menu::arrangeByRecords() {
  if (stashCovers_ && fieldOf(sortMode_) == kByName) {  // by what is shown: Stash's titles
    arrangeByTitles();
    return;
  }
  const std::vector<FolderListing::Entry>& entries = listing_.entries();
  if (!FolderListing::byLibrary(sortMode_) || fileKind_ != FileKind::Video ||
      records_.size() != entries.size())
    return;
  const int field = fieldOf(sortMode_);
  const bool down = sortMode_ == kFieldOrders[field].down;
  auto key = [&](size_t i) { return recordKey(field, records_[i], 0); };
  std::vector<size_t> order(entries.size());
  std::iota(order.begin(), order.end(), 0);
  // The listing comes by name, folders first: a stable sort keeps that for ties.
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    if (entries[a].directory || entries[b].directory)
      return entries[a].directory && !entries[b].directory;
    const double ka = key(a), kb = key(b);
    if ((ka > 0) != (kb > 0)) return ka > 0;
    return down ? ka > kb : ka < kb;
  });
  listing_.arrange(order);
  std::vector<VideoRecord> records;
  records.reserve(order.size());
  for (const size_t i : order) records.push_back(records_[i]);
  records_ = std::move(records);
}

void Menu::refreshDirectory() {
  if (!directory().empty()) openDirectory(directory());
}

// Remembers the folder's search and the item in view, if it is listed.
void Menu::saveBrowsing() {
  if (!listShowsFolder_ || directory().empty() || fileKind_ != FileKind::Video ||
      listing_.reading())
    return;
  Browsing& b = browsing_[directory()];
  b.filter = fileFilter_;
  if (!listAnchor(b.anchor, b.rowsFromTop, &b.index)) b.anchor.clear();
}

int Menu::folderColumns() const {
  return gallery_ && fileKind_ == FileKind::Video ? kGalleryColumns : 1;
}

int Menu::folderRows() const {
  if (folderColumns() > 1) return editingFilter() ? kEditingGalleryRows : kGalleryRows;
  return editingFilter() ? kEditingFileRows : kFileRows;
}

// The paths the folder's list shows, in order.
std::vector<std::string> Menu::listedPaths() const {
  std::vector<std::string> paths;
  for (const FolderListing::Entry& entry : listing_.entries())
    if (matchesFilter(entry, fileFilter_)) paths.push_back(entry.path);
  return paths;
}

bool Menu::matchesFilter(const FolderListing::Entry& entry, const std::string& filter) const {
  return stashCovers_ ? stashCovers_->matches(entry, filter) : FolderListing::matches(entry, filter);
}

int Menu::folderMatchCount() const {
  int n = 0;
  for (const FolderListing::Entry& entry : listing_.entries()) n += matchesFilter(entry, fileFilter_);
  return n;
}

// The item to keep in view: the playing file if it is shown whole, else the
// item last scrolled to if it still is (so switching list and thumbnails
// back and forth stays put), else the first item shown whole; and how many
// rows below the list's top it is.
bool Menu::listAnchor(std::string& path, double& rowsFromTop, int* index) const {
  if (!listShowsFolder_) return false;
  const std::vector<std::string> paths = listedPaths();
  const int columns = folderColumns();
  const double top = scrollOffset_ + scrollFraction_;
  int first = -1, last = -1;
  for (int i = 0; i < static_cast<int>(paths.size()); ++i) {
    const double row = i / columns;
    if (row < top - 1e-6 || row > top + folderRows() - 1 + 1e-6) continue;
    if (first < 0) first = i;
    if (paths[i] == lastAnchor_) last = i;
    if (paths[i] == playingFile()) {
      last = i;
      break;
    }
  }
  if (last >= 0) first = last;
  if (first < 0) return false;
  path = paths[first];
  rowsFromTop = first / columns - top;
  if (index) *index = first;
  return true;
}

// Scrolls so the item is that many rows below the list's top (as near as the
// list allows); if it is not listed, the one now at `index` (none: to the
// top).
void Menu::scrollToItem(const std::string& path, double rowsFromTop, int index) {
  lastAnchor_ = path;
  const std::vector<std::string> paths = listedPaths();
  const auto found = std::find(paths.begin(), paths.end(), path);
  const int i = found != paths.end() ? static_cast<int>(found - paths.begin())
                                     : std::min(index, static_cast<int>(paths.size()) - 1);
  double position = 0;
  if (i >= 0) {
    const int columns = folderColumns();
    const int rows = (static_cast<int>(paths.size()) + columns - 1) / columns;
    const double row = i / columns;
    position = std::clamp(row - rowsFromTop, 0.0, std::max(0.0, rows - folderRows() + 0.0));
  }
  scrollOffset_ = static_cast<int>(std::floor(position));
  scrollFraction_ = position - scrollOffset_;
}

// Scrolls as little as needed to show the item whole, if it is listed.
void Menu::scrollIntoView(const std::string& path) {
  const std::vector<std::string> paths = listedPaths();
  const auto found = std::find(paths.begin(), paths.end(), path);
  if (found == paths.end()) return;
  const double row = (found - paths.begin()) / folderColumns();
  const double top = scrollOffset_ + scrollFraction_;
  if (row < top)
    scrollToItem(path, 0);
  else if (row > top + folderRows() - 1)
    scrollToItem(path, folderRows() - 1);
}

ListPlace Menu::listPlace(const std::vector<std::string>& paths, int columns) const {
  ListPlace place;
  const double top = scrollOffset_ + scrollFraction_;
  for (int i = 0; i < static_cast<int>(paths.size()); ++i) {
    const double row = i / columns;
    if (row < top - 1e-6 || row > top + visibleRows_ - 1 + 1e-6) continue;
    const bool playing = paths[i] == playingFile();
    if (place.path.empty() || playing) place = {paths[i], i, row - top};
    if (playing) break;
  }
  return place;
}

void Menu::scrollToPlace(const ListPlace& place, const std::vector<std::string>& paths,
                         int columns) {
  if (place.path.empty()) return;
  const auto found = std::find(paths.begin(), paths.end(), place.path);
  const int i = found != paths.end() ? static_cast<int>(found - paths.begin()) : place.index;
  const double position = std::max(0.0, i / columns - place.rowsFromTop);
  scrollOffset_ = static_cast<int>(std::floor(position));
  scrollFraction_ = position - scrollOffset_;
}

// A folder row: opens the folder.
Menu::ListItem Menu::folderItem(const std::string& label, const std::string& path) {
  ListItem item{label, false, nullptr};
  item.folder = true;
  item.action = [this, path] {
    openDirectory(path);
    return MenuCommand{};
  };
  return item;
}

// A file row: opens the file (a video or photo, or a subtitle file to load).
Menu::ListItem Menu::fileItem(const std::string& label, const std::string& path) {
  ListItem item{label, path == view_.currentFile, nullptr};
  item.action = [this, path] {
    if (fileKind_ == FileKind::Video) return openVideo(path);
    MenuCommand c;
    c.path = path;
    c.type = MenuCommand::LoadSubtitle;
    showPage(Page::Main);
    return c;
  };
  if (fileKind_ == FileKind::Video) item.thumbPath = path;
  return item;
}

std::vector<Menu::ListItem> Menu::fileItems() {
  const std::vector<FolderListing::Entry>& entries = listing_.entries();
  std::vector<size_t> matches;
  for (size_t i = 0; i < entries.size(); ++i)
    if (matchesFilter(entries[i], fileFilter_)) matches.push_back(i);
  const int columns = folderColumns(), rows = folderRows();
  rowHeight_ = columns > 1 ? kGalleryRowH : kFileRowH;
  int first, end;
  clampScroll((static_cast<int>(matches.size()) + columns - 1) / columns, rows, first, end);
  first *= columns;
  end *= columns;
  std::vector<ListItem> items(matches.size());
  const bool rated = fileKind_ == FileKind::Video && library_ && library_->available() &&
                     records_.size() == entries.size();
  for (size_t n = 0; n < matches.size(); ++n) {
    const size_t i = matches[n];
    const FolderListing::Entry& entry = entries[i];
    // Offscreen entries need only their thumbnail path for prefetching.
    if (static_cast<int>(n) < first || static_cast<int>(n) >= end) {
      if (!entry.directory && fileKind_ == FileKind::Video) items[n].thumbPath = entry.path;
      continue;
    }
    if (entry.directory) {
      items[n] = folderItem(entry.name, entry.path);
      continue;
    }
    ListItem item = fileItem(entry.name, entry.path);
    item.size = sizeText(entry.size);
    item.date = dateText(entry.modified);
    if (rated && isVideoFile(entry.path)) {
      const VideoRecord& r = records_[i];
      item.ratingPath = entry.path;
      item.rating = r.rating;
      if (r.playCount > 0) item.plays = std::to_string(r.playCount);
      // Sorted by when they were played, the date shown is that.
      if (fieldOf(sortMode_) == kByPlayed) item.date = dateText(r.lastPlayedAt);
    }
    items[n] = std::move(item);
  }
  return items;
}

void Menu::clampScroll(int count, int rows, int& first, int& end) {
  const int maxOffset = std::max(0, count - rows);
  scrollOffset_ = std::clamp(scrollOffset_, 0, maxOffset);
  if (scrollOffset_ >= maxOffset) scrollFraction_ = 0;
  first = scrollOffset_;
  // And those peeking in at the bottom (cut off at listBottom_), at least one.
  const double shown = rowHeight_ > 0 ? std::ceil((listBottom_ - listTop_) / rowHeight_) : 0;
  end = std::min(count, scrollOffset_ + std::max(rows, static_cast<int>(shown)) + 1);
}

// Rows from listTop_ down; the caller sets listTop_, listBottom_ and listRight_.
void Menu::addList(const std::vector<ListItem>& items, double x0, double rowH, int rows) {
  visibleRows_ = rows;
  rowHeight_ = rowH;
  listCount_ = static_cast<int>(items.size());
  int first, end;
  clampScroll(listCount_, rows, first, end);
  constexpr double trashW = 90;
  for (int i = first; i < end; ++i) {
    Button b;
    b.panel = kBrowser;
    b.x = x0;
    b.y = listTop_ + (i - scrollOffset_ - scrollFraction_) * rowH;
    b.w = listRight_ - x0 - 30;
    b.h = rowH - kRowGap;
    b.label = items[i].label;
    b.selected = items[i].selected;
    b.action = items[i].action;
    b.listRow = true;
    b.scrolls = true;
    setThumbnailOrIcon(b, items[i]);
    b.size = items[i].size;
    b.date = items[i].date;
    b.plays = items[i].plays;
    b.fontSize = kTextSize;
    double right = b.x + b.w;
    if (items[i].trash) {
      Button trash;
      trash.panel = kBrowser;
      trash.x = right - trashW;
      trash.y = b.y;
      trash.w = trashW;
      trash.h = b.h;
      trash.icon = Icon::Trash;
      trash.action = items[i].trash;
      trash.tooltip = text().tipDelete;
      trash.scrolls = true;
      right -= trashW + 10;
      b.w = right - b.x;
      buttons_.push_back(b);
      buttons_.push_back(trash);
    } else {
      buttons_.push_back(b);
    }
    const size_t rowIndex = buttons_.size() - (items[i].trash ? 2 : 1);
    if (!items[i].trash && !items[i].folder && !items[i].thumbPath.empty()) {
      buttons_[rowIndex].offlinePath = items[i].thumbPath;
      if (listColumns_)
        addOfflineButtons(buttons_[rowIndex], listColumns(b.x + b.w).status, 0);
    }
    // A name too long for its row shows in full while pointed at; one from
    // Stash with the file's name under it.
    Button& row = buttons_[rowIndex];
    const std::string fileName = stashCovers_ ? describeFromStash(row, items[i].thumbPath) : "";
    const bool cut = textWidth(row.label, kTextSize, false) >
                     listNameWidth(row) * (row.byline.empty() ? 1 : kBylineShare);
    if (cut) row.tooltip = row.label;
    if (!fileName.empty()) row.tooltip = cut ? row.label + "\n" + fileName : fileName;
    // The stars, in their column (over the row, so they take the pointer there).
    if (listColumns_ && !items[i].ratingPath.empty()) {
      const double x = listColumns(row.x + row.w).rating;
      buttons_.push_back(ratingButton(items[i].ratingPath, items[i].rating, kBrowser, x, b.y + 2,
                                      kRowStarW, b.h - 4));
      buttons_.back().ratingVideo = items[i].ratingVideo;
      buttons_.back().scrolls = true;
    }
  }
  Button row;
  row.h = rowH - kRowGap;
  wantThumbnails(items, first, end, rows, listThumbW(row));
  addScrollbar();
}

// Cards from listTop_ down, `rows` of them on screen; scrolling counts grid rows.
void Menu::addGallery(const std::vector<ListItem>& items, double x0, int rows) {
  constexpr int columns = kGalleryColumns;
  constexpr double gap = kGalleryGap, rowH = kGalleryRowH;
  const double cardW = (listRight_ - 30 - x0 - (columns - 1) * gap) / columns;
  const int count = static_cast<int>(items.size());
  visibleRows_ = rows;
  rowHeight_ = rowH;
  listCount_ = (count + columns - 1) / columns;
  int firstRow, endRow;
  clampScroll(listCount_, visibleRows_, firstRow, endRow);
  const int first = firstRow * columns;
  const int end = std::min(count, endRow * columns);
  for (int i = first; i < end; ++i) {
    const int cell = i - first;
    Button b;
    b.panel = kBrowser;
    b.x = x0 + (cell % columns) * (cardW + gap);
    b.y = listTop_ + (cell / columns - scrollFraction_) * rowH;
    b.w = cardW;
    b.h = rowH - gap;
    b.label = items[i].label;
    b.selected = items[i].selected;
    b.action = items[i].action;
    b.card = true;
    b.scrolls = true;
    setThumbnailOrIcon(b, items[i]);
    b.detail = items[i].size + (items[i].size.empty() ? "" : "   ") + items[i].date;
    b.plays = items[i].plays;
    b.rating = items[i].rating;
    // A name longer than its two lines (one over a byline) shows in full
    // while pointed at; one from Stash with the file's name under it.
    const std::string fileName = stashCovers_ ? describeFromStash(b, items[i].thumbPath) : "";
    const bool cut = textWidth(b.label, 26, false) > (b.byline.empty() ? 1.8 : 0.9) * (b.w - 24);
    if (cut) b.tooltip = b.label;
    if (!fileName.empty()) b.tooltip = cut ? b.label + "\n" + fileName : fileName;
    if (!items[i].folder && !items[i].thumbPath.empty()) b.offlinePath = items[i].thumbPath;
    buttons_.push_back(b);
    if (!items[i].trash && !b.offlinePath.empty()) addOfflineButtons(b, 0, b.y + b.h - 40);
    if (items[i].trash) {  // small, in the card's upper right corner
      Button trash;
      trash.panel = kBrowser;
      trash.x = b.x + b.w - 66;
      trash.y = b.y + 12;
      trash.w = trash.h = 54;
      trash.radius = 27;
      trash.icon = Icon::Trash;
      trash.dangerHover = true;
      trash.action = items[i].trash;
      trash.tooltip = text().tipDelete;
      trash.scrolls = true;
      buttons_.push_back(trash);
    }
    if (!items[i].ratingPath.empty()) {  // the stars over the thumbnail's lower left corner
      Button stars = cardRating(items[i].ratingPath, items[i].rating, b);
      stars.scrolls = true;
      stars.ratingVideo = items[i].ratingVideo;
      buttons_.push_back(stars);
    }
  }
  wantThumbnails(items, first, end, visibleRows_ * columns, cardW - 16);
  addScrollbar();
}

// The list's scroll bar, if it scrolls: clicking or dragging along it scrolls there.
void Menu::addScrollbar() {
  if (listCount_ <= visibleRows_) return;
  Button bar;
  bar.panel = kBrowser;
  bar.x = listRight_ - 24;
  bar.y = listTop_;
  bar.w = 24;
  bar.h = listBottom_ - listTop_ - 6;
  bar.radius = 12;
  bar.bare = true;
  bar.scrollbar = true;
  bar.action = [] { return MenuCommand{}; };  // handled in click()/drag()
  buttons_.push_back(bar);
}

Menu::ScrollExtent Menu::scrollExtent() const {
  if (groupedPage()) {
    const double shown = listBottom_ - listTop_;
    return {pageScroll_, shown, shown + pageScrollMax_};
  }
  return {scrollOffset_ + scrollFraction_, static_cast<double>(visibleRows_),
          static_cast<double>(listCount_)};
}

void Menu::scrollToPointer(const Button& bar) {
  const ScrollExtent e = scrollExtent();
  const double thumb = std::max(40.0, bar.h * e.shown / std::max(1.0, e.total));
  const double fraction = std::clamp((pointerY_ - bar.y - thumb / 2) / (bar.h - thumb), 0.0, 1.0);
  const double position = fraction * std::max(0.0, e.total - e.shown);
  if (groupedPage()) {
    if (std::abs(position - pageScroll_) > 0.01) {
      pageScroll_ = position;
      browserChanged(listTop_ - 4, listBottom_ + 4);
    }
    return;
  }
  // By whole texture pixels, as scroll().
  const double perRow = rowHeight_ * menu_geometry::kRasterScale;
  const double now = scrollOffset_ + scrollFraction_;
  setListPosition(std::clamp(now + std::round((position - now) * perRow) / perRow, 0.0,
                             std::max(0.0, e.total - e.shown)));
}

// A folder's icon; a video's thumbnail, or with thumbnails off its icon
// (and its Stash cover, if it has one: covers read no files).
void Menu::setThumbnailOrIcon(Button& b, const ListItem& item) const {
  if (item.folder) {
    b.icon = Icon::Folder;
  } else if (!view_.thumbnails && !item.thumbPath.empty()) {
    b.icon = Icon::Video;
    if (stashCoversShown()) b.thumbPath = item.thumbPath;
  } else {
    b.thumbPath = item.thumbPath;
  }
}

void Menu::setThumbnailer(Thumbnailer* thumbnailer) {
  thumbnailer_ = thumbnailer;
  // Pasted pixel for pixel (drawThumbnail()).
  constexpr float scale = menu_geometry::kRasterScale;
  thumbnailer->setSizes(static_cast<int>(std::ceil(kCardPictureW * scale)),
                        static_cast<int>(std::ceil(kRowPictureW * scale)));
}

void Menu::wantThumbnails(const std::vector<ListItem>& items, int first, int end, int page,
                          double pictureW) {
  if (!thumbnailer_ || (page_ != Page::Files && page_ != Page::Library && page_ != Page::Offline))
    return;
  if (!view_.thumbnails && !stashCovers_) {  // none read from the files; what was waiting is dropped
    thumbnailer_->want({}, 0);
    return;
  }
  // Most urgent first: on screen, the next pages, the end and the start of
  // the folder (for the jump buttons), a little above. Bounded, so a huge
  // folder doesn't keep the decoder busy for minutes or fill the memory.
  const int count = static_cast<int>(items.size());
  std::vector<std::string> wanted;
  std::set<int> taken;
  auto add = [&](int i) {
    if (i >= 0 && i < count && !items[i].thumbPath.empty() && taken.insert(i).second)
      wanted.push_back(items[i].thumbPath);
  };
  for (int i = first; i < end; ++i) add(i);
  const size_t shown = wanted.size();
  for (int i = end; i < end + std::max(30, page * 3); ++i) add(i);
  for (int i = count - page * 2; i < count; ++i) add(i);
  for (int i = 0; i < page * 2; ++i) add(i);
  for (int i = first - 1; i >= first - page * 2; --i) add(i);
  if (wantedAlready(wanted, shown, pictureW)) return;
  const int width = static_cast<int>(std::ceil(pictureW * menu_geometry::kRasterScale));
  if (!stashCovers_) {
    thumbnailer_->want(wanted, width);
    return;
  }
  // Stash's covers in the same order; frames only of the videos without one.
  wantStashImages(wanted, shown, pictureW);
  std::vector<std::string> frames;
  if (view_.thumbnails)
    for (const std::string& path : wanted)
      if (!stashCovers_->coverComing(path)) frames.push_back(path);
  thumbnailer_->want(frames, width);
}

bool Menu::wantedAlready(const std::vector<std::string>& wanted, size_t shown, double pictureW) {
  // While a list scrolls, what comes into view was asked for a moment ago
  // with the next pages: the order is brought up to date a few times a
  // second. The same is asked again now and then all the same: covers that
  // failed meanwhile leave their videos to the frame thumbnails.
  const double now = steadySeconds();
  if (!(stashCovers_ && stashCovers_->wantsFirst()) && wanted_.pictureW == pictureW &&
      wanted_.thumbnails == view_.thumbnails && wanted_.covers == stashCoversShown()) {
    const auto asked = [&](const std::string& path) {
      return std::find(wanted_.paths.begin(), wanted_.paths.end(), path) != wanted_.paths.end();
    };
    if (now - wanted_.at < 0.5 && wanted_.shown == shown && wanted_.paths == wanted) return true;
    if (now - wanted_.at < 0.15 && std::all_of(wanted.begin(), wanted.begin() + shown, asked))
      return true;
  }
  wanted_ = {wanted, shown, pictureW, now, view_.thumbnails, stashCoversShown()};
  return false;
}

// The toolbar's second row: list and thumbnails, "Sort by"
// with the order's field and direction, the folder's (or the library's)
// search, and to the top / end.
void Menu::layoutFileTools() {
  const Strings& t = text();
  const double y = kBrowserToolsY + toolsShift(), h = kBrowserToolsH;
  const bool library = page_ == Page::Library, videos = library || fileKind_ == FileKind::Video;
  const SortMode mode = library ? librarySort_ : sortMode_;
  double x = kPageX + kPagePad;
  auto icon = [&](Icon i, const char* tip, bool on, std::function<MenuCommand()> action,
                  double left) -> Button& {
    Button& b = addRound(i, tip, std::move(action), left, y, h);
    b.panel = kBrowser;
    b.bare = true;
    b.on = on;
    return b;
  };
  if (videos) {
    // List or thumbnails, the chosen one blue. The item at the top (or the
    // playing one) stays in view.
    auto view = [&](bool gallery, Icon i, const char* tip) {
      icon(i, tip, gallery_ == gallery,
           [this, gallery] {
             if (gallery_ == gallery) return MenuCommand{};
             std::string anchor;
             double rows = 0;
             const bool anchored = listAnchor(anchor, rows);
             gallery_ = gallery;
             if (anchored) scrollToItem(anchor, 0);
             return MenuCommand{};
           },
           x);
      x += h + 8;
    };
    view(false, Icon::List, t.tipList);
    view(true, Icon::Grid, t.tipGallery);
    x += 16;
  }
  if (videos) {
    // "Sort by  Name ▾  ▲": the field (its list of fields opens below it)
    // and the direction (choosing it turns it around); they apply at once.
    Button& label = addLabel(x, y, textWidth(t.sortBy, 26, false) + 8, h, t.sortBy, 26, true);
    x += label.w + 8;
    const int field = fieldOf(mode);
    Button value = textButton(std::string(t.sortFields[field]) + " ▾", Icon::None, [this] {
      dropdown_ = dropdown_ == Dropdown::Sort ? Dropdown::None : Dropdown::Sort;
      return MenuCommand{};
    });
    value.panel = kBrowser;
    value.bare = true;
    value.on = true;  // blue dropdown value
    value.x = x;
    value.y = y;
    value.h = h;
    value.fontSize = 26;
    value.w = textWidth(value.label, 26, false) + 40;
    buttons_.push_back(value);
    x += value.w + 4;
    const FieldOrders& orders = kFieldOrders[field];
    const bool up = mode == orders.up;
    const SortMode turned = up ? orders.down : orders.up;
    Button direction = textButton(up ? "▲" : "▼", Icon::None, [this, turned] {
      sortBy(turned);
      return MenuCommand{};
    });
    direction.panel = kBrowser;
    direction.bare = true;
    direction.on = true;
    direction.x = x;
    direction.y = y;
    direction.w = direction.h = h;
    direction.radius = h / 2;
    direction.fontSize = 26;
    direction.tooltip = t.sortModes[static_cast<int>(mode)];
    buttons_.push_back(direction);
    x += h + 16;
    if (library) layoutLibraryShow(x);
    if (dropdown_ == Dropdown::Sort) {
      // The fields stacked under it, the chosen one blue.
      std::vector<int> fields(std::begin(kFolderFields), std::end(kFolderFields));
      if (library) fields.assign(std::begin(kLibraryFields), std::end(kLibraryFields));
      double w = 0;
      for (const int f : fields) w = std::max(w, textWidth(t.sortFields[f], 26, false));
      for (size_t i = 0; i < fields.size(); ++i) {
        const int f = fields[i];
        Button e = textButton(t.sortFields[f], Icon::None,
                              [this, f] {
                                dropdown_ = Dropdown::None;
                                sortByField(f);
                                return MenuCommand{};
                              },
                              f == field);
        e.panel = kBrowser;
        e.popup = true;
        e.fontSize = 26;
        e.radius = glass() ? 28 : 16;
        e.x = value.x;
        e.y = y + h + 16 + i * 64;
        e.w = w + 60;
        e.h = 56;
        buttons_.push_back(e);
      }
    }
  }

  // To the top / end, at the right end.
  double right = kPageX + kPageWidth - kPagePad - h;
  icon(Icon::JumpBottom, t.tipBottom, false,
       [this] {
         scrollOffset_ = 1 << 30;  // clamped to the last page by the layout
         return MenuCommand{};
       },
       right);
  right -= h + 8;
  icon(Icon::JumpTop, t.tipTop, false,
       [this] {
         scrollOffset_ = 0;
         scrollFraction_ = 0;
         return MenuCommand{};
       },
       right);
  right -= 16;

  // The search: what it finds is listed as you type. The field opens the
  // keyboard; Clear (in its right end) empties it.
  const bool editing = editingFilter();
  const std::string& filter = filterText();
  Button search = textButton(filter.empty() && !editing
                                 ? std::string(library ? t.tipSearchLibrary : t.tipSearchFolder)
                                 : filter,
                             Icon::Search,
                             [this] {
                               editing_ = true;
                               return MenuCommand{};
                             },
                             false);
  search.panel = kBrowser;
  search.x = x;
  search.y = y;
  search.w = right - x;
  search.h = h;
  search.fontSize = kTextSize;
  search.field = true;
  search.on = editing;  // blue outline while typing
  search.caret = editing ? caret() : -1;
  search.placeholder = filter.empty() && !editing;
  search.tooltip = library ? t.tipSearchLibrary : t.tipSearchFolder;
  search.radius = 12;
  searchRight_ = right;
  buttons_.push_back(search);
  if (!filter.empty()) {
    Button clear = textButton(t.clear, Icon::None, [this] {
      setFilter("");
      return MenuCommand{};
    });
    clear.panel = kBrowser;
    clear.bare = true;
    clear.on = true;
    clear.fontSize = 26;
    clear.w = textWidth(t.clear, 26, false) + 40;
    clear.x = right - clear.w - 6;
    clear.y = y + 4;
    clear.h = h - 8;
    clear.radius = clear.h / 2;
    buttons_.push_back(clear);
    searchRight_ = clear.x;
  }
}

// The list's column headings: Name, Rating, Plays, Size and Date modified
// (in the library: Date added; Last played while sorted by it). The one it
// is sorted by is blue with its direction; choosing it again turns the
// direction around, choosing another keeps the direction.
void Menu::layoutListHeader(double x0, double rowW) {
  const Strings& t = text();
  const ListColumns c = listColumns(x0 + rowW);
  const bool library = page_ == Page::Library;
  const SortMode mode = library ? librarySort_ : sortMode_;
  const int sorted = fieldOf(mode);
  auto heading = [&](int field, double x, double w, bool rightAligned) {
    const bool on = field == sorted;
    const FieldOrders& orders = kFieldOrders[field];
    std::string label = t.sortFields[field];
    if (on) label += mode == orders.up ? "  ▲" : "  ▼";
    Button b = textButton(label, Icon::None, [this, field] {
      sortByField(field);
      return MenuCommand{};
    });
    b.panel = kBrowser;
    b.bare = true;
    b.on = on;
    b.fontSize = 24;
    b.w = textWidth(label, 24, false) + 24;
    b.x = rightAligned ? x + w - b.w + 12 : x - 12;
    b.y = kListHeaderY + toolsShift();
    b.h = kListHeaderH;
    b.radius = 10;
    b.tooltip = on ? t.sortModes[static_cast<int>(mode)] : t.tipSortColumn;
    buttons_.push_back(b);
  };
  Button row;
  row.h = kFileRowH - kRowGap;
  heading(kByName, x0 + 10 + listThumbW(row) + 16, 400, false);
  if (offline_) {  // the status column's: the Offline place
    Button& place = addRound(Icon::Download, t.offline.nav,
                             [this] {
                               showOffline();
                               return MenuCommand{};
                             },
                             c.status, kListHeaderY + toolsShift() + 2, kStatusColumnW);
    place.panel = kBrowser;
    place.bare = true;
  }
  heading(kByRating, c.rating + 6, kRatingColumnW, false);
  heading(kByPlays, c.plays, kPlaysColumnW, true);
  heading(kBySize, c.size, kSizeColumnW, true);
  heading(sorted == kByPlayed ? kByPlayed : library ? kByAdded : kByDate, c.date, kDateColumnW,
          false);
}

// Sorts the folder at once, keeping the search and the item in view; or the
// library view, from its top.
void Menu::sortBy(SortMode mode) {
  const bool library = page_ == Page::Library;
  (library ? libraryDescending_ : sortDescending_) = mode == kFieldOrders[fieldOf(mode)].down;
  if (library) {
    librarySort_ = mode;
    sortLibrary();
    scrollOffset_ = 0;
    scrollFraction_ = 0;
    dirty_ = true;
    return;
  }
  sortMode_ = mode;
  rememberFolderView();
  openDirectory(directory());  // reads dates or sizes if needed
}

void Menu::sortByField(int field) {
  const bool library = page_ == Page::Library;
  const SortMode mode = library ? librarySort_ : sortMode_;
  const FieldOrders& orders = kFieldOrders[field];
  if (field == fieldOf(mode))
    sortBy(mode == orders.up ? orders.down : orders.up);
  else
    sortBy((library ? libraryDescending_ : sortDescending_) ? orders.down : orders.up);
}

// The browser's page for the folder: up and the location;
// list or thumbnails, the order (with thumbnails) and the search; then the
// list with its column headings, or four columns of thumbnails.
void Menu::layoutFilesPage() {
  const Strings& t = text();
  const double listX = kPageX + kPagePad;
  listRight_ = kPageX + kPageWidth - kPagePad + 6;
  listShowsFolder_ = true;
  listColumns_ = false;

  Button& up = addRound(Icon::FolderUp, t.tipParent,
                        [this] {
                          const std::string parent =
                              remote::isRemote(directory()) ? remote::parent(directory())
                                                      : fs::path(directory()).parent_path().string();
                          if (!parent.empty())
                            openDirectory(parent);
                          else if (remote::isRemote(directory()))
                            showNetwork();
                          return MenuCommand{};
                        },
                        kPageX + kPagePad, kBrowserRowY, kBrowserRowH);
  up.panel = kBrowser;
  up.bare = true;
  // The location: hovering shows all of it.
  const double whereRight = stashFolders_.picking
                                ? layoutStashFolderChoice()
                                : kPageX + kPageWidth - kPagePad - kBrowserRowH - 16;
  Button where;
  where.panel = kBrowser;
  where.x = kPageX + kPagePad + kBrowserRowH + 16;
  where.y = kBrowserRowY;
  where.w = whereRight - where.x;
  headingRight_ = whereRight;
  where.h = kBrowserRowH;
  where.bare = true;
  where.label = " ";  // drawn by drawFilesHeading()
  where.tooltip = shownLocation(directory());
  where.action = [] { return MenuCommand{}; };
  buttons_.push_back(where);

  const bool pinned = places_.pinned(directory());
  if (!stashFolders_.picking) {
  Button& pin = addRound(Icon::Pin, pinned ? t.unpinFolder : t.pinFolder,
                         [this] {
                           places_.togglePin(directory());
                           refreshPlaces();
                           return MenuCommand{};
                         },
                         kPageX + kPageWidth - kPagePad - kBrowserRowH,
                         kBrowserRowY, kBrowserRowH);
  pin.panel = kBrowser;
  pin.bare = true;
  pin.on = pinned;
  pin.disabled = !places_.ready() || directory().find('\n') != std::string::npos;
  }

  if (smbForm_.kind != SmbForm::Kind::None) {
    listCount_ = 0;
    layoutSmbForm(kBrowserToolsY);
    return;
  }
  // While typing, the keyboard takes the bottom of the page.
  const bool editing = editingFilter();
  listBottom_ = editing ? kKeyboardY - 16 : kListBottom;
  if (gallery_ && fileKind_ == FileKind::Video) {
    listTop_ = kGridTop;
    addGallery(fileItems(), listX, editing ? kEditingGalleryRows : kGalleryRows);
  } else {
    listTop_ = kListTop;
    listColumns_ = true;
    addList(fileItems(), listX, kFileRowH, editing ? kEditingFileRows : kFileRows);
    layoutListHeader(listX, listRight_ - listX - 30);
  }
  if (editing) layoutKeyboard();
  layoutFileTools();
}

// The location and the folder search's count.
void Menu::drawFilesHeading(cairo_t* cr) {
  const Strings& t = text();
  const Palette& pal = palette();
  const double x = kPageX + kPagePad + kBrowserRowH + 24;
  std::string where = shownLocation(directory());
  if (fileKind_ == FileKind::Subtitle) where = std::string(t.chooseSubtitleFile) + ":  " + where;
  if (stashFolders_.picking) {
    const StashFoldersStrings& s = t.stashFolders;
    where = std::string(stashFolders_.notThere ? s.notInFolder : s.pickTitle) + "  " + where;
  }
  drawText(cr, where, x, kBrowserRowY, headingRight_ - x, kBrowserRowH, kTextSize, false, false,
           pal.text.r, pal.text.g, pal.text.b);
  if (!fileFilter_.empty()) {
    char count[64];
    std::snprintf(count, sizeof(count), t.matches, folderMatchCount(),
                  static_cast<int>(listing_.entries().size()));
    const double w = textWidth(count, 24, false);
    drawText(cr, count, searchRight_ - w - 16, kBrowserToolsY, w + 2, kBrowserToolsH, 24, false,
             false, pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a);
  }
}

// Over everything: what an empty list means.
void Menu::drawListStates(cairo_t* cr) {
  const Strings& t = text();
  const Palette& pal = palette();
  const char* empty = networkState();
  if (smbForm_.kind != SmbForm::Kind::None)
    return;
  else if (page_ == Page::Files && listing_.reading() && listing_.entries().empty())
    empty = t.readingFolder;
  else if (page_ == Page::Files && listing_.status() == remote::Status::Offline)
    empty = t.network.offline;
  else if (page_ == Page::Files && listing_.unavailable())
    empty = t.folderUnavailable;
  else if (page_ == Page::Files && listing_.entries().empty())
    empty = fileKind_ == FileKind::Video ? t.noVideos : t.noSubtitleFiles;
  char noMatches[512];
  if (!empty && page_ == Page::Files && folderMatchCount() == 0) {
    std::snprintf(noMatches, sizeof(noMatches), t.noMatches, fileFilter_.c_str());
    empty = noMatches;
  }
  if (page_ == Page::Library && !libraryPinHelp() && libraryMatchCount() == 0) {
    if (libraryFilter_.empty()) {
      empty = stashCovers_ && !stashCatalog() ? t.libraryEmptyStash : t.libraryEmpty;
    } else {
      std::snprintf(noMatches, sizeof(noMatches), t.libraryNoMatches, libraryFilter_.c_str());
      empty = noMatches;
    }
  }
  if (empty)
    drawText(cr, empty, kPageX + kPagePad + 24, listTop_, kPageWidth - 2 * kPagePad - 48,
             kFileRowH, 27, false, false, pal.textDim.r, pal.textDim.g, pal.textDim.b,
             pal.textDim.a);
}

// A list's scroll bar: the track, and the visible part of the list on it.
void Menu::drawScrollbar(cairo_t* cr, const Button& bar) const {
  double x = 0, w = 0;
  scrollbarTrack(bar, x, w);
  roundedRect(cr, x, bar.y, w, bar.h, w / 2);
  cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
  cairo_fill(cr);
  const ScrollExtent e = scrollExtent();
  const double thumb = std::max(40.0, bar.h * e.shown / e.total);
  const double y = bar.y + (bar.h - thumb) * std::min(1.0, e.position / (e.total - e.shown));
  roundedRect(cr, x, y, w, thumb, w / 2);
  cairo_set_source_rgba(cr, 1, 1, 1, 0.55);
  cairo_fill(cr);
}
