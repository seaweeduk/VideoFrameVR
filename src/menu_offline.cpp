#include "menu_style.h"

#include "file_util.h"
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
constexpr double kCardsY = 92, kCardH = 104;
constexpr double kRowEndW = 64;  // right of the place's rows: Cancel or Remove while pointed at
// Done rows' columns from their right end: the drive, size and date.
constexpr double kDriveColumnW = 190;

constexpr Rgba kFailedText = {1.0, 0.55, 0.5, 1};
constexpr Rgba kPausedBar = {0.62, 0.62, 0.66, 1};

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

// A download's bar: blue while copying, grey while paused, on a dark track.
void progressBar(cairo_t* cr, double x, double y, double w, double fraction, bool paused,
                 const Palette& pal) {
  roundedRect(cr, x, y, w, 6, 3);
  cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
  cairo_fill(cr);
  const double done = w * std::clamp(fraction, 0.0, 1.0);
  if (done < 1) return;
  roundedRect(cr, x, y, std::max(6.0, done), 6, 3);
  setColor(cr, paused ? kPausedBar : pal.accent);
  cairo_fill(cr);
}

}  // namespace

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
// downloaded or queued.
bool Menu::downloadable(const std::string& path) const {
  if (!offline_ || path.empty() || !isVideoFile(path)) return false;
  if (!remote::isRemote(path) && !isStashScene(path) && path[0] != '/') return false;
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
  if (tile) {
    tw = row.w - 16, th = tw * 9 / 16, tx = row.x + 8, ty = row.y + 8;
  } else {
    th = row.h - 4, tw = listThumbW(row), tx = row.x + 10, ty = row.y + 2;
  }
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
      b.x = row.x + 6, b.y = lastLineY - 2, b.w = 38, b.h = 34;
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
      b.radius = 5;
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
  const double radius = tile ? 10 : 5;
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
      setColor(cr, kPausedBar);
    cairo_fill(cr);
  }
  const Rgba ink = d->state == Offline::State::Failed ? kFailedText
                   : d->state == Offline::State::Paused ? kPausedBar
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
  bool bold = true;
  if (d->state == Offline::State::Queued) label = t.shortStates[0];
  else if (d->state == Offline::State::Waiting) label = t.shortStates[1];
  else if (d->state == Offline::State::Failed) label = t.shortStates[2];
  else label = std::to_string(percentOf(*d)) + " %";
  const int size = running(*d) ? 32 : 28;
  drawText(cr, label, x + 4, y - 8, w - 8, h, size, bold, true, ink.r, ink.g, ink.b, ink.a);
  cairo_restore(cr);
}

void Menu::drawOfflineTick(cairo_t* cr, double x, double cy, double size) const {
  const Palette& pal = palette();
  drawIcon(cr, Icon::Check, x, cy, size, pal.textDim.r, pal.textDim.g, pal.textDim.b);
}

// A drive's space: its offline videos in blue, the rest used in grey.
void Menu::drawDriveBar(cairo_t* cr, const Offline::Drive& d, double x, double y, double w,
                        double h) const {
  const Palette& pal = palette();
  roundedRect(cr, x, y, w, h, h / 2);
  cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
  cairo_fill(cr);
  if (d.total == 0) return;
  const double used = std::min(1.0, static_cast<double>(d.total - std::min(d.free, d.total)) / d.total);
  const double mine = std::min(used, static_cast<double>(d.used) / d.total);
  cairo_save(cr);
  roundedRect(cr, x, y, w, h, h / 2);
  cairo_clip(cr);
  cairo_rectangle(cr, x, y, w * used, h);
  cairo_set_source_rgba(cr, 0.55, 0.55, 0.58, 1);
  cairo_fill(cr);
  cairo_rectangle(cr, x, y, w * mine, h);
  setColor(cr, pal.accent);
  cairo_fill(cr);
  cairo_restore(cr);
}

// The Offline place: Pause all; the drives, the one marked New downloads
// where they go (Settings' Save downloads to); then the list: To go, the one
// copying taller with its bar and its Pause and Cancel, and On this device,
// newest first, Remove on the row pointed at.
void Menu::layoutOfflinePage() {
  const Strings& base = text();
  const OfflineStrings& t = base.offline;
  const double x0 = kPageX + kPagePad;
  listRight_ = kPageX + kPageWidth - kPagePad + 6;
  listShowsFolder_ = false;
  listColumns_ = false;
  if (!offline_) return;
  const double right = kPageX + kPageWidth - kPagePad;
  if (offline_->toGo() > 0) {
    const bool paused = offline_->pausedAll();
    Button pause = textButton(paused ? t.resumeAll : t.pauseAll, paused ? Icon::PlayLine : Icon::PauseLine,
                              [this, paused] {
                                offline_->pauseAll(!paused);
                                return MenuCommand{};
                              });
    pause.panel = kBrowser;
    pause.x = right - pause.w;
    pause.y = kBrowserRowY;
    pause.h = kBrowserRowH;
    buttons_.push_back(pause);
  }

  // The drives, side by side.
  const std::vector<Offline::Drive>& drives = offline_->drives();
  const std::string chosen = downloadRoot();
  const double gap = 16;
  const double cardW = drives.empty() ? 0 : (right - x0 - gap * (drives.size() - 1)) / drives.size();
  for (size_t i = 0; i < drives.size(); ++i) {
    Button card;
    card.panel = kBrowser;
    card.x = x0 + i * (cardW + gap);
    card.y = kCardsY;
    card.w = cardW;
    card.h = kCardH;
    card.offlineLook = static_cast<int>(OfflineLook::Drive);
    card.offlinePath = drives[i].root;
    card.on = drives[i].root == chosen;
    card.radius = 16;
    card.action = [root = drives[i].root] {
      MenuCommand c;
      c.type = MenuCommand::SetDownloadsTo;
      c.path = root;
      return c;
    };
    buttons_.push_back(card);
  }

  listTop_ = drives.empty() ? kCardsY : kCardsY + kCardH + 18;
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
    entries.push_back({nullptr, std::string(t.toGo) + "   " + std::to_string(toGo.size()), 1});
    for (const auto* d : toGo) entries.push_back({d, "", running(*d) ? 2 : 1});
  }
  if (!done.empty()) {
    int present = 0;
    for (const auto* d : done) present += d->present;
    entries.push_back({nullptr, std::string(t.onDevice) + "   " + std::to_string(present), 1});
    for (const auto* d : done) entries.push_back({d, "", 1});
  }
  int units = 0;
  for (const Entry& e : entries) units += e.units;
  rowHeight_ = kFileRowH;
  visibleRows_ = std::max(1, static_cast<int>((listBottom_ - listTop_) / kFileRowH));
  listCount_ = units;
  int first, end;
  clampScroll(listCount_, visibleRows_, first, end);
  const double rowW = listRight_ - 30 - kRowEndW - x0;
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
    if (!e.d) {
      Button& heading = addLabel(x0 + 8, y, rowW - 8, kFileRowH - kRowGap, e.heading, kTextSize, true);
      heading.scrolls = true;
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
    row.listRow = true;
    row.scrolls = true;
    row.fontSize = kTextSize;
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
    const int64_t id = d.id;
    // Right of the row while it is pointed at: Remove, or Cancel.
    if (d.state == Offline::State::Done || e.units == 1) {
      const bool remove = d.state == Offline::State::Done;
      Button& end = addRound(remove ? Icon::Trash : Icon::Close, remove ? t.tipRemove : t.tipCancel,
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
                             x0 + rowW + 10, y, row.h);
      end.panel = kBrowser;
      end.bare = true;
      end.hoverOnly = true;
      end.scrolls = true;
      end.dangerHover = remove;
    }
    if (d.state == Offline::State::Failed) {
      Button again = textButton(t.tryAgain, Icon::None, [this, id] {
        offline_->retry(id);
        return MenuCommand{};
      });
      again.panel = kBrowser;
      again.fontSize = 24;
      again.w = textWidth(t.tryAgain, 24, false) + 40;
      again.x = row.x + row.w - again.w - 6;
      again.y = row.y + 4;
      again.h = row.h - 8;
      again.radius = again.h / 2;
      again.scrolls = true;
      buttons_.push_back(again);
    }
    if (e.units == 2) {  // Pause (all, as one copies at a time) and Cancel
      const bool paused = offline_->pausedAll();
      Button cancel = textButton(base.cancel, Icon::Close, [this, id] {
        offline_->cancel(id);
        return MenuCommand{};
      });
      Button pause = textButton(paused ? t.resume : t.pause, paused ? Icon::PlayLine : Icon::PauseLine,
                                [this, paused] {
                                  offline_->pauseAll(!paused);
                                  return MenuCommand{};
                                });
      double bx = row.x + row.w - 12;
      for (Button* b : {&cancel, &pause}) {
        b->panel = kBrowser;
        b->h = 56;
        b->x = bx - b->w;
        b->y = row.y + (row.h - b->h) / 2;
        b->scrolls = true;
        b->plain = true;
        bx = b->x - 12;
      }
      buttons_.push_back(pause);
      buttons_.push_back(cancel);
    }
  }
  Button probe;
  probe.h = kFileRowH - kRowGap;
  if (firstItem >= 0) wantThumbnails(items, firstItem, endItem, visibleRows_, listThumbW(probe));
  addScrollbar();
}

// "Offline", what is to go or here; with nothing yet, what to do.
void Menu::drawOfflineHeading(cairo_t* cr) {
  if (!offline_) return;
  const OfflineStrings& t = text().offline;
  const Palette& pal = palette();
  const double x = kPageX + kPagePad + 8;
  drawText(cr, t.nav, x, kBrowserRowY, 400, kBrowserRowH, 30, true, false, pal.text.r, pal.text.g,
           pal.text.b);
  const double titleW = textWidth(t.nav, 30, true);
  int toGo = 0, here = 0;
  uint64_t left = 0, kept = 0;
  double speed = 0;
  for (const Offline::Download& d : offline_->downloads()) {
    if (d.state == Offline::State::Done) {
      here += d.present;
      if (d.present) kept += d.size;
      continue;
    }
    ++toGo;
    left += d.size > d.copied ? d.size - d.copied : 0;
    if (d.state == Offline::State::Copying) speed = d.speed;
  }
  std::string summary;
  if (toGo > 0 && speed > 0)
    summary = printed(t.summaryTime, toGo, sizeText(left).c_str(), timeLeft(left / speed).c_str());
  else if (toGo > 0)
    summary = printed(t.summary, toGo, sizeText(left).c_str());
  else if (here > 0)
    summary = printed(t.summaryDone, here, sizeText(kept).c_str());
  drawText(cr, summary, x + titleW + 36, kBrowserRowY, 1000, kBrowserRowH, 24, false, false,
           pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a);
  if (offline_->downloads().empty()) {
    const double dim[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a};
    drawTextLines(cr, t.empty, kPageX + kPagePad + 24, listTop_ + 12, kPageWidth - 2 * kPagePad - 48,
                  27, 2, dim, false);
  }
}

void Menu::drawOfflineButton(cairo_t* cr, const Button& b, const double ink[4]) const {
  const Strings& base = text();
  const OfflineStrings& t = base.offline;
  const Palette& pal = palette();
  const double dim[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a};
  const Offline::Download* d = nullptr;
  if (b.downloadId)
    for (const Offline::Download& x : offline_->downloads())
      if (x.id == b.downloadId) d = &x;
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
    case OfflineLook::Running: {
      if (!d) return;
      const bool tall = static_cast<OfflineLook>(b.offlineLook) == OfflineLook::Running;
      const bool gone = d->state == Offline::State::Done && !d->present;
      if (gone) cairo_push_group(cr);
      const double th = b.h - (tall ? 8 : 4), tw = (view_.thumbnails || stashCoversShown()) ? th * 16 / 9 : th;
      const double tx = b.x + (tall ? 8 : 10), ty = b.y + (b.h - th) / 2;
      if (!b.thumbPath.empty() && (b.icon == Icon::None || pictureOf(b.thumbPath, tw)))
        drawThumbnail(cr, b.thumbPath, tx, ty, tw, th, tall ? 8 : 5);
      else if (b.icon != Icon::None)
        drawIcon(cr, b.icon, tx + tw / 2, ty + th / 2, th * 0.7, ink[0], ink[1], ink[2]);
      const double nameX = tx + tw + 16;
      double end = b.x + b.w - 16;  // where the name may reach
      // What shows at the right end, from there.
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
        stateInk = kFailedText;
        end -= textWidth(t.tryAgain, 24, false) + 40 + 12;
      }
      const int size = b.fontSize > 0 ? b.fontSize : kTextSize;
      if (tall) {
        const bool paused = d->state == Offline::State::Paused;
        double buttons = 12;
        for (const Button& x : {textButton(base.cancel, Icon::Close, nullptr),
                                textButton(offline_->pausedAll() ? t.resume : t.pause,
                                           Icon::PauseLine, nullptr)})
          buttons += x.w + 12;
        end = b.x + b.w - buttons - 20;
        const std::string drive = driveName(d->root, false);
        const std::string copied = sizeText(d->copied), total = sizeText(d->size);
        std::string line;
        if (paused)
          line = printed(offline_->pausedAll() ? t.paused : t.pausedPlaying, percentOf(*d),
                         copied.c_str(), total.c_str());
        else if (d->speed > 0 && d->size > d->copied)
          line = printed(t.copying, percentOf(*d), copied.c_str(), total.c_str(),
                         sizeText(static_cast<uint64_t>(d->speed)).c_str(),
                         timeLeft((d->size - d->copied) / d->speed).c_str(), drive.c_str());
        else
          line = printed(t.copyingStart, percentOf(*d), copied.c_str(), total.c_str(), drive.c_str());
        const double nameW = end - nameX;
        const double labelW = std::min(textWidth(b.label, size, false) + 2,
                                       b.byline.empty() ? nameW : nameW * kBylineShare);
        drawText(cr, b.label, nameX, b.y + 4, labelW, 40, size, false, false, ink[0], ink[1], ink[2]);
        if (!b.byline.empty() && nameW - labelW - 18 > 60)
          drawText(cr, b.byline, nameX + labelW + 18, b.y + 4, nameW - labelW - 18, 40, 24, false,
                   false, dim[0], dim[1], dim[2]);
        progressBar(cr, nameX, b.y + b.h / 2 - 1, end - nameX, d->fraction(), paused, pal);
        drawText(cr, line, nameX, b.y + b.h / 2 + 10, end - nameX, 36, 22, false, false, dim[0],
                 dim[1], dim[2]);
        return;
      }
      if (d->state == Offline::State::Done) {
        // The drive, the size and when it was downloaded; or that it isn't there.
        const double dateX = b.x + b.w - 16 - kDateColumnW;
        const double sizeX = dateX - 24 - kSizeColumnW;
        const double driveX = sizeX - 24 - kDriveColumnW;
        end = driveX - 16;
        if (const Offline::Drive* drive = offline_->drive(d->root)) {
          drawIcon(cr, driveIcon(*drive), driveX + 14, b.y + b.h / 2, 24, dim[0], dim[1], dim[2]);
          drawText(cr, driveName(*drive, false), driveX + 36, b.y, kDriveColumnW - 36, b.h, 24, false,
                   false, dim[0], dim[1], dim[2]);
        } else {
          drawIcon(cr, Icon::Card, driveX + 14, b.y + b.h / 2, 24, dim[0], dim[1], dim[2]);
          drawText(cr, baseName(d->root), driveX + 36, b.y, kDriveColumnW - 36, b.h, 24, false,
                   false, dim[0], dim[1], dim[2]);
        }
        if (gone) {
          drawText(cr, t.missing, sizeX, b.y, b.x + b.w - 16 - sizeX, b.h, 24, false, false, dim[0],
                   dim[1], dim[2]);
        } else {
          const std::string bytes = sizeText(d->size);
          const double w = textWidth(bytes, 24, false);
          drawText(cr, bytes, sizeX + kSizeColumnW - w, b.y, w + 2, b.h, 24, false, false, dim[0],
                   dim[1], dim[2]);
          drawText(cr, dateText(d->doneAt), dateX, b.y, kDateColumnW, b.h, 24, false, false, dim[0],
                   dim[1], dim[2]);
        }
      } else if (!state.empty()) {
        const double w = std::min(textWidth(state, 24, false) + 2, (end - nameX) * 0.5);
        drawText(cr, state, end - w, b.y, w, b.h, 24, false, false, stateInk.r, stateInk.g,
                 stateInk.b);
        end -= w + 24;
      }
      const double nameW = end - nameX;
      if (b.byline.empty()) {
        drawText(cr, b.label, nameX, b.y, nameW, b.h, size, false, false, ink[0], ink[1], ink[2]);
      } else {
        const double labelW = std::min(textWidth(b.label, size, false) + 2,
                                       std::max(nameW * kBylineShare, nameW - textWidth(b.byline, 24, false) - 18));
        drawText(cr, b.label, nameX, b.y, labelW, b.h, size, false, false, ink[0], ink[1], ink[2]);
        if (nameW - labelW - 18 > 60)
          drawText(cr, b.byline, nameX + labelW + 18, b.y, nameW - labelW - 18, b.h, 24, false,
                   false, dim[0], dim[1], dim[2]);
      }
      if (gone) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, 0.5);
      }
      return;
    }
    case OfflineLook::Drive:
    case OfflineLook::DriveChoice: {
      const Offline::Drive* drive = offline_->drive(b.offlinePath);
      if (!drive) return;
      const bool choice = static_cast<OfflineLook>(b.offlineLook) == OfflineLook::DriveChoice;
      roundedRect(cr, b.x, b.y, b.w, b.h, b.radius);
      setColor(cr, choice ? pal.button : pal.field, choice ? 1 : 0.55);
      cairo_fill(cr);
      if (b.on) {
        roundedRect(cr, b.x + 1.5, b.y + 1.5, b.w - 3, b.h - 3, b.radius);
        setColor(cr, pal.edge);
        cairo_set_line_width(cr, 3);
        cairo_stroke(cr);
      }
      const double pad = choice ? 28 : 18;
      const std::string name = driveName(*drive, !choice);
      const std::string free = printed(t.freeOf, sizeText(drive->free).c_str(),
                                       sizeText(drive->total).c_str());
      if (choice) {  // name, what it is, the bar, its room
        drawIcon(cr, driveIcon(*drive), b.x + pad + 16, b.y + 44, 34, 1, 1, 1);
        drawText(cr, drive->internal ? t.internal : drive->card ? base.drives.card : base.drives.usbDrive,
                 b.x + pad + 48, b.y + 20, b.w - 2 * pad - 48, 48, 30, true, false, 1, 1, 1);
        const std::string what = drive->internal
                                     ? (kStandaloneHeadset ? t.internalHeadset : t.internalComputer)
                                     : drive->label;
        drawText(cr, what, b.x + pad, b.y + 72, b.w - 2 * pad, 36, 24, false, false, dim[0], dim[1],
                 dim[2]);
        drawDriveBar(cr, *drive, b.x + pad, b.y + 120, b.w - 2 * pad, 10);
        drawText(cr, free, b.x + pad, b.y + 140, b.w - 2 * pad, 36, 24, false, false, 1, 1, 1);
        return;
      }
      drawIcon(cr, driveIcon(*drive), b.x + pad + 14, b.y + 30, 28, 1, 1, 1);
      // "New downloads" with its round mark at the right end.
      const double mark = 30, markX = b.x + b.w - pad - mark;
      const double newW = textWidth(t.newDownloads, 22, false);
      if (b.on)
        drawText(cr, t.newDownloads, markX - 12 - newW - 2, b.y + 8, newW + 4, 44, 22, false, false,
                 dim[0], dim[1], dim[2]);
      drawText(cr, name, b.x + pad + 42, b.y + 8, markX - 12 - (b.on ? newW + 16 : 0) - (b.x + pad + 42),
               44, 26, false, false, 1, 1, 1);
      cairo_new_path(cr);
      cairo_arc(cr, markX + mark / 2, b.y + 30, mark / 2 - 1.5, 0, 2 * M_PI);
      if (b.on) {
        setColor(cr, pal.accent);
        cairo_set_line_width(cr, 3);
        cairo_stroke(cr);
        cairo_arc(cr, markX + mark / 2, b.y + 30, mark / 2 - 7, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 1, 1, 1, 1);
        cairo_fill(cr);
      } else {
        cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
        cairo_set_line_width(cr, 2);
        cairo_stroke(cr);
      }
      drawDriveBar(cr, *drive, b.x + pad, b.y + 58, b.w - 2 * pad, 8);
      const std::string mine = printed(t.offlineVideos, drive->used ? sizeText(drive->used).c_str() : "0");
      roundedRect(cr, b.x + pad, b.y + 82, 10, 10, 2);
      setColor(cr, pal.accent);
      cairo_fill(cr);
      const double mineW = textWidth(mine, 22, false);
      drawText(cr, mine, b.x + pad + 18, b.y + 70, mineW + 4, 34, 22, false, false, dim[0], dim[1],
               dim[2]);
      drawText(cr, "·  " + free, b.x + pad + 18 + mineW + 14, b.y + 70,
               b.w - 2 * pad - mineW - 32, 34, 22, false, false, 1, 1, 1);
      return;
    }
    case OfflineLook::Box: {  // the Scene tab's: "Offline", then Download or the state
      const Offline::Download* own = downloadOf(b.offlinePath);
      roundedRect(cr, b.x, b.y, b.w, b.h, b.radius);
      setColor(cr, pal.field, 0.55);
      cairo_fill_preserve(cr);
      setColor(cr, pal.edge, 0.6);
      cairo_set_line_width(cr, 2);
      cairo_stroke(cr);
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
          lineInk = kFailedText;
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
        progressBar(cr, b.x + pad, b.y + b.h - 28, b.w - 2 * pad, own->fraction(),
                    own->state == Offline::State::Paused, pal);
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

double Menu::dialogHeight() const {
  switch (dialog_) {
    case Dialog::SaveTo: return 540;
    case Dialog::NoRoom: return 460;
    case Dialog::KeyRefused: return 440;
    case Dialog::HostKeyChanged: return 500;
    case Dialog::ForgetServer: return 410;
    case Dialog::ForgetOffline: return 470;
    case Dialog::ConfirmDelete:
    case Dialog::Deleting:
      return deleteInfo_.stash != DeleteInfo::Stash::None || deleteInfo_.sourceChanged ? 460 : kDialogH;
    default: return kDialogH;
  }
}

// The dialog's own buttons: the drives to choose from (pushed here), and
// along its bottom (into `row`).
void Menu::layoutOfflineDialog(double boxY, std::vector<Button>& row) {
  const Strings& base = text();
  const OfflineStrings& t = base.offline;
  auto close = [this] {
    dialog_ = Dialog::None;
    return MenuCommand{};
  };
  row.push_back(textButton(base.cancel, Icon::None, close));
  if (!offline_) return;
  if (dialog_ == Dialog::SaveTo) {
    // One press picks the drive, remembers it and downloads there.
    const std::vector<Offline::Drive>& drives = offline_->drives();
    const double gap = 30, w = (kDialogW - 100 - gap * (drives.size() - 1)) / drives.size();
    const std::string chosen = downloadRoot();
    for (size_t i = 0; i < drives.size(); ++i) {
      Button card;
      card.panel = kBrowser;
      card.popup = true;
      card.x = kPageCenterX - kDialogW / 2 + 50 + i * (w + gap);
      card.y = boxY + 150;
      card.w = w;
      card.h = 190;
      card.radius = 22;
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
  } else if (dialog_ == Dialog::NoRoom && !offlineAsk_.other.empty()) {
    const Offline::Drive* other = offline_->drive(offlineAsk_.other);
    if (!other) return;
    Button instead = textButton(printed(t.instead, driveName(*other, false).c_str(),
                                        sizeText(roomOn(*offline_, *other)).c_str()),
                                driveIcon(*other), [this] {
                                  dialog_ = Dialog::None;
                                  const OfflineAsk& a = offlineAsk_;
                                  offline_->add(a.path, a.size, a.oshash, a.other);
                                  return MenuCommand{};
                                },
                                true);
    row.push_back(instead);
  } else if (dialog_ == Dialog::RemoveDownload) {
    Button remove = textButton(t.remove, Icon::Trash, [this] {
      dialog_ = Dialog::None;
      offline_->remove(offlineAsk_.id);
      return MenuCommand{};
    });
    remove.danger = true;
    row.push_back(remove);
  }
}

void Menu::drawOfflineDialog(cairo_t* cr, double boxX, double boxY) {
  const OfflineStrings& t = text().offline;
  const double textX = boxX + 50, textW = kDialogW - 100;
  if (!offline_) return;
  const std::string name = [&] {
    Button b;
    b.label = baseName(dialogPath_);
    if (stashCovers_) describeFromStash(b, dialogPath_);
    return b.label;
  }();
  if (dialog_ == Dialog::SaveTo) {
    drawText(cr, t.saveToTitle, textX, boxY + 30, textW, 60, 38, true, true, 1, 1, 1);
    const std::string what =
        offlineAsk_.size ? name + " · " + sizeText(offlineAsk_.size) : name;
    drawText(cr, what, textX, boxY + 90, textW, 44, 26, false, true, 0.85, 0.85, 0.9);
    drawText(cr, t.saveToHint, textX, boxY + 352, textW, 44, 24, false, true, 0.7, 0.7, 0.75);
  } else if (dialog_ == Dialog::NoRoom) {
    const Offline::Drive* drive = offline_->drive(offlineAsk_.root);
    const std::string driveText = drive ? driveName(*drive, false) : baseName(offlineAsk_.root);
    drawText(cr, printed(t.noRoomTitle, driveText.c_str()), textX, boxY + 30, textW, 60, 38, true,
             true, 1, 1, 1);
    drawText(cr, name, textX, boxY + 95, textW, 50, 28, false, true, 0.85, 0.85, 0.9);
    const uint64_t room = drive ? roomOn(*offline_, *drive) : 0;
    drawText(cr, printed(t.needs, sizeText(offlineAsk_.size).c_str(), room ? sizeText(room).c_str() : "0"),
             textX, boxY + 155, textW, 50, 26, true, true, 1.0, 0.45, 0.4);
    const std::string hint =
        offlineAsk_.other.empty()
            ? std::string(t.noRoomHintOnly)
            : printed(t.noRoomHint, driveName(offlineAsk_.other, false).c_str());
    const double dim[4] = {0.75, 0.75, 0.8, 1};
    drawTextLines(cr, hint, textX, boxY + 222, textW, 24, 2, dim, true);
  } else if (dialog_ == Dialog::RemoveDownload) {
    const Offline::Download* d = nullptr;
    for (const Offline::Download& x : offline_->downloads())
      if (x.id == offlineAsk_.id) d = &x;
    drawText(cr, t.removeTitle, textX, boxY + 30, textW, 60, 38, true, true, 1, 1, 1);
    if (d)
      drawText(cr,
               printed(t.removeWhat, name.c_str(), sizeText(d->size).c_str(),
                       driveName(d->root, false).c_str()),
               textX, boxY + 95, textW, 50, 28, false, true, 0.85, 0.85, 0.9);
    const std::string warning =
        remote::isRemote(dialogPath_) ? printed(t.removeWarning, serverName(remote::root(dialogPath_)).c_str())
                                : std::string(t.removeWarningLocal);
    drawText(cr, warning, textX, boxY + 160, textW, 50, 26, true, true, 1.0, 0.45, 0.4);
  }
}
