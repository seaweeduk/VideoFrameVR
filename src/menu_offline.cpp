#include "menu_parts.h"

#include "menu_list_parts.h"

#include "file_util.h"
#include "menu_stash.h"  // stashNumber
#include "menu_stash_covers.h"
#include "platform.h"
#include "stash.h"
#include "stash_stream.h"

#include <cmath>
#include <cstdarg>

// Offline downloads in the menu (docs/alpha/offline-downloads.md): the
// Offline place after the Library, the status of a video on its row or tile
// (its download button while pointed at, the tick once downloaded, the
// state on its picture while it is to go), the questions before a download
// (which drive, and when it won't fit) and before removing one. The copying
// itself is Offline's (offline.h), on its own threads.

namespace {

// How Menu::Button::offlineLook draws a button.
enum class OfflineLook { None, Download, Row, Running, Drive, DriveChoice, Box };

// Room a download leaves on its drive, as Offline keeps it.
constexpr uint64_t kSpare = 64ull << 20;
// The drives: tiles in a well on the tools row.
constexpr double kDrivesH = 100;
// Done rows' columns before Remove's cell: the drive (at least this wide,
// its name after its icon), size and date.
constexpr double kDriveColumnW = 190, kDriveTextX = 36;
// The one copying: Pause and Cancel at its end.
constexpr double kRunningButtonH = 48;
// A download's bar, and a drive's.
constexpr double kBarH = kit::kSliderTrackH;

std::string printed(const char* format, ...) __attribute__((format(printf, 1, 2)));
std::string printed(const char* format, ...) {
  char buf[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  return buf;
}

std::string baseName(const std::string& path) { return path.substr(path.find_last_of('/') + 1); }

int percentOf(const Offline::Download& d) {
  return static_cast<int>(std::floor(std::clamp(d.fraction(), 0.0, 1.0) * 100));
}

bool running(const Offline::Download& d) {
  return d.state == Offline::State::Copying || d.state == Offline::State::Paused;
}

}  // namespace

void Menu::drawProgressBar(cairo_t* cr, double x, double y, double w, double fraction,
                           bool paused) const {
  drawFinish(cr, Finish::Track, x, y, w, kBarH, kBarH / 2);
  const double done = w * std::clamp(fraction, 0.0, 1.0);
  if (done < 1) return;
  if (!paused) {
    drawAccentFill(cr, x, y, std::max(kBarH, done), kBarH, 1, kBarH / 2, w);
    return;
  }
  roundedRect(cr, x, y, std::max(kBarH, done), kBarH, kBarH / 2);
  setColor(cr, palette().textDim, 0.7);
  cairo_fill(cr);
}

void Menu::setOffline(Offline* offline) {
  offline_ = offline;
  if (stashCovers_) stashCovers_->setOffline(offline);
}

void Menu::showOffline() {
  if (!offline_) return;
  showPage(Page::Offline);
  scrollOffset_ = static_cast<int>(std::floor(offlineScroll_));
  scrollFraction_ = offlineScroll_ - scrollOffset_;
}

// The library's oshash of a file, else Stash's.
std::string Menu::oshashFor(const std::string& path) const {
  std::string oshash = library_ ? library_->oshashOf(path) : std::string();
  if (oshash.empty() && stash_ && stash_->catalog())
    if (const int scene = stashSceneAt(*stash_->catalog(), path); scene >= 0)
      oshash = stash_->catalog()->scenes()[scene].oshash;
  return oshash;
}

// As listed: in the folder, the library or Stash; 0 if not known.
uint64_t Menu::sizeFor(const std::string& path) const {
  for (const FolderListing::Entry& e : listing_.entries())
    if (e.path == path && e.size > 0) return e.size;
  for (const LibraryItem& item : libraryItems_)
    if (item.entry.path == path && item.entry.size > 0) return item.entry.size;
  if (stash_ && stash_->catalog())
    if (const int scene = stashSceneAt(*stash_->catalog(), path); scene >= 0)
      return stash_->catalog()->scenes()[scene].size;
  return 0;
}

const Offline::Download* Menu::downloadOf(const std::string& path) const {
  if (!offline_ || path.empty()) return nullptr;
  return offline_->find(path, oshashFor(path));
}

// A video file here, on a share or Stash's stream that answers, not
// downloaded or queued; its name UTF-8, as the list of downloads keeps it.
bool Menu::downloadable(const std::string& path) const {
  if (!offline_ || path.empty() || !isVideoFile(path)) return false;
  if (!remote::isRemote(path) && !isStashScene(path) && path[0] != '/') return false;
  if (!validUtf8(path)) return false;
  return !offline_->serverDown(path) && !downloadOf(path);
}

// Downloaded and there now; not shown where its server doesn't answer (all
// that shows there plays from this device).
bool Menu::downloadedMark(const std::string& path) const {
  const Offline::Download* d = downloadOf(path);
  return d && d->state == Offline::State::Done && d->present && !offline_->serverDown(d->source);
}

std::string Menu::downloadRoot() const {
  if (!offline_) return {};
  if (!view_.downloadsTo.empty() && offline_->drive(view_.downloadsTo)) return view_.downloadsTo;
  for (const Offline::Drive& d : offline_->drives())
    if (d.internal) return d.root;
  return offline_->drives().empty() ? std::string() : offline_->drives().front().root;
}

std::string Menu::driveName(const Offline::Drive& d, bool full) const {
  const Strings& t = text();
  if (d.internal) return t.offline.internal;
  const char* kind = d.card ? t.drives.card : t.drives.usbDrive;
  if (d.label.empty()) return kind;
  return full ? std::string(kind) + " · " + d.label : d.label;
}

std::string Menu::driveName(const std::string& root, bool full) const {
  if (const Offline::Drive* d = offline_ ? offline_->drive(root) : nullptr) return driveName(*d, full);
  return baseName(root);
}

Menu::Icon Menu::driveIcon(const Offline::Drive& d) const {
  return d.internal ? Icon::Storage : d.card ? Icon::Card : Icon::Storage;
}

namespace {

// "2 min", "1 h 05 min", "under a minute".
std::string timeLeft(double seconds) {
  const OfflineStrings& t = strings().offline;
  if (seconds < 60) return t.underMinute;
  const int minutes = static_cast<int>(std::ceil(seconds / 60));
  if (minutes < 60) return printed(t.minutes, minutes);
  return printed(t.hours, minutes / 60, minutes % 60);
}

// Bytes a drive can take now, after what is queued for it.
uint64_t roomOn(const Offline& offline, const Offline::Drive& d) {
  const uint64_t taken = offline.pending(d.root) + kSpare;
  return d.free > taken ? d.free - taken : 0;
}

}  // namespace

// The download button: the first time with a card or USB drive in, the
// question where downloads go; else to the drive chosen.
MenuCommand Menu::startDownload(const std::string& path) {
  if (!offline_ || !downloadable(path)) return {};
  offlineAsk_ = {};
  offlineAsk_.path = path;
  offlineAsk_.size = sizeFor(path);
  offlineAsk_.oshash = oshashFor(path);
  const bool removable = std::any_of(offline_->drives().begin(), offline_->drives().end(),
                                     [](const Offline::Drive& d) { return !d.internal; });
  if (view_.downloadsTo.empty() && removable) {
    openDialog(Dialog::SaveTo, path);
    return {};
  }
  return downloadTo(downloadRoot());
}

// Queues the download for the drive, or says it won't fit (and where it would).
MenuCommand Menu::downloadTo(const std::string& root) {
  const OfflineAsk ask = offlineAsk_;
  const Offline::Drive* drive = offline_->drive(root);
  if (drive && ask.size > 0 && ask.size > roomOn(*offline_, *drive)) {
    offlineAsk_.root = root;
    offlineAsk_.other.clear();
    for (const Offline::Drive& d : offline_->drives())  // internal storage first
      if (d.root != root && ask.size <= roomOn(*offline_, d)) {
        offlineAsk_.other = d.root;
        break;
      }
    openDialog(Dialog::NoRoom, ask.path);
    return {};
  }
  offline_->add(ask.path, ask.size, ask.oshash, root);
  dirty_ = true;
  return {};
}

// What changed: anything but the progress redraws the browser; progress
// alone only the rows, tiles and parts showing it.
void Menu::offlineChanged() {
  std::string shape = std::string(offline_->pausedAll() ? "P" : "") + (offline_->stashDown() ? "S" : "");
  for (const Offline::Download& d : offline_->downloads())
    shape += std::to_string(d.id) + ":" + std::to_string(static_cast<int>(d.state)) +
             std::to_string(static_cast<int>(d.why)) + (d.present ? "+" : "-") + d.root + "|";
  for (const Offline::Drive& d : offline_->drives()) shape += d.root + "|";
  for (const std::string& host : offline_->downServers()) shape += "!" + host;
  if (shape != offlineShape_) {
    offlineShape_ = shape;
    ++offlineShapeGeneration_;
    // Away from home, the library shows the copies in place of what's on the server.
    if (!libraryItems_.empty() && placeLibraryItems()) sortLibrary();
    if (page_ != Page::Main) browserChanged();
    return;
  }
  if (page_ == Page::Main) return;
  if (page_ == Page::Offline) browserChanged(0, listTop_ - 4);  // the summary and the drives
  for (const Button& b : buttons_) {
    if (b.panel != kBrowser) continue;
    const Offline::Download* d = nullptr;
    if (b.downloadId)
      for (const Offline::Download& x : offline_->downloads())
        if (x.id == b.downloadId) d = &x;
    if (!d && !b.offlinePath.empty() && b.offlineLook != static_cast<int>(OfflineLook::Drive))
      d = downloadOf(b.offlinePath);
    if (d && d->state == Offline::State::Copying)
      browserChanged(b.y - 2, b.y + b.h + 2, b.x - 2, b.x + b.w + 2);
  }
}

void Menu::addOfflineButtons(const Button& item, double statusX, double lastLineY) {
  const Button row = item;  // pushing may move it
  const std::string& path = row.offlinePath;
  if (!offline_ || path.empty()) return;
  const OfflineStrings& t = text().offline;
  const bool tile = row.card;
  double tx, ty, tw, th;
  listPicture(row, tx, ty, tw, th);
  Button b;
  b.panel = row.panel;
  b.scrolls = row.scrolls;
  b.side = row.side;
  b.offlinePath = path;
  const Offline::Download* d = downloadOf(path);
  if (!d) {
    if (!downloadable(path)) return;
    b.offlineLook = static_cast<int>(OfflineLook::Download);
    b.hoverOnly = true;  // right after its row: shown while either is pointed at
    b.tooltip = t.tipDownload;
    if (tile) {
      b.w = b.h = 54;
      b.x = tx + tw - b.w - 10;
      b.y = ty + 10;
    } else {
      b.w = b.h = std::min(kStatusColumnW, row.h);
      b.x = statusX + (kStatusColumnW - b.w) / 2;
      b.y = row.y + (row.h - b.h) / 2;
    }
    b.radius = b.w / 2;
    b.action = [this, path] { return startDownload(path); };
    buttons_.push_back(b);
    return;
  }
  b.bare = true;
  b.action = [this] {
    showOffline();
    return MenuCommand{};
  };
  if (d->state == Offline::State::Done) {
    if (!downloadedMark(path)) return;
    b.tooltip = printed(t.tipDownloaded, driveName(d->root, true).c_str());
    if (tile) {
      b.x = row.x + kit::kSpace - 8, b.y = lastLineY - 2, b.w = 38, b.h = 34;
    } else {
      b.x = statusX, b.y = row.y, b.w = kStatusColumnW, b.h = row.h;
    }
    b.radius = 8;
  } else {  // its state on the picture
    b.tooltip = t.tipShow;
    if (tile) {
      b.w = tw * 0.5, b.h = th * 0.42;
      b.x = tx + (tw - b.w) / 2, b.y = ty + (th - b.h) / 2 - 8;
      b.radius = 12;
    } else {
      b.x = tx, b.y = ty, b.w = tw, b.h = th;
      b.radius = kit::kPieceCorner;
    }
  }
  buttons_.push_back(b);
}

// A video to go: its picture darkened, and its state on it. No rings:
// "Queued", or a thin bar along the bottom (blue while copying, grey while
// paused) with the percent on tiles. List rows show an icon instead of words.
void Menu::drawOfflineState(cairo_t* cr, const Button& b, double x, double y, double w,
                            double h) const {
  if (!offline_ || b.offlinePath.empty()) return;
  const Offline::Download* d = downloadOf(b.offlinePath);
  if (!d || d->state == Offline::State::Done) return;
  const OfflineStrings& t = text().offline;
  const Palette& pal = palette();
  const bool tile = b.card;
  const double radius = tile || !b.stashLook ? kit::kPieceCorner : 5;
  cairo_save(cr);
  roundedRect(cr, x, y, w, h, radius);
  cairo_clip_preserve(cr);
  cairo_set_source_rgba(cr, 0, 0, 0, 0.58);
  cairo_fill(cr);
  const bool bar = running(*d) || (d->state == Offline::State::Waiting && d->copied > 0);
  if (bar) {
    const double barH = tile ? 6 : 4;
    cairo_rectangle(cr, x, y + h - barH, w, barH);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.22);
    cairo_fill(cr);
    cairo_rectangle(cr, x, y + h - barH, w * std::clamp(d->fraction(), 0.0, 1.0), barH);
    if (d->state == Offline::State::Copying)
      setColor(cr, pal.accent);
    else
      setColor(cr, pal.textDim);
    cairo_fill(cr);
  }
  const Rgba ink = d->state == Offline::State::Failed ? pal.danger
                   : d->state == Offline::State::Paused ? pal.textDim
                                                        : Rgba{1, 1, 1, 1};
  if (!tile) {  // too small for words: an icon
    const bool failed = d->state == Offline::State::Failed;
    if (failed || d->state == Offline::State::Queued || d->state == Offline::State::Waiting)
      drawIcon(cr, failed ? Icon::Warning : Icon::Download, x + w / 2, y + h / 2,
               std::min(w, h) * 0.6, ink.r, ink.g, ink.b);
    cairo_restore(cr);
    return;
  }
  std::string label;
  if (d->state == Offline::State::Queued) label = t.shortStates[0];
  else if (d->state == Offline::State::Waiting) label = t.shortStates[1];
  else if (d->state == Offline::State::Failed) label = t.shortStates[2];
  else label = std::to_string(percentOf(*d)) + " %";
  drawText(cr, label, x + 4, y, w - 8, h, kit::kGroupTitleSize, true, true, ink.r, ink.g, ink.b, ink.a);
  cairo_restore(cr);
}

void Menu::drawOfflineTick(cairo_t* cr, double x, double cy, double size) const {
  const Palette& pal = palette();
  drawIcon(cr, Icon::Check, x, cy, size, pal.textDim.r, pal.textDim.g, pal.textDim.b);
}

// A drive's space on a sunk track: its offline videos in the accent, the
// rest used in grey.
void Menu::drawDriveBar(cairo_t* cr, const Offline::Drive& d, double x, double y, double w,
                        double h) const {
  drawFinish(cr, Finish::Track, x, y, w, h, h / 2);
  if (d.total == 0) return;
  const double used = std::min(1.0, static_cast<double>(d.total - std::min(d.free, d.total)) / d.total);
  const double mine = std::min(used, static_cast<double>(d.used) / d.total);
  if (w * used >= 1) {
    cairo_save(cr);
    cairo_rectangle(cr, x, y, w * used, h);
    cairo_clip(cr);
    roundedRect(cr, x, y, w, h, h / 2);
    setColor(cr, palette().textDim, 0.55);
    cairo_fill(cr);
    cairo_restore(cr);
  }
  if (w * mine >= 1) drawAccentFill(cr, x, y, w * mine, h, 1, h / 2, w);
}

namespace {

// What is to go (and how long it takes) or here.
std::string offlineSummary(const Offline& offline) {
  const OfflineStrings& t = strings().offline;
  int toGo = 0, here = 0;
  uint64_t left = 0, kept = 0;
  double speed = 0;
  for (const Offline::Download& d : offline.downloads()) {
    if (d.state == Offline::State::Done) {
      here += d.present;
      if (d.present) kept += d.size;
      continue;
    }
    ++toGo;
    left += d.size > d.copied ? d.size - d.copied : 0;
    if (d.state == Offline::State::Copying) speed = d.speed;
  }
  if (toGo > 0 && speed > 0)
    return printed(t.summaryTime, stashNumber(toGo).c_str(), sizeText(left).c_str(),
                   timeLeft(left / speed).c_str());
  if (toGo > 0) return printed(t.summary, stashNumber(toGo).c_str(), sizeText(left).c_str());
  if (here > 0) return printed(t.summaryDone, stashNumber(here).c_str(), sizeText(kept).c_str());
  return {};
}

}  // namespace

// The Offline place: its header (Pause all); the drives in a well, the one
// new downloads go to (Settings' Save downloads to) chosen; then the list:
// To go, the one copying taller with its bar and its Pause and Cancel, and
// On this device, newest first, Remove at the end of the row pointed at.
void Menu::layoutOfflinePage() {
  const Strings& base = text();
  const OfflineStrings& t = base.offline;
  contentEdges();
  listShowsFolder_ = false;
  listColumns_ = false;
  if (!offline_) return;
  double right = kContentRight;
  if (offline_->toGo() > 0) {
    const bool paused = offline_->pausedAll();
    headerButton(right, paused ? t.resumeAll : t.pauseAll, [this, paused] {
      offline_->pauseAll(!paused);
      return MenuCommand{};
    });
  }
  contentTitle(t.nav, offlineSummary(*offline_), right);

  // The drives, side by side: pieces of a well.
  const std::vector<Offline::Drive>& drives = offline_->drives();
  if (!drives.empty()) {
    constexpr double in = kit::kSpacePiece;
    Button well;
    well.panel = kBrowser;
    well.well = true;
    well.x = kContentLeft;
    well.y = kBrowserToolsY;
    well.w = kContentRight - kContentLeft;
    well.h = kDrivesH;
    well.radius = kit::kCorner;
    buttons_.push_back(well);
    const std::string chosen = downloadRoot();
    const double w = (well.w - in * (drives.size() + 1)) / drives.size();
    for (size_t i = 0; i < drives.size(); ++i) {
      Button tile;
      tile.panel = kBrowser;
      tile.piece = true;
      tile.x = well.x + in + i * (w + in);
      tile.y = well.y + in;
      tile.w = w;
      tile.h = well.h - 2 * in;
      tile.radius = kit::kPieceCorner;
      tile.offlineLook = static_cast<int>(OfflineLook::Drive);
      tile.offlinePath = drives[i].root;
      tile.selected = drives[i].root == chosen;
      tile.action = [root = drives[i].root] {
        MenuCommand c;
        c.type = MenuCommand::SetDownloadsTo;
        c.path = root;
        return c;
      };
      buttons_.push_back(tile);
    }
  }

  listTop_ = drives.empty() ? kBrowserToolsY : kListTop;
  listBottom_ = kListBottom;
  // The list, in rows of kFileRowH: headings, the one copying taking two.
  struct Entry {
    const Offline::Download* d = nullptr;  // null: a heading
    std::string heading;
    int units = 1;
  };
  std::vector<const Offline::Download*> toGo, done;
  for (const Offline::Download& d : offline_->downloads())
    (d.state == Offline::State::Done ? done : toGo).push_back(&d);
  std::stable_sort(done.begin(), done.end(), [](const auto* a, const auto* b) {
    if (a->present != b->present) return a->present;
    return a->doneAt > b->doneAt;
  });
  std::vector<Entry> entries;
  if (!toGo.empty()) {
    entries.push_back({nullptr, std::string(t.toGo) + "   " + stashNumber(static_cast<long>(toGo.size())), 1});
    for (const auto* d : toGo) entries.push_back({d, "", running(*d) ? 2 : 1});
  }
  if (!done.empty()) {
    int present = 0;
    for (const auto* d : done) present += d->present;
    entries.push_back({nullptr, std::string(t.onDevice) + "   " + stashNumber(present), 1});
    for (const auto* d : done) entries.push_back({d, "", 1});
  }
  int units = 0;
  for (const Entry& e : entries) units += e.units;
  rowHeight_ = kFileRowH;
  visibleRows_ = std::max(1, static_cast<int>((listBottom_ - listTop_) / kFileRowH));
  listCount_ = units;
  int first, end;
  clampScroll(listCount_, visibleRows_, first, end);
  const double x0 = listLeft_, rowW = listRight_ - 30 - x0;
  constexpr double rowH = kFileRowH - kRowGap;
  // The drive column: as wide as the longest drive name in it needs, the
  // columns taking at most half the row.
  driveColumnW_ = kDriveColumnW;
  std::vector<std::string> roots;
  for (const auto* d : done)
    if (std::find(roots.begin(), roots.end(), d->root) == roots.end()) roots.push_back(d->root);
  for (const std::string& root : roots) {
    const Offline::Drive* drive = offline_->drive(root);
    const std::string name = drive ? driveName(*drive, false) : baseName(root);
    driveColumnW_ = std::max(driveColumnW_, kDriveTextX + std::ceil(textWidth(name, 24, false)) + 2);
  }
  const double most = rowW / 2 - kSizeColumnW - kDateColumnW - 2 * kColumnGap;
  driveColumnW_ = std::min(driveColumnW_, std::max(kDriveColumnW, most));
  std::vector<ListItem> items;
  int firstItem = -1, endItem = 0;
  int at = 0;
  for (const Entry& e : entries) {
    const int from = at;
    at += e.units;
    if (e.d) items.push_back(ListItem{"", false, nullptr});
    // A scene from Stash's stream as it plays now (its source names it on disk).
    const std::string source = !e.d ? std::string()
                               : isStashSource(e.d->source) ? stashSourceLocator(e.d->source)
                                                            : e.d->source;
    if (e.d)
      items.back().thumbPath =
          e.d->present && (source.empty() || offline_->serverDown(source)) ? e.d->path : source;
    if (at <= first || from >= end) continue;
    const double y = listTop_ + (from - scrollOffset_ - scrollFraction_) * kFileRowH;
    if (!e.d) {  // a heading, at the bottom of its row, over the rows it heads
      Button heading;
      heading.panel = kBrowser;
      heading.x = x0;
      heading.y = y + rowH - kListHeaderH;
      heading.w = rowW;
      heading.h = kListHeaderH;
      heading.label = e.heading;
      heading.listLook = static_cast<int>(ListLook::Section);
      heading.scrolls = true;
      buttons_.push_back(heading);
      continue;
    }
    if (firstItem < 0) firstItem = static_cast<int>(items.size()) - 1;
    endItem = static_cast<int>(items.size());
    const Offline::Download& d = *e.d;
    Button row;
    row.panel = kBrowser;
    row.x = x0;
    row.y = y;
    row.w = rowW;
    row.h = e.units * kFileRowH - kRowGap;
    row.radius = kit::kCorner;
    row.listRow = true;
    row.scrolls = true;
    row.fontSize = kit::kRowSize;
    row.downloadId = d.id;
    row.offlineLook = static_cast<int>(e.units == 2 ? OfflineLook::Running : OfflineLook::Row);
    row.label = baseName(d.source);
    setThumbnailOrIcon(row, items.back());
    const std::string fileName = stashCovers_ ? describeFromStash(row, d.source) : "";
    if (!fileName.empty()) row.tooltip = fileName;
    const std::string play = d.present ? d.path : source;  // none: Stash's other server
    row.selected = !play.empty() && (play == view_.currentFile || d.source == view_.currentFile);
    row.action = [this, play] {
      if (play.empty()) return MenuCommand{};
      return openVideo(play);
    };
    buttons_.push_back(row);
    const size_t rowIndex = buttons_.size() - 1;
    const int64_t id = d.id;
    double end = row.x + row.w - kit::kSpace;  // the buttons in its end, right to left
    if (e.units == 1) {  // Remove or Cancel, round, while it is pointed at
      const bool remove = d.state == Offline::State::Done;
      Button& button = addRound(remove ? Icon::Trash : Icon::Close, remove ? t.tipRemove : t.tipCancel,
                                [this, id, remove, source = d.source] {
                                  if (!remove) {
                                    offline_->cancel(id);
                                    return MenuCommand{};
                                  }
                                  offlineAsk_ = {};
                                  offlineAsk_.id = id;
                                  openDialog(Dialog::RemoveDownload, source);
                                  return MenuCommand{};
                                },
                                end - kRowRoundSize, y + (rowH - kRowRoundSize) / 2, kRowRoundSize);
      button.panel = kBrowser;
      button.bare = true;
      button.hoverOnly = true;
      button.scrolls = true;
      button.iconSize = 24;
      button.dangerHover = remove;
      end = button.x - kColumnGap;
    }
    if (d.state == Offline::State::Failed) {
      Button again = textButton(t.tryAgain, Icon::None, [this, id] {
        offline_->retry(id);
        return MenuCommand{};
      });
      again.panel = kBrowser;
      again.fontSize = kit::kWellSize;
      again.w = textWidth(t.tryAgain, kit::kWellSize, false) + 2 * kButtonPadX;
      again.h = kRowRoundSize;
      again.x = end - again.w;
      again.y = y + (rowH - again.h) / 2;
      again.radius = kit::kPieceCorner;
      again.scrolls = true;
      buttons_.push_back(again);
      end = again.x - kColumnGap;
    }
    if (e.units == 2) {  // Pause (all, as one copies at a time) and Cancel
      const bool paused = offline_->pausedAll();
      Button cancel = textButton(base.cancel, Icon::None, [this, id] {
        offline_->cancel(id);
        return MenuCommand{};
      });
      Button pause = textButton(paused ? t.resume : t.pause, Icon::None, [this, paused] {
        offline_->pauseAll(!paused);
        return MenuCommand{};
      });
      for (Button* b : {&cancel, &pause}) {
        b->panel = kBrowser;
        b->fontSize = kit::kWellSize;
        b->w = textWidth(b->label, kit::kWellSize, false) + 2 * kButtonPadX;
        b->h = kRunningButtonH;
        b->x = end - b->w;
        b->y = row.y + (row.h - b->h) / 2;
        b->radius = kit::kCorner;
        b->scrolls = true;
        end = b->x - kit::kSpaceTight;
      }
      buttons_.push_back(pause);
      buttons_.push_back(cancel);
      end += kit::kSpaceTight - kit::kSpaceEdge;
    }
    // What the row draws stops before its buttons (drawDownloadRow()).
    buttons_[rowIndex].inner = row.x + row.w - kit::kSpace - end;
  }
  Button probe;
  probe.h = rowH;
  if (firstItem >= 0) wantThumbnails(items, firstItem, endItem, visibleRows_, listThumbW(probe));
  addScrollbar();
}

// "Offline" and what is to go or here (layoutOfflinePage()); with nothing
// yet, what to do (drawListStates()).
void Menu::drawOfflineHeading(cairo_t* cr) {
  if (offline_) drawPageHead(cr);
}

// A download's row: its picture and name; the one copying with its bar and
// how it goes under its name; the state or, once done, the drive, size and
// date at the right, before its buttons.
void Menu::drawDownloadRow(cairo_t* cr, const Button& b, const double ink[4]) const {
  const OfflineStrings& t = text().offline;
  const Palette& pal = palette();
  const double dim[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a};
  const Offline::Download* d = nullptr;
  for (const Offline::Download& x : offline_->downloads())
    if (x.id == b.downloadId) d = &x;
  if (!d) return;
  const bool gone = d->state == Offline::State::Done && !d->present;
  if (gone) cairo_push_group(cr);
  double tx, ty, tw, th;
  listPicture(b, tx, ty, tw, th);
  const double nameX = tx + tw + kRowNameGap;
  double end = b.x + b.w - kit::kSpace - b.inner;  // where the name may reach
  if (static_cast<OfflineLook>(b.offlineLook) == OfflineLook::Running) {
    const bool paused = d->state == Offline::State::Paused;
    const std::string drive = driveName(d->root, false);
    const std::string copied = sizeText(d->copied), total = sizeText(d->size);
    std::string line;
    if (paused)
      line = printed(offline_->pausedAll() ? t.paused : t.pausedPlaying, percentOf(*d), copied.c_str(),
                     total.c_str());
    else if (d->speed > 0 && d->size > d->copied)
      line = printed(t.copying, percentOf(*d), copied.c_str(), total.c_str(),
                     sizeText(static_cast<uint64_t>(d->speed)).c_str(),
                     timeLeft((d->size - d->copied) / d->speed).c_str(), drive.c_str());
    else
      line = printed(t.copyingStart, percentOf(*d), copied.c_str(), total.c_str(), drive.c_str());
    drawRowLead(cr, b, ink, end, b.y + 6, 40);
    drawProgressBar(cr, nameX, b.y + 52, end - nameX, d->fraction(), paused);
    drawText(cr, line, nameX, b.y + 64, end - nameX, 36, kit::kGroupTitleSize, false, false, dim[0],
             dim[1], dim[2]);
    return;
  }
  if (d->state == Offline::State::Done) {
    // The drive, the size and when it was downloaded; or that it isn't there.
    const double dateX = end - kDateColumnW;
    const double sizeX = dateX - kColumnGap - kSizeColumnW;
    const double driveX = sizeX - kColumnGap - driveColumnW_;
    const Offline::Drive* drive = offline_->drive(d->root);
    drawIcon(cr, drive ? driveIcon(*drive) : Icon::Card, driveX + 14, b.y + b.h / 2, 24, dim[0], dim[1],
             dim[2]);
    drawText(cr, drive ? driveName(*drive, false) : baseName(d->root), driveX + kDriveTextX, b.y,
             driveColumnW_ - kDriveTextX, b.h, 24, false, false, dim[0], dim[1], dim[2]);
    if (gone) {
      drawText(cr, t.missing, sizeX, b.y, end - sizeX, b.h, 24, false, false, dim[0], dim[1], dim[2]);
    } else {
      const std::string bytes = sizeText(d->size);
      const double w = textWidth(bytes, 24, false);
      drawText(cr, bytes, sizeX + kSizeColumnW - w, b.y, w + 2, b.h, 24, false, false, dim[0], dim[1],
               dim[2]);
      drawText(cr, dateText(d->doneAt), dateX, b.y, kDateColumnW, b.h, 24, false, false, dim[0],
               dim[1], dim[2]);
    }
    end = driveX - kColumnGap;
  } else {  // its state, at the right
    std::string state;
    Rgba stateInk = pal.textDim;
    if (d->state == Offline::State::Queued) {
      state = printed(t.queuedSize, sizeText(d->size).c_str());
    } else if (d->state == Offline::State::Waiting) {
      state = d->why == Offline::Why::Drive    ? t.waitingDrive
              : d->why == Offline::Why::Broken ? t.restarting
                                               : t.waitingServer;
      if (d->size) state += " · " + sizeText(d->size);
    } else if (d->state == Offline::State::Failed) {
      state = d->why == Offline::Why::NoRoom ? t.failedNoRoom : t.failedGone;
      stateInk = pal.danger;
    }
    if (!state.empty()) {
      const double w = std::min(textWidth(state, 24, false) + 2, (end - nameX) * 0.5);
      drawText(cr, state, end - w, b.y, w, b.h, 24, false, false, stateInk.r, stateInk.g, stateInk.b);
      end -= w + kColumnGap;
    }
  }
  drawRowLead(cr, b, ink, end, b.y, b.h);
  if (gone) {
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, 0.5);
  }
}

// A drive as a piece of the Offline place's well: its icon, name and (the
// chosen one) "New downloads"; its bar, and what its offline videos take
// and what is free.
void Menu::drawDriveTile(cairo_t* cr, const Button& b, const double ink[4]) const {
  const OfflineStrings& t = text().offline;
  const Palette& pal = palette();
  const Offline::Drive* drive = offline_->drive(b.offlinePath);
  if (!drive) return;
  constexpr double pad = kit::kSpace, icon = 28, lineSize = kit::kGroupTitleSize;
  const Rgba& iconInk = b.selected ? pal.textAccent : pal.textDim;
  drawIcon(cr, driveIcon(*drive), b.x + pad + icon / 2, b.y + 26, icon, iconInk.r, iconInk.g, iconInk.b);
  const double nameX = b.x + pad + icon + 12;
  double nameEnd = b.x + b.w - pad;
  if (b.selected) {
    const double w = textWidth(t.newDownloads, lineSize, false) + 2;
    drawText(cr, t.newDownloads, nameEnd - w, b.y + 6, w, 40, lineSize, false, false, pal.textDim.r,
             pal.textDim.g, pal.textDim.b);
    nameEnd -= w + kit::kSpace;
  }
  drawText(cr, driveName(*drive, false), nameX, b.y + 6, nameEnd - nameX, 40, kit::kRowSize, false,
           false, ink[0], ink[1], ink[2]);
  drawDriveBar(cr, *drive, b.x + pad, b.y + 50, b.w - 2 * pad, kBarH);
  // A key in the accent before what the offline videos take, then what is free.
  const std::string mine = printed(t.offlineVideos, drive->used ? sizeText(drive->used).c_str() : "0");
  const std::string free =
      "·  " + printed(t.freeOf, sizeText(drive->free).c_str(), sizeText(drive->total).c_str());
  const double lineY = b.y + 60, lineH = 28;
  roundedRect(cr, b.x + pad, lineY + lineH / 2 - 5, 10, 10, 2);
  setColor(cr, pal.accent);
  cairo_fill(cr);
  const double mineW = textWidth(mine, lineSize, false);
  drawText(cr, mine, b.x + pad + 18, lineY, mineW + 4, lineH, lineSize, false, false, pal.textDim.r,
           pal.textDim.g, pal.textDim.b);
  const double freeX = b.x + pad + 18 + mineW + 12;
  drawText(cr, free, freeX, lineY, b.x + b.w - pad - freeX, lineH, lineSize, false, false, ink[0],
           ink[1], ink[2]);
}

void Menu::drawOfflineButton(cairo_t* cr, const Button& b, const double ink[4]) const {
  const Strings& base = text();
  const OfflineStrings& t = base.offline;
  const Palette& pal = palette();
  const double dim[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a};
  switch (static_cast<OfflineLook>(b.offlineLook)) {
    case OfflineLook::Download: {
      if (b.w >= 50) {  // on a picture: a dark disc under it
        cairo_arc(cr, b.x + b.w / 2, b.y + b.h / 2, b.w / 2, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
        cairo_fill(cr);
        drawIcon(cr, Icon::Download, b.x + b.w / 2, b.y + b.h / 2, b.w * 0.56, 1, 1, 1);
      } else {
        drawIcon(cr, Icon::Download, b.x + b.w / 2, b.y + b.h / 2, 26, ink[0], ink[1], ink[2]);
      }
      return;
    }
    case OfflineLook::Row:
    case OfflineLook::Running:
      drawDownloadRow(cr, b, ink);
      return;
    case OfflineLook::Drive:
      drawDriveTile(cr, b, ink);
      return;
    case OfflineLook::DriveChoice: {  // Save to's: name, what it is, the bar, its room
      const Offline::Drive* drive = offline_->drive(b.offlinePath);
      if (!drive) return;
      drawFinish(cr, Finish::Raised, b.x, b.y, b.w, b.h, b.radius);
      if (b.on) {
        roundedRect(cr, b.x + 1.5, b.y + 1.5, b.w - 3, b.h - 3, b.radius);
        setColor(cr, pal.edge);
        cairo_set_line_width(cr, 3);
        cairo_stroke(cr);
      }
      constexpr double pad = 28;
      const std::string free = printed(t.freeOf, sizeText(drive->free).c_str(),
                                       sizeText(drive->total).c_str());
      drawIcon(cr, driveIcon(*drive), b.x + pad + 16, b.y + 44, 34, 1, 1, 1);
      drawText(cr, drive->internal ? t.internal : drive->card ? base.drives.card : base.drives.usbDrive,
               b.x + pad + 48, b.y + 20, b.w - 2 * pad - 48, 48, 30, true, false, 1, 1, 1);
      const std::string what = drive->internal
                                   ? (kStandaloneHeadset ? t.internalHeadset : t.internalComputer)
                                   : drive->label;
      drawText(cr, what, b.x + pad, b.y + 72, b.w - 2 * pad, 36, 24, false, false, dim[0], dim[1],
               dim[2]);
      drawDriveBar(cr, *drive, b.x + pad, b.y + 121, b.w - 2 * pad, kBarH);
      drawText(cr, free, b.x + pad, b.y + 140, b.w - 2 * pad, 36, 24, false, false, 1, 1, 1);
      return;
    }
    case OfflineLook::Box: {  // the Scene tab's: "Offline", then Download or the state
      const Offline::Download* own = downloadOf(b.offlinePath);
      drawFinish(cr, Finish::Sunk, b.x, b.y, b.w, b.h, b.radius);
      const double pad = 20;
      drawText(cr, t.nav, b.x + pad, b.y + 10, b.w - 2 * pad, 40, 26, true, false, 1, 1, 1);
      std::string line;
      Rgba lineInk = pal.textDim;
      bool bar = false;
      if (!own) {
        const uint64_t bytes = sizeFor(b.offlinePath);
        line = bytes ? std::string(t.download) + " · " + sizeText(bytes) : t.download;
        lineInk = pal.textAccent;
        drawIcon(cr, Icon::Download, b.x + pad + 12, b.y + 74, 26, lineInk.r, lineInk.g, lineInk.b);
        drawText(cr, line, b.x + pad + 36, b.y + 54, b.w - 2 * pad - 36, 40, 24, false, false,
                 lineInk.r, lineInk.g, lineInk.b);
        return;
      }
      const std::string copied = sizeText(own->copied), total = sizeText(own->size);
      switch (own->state) {
        case Offline::State::Queued: line = printed(t.queuedSize, total.c_str()); break;
        case Offline::State::Copying:
          bar = true;
          line = own->speed > 0 && own->size > own->copied
                     ? printed(t.copying, percentOf(*own), copied.c_str(), total.c_str(),
                               sizeText(static_cast<uint64_t>(own->speed)).c_str(),
                               timeLeft((own->size - own->copied) / own->speed).c_str(),
                               driveName(own->root, false).c_str())
                     : printed(t.copyingStart, percentOf(*own), copied.c_str(), total.c_str(),
                               driveName(own->root, false).c_str());
          break;
        case Offline::State::Paused:
          bar = true;
          line = printed(offline_->pausedAll() ? t.paused : t.pausedPlaying, percentOf(*own),
                         copied.c_str(), total.c_str());
          break;
        case Offline::State::Waiting:
          line = own->why == Offline::Why::Drive ? t.waitingDrive : t.waitingServer;
          break;
        case Offline::State::Failed:
          line = own->why == Offline::Why::NoRoom ? t.failedNoRoom : t.failedGone;
          lineInk = pal.danger;
          break;
        case Offline::State::Done:
          line = own->present ? printed(t.downloaded, driveName(own->root, true).c_str()) : t.missing;
          if (own->present) drawOfflineTick(cr, b.x + pad + 12, b.y + 74, 24);
          break;
      }
      const double lineX = own->state == Offline::State::Done && own->present ? b.x + pad + 36 : b.x + pad;
      drawText(cr, line, lineX, b.y + 54, b.x + b.w - pad - lineX, 40, 22, false, false, lineInk.r,
               lineInk.g, lineInk.b);
      if (bar)
        drawProgressBar(cr, b.x + pad, b.y + b.h - 28, b.w - 2 * pad, own->fraction(),
                        own->state == Offline::State::Paused);
      return;
    }
    case OfflineLook::None:
      return;
  }
}

void Menu::addOfflineBox(const std::string& path, double x, double y, double w, double h) {
  if (!offline_ || (!downloadOf(path) && !downloadable(path))) return;
  Button b;
  b.panel = kBrowser;
  b.x = x;
  b.y = y;
  b.w = w;
  b.h = h;
  b.radius = 18;
  b.bare = true;
  b.offlineLook = static_cast<int>(OfflineLook::Box);
  b.offlinePath = path;
  b.action = [this, path] {
    if (!downloadOf(path)) return startDownload(path);
    showOffline();
    return MenuCommand{};
  };
  b.tooltip = downloadOf(path) ? text().offline.tipShow : "";
  buttons_.push_back(b);
}

// Save downloads to (the drives as cards to pick from), Won't fit, and
// Remove a download.
void Menu::layoutOfflineDialog(DialogLayout& dialog) {
  using Ink = DialogLayout::Ink;
  const Strings& base = text();
  const OfflineStrings& t = base.offline;
  auto close = [this] {
    dialog_ = Dialog::None;
    return MenuCommand{};
  };
  if (!offline_) {
    dialog.button(textButton(base.cancel, Icon::None, close));
    return;
  }
  const std::string name = [&] {
    Button b;
    b.label = baseName(dialogPath_);
    if (stashCovers_) describeFromStash(b, dialogPath_);
    return b.label;
  }();
  dialog.button(textButton(base.cancel, Icon::None, close));
  if (dialog_ == Dialog::SaveTo) {
    dialog.title(t.saveToTitle);
    dialog.line(offlineAsk_.size ? name + " · " + sizeText(offlineAsk_.size) : name, Ink::Strong, 2);
    // One press picks the drive, remembers it and downloads there.
    const std::vector<Offline::Drive>& drives = offline_->drives();
    constexpr double cardH = 190;
    dialog.room(cardH + kit::kSpace, [this, drives](double x, double y, double w) {
      const double gap = kit::kSpace;
      const double each = (w - gap * (drives.size() - 1)) / drives.size();
      for (size_t i = 0; i < drives.size(); ++i) {
        Button card;
        card.panel = kBrowser;
        card.popup = true;
        card.x = x + i * (each + gap);
        card.y = y + kit::kSpaceTight;
        card.w = each;
        card.h = cardH;
        card.radius = kit::kGroupCorner;
        card.offlineLook = static_cast<int>(OfflineLook::DriveChoice);
        card.offlinePath = drives[i].root;
        card.action = [this, root = drives[i].root] {
          dialog_ = Dialog::None;
          downloadTo(root);
          MenuCommand c;
          c.type = MenuCommand::SetDownloadsTo;
          c.path = root;
          return c;
        };
        buttons_.push_back(card);
      }
    });
    dialog.line(t.saveToHint);
  } else if (dialog_ == Dialog::NoRoom) {
    const Offline::Drive* drive = offline_->drive(offlineAsk_.root);
    const std::string driveText = drive ? driveName(*drive, false) : baseName(offlineAsk_.root);
    dialog.title(printed(t.noRoomTitle, driveText.c_str()));
    dialog.line(name, Ink::Strong, 2);
    const uint64_t room = drive ? roomOn(*offline_, *drive) : 0;
    dialog.line(printed(t.needs, sizeText(offlineAsk_.size).c_str(), room ? sizeText(room).c_str() : "0"),
                Ink::Warn);
    dialog.line(offlineAsk_.other.empty()
                    ? std::string(t.noRoomHintOnly)
                    : printed(t.noRoomHint, driveName(offlineAsk_.other, false).c_str()),
                Ink::Dim, 2);
    const Offline::Drive* other = offlineAsk_.other.empty() ? nullptr : offline_->drive(offlineAsk_.other);
    if (!other) return;
    dialog.button(textButton(printed(t.instead, driveName(*other, false).c_str(),
                                     sizeText(roomOn(*offline_, *other)).c_str()),
                             driveIcon(*other), [this] {
                               dialog_ = Dialog::None;
                               const OfflineAsk& a = offlineAsk_;
                               offline_->add(a.path, a.size, a.oshash, a.other);
                               return MenuCommand{};
                             },
                             true));
  } else if (dialog_ == Dialog::RemoveDownload) {
    const Offline::Download* d = nullptr;
    for (const Offline::Download& x : offline_->downloads())
      if (x.id == offlineAsk_.id) d = &x;
    dialog.title(t.removeTitle);
    if (d)
      dialog.line(printed(t.removeWhat, name.c_str(), sizeText(d->size).c_str(),
                          driveName(d->root, false).c_str()),
                  Ink::Strong, 2);
    dialog.line(remote::isRemote(dialogPath_)
                    ? printed(t.removeWarning, serverName(remote::root(dialogPath_)).c_str())
                    : std::string(t.removeWarningLocal),
                Ink::Warn);
    Button remove = textButton(t.remove, Icon::Trash, [this] {
      dialog_ = Dialog::None;
      offline_->remove(offlineAsk_.id);
      return MenuCommand{};
    });
    remove.danger = true;
    dialog.button(remove);
  }
}
