#include "menu_list_parts.h"
#include "menu_parts.h"
#include "menu_style.h"

#include "menu_stash.h"
#include "menu_stash_covers.h"
#include "stash.h"
#include "stash_stream.h"
#include "stash_text.h"
#include "thumbnailer.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cctype>
#include <cmath>

// The strip's Scene tab (docs/stash.md): the playing video's Stash details
// on the page, beside the navigation. On the left its cover, the app's
// rating and plays, and the file; on the right a column that scrolls on its
// own (Menu::SideColumn): the title, studio and date, the shown performers
// as portraits, the details and the tags as chips to choose, or in their
// place the similar scenes; under it where to go next. A video Stash
// doesn't know gets a short note why. Nothing here reads files or waits:
// the catalog and the images are in memory, and the similar scenes are
// scored once per scene; tag statistics are built on the catalog worker.

namespace {

constexpr double kTop = kPagePad;
constexpr double kLeftW = 620;  // the cover and the file
constexpr double kCoverH = kLeftW * 9 / 16;
constexpr double kLeftX = Menu::kPageX + kPagePad;
constexpr double kRightX = kLeftX + kLeftW + 32;
constexpr double kRight = Menu::kPageX + Menu::kPageWidth - kPagePad;
constexpr double kTextW = kRight - kRightX - 20;  // the column's content, clear of its scroll mark
constexpr double kActionH = 64;
constexpr double kActionY = Menu::kBrowserHeight - kPagePad - kActionH;
constexpr double kColumnTop = kTop - 4, kColumnBottom = kActionY - 16;
constexpr int kCastColumns = 5;  // her cards, as the Performers page's
constexpr double kTagH = 48, kTagStep = 58, kTagGap = 10;
constexpr double kOrderH = 44;  // the tags' order well
constexpr double kChipIconW = 30;  // an editing chip's × (or Add tags' +)
constexpr double kTagPad = 18;  // a tag chip's text from its ends
constexpr int kTagRows = 3;  // before "+N more"
constexpr int kDetailLines = 3, kAllDetailLines = 40;
constexpr size_t kSimilarCount = 12;
constexpr double kTileGap = kGalleryGap;
constexpr double kLogoShare = 0.36;  // of a tile's picture, as menu_stash_covers.cpp draws it

std::string upper(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  return s;
}

// How many lines `text` wraps to in w at `size`, and how high at most
// `lines` of them are, as drawTextLines() draws them.
struct Wrapped {
  int lines = 0;
  double height = 0;
};
Wrapped wrapped(cairo_t* cr, const std::string& text, int size, double w, int lines) {
  PangoLayout* layout = pango_cairo_create_layout(cr);
  PangoFontDescription* font = pango_font_description_from_string(kFont);
  pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
  pango_layout_set_font_description(layout, font);
  pango_layout_set_text(layout, text.c_str(), -1);
  pango_layout_set_width(layout, static_cast<int>(w * PANGO_SCALE));
  pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
  Wrapped r;
  r.lines = pango_layout_get_line_count(layout);
  pango_layout_set_height(layout, -std::max(1, lines));
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  int pw = 0, ph = 0;
  pango_layout_get_pixel_size(layout, &pw, &ph);
  r.height = ph;
  pango_font_description_free(font);
  g_object_unref(layout);
  return r;
}

// The catalog ranks scenes; the menu adds a short shared-tag caption. None
// hidden away from home (`history`, if it is the catalog's).
std::vector<std::pair<int, std::string>> similarScenes(const StashCatalog& c, int of,
                                                       bool malePerformers,
                                                       const std::vector<StashHistory>& history) {
  const StashSceneStrings& t = strings().stashScene;
  const std::vector<StashScene>& scenes = c.scenes();
  std::vector<char> tagged(c.tags().size(), 0);
  for (const int tag : scenes[of].tags) tagged[tag] = 1;
  std::vector<std::pair<int, std::string>> list;
  const bool away = history.size() == scenes.size();
  for (const int i : c.similarScenes(of, malePerformers, kSimilarCount)) {
    if (away && history[i].hidden) continue;
    int common = 0;
    for (const int tag : scenes[i].tags) common += tagged[tag];
    list.emplace_back(i, common == 1 ? std::string(t.oneTagInCommon)
                                     : stashPrinted(t.tagsInCommon, common));
  }
  return list;
}

}  // namespace

// The tab between Stereo focus and Quit, there while Stash is on whatever
// plays, so the strip never shifts; greyed while nothing local plays.
void StashPages::layoutSceneTab(Menu& m, Menu::StripLayout& strip) {
  const bool open = m.page_ == Menu::Page::StashSceneDetails;
  Button& b = strip.tab(strings().stashScene.tab, Menu::Icon::None,
                        open, [&m, open] {
                          if (open)
                            m.showPage(Menu::Page::Main);
                          else
                            showSceneDetails(m);
                          return MenuCommand{};
                        });
  b.stashLook = static_cast<int>(StashLook::Tab);
  b.stashIcon = static_cast<int>(StashIcon::Stash);
  b.disabled = !open && (m.view_.currentFile.empty() || m.view_.streaming || m.view_.photo);
}

// The page has the side column to itself: the rail's place is kept for it.
void StashPages::showSceneDetails(Menu& m) {
  StashSceneTab& d = state(m).sceneTab;
  m.showPage(Menu::Page::StashSceneDetails);
  d.otherSide = m.side_.position;
  m.side_.position = d.path == m.view_.currentFile ? d.scroll : 0;
}

void StashPages::leaveSceneDetails(Menu& m) {
  StashSceneTab& d = state(m).sceneTab;
  d.scroll = m.side_.position;
  m.side_.position = d.otherSide;
}

// Each frame while it shows: a new catalog, the library's ratings and
// plays, a sync started or done (the note on a video Stash doesn't know),
// or a tag save's progress. A save that worked ends the editing: its tags
// show at once, until a sync after it has them.
void StashPages::updateSceneDetails(Menu& m) {
  if (m.page_ != Menu::Page::StashSceneDetails) return;
  StashSceneTab& d = state(m).sceneTab;
  const uint64_t catalogNow = m.stash_->generation();
  const uint64_t libraryNow = m.library_ ? m.library_->generation() : 0;
  const StashStatus& status = m.stash_->status();
  const int64_t sync = status.syncing ? -2 : status.lastSync;
  const StashWriteBack* w = m.stash_->writeBack();
  const int save = w ? static_cast<int>(w->tagSave().state) : -1;
  if (catalogNow == d.shownCatalog && libraryNow == d.shownLibrary && sync == d.shownSync &&
      save == d.shownSave)
    return;
  d.shownCatalog = catalogNow;
  d.shownLibrary = libraryNow;
  d.shownSync = sync;
  d.shownSave = save;
  if (d.saving && w && w->tagSave().scene == d.scene &&
      w->tagSave().state == StashTagSave::State::Saved) {
    // Over an earlier save no sync has brought yet.
    if (d.savedScene != d.scene || status.lastSync > d.savedAt) {
      d.savedAdded.clear();
      d.savedRemoved.clear();
    }
    auto move = [](std::vector<int64_t>& from, std::vector<int64_t>& to, int64_t id) {
      from.erase(std::remove(from.begin(), from.end(), id), from.end());
      to.push_back(id);
    };
    for (const int64_t id : d.sentAdded) move(d.savedRemoved, d.savedAdded, id);
    for (const int64_t id : d.sentRemoved) move(d.savedAdded, d.savedRemoved, id);
    d.savedScene = d.scene;
    d.savedAt = w->tagSave().at;
    d.savedNote = d.summary;
    d.editing = d.adding = d.confirm = d.saving = false;
    d.added.clear();
    d.struck.clear();
    m.editing_ = false;
  }
  m.browserChanged();
}

void StashPages::layoutSceneDetails(Menu& m) {
  StashMenu& st = state(m);
  StashSceneTab& d = st.sceneTab;
  const StashStrings& ts = strings().stash;
  const StashSceneStrings& t = strings().stashScene;
  const Strings& base = strings();
  const StashCatalog* c = catalog(m);
  if (c) takeChanges(m);  // the scenes hidden away from home
  const std::string& path = m.view_.currentFile;
  const bool local = !path.empty() && !m.view_.streaming;
  // A copy on this device: its video's scene.
  const int index = c && local && m.stashCovers_ ? m.stashCovers_->sceneIndex(path) : -1;
  const StashScene* s = index >= 0 ? &c->scenes()[index] : nullptr;
  const bool male = m.view_.stash.malePerformers;
  // Editing tags needs write-back on; turned off, the edits go, as they do
  // for another video.
  const bool canEdit = s && m.view_.stash.writeBack && m.stash_->writeBack() &&
                       mediaServer(m.view_.stash.kind).abilities().editTags;
  if (path != d.path || (s ? s->id : 0) != d.scene || (d.editing && !canEdit)) {
    if (d.editing && st.typing == StashMenu::Typing::Tags) m.editing_ = false;
    d.editing = d.adding = d.confirm = d.saving = false;
    d.added.clear();
    d.struck.clear();
    d.search.clear();
  }
  // Another video (or Stash knows it now): its choices start afresh.
  if (path != d.path || (s ? s->id : 0) != d.scene) {
    d.path = path;
    d.scene = s ? s->id : 0;
    d.tags.clear();
    d.tagsOpen = d.detailsOpen = d.similar = false;
    d.savedNote.clear();
    d.scroll = 0;
    m.side_.position = 0;
  }
  const bool typing = d.adding && m.editing_ && st.typing == StashMenu::Typing::Tags;
  // Pictures arriving redraw all of the page.
  m.listTop_ = kTop;
  m.listBottom_ = kListBottom;
  d.dividerY = d.boxY = d.boxH = 0;
  cairo_t* measure = m.canvases_[Menu::kMain].cr();
  std::vector<StashImages::Request> wanted;
  std::vector<std::string> frames;

  // The left column: the cover (drawn by the heading), the rating and plays, the file.
  if (local && s && m.stashCoversShown() && !s->cover.empty())
    wanted.push_back(Stash::coverRequest(*s, stashPixels(kLeftW)));
  if (local && m.view_.thumbnails && !m.stashCovers_->coverComing(path)) frames.push_back(path);
  double y = kTop + kCoverH + 18;
  if (local && !m.view_.photo && m.library_ && m.library_->available()) {
    m.addLabel(kLeftX, y, 140, 56, t.yourRating, kHintSize, true);
    m.buttons_.push_back(m.ratingButton(path, m.current_.rating, Menu::kBrowser,
                                        kLeftX + 140 + kRatingLeadW, y, 50, 56));
    y += 62;
    const VideoRecord& r = m.current_;
    std::string plays = t.notPlayed;
    if (r.playCount > 0) {
      plays = stashPrinted(t.played, r.playCount);
      if (r.lastPlayedAt > 0)
        plays += "  ·  " + stashPrinted(t.lastPlayed, stashAgo(r.lastPlayedAt));
    }
    m.addLabel(kLeftX, y, kLeftW, 34, plays, kit::kWellSize, false);
    y += 36;
  }
  if (local) {
    d.dividerY = y + 8;
    y += 22;
    const int w = s && s->width > 0 ? s->width : m.view_.videoWidth;
    const int h = s && s->height > 0 ? s->height : m.view_.videoHeight;
    const double fps = s && s->fps > 0 ? s->fps : m.view_.playback.fps;
    const std::string codec = s && !s->codec.empty() ? s->codec : m.view_.playback.videoCodec;
    std::vector<std::string> first;
    if (w > 0 && h > 0) first.push_back(std::to_string(w) + " × " + std::to_string(h));
    if (!m.view_.photo) {
      first.push_back(upper(codec));
      if (fps > 0) first.push_back(stashPrinted("%.4g fps", fps));
    }
    // The format it plays in.
    const VideoFormat& f = m.view_.format;
    std::string format = ts.flat;
    if (f.projection != Projection::Flat || f.stereo != StereoLayout::Mono) {
      format = std::string(f.projection != Projection::Flat ? ts.vr : ts.flat) + " · " +
               base.projections[static_cast<int>(f.projection)];
      if (f.stereo != StereoLayout::Mono) format += std::string(" · ") + base.stereo[static_cast<int>(f.stereo)];
    }
    const double duration = s && s->duration > 0 ? s->duration : m.view_.playback.duration;
    const std::string second =
        stashJoined({format, duration > 0 && !m.view_.photo ? formatTime(duration) : "",
                     s ? sizeText(s->size) : ""});
    for (const std::string& line : {stashJoined(first), second}) {
      m.addLabel(kLeftX, y, kLeftW, 34, line, kit::kWellSize, false);
      y += 34;
    }
    m.addLabel(kLeftX, y + 6, kLeftW, 0, isStashScene(path) ? t.fromStash : path, kHintSize, true, 2);
    // Offline: Download, or how its download is doing (menu_offline.cpp).
    if (!m.view_.photo && !typing)  // the keyboard is there
      m.addOfflineBox(path, kLeftX, kActionY + kActionH - 124, kLeftW, 124);
  }

  auto finish = [&] {
    m.stash_->images().want(wanted);
    if (m.thumbnailer_) m.thumbnailer_->want(frames, stashPixels(kLeftW));
  };

  // A video Stash doesn't know, nothing playing, or no catalog yet.
  if (!s) {
    if (path.empty()) {
      m.addLabel(kRightX, kTop - 4, kRight - kRightX, 48, t.nothingPlaying, kit::kRowSize, true);
    } else if (!c && local) {
      const StashStatus& status = m.stash_->status();
      m.addLabel(kRightX, kTop - 4, kRight - kRightX, 48,
                 status.problem == StashProblem::TooOld ? status.error
                 : status.error.empty()                 ? std::string(ts.loading)
                                                        : std::string(ts.noCatalog),
                 kit::kRowSize, true);
    } else {
      m.addLabel(kRightX, kTop - 4, kRight - kRightX, 48,
                 local ? path.substr(path.find_last_of('/') + 1) : m.view_.playback.title, kit::kTitleSize, false);
      d.boxY = kTop + 64;
      d.boxH = local ? 300 : 180;
      const double bx = kRightX + 28, bw = kRight - kRightX - 56;
      m.addLabel(bx + 46, d.boxY + 22, bw - 46, 40, t.notInStash, kit::kRowSize, false);
      // Where Stash's folders are here: its library folders mapped, else
      // the mappings' folders.
      std::vector<std::string> folders;
      if (c)
        for (const StashCatalogFolder& f : c->folders())
          if (!f.folder.empty())
            if (std::string here = m.view_.stash.localPath(f.folder); !here.empty())
              folders.push_back(std::move(here));
      if (folders.empty())
        for (const StashFolderMap& f : m.view_.stash.folders) folders.push_back(f.to);
      const bool inside = std::any_of(folders.begin(), folders.end(), [&](const std::string& f) {
        return !f.empty() && stashUnder(path, f);
      });
      std::string shown;
      for (const std::string& f : folders) shown += (shown.empty() ? "" : ", ") + f;
      const std::string why = !local   ? t.notLocal
                              : inside ? t.notScanned
                                       : stashPrinted(t.outsideFolders, shown);
      m.addLabel(bx, d.boxY + 76, bw, 0, why, kit::kWellSize, true, 3);
      if (local) {
        const StashStatus& status = m.stash_->status();
        Button sync = m.textButton(ts.syncNow, Menu::Icon::None, command(MenuCommand::SyncStash));
        sync.tooltip = ts.tipSync;
        sync.panel = Menu::kBrowser;
        sync.x = bx;
        sync.y = d.boxY + d.boxH - 28 - 56;
        sync.h = 56;
        sync.disabled = status.syncing;
        m.buttons_.push_back(sync);
        const std::string state =
            status.syncing ? ts.syncing
            : status.lastSync > 0
                ? stashPrinted(t.lastSync, stashAgo(status.lastSync), stashScenes(status.scenes))
                : ts.notSynced;
        m.addLabel(sync.x + sync.w + 24, sync.y, bw - sync.w - 24, 56, state, kHintSize, true);
      }
    }
    finish();
    return;
  }

  // The right column, laid out from its top and shown where it is scrolled to.
  std::vector<Button> column;
  double cy = 0;
  auto text = [&](double x, double ty, double w, double h, const std::string& label, int size,
                  bool dim, int lines) {
    Button b;
    b.panel = Menu::kBrowser;
    b.x = x;
    b.y = ty;
    b.w = w;
    b.h = h;
    b.label = label;
    b.staticText = true;
    b.fontSize = size;
    b.dim = dim;
    b.lines = lines;
    column.push_back(b);
  };
  auto chip = [&](const std::string& label, const std::string& count, bool on, bool outlined,
                  std::function<void()> action, double x, double ty, double w) {
    Button b;
    b.panel = Menu::kBrowser;
    b.x = x;
    b.y = ty;
    b.w = w;
    b.h = kTagH;
    b.label = label;
    b.size = count;
    b.latched = on;  // chosen, or added while editing
    b.dim = outlined;  // More, "+N more": a link
    b.bare = outlined;
    b.radius = kTagH / 2;
    b.stashLook = static_cast<int>(StashLook::TagChip);
    b.action = [action] {
      action();
      return MenuCommand{};
    };
    column.push_back(b);
    return &column.back();
  };
  auto chipW = [&](const std::string& label, const std::string& count) {
    return std::min(kTextW, m.textWidth(label, kit::kWellSize, false) + 2 * kTagPad +
                                (count.empty() ? 0 : kit::kSpaceTight + m.textWidth(count, kCountSize, false)));
  };
  std::vector<int> cast;
  for (const int p : s->performers)
    if (stashShown(c->performers()[p].gender, male)) cast.push_back(p);

  // The scene's tags, with a save over them until a sync after it; while
  // editing, what Save would send: Stash ids to add (a merged tag's most
  // used) and to remove (all of a merged tag's).
  const auto& allTags = c->tags();
  auto firstId = [&](int tag) { return allTags[tag].ids.empty() ? 0 : allTags[tag].ids.front(); };
  auto has = [](const std::vector<int64_t>& list, int64_t id) {
    return std::find(list.begin(), list.end(), id) != list.end();
  };
  std::vector<int> have = s->tags;
  if (d.savedScene == s->id && m.stash_->status().lastSync <= d.savedAt) {
    have.erase(std::remove_if(have.begin(), have.end(),
                              [&](int tag) { return has(d.savedRemoved, firstId(tag)); }),
               have.end());
    for (const int64_t id : d.savedAdded) {
      const int tag = c->tagById(id);
      if (tag >= 0 && !has(d.savedRemoved, id) && std::find(have.begin(), have.end(), tag) == have.end())
        have.push_back(tag);
    }
  }
  auto onScene = [&](int64_t id) {
    const int tag = c->tagById(id);
    return std::find(have.begin(), have.end(), tag) != have.end();
  };
  std::vector<int64_t> sendAdd, sendRemove, removed;  // removed: by the first id of each
  if (d.editing) {
    for (const int64_t id : d.added)
      if (!has(d.struck, id) && !onScene(id)) sendAdd.push_back(id);
    for (const int tag : have)
      if (has(d.struck, firstId(tag))) {
        sendRemove.insert(sendRemove.end(), allTags[tag].ids.begin(), allTags[tag].ids.end());
        removed.push_back(firstId(tag));
      }
  }
  const int addCount = static_cast<int>(sendAdd.size()), removeCount = static_cast<int>(removed.size());
  if (d.editing)
    d.summary = stashJoined({addCount ? stashPrinted(t.added, addCount) : "",
                             removeCount ? stashPrinted(t.removed, removeCount) : ""},
                            ", ");
  // While a save is under way the edits stay as they are.
  auto saving = [&m] {
    const StashWriteBack* w = m.stash_->writeBack();
    return state(m).sceneTab.saving && w && w->tagSave().state == StashTagSave::State::Saving;
  };
  auto save = [&m, saving, scene = s->id, sendAdd, sendRemove, removed] {
    if (saving()) return MenuCommand{};
    StashSceneTab& tab = state(m).sceneTab;
    tab.saving = true;
    tab.confirm = false;
    tab.sentAdded = sendAdd;
    tab.sentRemoved = removed;
    m.stash_->saveTags(scene, sendAdd, sendRemove);
    return MenuCommand{};
  };
  const StashWriteBack* wb = m.stash_->writeBack();
  const StashTagSave::State saveState = wb ? wb->tagSave().state : StashTagSave::State::None;
  const bool busy = d.saving && saveState == StashTagSave::State::Saving;
  const bool failed = d.saving && saveState == StashTagSave::State::Failed;
  // A tag struck, or kept again; an added one taken back.
  auto strike = [&m, saving](int64_t id) {
    if (saving()) return;
    StashSceneTab& tab = state(m).sceneTab;
    const auto it = std::find(tab.struck.begin(), tab.struck.end(), id);
    if (it != tab.struck.end())
      tab.struck.erase(it);
    else
      tab.struck.push_back(id);
    tab.confirm = tab.saving = false;
  };
  auto stopTags = [&m] {
    StashSceneTab& tab = state(m).sceneTab;
    tab.editing = tab.adding = tab.confirm = tab.saving = false;
    tab.added.clear();
    tab.struck.clear();
    tab.search.clear();
    m.editing_ = false;
  };

  double columnTop = kColumnTop, columnBottom = kColumnBottom;
  if (d.adding) {
    // Add tags in the column's place: a find field over every tag, the
    // keyboard under it; a tap adds one, again takes it back.
    m.addLabel(kRightX, kTop - 4, kTextW - 300, 56, t.addTags, kit::kTitleSize, false);
    Button back = actionButton(
        m, t.backToTags, StashIcon::Back,
        [&m] {
          state(m).sceneTab.adding = false;
          m.editing_ = false;
          m.side_.position = 0;
          return MenuCommand{};
        },
        0, kTop - 4, 56);
    back.x = kRight - back.w;
    m.buttons_.push_back(back);
    const std::string hint = stashPrinted(t.findTag, stashNumber(static_cast<long>(allTags.size())));
    Button field = m.textButton(d.search, Menu::Icon::Search, [&m] {
      state(m).typing = StashMenu::Typing::Tags;
      m.editing_ = true;
      return MenuCommand{};
    });
    field.panel = Menu::kBrowser;
    field.x = kRightX;
    field.y = kTop + 64;
    field.w = kRight - kRightX;
    field.h = 56;
    field.fontSize = Menu::kTextSize;
    field.field = true;
    field.on = typing;
    field.caret = typing ? m.caret() : -1;
    field.example = hint;  // while empty, typed into or not
    field.stashLook = static_cast<int>(StashLook::Field);  // its icon even while empty
    field.radius = 12;
    m.buttons_.push_back(field);
    if (!d.search.empty()) {
      Button clear = m.textButton(base.clear, Menu::Icon::None, [&m] {
        state(m).typing = StashMenu::Typing::Tags;
        setText(m, "");
        return MenuCommand{};
      });
      clear.panel = Menu::kBrowser;
      clear.bare = true;
      clear.on = true;
      clear.fontSize = 26;
      clear.w = m.textWidth(base.clear, 26, false) + 40;
      clear.x = kRight - clear.w - 6;
      clear.y = field.y + 4;
      clear.h = field.h - 8;
      clear.radius = clear.h / 2;
      m.buttons_.push_back(clear);
    }
    // The rows: every tag with all the words, most used first, as the
    // full tag list finds and names them.
    if (d.foundFor != d.search || d.foundGeneration != m.stash_->generation()) {
      d.foundFor = d.search;
      d.foundGeneration = m.stash_->generation();
      d.found.clear();
      const std::string words = stashFold(d.search);
      for (int i = 0; i < static_cast<int>(allTags.size()); ++i) {
        bool match = words.empty();
        for (size_t n = 0; !match && n < allTags[i].names.size(); ++n)
          match = stashFold(allTags[i].names[n]).find(words) != std::string::npos;
        if (match) d.found.push_back(i);
      }
      std::stable_sort(d.found.begin(), d.found.end(),
                       [&](int a, int b) { return allTags[a].scenes > allTags[b].scenes; });
    }
    columnTop = field.y + field.h + 16;
    columnBottom = typing ? kKeyboardY - 16 : kColumnBottom;
    if (d.found.empty() && !d.search.empty())
      text(kRightX, 0, kTextW, 48, stashPrinted(t.noTagFound, d.search), kit::kWellSize, true, 1);
    // Only the rows in view are made: the list may hold every tag.
    constexpr double rowStep = 54, rowH = 50;
    const double cellW = (kTextW - 16) / 2;
    const size_t rows = (d.found.size() + 1) / 2;
    // Held in the column first (as below), so that these are the rows it shows.
    m.side_.position = std::clamp(m.side_.position, 0.0,
                                  std::max(0.0, rows * rowStep + 8 - (columnBottom - columnTop)));
    const size_t from = static_cast<size_t>(std::max(0.0, m.side_.position / rowStep));
    const size_t to = std::min(
        rows, static_cast<size_t>((m.side_.position + columnBottom - columnTop) / rowStep) + 2);
    for (size_t i = from * 2; i < std::min(d.found.size(), to * 2); ++i) {
      const StashTag& tg = allTags[d.found[i]];
      const int64_t id = tg.ids.empty() ? 0 : tg.ids.front();
      const bool there = onScene(id);
      std::string label = tg.name;
      for (size_t n = 1; n < tg.names.size(); ++n) label += " · " + tg.names[n];
      Button b = facetButton(m, label, there ? t.onScene : stashNumber(tg.scenes),
                             has(d.added, id) && !has(d.struck, id) ? 1 : 0,
                             [&m, saving, id] {
                               if (saving()) return;
                               StashSceneTab& tab = state(m).sceneTab;
                               const auto it = std::find(tab.added.begin(), tab.added.end(), id);
                               const auto out = std::find(tab.struck.begin(), tab.struck.end(), id);
                               if (it != tab.added.end() && out == tab.struck.end()) {
                                 tab.added.erase(it);
                               } else {
                                 if (it == tab.added.end()) tab.added.push_back(id);
                                 if (out != tab.struck.end()) tab.struck.erase(out);
                               }
                               tab.confirm = tab.saving = false;
                             },
                             kRightX + (i % 2) * (cellW + 16), (i / 2) * rowStep, cellW, rowH);
      b.disabled = there;
      column.push_back(b);
    }
    cy = rows * rowStep;
  } else if (!d.similar) {
    // Title; studio, release date, code and director.
    const std::string title = s->displayTitle();
    const Wrapped tw = wrapped(measure, title, kit::kTitleSize, kTextW, 2);
    text(kRightX, cy, kTextW, tw.height, title, kit::kTitleSize, false, 2);
    cy += tw.height + 8;
    double x = kRightX;
    if (s->studio >= 0) {
      const StashStudio& studio = c->studios()[s->studio];
      Button b = actionButton(
          m, studio.name, StashIcon::Studios,
          [&m, id = studio.id] {
            showStudio(m, id);
            return MenuCommand{};
          },
          x - 12, cy, 48);
      b.bare = true;
      b.on = true;
      b.w = std::min(b.w, 520.0);
      column.push_back(b);
      x = b.x + b.w + 4;
    }
    const std::string facts = stashJoined(
        {s->date, s->code, s->director.empty() ? "" : stashPrinted(t.director, s->director)},
        "  ·  ");
    if (!facts.empty()) text(x + 8, cy, kRightX + kTextW - x - 8, 48, facts, kit::kWellSize, true, 1);
    cy += 60;

    // The cast: portraits with her age then; one opens her page.
    if (cast.empty()) {
      text(kRightX, cy, kTextW, 40, s->performers.empty() ? t.noPerformers : t.onlyMen, kit::kWellSize,
           true, 1);
      cy += 52;
    } else {
      const double cardW = (kTextW - (kCastColumns - 1) * kTileGap) / kCastColumns;
      const double cardH = performerCardHeight(cardW, 2);
      for (size_t i = 0; i < cast.size(); ++i) {
        const StashPerformer& p = c->performers()[cast[i]];
        Button b;
        b.panel = Menu::kBrowser;
        b.x = kRightX + (i % kCastColumns) * (cardW + kTileGap);
        b.y = cy + (i / kCastColumns) * (cardH + kTileGap);
        b.w = cardW;
        b.h = cardH;
        b.card = true;
        b.radius = kCardCorner;
        b.label = p.name;
        const int age = stashAge(p.born, s->day);
        if (age >= 0) b.detail = stashPrinted(t.ageThen, age);
        b.stashId = p.id;
        b.stashLook = static_cast<int>(StashLook::PerformerCard);
        b.action = [&m, id = p.id] {
          showPerformer(m, id);
          return MenuCommand{};
        };
        if (m.textWidth(p.name, kit::kRowSize, false) > cardW - 2 * kit::kSpace) b.tooltip = p.name;
        column.push_back(b);
        if (!p.image.empty())
          wanted.push_back(Stash::portraitRequest(p, stashPixels(cardW - 2 * kCardIn)));
      }
      cy += ((cast.size() + kCastColumns - 1) / kCastColumns) * (cardH + kTileGap) + 6;
    }

    // The details: three lines, More shows all of them.
    if (!s->details.empty()) {
      const bool open = d.detailsOpen;
      const bool more =
          wrapped(measure, s->details, kit::kWellSize, kTextW - 130, kDetailLines).lines > kDetailLines;
      const double w = more && !open ? kTextW - 130 : kTextW;
      const int lines = open ? kAllDetailLines : kDetailLines;
      const Wrapped shown = wrapped(measure, s->details, kit::kWellSize, w, lines);
      text(kRightX, cy, w, shown.height, s->details, kit::kWellSize, false, lines);
      if (more) {
        const char* label = open ? t.lessText : t.moreText;
        chip(label, "", false, true, [&m] { state(m).sceneTab.detailsOpen ^= true; },
             kRightX + kTextW - 110, open ? cy + shown.height + 8 : cy + shown.height - kTagH, 110);
      }
      cy += shown.height + (more && open ? kTagH + 16 : 0) + 20;
    }

    // The tags: chosen ones then give "Scenes with these" below; while
    // editing, each is struck or kept.
    std::vector<int> tags = have;
    if (!tags.empty() || canEdit) {
      const auto& all = c->tags();
      if (d.tagOrder == 0) {  // common first
        std::stable_sort(tags.begin(), tags.end(),
                         [&](int a, int b) { return all[a].scenes > all[b].scenes; });
      } else if (d.tagOrder == 1) {  // specific first, those on very few scenes last
        std::stable_sort(tags.begin(), tags.end(), [&](int a, int b) {
          const bool fewA = all[a].scenes < 4, fewB = all[b].scenes < 4;
          return fewA != fewB ? fewB : all[a].scenes < all[b].scenes;
        });
      } else {
        std::vector<std::pair<std::string, int>> keys;
        for (const int tag : tags) keys.emplace_back(stashCollateKey(all[tag].name), tag);
        std::stable_sort(keys.begin(), keys.end());
        for (size_t i = 0; i < keys.size(); ++i) tags[i] = keys[i].second;
      }
      // Its heading and count, the order well, then Edit tags at the right.
      const double headY = cy + (kTagH - kOrderH) / 2;
      Button heading;
      heading.panel = Menu::kBrowser;
      heading.listLook = static_cast<int>(ListLook::Heading);
      heading.x = kRightX - 12;  // its text in line with the column's
      heading.y = headY;
      heading.w = m.textWidth(t.tags, kit::kGroupTitleSize, true) + 24;
      heading.h = kOrderH;
      heading.label = t.tags;
      column.push_back(heading);
      const std::string tagCount = stashNumber(static_cast<long>(tags.size()));
      const double countX = kRightX + heading.w - 24 + kit::kSpaceTight;
      const double countW = m.textWidth(tagCount, kCountSize, false) + 4;
      text(countX, headY, countW, kOrderH, tagCount, kCountSize, true, 1);
      std::vector<Menu::ToolPiece> orders;
      for (int order = 0; order < 3; ++order) {
        Menu::ToolPiece piece;
        piece.label = t.tagOrders[order];
        piece.tooltip = t.tipTagOrders[order];
        piece.chosen = d.tagOrder == order;
        piece.action = [&m, order] {
          state(m).sceneTab.tagOrder = order;
          return MenuCommand{};
        };
        orders.push_back(std::move(piece));
      }
      const size_t well = m.buttons_.size();
      double sx = m.toolsWell(countX + countW + kit::kSpace, headY, orders, kOrderH);
      for (size_t i = well; i < m.buttons_.size(); ++i) column.push_back(std::move(m.buttons_[i]));
      m.buttons_.resize(well);
      // At the right: Edit tags, or while editing what a tap does.
      double right = kRightX + kTextW;
      if (canEdit && !d.editing) {
        Button edit = m.textButton(t.editTags, Menu::Icon::Pencil, [&m] {
          StashSceneTab& tab = state(m).sceneTab;
          tab.editing = true;
          tab.added.clear();
          tab.struck.clear();
          tab.tags.clear();
          tab.savedNote.clear();
          tab.confirm = tab.saving = false;
          return MenuCommand{};
        });
        edit.panel = Menu::kBrowser;
        edit.fontSize = kit::kWellSize;
        edit.x = right - edit.w;
        edit.y = headY;
        edit.h = kOrderH;
        edit.radius = kit::kCorner;
        edit.tooltip = t.tipEditTags;
        column.push_back(edit);
        right = edit.x - kit::kSpace;
      }
      const std::string note = d.editing         ? std::string(t.editHint)
                               : !d.savedNote.empty() ? stashPrinted(t.saved, d.savedNote)
                               : d.tags.empty()  ? std::string(t.pickTags)
                                                 : std::string();
      if (!note.empty()) {
        const double hw = m.textWidth(note, kHintSize, false) + 4;
        if (right - hw > sx + kit::kSpace)
          text(right - hw, headY, hw, kOrderH, note, kHintSize, d.savedNote.empty() || d.editing, 1);
      }
      cy += kTagStep + 4;

      auto chosen = [&](int tag) {
        return std::any_of(d.tags.begin(), d.tags.end(),
                           [&](int64_t id) { return c->tagById(id) == tag; });
      };
      // Wrapping chips, row by row; folded, kTagRows of them with "+N more".
      // Editing: all of them, after Add tags and those added, each with ×.
      const bool open = d.tagsOpen || d.editing;
      const double end = kRightX + kTextW;
      double x = kRightX;
      int row = 0;
      size_t shown = 0;
      const size_t first = column.size();
      auto place = [&](double w) {
        if (x > kRightX && x + w > end) {
          ++row;
          x = kRightX;
        }
        const double at = x;
        x += w + kTagGap;
        return at;
      };
      // An edited chip: × to strike it, struck faded with ↶ to keep it.
      auto editable = [&](Button* b, int64_t id) {
        b->icon = Menu::Icon::Close;
        b->crossed = has(d.struck, id);
        if (b->crossed) {  // flat and faded, to be removed
          b->latched = false;
          b->bare = true;
        }
        b->action = [strike, id] {
          strike(id);
          return MenuCommand{};
        };
      };
      if (d.editing) {
        const double w = chipW(t.addTags, "") + kChipIconW;
        Button* add = chip(t.addTags, "", false, true,
                           [&m, saving] {
                             if (saving()) return;
                             StashSceneTab& tab = state(m).sceneTab;
                             tab.adding = true;
                             tab.confirm = false;
                             tab.search.clear();
                             state(m).typing = StashMenu::Typing::Tags;
                             m.editing_ = true;
                             m.side_.position = 0;
                           },
                           place(w), cy, w);
        add->icon = Menu::Icon::Plus;
        for (const int64_t id : d.added) {
          const int tag = c->tagById(id);
          if (tag < 0 || onScene(id)) continue;
          const std::string count = stashNumber(all[tag].scenes);
          const double cw = chipW(all[tag].name, count) + kChipIconW;
          const double at = place(cw);
          editable(chip(all[tag].name, count, true, false, nullptr, at, cy + row * kTagStep, cw), id);
        }
      }
      for (const int tag : tags) {
        const StashTag& tg = all[tag];
        const std::string count = stashNumber(tg.scenes);
        const double w = chipW(tg.name, count) + (d.editing ? kChipIconW : 0);
        const double sx0 = x;
        const int row0 = row;
        const double at = place(w);
        if (!open && row >= kTagRows) {
          x = sx0;
          row = row0;
          break;
        }
        const int64_t id = tg.ids.empty() ? 0 : tg.ids.front();
        Button* b = chip(
            tg.name, count, chosen(tag), false,
            [&m, id] {
              StashSceneTab& tab = state(m).sceneTab;
              const StashCatalog* now = catalog(m);
              const int merged = now ? now->tagById(id) : -1;
              const size_t before = tab.tags.size();
              tab.tags.erase(std::remove_if(tab.tags.begin(), tab.tags.end(),
                                            [&](int64_t other) {
                                              return other == id ||
                                                     (merged >= 0 && now->tagById(other) == merged);
                                            }),
                             tab.tags.end());
              if (tab.tags.size() == before) tab.tags.push_back(id);
              tab.savedNote.clear();
            },
            at, cy + row * kTagStep, w);
        if (d.editing) editable(b, id);
        b->tooltip = d.editing ? "" : t.pickTags;
        if (tg.names.size() > 1)
          b->tooltip = stashJoined(tg.names);
        else if (w >= kTextW)
          b->tooltip = tg.name;
        ++shown;
      }
      if (!open && shown < tags.size()) {
        // "+N more" at the end of the last row, taking chips off it until it fits.
        while (true) {
          const std::string more = stashPrinted(t.moreTags, static_cast<int>(tags.size() - shown));
          const double w = chipW(more, "");
          if (column.size() == first) {
            chip(more, "", false, true, [&m] { state(m).sceneTab.tagsOpen = true; }, kRightX, cy, w);
            break;
          }
          const double lastEnd = column.back().x + column.back().w, lastY = column.back().y;
          if (lastEnd + kTagGap + w <= end) {
            row = static_cast<int>(std::lround((lastY - cy) / kTagStep));
            chip(more, "", false, true, [&m] { state(m).sceneTab.tagsOpen = true; },
                 lastEnd + kTagGap, lastY, w);
            break;
          }
          column.pop_back();
          --shown;
        }
      } else if (open && !d.editing) {
        const double w = chipW(t.fewerTags, "");
        const double at = place(w);
        chip(t.fewerTags, "", false, true, [&m] { state(m).sceneTab.tagsOpen = false; }, at,
             cy + row * kTagStep, w);
      }
      cy += (row + 1) * kTagStep;
    }
  } else {
    // Similar scenes in the details' place, each with why.
    text(kRightX, cy, kTextW - 300, 56, t.similar, kit::kTitleSize, false, 1);
    Button back = actionButton(
        m, t.backToDetails, StashIcon::Back,
        [&m] {
          state(m).sceneTab.similar = false;
          m.side_.position = 0;
          return MenuCommand{};
        },
        0, cy, 56);
    back.x = kRightX + kTextW - back.w;
    column.push_back(back);
    cy += 64;
    const Wrapped hint = wrapped(measure, t.similarHint, kHintSize, kTextW, 2);
    text(kRightX, cy, kTextW, hint.height, t.similarHint, kHintSize, true, 2);
    cy += hint.height + 16;
    if (d.similarGeneration != m.stash_->generation() || d.similarOf != s->id ||
        d.similarMale != male || d.similarAway != st.awayGeneration) {
      d.similarGeneration = m.stash_->generation();
      d.similarOf = s->id;
      d.similarMale = male;
      d.similarAway = st.awayGeneration;
      d.similarScenes = similarScenes(*c, index, male, st.history);
    }
    if (d.similarScenes.empty()) {
      text(kRightX, cy, kTextW, 48, t.noSimilar, kit::kWellSize, true, 1);
      cy += 56;
    }
    // Played from here, Previous and Next go through them after this one.
    std::vector<std::string> paths{stashSceneLocation(*s, m.offline_)};
    for (const auto& [i, why] : d.similarScenes)
      paths.push_back(stashSceneLocation(c->scenes()[i], m.offline_));
    constexpr int columns = 3;
    const double cardW = (kTextW - (columns - 1) * kTileGap) / columns;
    const double pictureW = cardW - 16;
    const double tileH = cardHeight(cardW);
    std::vector<int> studios;
    for (size_t k = 0; k < d.similarScenes.size(); ++k) {
      const StashScene& sc = c->scenes()[d.similarScenes[k].first];
      const std::string& location = paths[k + 1];
      Button b = sceneCard(m, *c, sc, m.library_ ? m.library_->record(location).playCount : 0, 0,
                           kRightX + (k % columns) * (cardW + kTileGap),
                           cy + (k / columns) * (tileH + kTileGap), cardW);
      b.byline = d.similarScenes[k].second;  // why it is similar, in place of her name
      b.action = [&m, paths, path = location] {
        m.playList_ = paths;
        m.playAt_ = path;
        MenuCommand cmd;
        cmd.type = MenuCommand::OpenFile;
        cmd.path = path;
        m.showPage(Menu::Page::Main);
        return cmd;
      };
      column.push_back(b);
      column.push_back(m.cardBadge(b, badge(sc)));
      if (m.stashCoversShown() && !sc.cover.empty())
        wanted.push_back(Stash::coverRequest(
            sc, static_cast<int>(std::ceil(pictureW * menu_geometry::kRasterScale))));
      if (m.stashCoversShown() && sc.studio >= 0 &&
          std::find(studios.begin(), studios.end(), sc.studio) == studios.end()) {
        studios.push_back(sc.studio);
        const StashStudio& studio = c->studios()[sc.studio];
        if (!studio.image.empty())
          wanted.push_back(Stash::logoRequest(studio, stashPixels(pictureW * kLogoShare)));
      }
      if (m.view_.thumbnails && !m.stashCovers_->coverComing(location)) frames.push_back(location);
    }
    cy += ((d.similarScenes.size() + columns - 1) / columns) * (tileH + kTileGap);
  }

  Menu::SideColumn& side = m.side_;
  side.x = kRightX;
  side.y = columnTop;
  side.w = kRight - kRightX;
  side.h = columnBottom - columnTop;
  side.max = std::max(0.0, cy + 8 - side.h);
  side.position = std::clamp(side.position, 0.0, side.max);
  for (Button& b : column) {
    b.y += side.y - side.position;
    b.side = true;
    if (b.y + b.h < side.y || b.y > side.y + side.h) continue;
    m.buttons_.push_back(std::move(b));
  }

  // Where to go from here: with tags chosen, their scenes; else more with
  // her, more from the studio, similar scenes.
  double ax = kRightX;
  auto go = [&](const std::string& label, const std::string& count, StashIcon icon,
                std::function<MenuCommand()> action, double w, bool selected) {
    Button b;
    b.panel = Menu::kBrowser;
    b.x = ax;
    b.y = kActionY;
    b.w = w;
    b.h = kActionH;
    b.label = label;
    b.size = count;
    b.selected = selected;
    b.radius = kit::kCorner;
    b.stashLook = static_cast<int>(StashLook::Go);
    b.stashIcon = static_cast<int>(icon);
    b.action = std::move(action);
    const ActionSpace a = actionSpace(kActionH);
    const double countW = count.empty() ? 0 : m.textWidth(count, kCountSize, false) + a.gap;
    const double lead = icon == StashIcon::None ? a.pad : a.pad + a.icon + a.gap;
    if (m.textWidth(label, kit::kRowSize, false) > w - lead - a.pad - countW) b.tooltip = label;
    m.buttons_.push_back(b);
    ax += w + kit::kSpace;
  };
  // Editing: Save to Stash with what it sends, and Cancel, which asks once
  // before throwing changes away; after a failed save, Try again.
  auto note = [&](const std::string& label) {
    if (ax > kRight - 40) return;
    const double h = wrapped(measure, label, kHintSize, kRight - ax - 6, 2).height;
    m.addLabel(ax + 6, kActionY + (kActionH - h) / 2, kRight - ax - 6, h, label, kHintSize, true, 2);
  };
  const int changes = addCount + removeCount;
  auto cancel = [&m, changes, stopTags] {
    if (changes > 0)
      state(m).sceneTab.confirm = true;
    else
      stopTags();
    return MenuCommand{};
  };
  if (d.editing && typing) {
    // The keyboard is there.
  } else if (d.editing && d.confirm) {
    go(changes == 1 ? std::string(t.discardOne) : stashPrinted(t.discard, changes), "",
       StashIcon::None,
       [stopTags] {
         stopTags();
         return MenuCommand{};
       },
       300, false);
    m.buttons_.back().danger = true;
    go(t.keepEditing, "", StashIcon::None,
       [&m] {
         state(m).sceneTab.confirm = false;
         return MenuCommand{};
       },
       220, false);
    note(t.nothingSent);
  } else if (d.editing && failed) {
    const double buttons = kRight - 220 - kit::kSpace - 160;
    m.addLabel(ax, kActionY, buttons - kit::kSpace - ax, kActionH, t.saveFailed, kit::kWellSize, false);
    ax = buttons;
    go(t.tryAgain, "", StashIcon::Stash, save, 220, true);
    go(base.cancel, "", StashIcon::None, cancel, 160, false);
  } else if (d.editing) {
    go(busy ? t.saving : t.saveToStash, changes ? d.summary : t.noChanges, StashIcon::Stash, save,
       460, true);
    m.buttons_.back().disabled = changes == 0;
    go(base.cancel, "", StashIcon::None, cancel, 160, false);
    m.buttons_.back().disabled = busy;
    // The last merged tag struck: which of Stash's tags it removes.
    int merged = -1;
    for (auto it = d.struck.rbegin(); it != d.struck.rend() && merged < 0; ++it)
      if (has(removed, *it) && allTags[c->tagById(*it)].ids.size() > 1) merged = c->tagById(*it);
    if (busy) {
      note(t.onlyChanges);
    } else if (merged >= 0) {
      const std::vector<std::string>& names = allTags[merged].names;
      note(names.size() == 2
               ? stashPrinted(t.removesBoth, stashJoined(names, ", "))
               : stashPrinted(t.removesAll, static_cast<int>(names.size()), stashJoined(names, ", ")));
    }
  } else if (!d.tags.empty()) {
    StashList list;
    list.format = st.list.format;
    list.sort = st.list.sort;
    list.descending = st.list.descending;
    list.tags = d.tags;
    list.tagsAll = true;
    if (d.countTags != d.tags || d.countGeneration != m.stash_->generation() ||
        d.countFormat != list.format) {
      d.countTags = d.tags;
      d.countGeneration = m.stash_->generation();
      d.countFormat = list.format;
      d.count = static_cast<int>(
          c->query(stashFilterOf(list, *c, male, static_cast<int64_t>(std::time(nullptr))), {},
                   &st.history, false)
              .scenes.size());
    }
    const int merged = c->tagById(d.tags.front());
    const std::string label = d.tags.size() == 1 && merged >= 0
                                  ? stashPrinted(t.scenesWithTag, c->tags()[merged].name)
                                  : stashPrinted(t.scenesWithTags, static_cast<int>(d.tags.size()));
    go(label, stashNumber(d.count), StashIcon::Scenes,
       [&m, list] {
         openScenes(m, list);
         return MenuCommand{};
       },
       560, true);
    go(base.clear, "", StashIcon::None,
       [&m] {
         state(m).sceneTab.tags.clear();
         return MenuCommand{};
       },
       150, false);
  } else {
    const double third = (kRight - kRightX - 2 * kit::kSpace) / 3;
    if (!cast.empty()) {
      const StashPerformer& p = c->performers()[cast.front()];
      go(stashPrinted(t.moreWith, p.name), p.scenes > 1 ? stashNumber(p.scenes - 1) : "",
         StashIcon::Performers,
         [&m, id = p.id] {
           showPerformer(m, id);
           return MenuCommand{};
         },
         third, false);
    }
    if (s->studio >= 0) {
      const StashStudio& studio = c->studios()[s->studio];
      go(stashPrinted(t.moreFrom, studio.name),
         studio.scenes > 1 ? stashNumber(studio.scenes - 1) : "", StashIcon::Studios,
         [&m, id = studio.id] {
           showStudio(m, id);
           return MenuCommand{};
         },
         third, false);
    }
    go(t.similar, "", StashIcon::Scenes,
       [&m] {
         StashSceneTab& tab = state(m).sceneTab;
         tab.similar = !tab.similar;
         m.side_.position = 0;
         return MenuCommand{};
       },
       third, d.similar);
  }
  if (typing) m.layoutKeyboard();
  finish();
}

// Under the buttons: the cover (or the app's own frame), the line over the
// file, and the box of a video Stash doesn't know.
void StashPages::drawSceneDetailsHeading(const Menu& m, cairo_t* cr) {
  const StashSceneTab& d = m.stashMenu_->sceneTab;
  const Palette& pal = m.palette();
  const std::string& path = m.view_.currentFile;
  if (!path.empty() && !m.view_.streaming) {
    m.drawThumbnail(cr, path, kLeftX, kTop, kLeftW, kCoverH, 14);
  } else {
    roundedRect(cr, kLeftX, kTop, kLeftW, kCoverH, 14);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.06);
    cairo_fill(cr);
  }
  if (d.dividerY > 0) {
    cairo_rectangle(cr, kLeftX, d.dividerY, kLeftW, 1.5);
    setColor(cr, pal.rule);
    cairo_fill(cr);
  }
  if (d.boxH > 0) {
    m.drawFinish(cr, Finish::Sunk, kRightX, d.boxY, kRight - kRightX, d.boxH, kit::kGroupCorner);
    const double dim[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a};
    drawIcon(cr, StashIcon::Stash, kRightX + 28 + 16, d.boxY + 22 + 20, 32, dim);
  }
}

void StashPages::drawSceneButton(const Menu& m, cairo_t* cr, const Button& b,
                                 const double ink[4]) {
  const Palette& pal = m.palette();
  const double dim[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a};
  switch (static_cast<StashLook>(b.stashLook)) {
    case StashLook::Tab: {  // as the strip's other tabs, with Stash's mark
      double icon[4];
      m.iconInk(b, ink, icon);
      const double iconX = b.x + kit::kSpace, textX = iconX + strip::kTabIcon + strip::kTabIconGap;
      drawIcon(cr, StashIcon::Stash, iconX + strip::kTabIcon / 2, b.y + b.h / 2, strip::kTabIcon,
               icon);
      m.drawText(cr, b.label, textX, b.y, b.x + b.w - textX - kit::kSpace + 4, b.h, b.fontSize,
                 false, false, ink[0], ink[1], ink[2]);
      return;
    }
    case StashLook::TagChip: {
      // Its label and count on the chip's own finish (raised, latched while
      // chosen); More and "+N more" in the accent, as links. Editing:
      // struck (to be removed) flat and faded, ↶ at its end to keep it; else
      // × to strike it. Add tags starts with +.
      if (b.crossed) cairo_push_group(cr);
      const double accent[4] = {pal.textAccent.r, pal.textAccent.g, pal.textAccent.b, 1};
      const double* text = b.dim ? accent : ink;
      const double countW = b.size.empty() ? 0 : m.textWidth(b.size, kCountSize, false) + kit::kSpaceTight;
      const bool plus = b.icon == Menu::Icon::Plus;
      const double lead = plus ? kChipIconW : 0;
      const double end = b.icon == Menu::Icon::Close ? kChipIconW : 0;
      if (plus) Menu::drawIcon(cr, Menu::Icon::Plus, b.x + kTagPad + 12, b.y + b.h / 2, 24);
      m.drawText(cr, b.label, b.x + kTagPad + lead, b.y, b.w - 2 * kTagPad - countW - lead - end, b.h,
                 kit::kWellSize, false, b.size.empty() && !plus, text[0], text[1], text[2]);
      if (!b.size.empty()) {
        const double* n = b.latched ? ink : dim;
        m.drawText(cr, b.size, b.x + b.w - kTagPad - end - countW + kit::kSpaceTight, b.y, countW, b.h,
                   kCountSize, false, false, n[0], n[1], n[2], b.latched ? 0.8 : 1);
      }
      if (b.crossed) {
        drawIcon(cr, StashIcon::Undo, b.x + b.w - 14 - end / 2, b.y + b.h / 2, 22, ink);
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, 0.35);
      } else if (end > 0) {
        Menu::drawIcon(cr, Menu::Icon::Close, b.x + b.w - 14 - end / 2, b.y + b.h / 2, 22);
      }
      return;
    }
    case StashLook::Go: {  // icon, label, and a count at the right end, as actionButton()'s
      const StashIcon icon = static_cast<StashIcon>(b.stashIcon);
      const ActionSpace a = actionSpace(b.h);
      const double countW = b.size.empty() ? 0 : m.textWidth(b.size, kCountSize, false) + a.gap;
      if (!b.size.empty())
        m.drawText(cr, b.size, b.x + b.w - a.pad - countW + a.gap, b.y, countW, b.h, kCountSize, false,
                   false, b.selected ? ink[0] : dim[0], b.selected ? ink[1] : dim[1],
                   b.selected ? ink[2] : dim[2], b.selected ? 0.85 : 1);
      if (icon == StashIcon::None) {
        m.drawText(cr, b.label, b.x + a.pad, b.y, b.w - 2 * a.pad - countW, b.h, kit::kRowSize, false,
                   true, ink[0], ink[1], ink[2]);
        return;
      }
      drawIcon(cr, icon, b.x + a.pad + a.icon / 2, b.y + b.h / 2, a.icon, ink);
      const double textX = b.x + a.pad + a.icon + a.gap;
      m.drawText(cr, b.label, textX, b.y, b.x + b.w - a.pad - countW - textX, b.h, kit::kRowSize,
                 false, false, ink[0], ink[1], ink[2]);
      return;
    }
    default:
      return;
  }
}
