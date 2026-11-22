#include "menu_style.h"

#include "menu_list_parts.h"
#include "menu_stash.h"
#include "menu_stash_covers.h"
#include "stash.h"
#include "stash_text.h"
#include "thumbnailer.h"

#include <algorithm>
#include <cctype>
#include <random>
#include <set>

// The Scenes page (docs/stash.md): the filter rail on the left, active
// filters as chips above the results, the results as a grid of tiles or a
// list, and in place of the results a full list of performers, tags,
// studios, years, a named look's values or the pinned lists. The query runs
// only when the list, the catalog or the male performers setting changes
// (well under a millisecond); the rail and the full list are made from its
// result then, and each layout adds buttons only for what shows.

namespace {

constexpr double kRailW = 400, kRailGap = kit::kSpace;
constexpr double kRowStep = kStashRowStep, kRowBox = 50;  // facet rows (rail and full lists)
constexpr double kLineGrow = kStashLineGrow;   // ... and each wrapped line, editing the rail
constexpr double kExcludeW = kRowBox;          // ... and their ⊘ target
constexpr double kChipH = kListHeaderH;
constexpr double kResultsGap = kit::kSpaceTight;  // under the chips
constexpr double kTileGap = kGalleryGap;
// The Sort by fields, in the dropdown's order: the dates together.
constexpr int kSortOrder[] = {int(StashSort::Released),   int(StashSort::Added),
                              int(StashSort::Modified),   int(StashSort::LastPlayed),
                              int(StashSort::Rating),     int(StashSort::Plays),
                              int(StashSort::Title),      int(StashSort::Duration),
                              int(StashSort::Resolution), int(StashSort::Size),
                              int(StashSort::CastSize),   int(StashSort::Random)};

int64_t unixNow() { return static_cast<int64_t>(std::time(nullptr)); }

// The grid's tiles across, from x0 to the list's right: about 400 wide.
int gridColumns(double x0, double right) {
  return std::max(1, static_cast<int>((right - kScrollbarRoom - x0 + kTileGap) / (400 + kTileGap)));
}

}  // namespace

// A change of the filters: the one before can be had back (Undo), and the
// results start at the top.
void StashPages::changeList(Menu& m, const std::function<void(StashList&)>& change) {
  StashMenu& st = state(m);
  StashList list = st.list;
  change(list);
  if (list == st.list) return;
  st.undo = st.list;
  st.list = list;
  st.stale = true;
  st.placePending = false;
  if (st.full == StashFull::None) {
    m.list_.offset = 0;
    m.list_.fraction = 0;
  } else {
    st.scroll = 0;
  }
  m.dirty_ = true;
}

void StashPages::editSections(Menu& m, bool on) {
  StashMenu& st = state(m);
  st.editSections = on;
  st.stale = true;
  m.side_.position = 0;
  m.dirty_ = true;
}

StashBrowse::Sources StashPages::sources(const Menu& m) {
  return {m.stash_, m.library_, m.offline_, m.downloads_.shapeGeneration};
}

StashBrowse::Page StashPages::listPage(const Menu& m) {
  return {*m.view_.stash, fixedPerformer(m), fixedStudio(m), m.page_ == Menu::Page::StashPerformers};
}

StashBrowse::Host StashPages::browseHost(Menu& m) {
  StashBrowse::Host h;
  h.catalog = [&m] { return catalog(m); };
  h.changeList = [&m](const std::function<void(StashList&)>& f) { changeList(m, f); };
  h.changePerformers = [&m](const std::function<void(StashPerformerChoice&)>& f) {
    changePerformers(m, f);
  };
  h.showFull = [&m](StashFull full, StashLookKind look) { showFull(m, full, look); };
  h.editSections = [&m](bool on) { editSections(m, on); };
  h.openPin = [&m](int pin) { openPin(m, pin); };
  // Two lines for a label too wide beside its switch and moves (as layoutRail
  // and the switch row lay them out).
  h.switchLines = [&m](const std::string& label, double indent) {
    const double w = kRailW - 14 - indent - 2 * (kRowBox + kit::kSpacePiece) - 88;
    return m.textWidth(label, Menu::kTextSize, false) > w ? 2 : 1;
  };
  return h;
}

void StashPages::showScenes(Menu& m) {
  StashMenu& st = state(m);
  m.showPage(Menu::Page::StashScenes);
  st.full = StashFull::None;
  m.list_.setPosition(st.scroll);
  m.side_.position = st.railPosition;
  st.placePending = true;
}

void StashPages::showOwnScenes(Menu& m) {
  showScenes(m);
  leavePin(m);
}

void StashPages::leaveScenes(Menu& m) {
  StashMenu& st = state(m);
  if (st.full == StashFull::None) {
    st.scroll = m.list_.offset + m.list_.fraction;
    st.place = resultsPlace(m);
  }
  st.full = StashFull::None;
  st.railPosition = m.side_.position;
}

std::vector<std::string> StashPages::resultPaths(Menu& m) {
  std::vector<std::string> paths;
  if (const StashCatalog* c = catalog(m))
    for (const int i : state(m).result.scenes)
      paths.push_back(stashSceneLocation(c->scenes()[i], m.offline_));
  return paths;
}

// As the last layout showed them: their place, none in a full list.
ListPlace StashPages::resultsPlace(Menu& m) {
  StashMenu& st = state(m);
  if (st.full != StashFull::None) return {};
  return m.listPlace(resultPaths(m), st.gallery ? gridColumns(st.resultsX, m.list_.right) : 1);
}

void StashPages::openScenes(Menu& m, const StashList& list) {
  StashMenu& st = state(m);
  if (m.page_ != Menu::Page::StashScenes) showScenes(m);
  leavePin(m);
  st.full = StashFull::None;
  if (st.list != list) {
    st.undo = st.list;
    st.list = list;
    st.stale = true;
  }
  m.editing_ = false;
  m.list_.offset = 0;
  m.list_.fraction = 0;
  st.placePending = false;
  m.dirty_ = true;
}

bool StashPages::enterList(Menu& m, std::optional<StashSavedList>& scenes,
                           const std::optional<StashSavedList>& own, int64_t id) {
  StashMenu& st = state(m);
  scenes = StashSavedList{st.list, st.undo, st.scroll, 0, st.fullByName, 0, st.place};
  st.full = StashFull::None;
  const bool kept = own && own->id == id;
  double scroll = 0;
  if (kept) {
    st.list = own->list;
    st.undo = own->undo;
    st.fullByName = own->fullByName;
    scroll = own->scroll;
    m.side_.position = own->rail;
    st.place = own->place;
  } else {
    StashList list;
    list.format = st.list.format;
    st.list = list;
    st.undo.reset();
    m.side_.position = 0;
  }
  st.scroll = scroll;
  st.placePending = kept;
  st.stale = true;
  m.list_.setPosition(scroll);
  m.dirty_ = true;
  return kept;
}

void StashPages::leaveList(Menu& m, std::optional<StashSavedList>& scenes,
                           std::optional<StashSavedList>& own, int64_t id) {
  StashMenu& st = state(m);
  if (!scenes) return;
  const double scroll =
      st.full == StashFull::None ? m.list_.offset + m.list_.fraction : st.scroll;
  own = StashSavedList{st.list, st.undo, scroll, m.side_.position, st.fullByName, id,
                       resultsPlace(m)};
  st.list = scenes->list;
  st.undo = scenes->undo;
  st.scroll = scenes->scroll;
  st.place = scenes->place;
  st.fullByName = scenes->fullByName;
  st.full = StashFull::None;
  st.fullSearch.clear();
  st.stale = true;
  scenes.reset();
}

void StashPages::showFull(Menu& m, StashFull full, StashLookKind look) {
  StashMenu& st = state(m);
  // A performer's or a studio's page shows the full list in place of its
  // scenes, the Performers page a look's in place of its cards.
  const bool cards = m.page_ == Menu::Page::StashPerformers && full == StashFull::Look;
  if (m.page_ != Menu::Page::StashScenes && m.page_ != Menu::Page::StashPerformer &&
      m.page_ != Menu::Page::StashStudio && !cards)
    showScenes(m);
  if (st.full == StashFull::None) {
    (cards ? st.performers.scroll : st.scroll) = m.list_.offset + m.list_.fraction;
    if (!cards && !st.placePending) st.place = resultsPlace(m);  // else as left, not laid out yet
  }
  st.full = full;
  st.fullLook = look;
  st.fullSearch.clear();
  st.fullStale = true;
  m.editing_ = false;
  m.list_.offset = 0;
  m.list_.fraction = 0;
  m.dirty_ = true;
}

int StashPages::pinOfList(Menu& m) {
  StashMenu& st = state(m);
  const StashList list = scopedList(m);
  for (size_t i = 0; i < st.pins.size(); ++i)
    if (st.pins[i].list == list) return static_cast<int>(i);
  return -1;
}

// While it is changed, the pin it was opened as; one it then matches wins.
int StashPages::shownPin(Menu& m) {
  StashMenu& st = state(m);
  if (m.page_ != Menu::Page::StashScenes || !st.ownScenes) return -1;
  const int pin = pinOfList(m);
  return pin >= 0 ? pin : st.pin;
}

// A pinned list as it was pinned, in the Scenes page's place: the page's
// own list is put aside until its row in the navigation brings it back, or
// another list opens there (openScenes()).
void StashPages::openPin(Menu& m, int pin) {
  StashMenu& st = state(m);
  if (pin < 0 || pin >= static_cast<int>(st.pins.size())) return;
  const bool here = m.page_ == Menu::Page::StashScenes;
  const bool listed = here && st.full == StashFull::None && !st.placePending;
  if (!here) showScenes(m);
  if (!st.ownScenes)
    st.ownScenes = StashSavedList{st.list,
                                  st.undo,
                                  listed ? m.list_.offset + m.list_.fraction : st.scroll,
                                  m.side_.position,
                                  st.fullByName,
                                  0,
                                  listed ? resultsPlace(m) : st.place};
  st.pin = pin;
  st.list = st.pins[pin].list;
  st.undo.reset();
  st.full = StashFull::None;
  st.fullSearch.clear();
  st.stale = true;
  st.placePending = false;
  m.editing_ = false;
  m.list_.offset = 0;
  m.list_.fraction = 0;
  m.side_.position = 0;
  m.dirty_ = true;
}

void StashPages::leavePin(Menu& m) {
  StashMenu& st = state(m);
  if (!st.ownScenes) return;
  const StashSavedList own = std::move(*st.ownScenes);
  st.ownScenes.reset();
  st.pin = -1;
  st.list = own.list;
  st.undo = own.undo;
  st.scroll = own.scroll;
  st.place = own.place;
  st.fullByName = own.fullByName;
  st.full = StashFull::None;
  st.fullSearch.clear();
  st.stale = true;
  st.placePending = true;
  m.editing_ = false;
  m.list_.setPosition(own.scroll);
  m.side_.position = own.rail;
  m.dirty_ = true;
}

// Plays a scene's file, with the results as the list Previous and Next go
// through; shuffled, one of them picked at random, and the player shuffles
// the list from it.
MenuCommand StashPages::open(Menu& m, const std::string& path, bool shuffled) {
  // The results of this catalog, should a new one have come since the layout.
  state(m).refresh(sources(m), listPage(m));
  return play(m, state(m).result.scenes, path, shuffled);
}

MenuCommand StashPages::play(Menu& m, const std::vector<int>& scenes, const std::string& path,
                             bool shuffled) {
  StashMenu& st = state(m);
  const StashCatalog* c = catalog(m);
  if (!c) return {};
  m.played_.list.clear();
  for (const int i : scenes) m.played_.list.push_back(stashSceneLocation(c->scenes()[i], m.offline_));
  if (m.played_.list.empty()) return {};
  m.played_.at = path;
  if (shuffled) {
    std::mt19937_64 random(static_cast<uint64_t>(unixNow()) ^ st.seed++);
    m.played_.at = m.played_.list[random() % m.played_.list.size()];
  }
  return m.openVideo(m.played_.at, shuffled);
}

bool Menu::listNeighbor(const std::string& path, int step, std::string& next,
                        const PlayOrder& order) {
  next.clear();
  if (played_.list.empty() || path != played_.at) {  // since opened from elsewhere
    played_.list.clear();
    played_.at.clear();
    return false;
  }
  next = neighborIn(played_.list, path, step, order);
  if (!next.empty()) played_.at = next;
  return true;
}

StashPages::Button StashPages::facetButton(Menu& m, const std::string& label,
                                           const std::string& count, int state,
                                           std::function<void()> include, double x, double y,
                                           double w, double h) {
  Button b;
  b.panel = Menu::kBrowser;
  b.x = x;
  b.y = y;
  b.w = w;
  b.h = h;
  b.label = label;
  b.size = count;
  b.latched = state == 1;
  b.dim = state == 2;  // left out
  b.bare = true;
  b.radius = kit::kCorner;
  b.part = PartKind::Stash, b.look = static_cast<int>(StashLook::Facet);
  b.action = [include] {
    include();
    return MenuCommand{};
  };
  const double countW = count.empty() ? 0 : m.textWidth(count, kCountSize, false) + kit::kSpace;
  if (m.textWidth(label, kit::kRowSize, false) > w - 2 * kit::kSpace - countW - (b.dim ? 32 : 0))
    b.tooltip = label;
  return b;
}

StashPages::Button StashPages::excludeButton(Menu&, int state, std::function<void()> exclude,
                                             double x, double y, double w, double h) {
  Button b;
  b.panel = Menu::kBrowser;
  b.x = x;
  b.y = y;
  b.w = w;
  b.h = h;
  b.on = state == 2;
  b.bare = true;
  b.dangerHover = true;
  b.radius = kit::kCorner;
  b.part = PartKind::Stash, b.look = static_cast<int>(StashLook::Exclude);
  b.tooltip = strings().stash.tipExclude;
  b.action = [exclude] {
    exclude();
    return MenuCommand{};
  };
  return b;
}

StashPages::Button StashPages::actionButton(Menu& m, const std::string& label, StashIcon icon,
                                            std::function<MenuCommand()> action, double x, double y,
                                            double h) {
  const ActionSpace a = actionSpace(h);
  Button b;
  b.panel = Menu::kBrowser;
  b.x = x;
  b.y = y;
  b.h = h;
  b.label = label;
  b.fontSize = h < 48 ? kHintSize : kit::kRowSize;
  b.w = m.textWidth(label, b.fontSize, false) + 2 * a.pad + (icon != StashIcon::None ? a.icon + a.gap : 0);
  b.radius = kit::kCorner;
  b.part = PartKind::Stash, b.look = static_cast<int>(StashLook::Action);
  b.stashIcon = static_cast<int>(icon);
  b.action = std::move(action);
  return b;
}

// The header row from x: Filters (latched while the rail shows; hidden,
// how many are active), the title and count (drawScenesHeading()), and at
// the right Shuffle play and pinning.
void StashPages::layoutHeader(Menu& m, double x0) {
  StashMenu& st = state(m);
  const StashStrings& t = strings().stash;
  const double y = kBrowserRowY, h = kBrowserRowH;
  Button filters = actionButton(
      m, t.filters, StashIcon::Filters,
      [&m] {
        StashMenu& s = state(m);
        s.railShown = !s.railShown;
        return MenuCommand{};
      },
      x0, y, h);
  filters.latched = st.railShown;
  filters.tooltip = t.tipFilters;
  const StashCatalog* c = catalog(m);
  const size_t active = c ? st.chips(st.list, *c, fixedPerformer(m)).size() : 0;
  if (active && !st.railShown) filters.badge = std::to_string(active);
  m.buttons_.push_back(filters);
  st.titleX = filters.x + filters.w + kit::kSpaceEdge;

  double right = kContentRight;
  auto add = [&](Button b) {
    b.x = right - b.w;
    right = b.x - kit::kSpaceTight;
    m.buttons_.push_back(b);
  };
  const int pin = pinOfList(m);
  if (pin >= 0) {
    add(actionButton(
        m, t.unpin, StashIcon::Pin,
        [&m, pin] {
          StashMenu& s = state(m);
          const bool shown = shownPin(m) == pin;
          s.pins.erase(s.pins.begin() + pin);
          saveStashPins(s.pins, s.kind);
          if (shown || s.pin == pin)  // stays shown, unpinned: Pin takes it back
            s.pin = -1;
          else if (s.pin > pin)
            --s.pin;
          return MenuCommand{};
        },
        0, y, h));
    add(actionButton(
        m, t.rename, StashIcon::None,
        [&m, pin] {
          StashMenu& s = state(m);
          s.renaming = pin;
          s.typing = StashMenu::Typing::PinName;
          m.editing_ = true;
          return MenuCommand{};
        },
        0, y, h));
  } else if (c && st.pinsLoaded) {
    add(actionButton(
        m, t.pin, StashIcon::Pin,
        [&m] {
          StashMenu& s = state(m);
          const StashList list = scopedList(m);
          s.pins.push_back({stashListName(list, catalog(m)), list});
          saveStashPins(s.pins, s.kind);
          // A pinned list changed and pinned anew shows as the new pin; the
          // Scenes page's own list pinned stays the Scenes page.
          if (s.ownScenes && m.page_ == Menu::Page::StashScenes)
            s.pin = static_cast<int>(s.pins.size()) - 1;
          return MenuCommand{};
        },
        0, y, h));
    m.buttons_.back().tooltip = t.tipPin;
  }
  if (c && !st.result.scenes.empty()) {
    add(actionButton(
        m, t.shuffle, StashIcon::Shuffle, [&m] { return open(m, "", true); }, 0, y, h));
    m.buttons_.back().tooltip = t.tipShuffle;
  }
  st.titleRight = right + kit::kSpaceTight - kit::kSpaceEdge;
}

// The tools row, as the File explorer's: list or cards, All / VR / 2D with
// their counts, the order, the search, and to the top and to the end.
void StashPages::layoutTools(Menu& m) {
  StashMenu& st = state(m);
  const StashStrings& t = strings().stash;
  const Strings& base = strings();
  const double y = kBrowserToolsY + bandHeight(m);
  auto view = [&m, &st](bool gallery, Menu::Icon icon, const char* tip) {
    Menu::ToolPiece p;
    p.icon = icon;
    p.tooltip = tip;
    p.chosen = st.gallery == gallery;
    p.action = [&m, gallery] {
      StashMenu& s = state(m);
      if (s.gallery == gallery) return MenuCommand{};
      s.gallery = gallery;
      m.list_.offset = 0;
      m.list_.fraction = 0;
      return MenuCommand{};
    };
    return p;
  };
  double x = m.toolsWell(kContentLeft, y,
                         {view(false, Menu::Icon::List, base.tipList),
                          view(true, Menu::Icon::Grid, base.tipGallery)}) +
             kColumnGap;
  // All / VR / 2D with their counts; kept in the settings. Never a choice
  // that shows nothing.
  std::vector<Menu::ToolPiece> formats;
  for (int f = 0; f < 3; ++f) {
    Menu::ToolPiece p;
    p.label = t.formats[f];
    if (catalog(m)) p.count = stashNumber(st.result.formats[f]);
    p.chosen = static_cast<int>(st.list.format) == f;
    p.disabled = catalog(m) && st.result.formats[f] == 0 && !p.chosen;
    p.action = [&m, f] {
      StashMenu& s = state(m);
      s.list.format = static_cast<StashFormat>(f);
      s.stale = true;
      if (s.full == StashFull::None) {
        m.list_.offset = 0;
        m.list_.fraction = 0;
      }
      MenuCommand c;
      c.type = MenuCommand::SetStashFormat;
      c.value = f;
      return c;
    };
    formats.push_back(std::move(p));
  }
  x = m.toolsWell(x, y, formats) + kColumnGap;
  x = layoutSort(
      m, x, t.sortFields, std::size(t.sortFields), static_cast<int>(st.list.sort),
      st.list.sort == StashSort::Random ? -1 : st.list.descending,
      [&m](int f) {
        StashMenu& s = state(m);
        const StashSort sort = static_cast<StashSort>(f);
        // A–Z for titles, else the most (newest, longest …) first.
        if (s.list.sort != sort) s.list.descending = sort != StashSort::Title;
        if (sort == StashSort::Random) s.seed = static_cast<uint64_t>(unixNow());
        s.list.sort = sort;
        s.stale = true;
        if (m.page_ != Menu::Page::StashScenes) return MenuCommand{};
        return MenuCommand{MenuCommand::SetStashScenesSort, double(s.list.descending),
                           kStashSortNames[static_cast<int>(s.list.sort)]};
      },
      [&m] {
        StashMenu& s = state(m);
        s.list.descending = !s.list.descending;
        s.stale = true;
        if (m.page_ != Menu::Page::StashScenes) return MenuCommand{};
        return MenuCommand{MenuCommand::SetStashScenesSort, double(s.list.descending),
                           kStashSortNames[static_cast<int>(s.list.sort)]};
      },
      kSortOrder);
  layoutSearch(m, x, layoutJumps(m), st.list.text, t.searchScenes, StashMenu::Typing::Search);
}

// The tools row's "Sort by" from x (Menu::toolsSort()): its fields in
// `order` (null: as listed); `descending` -1 offers no direction. Either
// starts the list at the top. Returns where the next part goes.
double StashPages::layoutSort(Menu& m, double x, const char* const* fields, int count, int chosen,
                              int descending, std::function<MenuCommand(int)> choose,
                              std::function<MenuCommand()> turn, const int* order) {
  std::vector<Menu::SortField> list;
  int at = 0;
  for (int i = 0; i < count; ++i) {
    const int f = order ? order[i] : i;
    if (f == chosen) at = i;
    list.push_back({fields[f], [&m, f, choose] {
                      m.list_.offset = 0;
                      m.list_.fraction = 0;
                      return choose(f);
                    }});
  }
  return m.toolsSort(x, kBrowserToolsY + bandHeight(m), list, at, descending < 0 ? -1 : !descending,
                     strings().stash.tipTurnOrder, [&m, turn] {
                       m.list_.offset = 0;
                       m.list_.fraction = 0;
                       return turn();
                     });
}

// To the top and to the end at the tools row's right end; returns where
// the search ends.
double StashPages::layoutJumps(Menu& m) {
  return m.toolsJumps(
      kBrowserToolsY + bandHeight(m),
      [&m] {
        m.list_.offset = 0;
        m.list_.fraction = 0;
        return MenuCommand{};
      },
      [&m] {
        m.list_.offset = 1 << 30;  // clamped to the last page by the layout
        return MenuCommand{};
      });
}

// The tools row's search from x to right, found as you type with the
// keyboard: `typing` says what it types into.
void StashPages::layoutSearch(Menu& m, double x, double right, const std::string& text,
                              const char* hint, StashMenu::Typing typing) {
  m.toolsSearch(
      x, kBrowserToolsY + bandHeight(m), right, text, hint,
      m.editing_ && state(m).typing == typing,
      [&m, typing] {
        state(m).typing = typing;
        m.editing_ = true;
        return MenuCommand{};
      },
      [&m, typing] {
        state(m).typing = typing;
        setText(m, "");
        return MenuCommand{};
      });
}

// The active filters, included latched and left out red with ⊘: tapping
// one takes it away; Undo brings back what the last change took, Clear all
// takes all. Those that don't fit on the line are counted. Returns where
// the results start.
double StashPages::layoutChips(Menu& m, double x, double right, double y) {
  StashMenu& st = state(m);
  const StashStrings& t = strings().stash;
  const StashCatalog* c = catalog(m);
  const double h = kChipH;
  if (st.undo) {
    Button undo = actionButton(
        m, t.undo, StashIcon::Undo,
        [&m] {
          StashMenu& s = state(m);
          if (!s.undo) return MenuCommand{};
          std::swap(s.list, *s.undo);
          s.undo.reset();
          s.stale = true;
          return MenuCommand{};
        },
        x, y, h);
    undo.bare = true;
    m.buttons_.push_back(undo);
    x += undo.w + kit::kSpaceTight;
  }
  const std::vector<StashChip> chips =
      c ? st.chips(st.list, *c, fixedPerformer(m)) : std::vector<StashChip>{};
  if (chips.empty()) {
    m.addLabel(x + kit::kSpace, y, right - x - kit::kSpace, h, t.noFilters, kHintSize, true);
    return y + h + kResultsGap;
  }
  Button clear = actionButton(
      m, t.clearAll, StashIcon::None,
      [&m] {
        changeList(m, [fixed = fixedPerformer(m)](StashList& l) {
          StashList cleared;
          cleared.format = l.format;
          cleared.sort = l.sort;
          cleared.descending = l.descending;
          if (fixed) cleared.performers = {fixed};
          l = cleared;
        });
        return MenuCommand{};
      },
      0, y, h);
  clear.bare = true;
  clear.x = right - clear.w;
  m.buttons_.push_back(clear);
  const double end = clear.x - kit::kSpaceTight;
  constexpr int size = kHintSize;
  for (size_t i = 0; i < chips.size(); ++i) {
    const StashChip& chip = chips[i];
    const double w =
        std::min(420.0, m.textWidth(chip.label, size, false) + 2 * kit::kSpace + (chip.out ? 30 : 0));
    const std::string more = stashPrinted(t.moreChips, static_cast<int>(chips.size() - i));
    const double moreW = i + 1 < chips.size() ? m.textWidth(more, size, false) + kit::kSpace : 0;
    if (x + w + moreW > end) {
      m.addLabel(x + kit::kSpaceTight, y, end - x, h, more, size, true);
      break;
    }
    Button b;
    b.panel = Menu::kBrowser;
    b.x = x;
    b.y = y;
    b.w = w;
    b.h = h;
    b.label = chip.label;
    b.fontSize = size;
    b.dim = chip.out;
    b.latched = !chip.out;
    b.bare = true;
    b.radius = h / 2;
    b.part = PartKind::Stash, b.look = static_cast<int>(StashLook::Chip);
    const auto remove = chip.remove;
    b.action = [&m, remove] {
      changeList(m, remove);
      return MenuCommand{};
    };
    b.tooltip = stashPrinted(t.tipRemoveFilter, chip.label);
    m.buttons_.push_back(b);
    x += w + kit::kSpaceTight;
  }
  return y + h + kResultsGap;
}

// The rail's lines that show, in the side column (which scrolls on its own).
void StashPages::layoutRail(Menu& m, const std::vector<StashRailLine>& rail, double height,
                            double top, double bottom) {
  const StashStrings& t = strings().stash;
  const double x = kContentLeft;
  Menu::SideColumn& side = m.side_;
  side.x = x;
  side.y = top;
  side.w = kRailW;
  side.h = bottom - top;
  side.max = std::max(0.0, height - side.h);
  side.position = std::clamp(side.position, 0.0, side.max);
  const double rowW = kRailW - 14;  // clear of its scroll mark
  double y = top - side.position;
  for (const StashRailLine& l : rail) {
    const double y0 = y;
    y += l.height;
    if (y < top || y0 > bottom) continue;
    switch (l.kind) {
      case StashRailLine::Heading: {
        // As a group's title, in line with the rows' names; with two or
        // more chosen, All of / Any of in a small well at its right.
        constexpr double h = kListHeaderH;
        Button b;
        b.panel = Menu::kBrowser;
        b.label = l.label;
        b.part = PartKind::List, b.look = static_cast<int>(ListLook::Heading);
        b.bare = true;
        b.dimInk = true;
        b.x = x + kit::kSpace - 12;
        b.y = y0 + kit::kSpaceTight;
        b.w = std::min(rowW, m.textWidth(l.label, kit::kGroupTitleSize, true) + 24);
        b.h = h;
        b.side = true;
        m.buttons_.push_back(b);
        if (l.mode >= 0) {
          std::vector<Menu::ToolPiece> modes;
          for (int all = 0; all < 2; ++all) {
            Menu::ToolPiece p;
            p.label = all ? t.allOf : t.anyOf;
            p.tooltip = t.tipMatch[all];
            p.chosen = l.mode == all;
            p.action = [setAll = l.setAll, all] {
              setAll(all == 1);
              return MenuCommand{};
            };
            modes.push_back(std::move(p));
          }
          const size_t first = m.buttons_.size();
          m.toolsWell(x + rowW - m.toolsWellWidth(modes), y0 + kit::kSpaceTight, modes, h);
          for (size_t i = first; i < m.buttons_.size(); ++i) m.buttons_[i].side = true;
        }
        break;
      }
      case StashRailLine::Sub:
        m.addLabel(x + kit::kSpace, y0 + 4, rowW - kit::kSpace, l.height - 4, l.label, kHintSize, true)
            .side = true;
        break;
      case StashRailLine::Note:
        m.addLabel(x + kit::kSpace, y0 + 6, rowW - kit::kSpace, 0, l.label, kHintSize, true, 2).side =
            true;
        break;
      case StashRailLine::Gap:
        break;
      case StashRailLine::Link: {
        Button b;
        b.panel = Menu::kBrowser;
        b.x = x;
        b.y = y0;
        b.w = rowW;
        b.h = kRowBox;
        b.label = l.label;
        b.bare = true;
        b.radius = kit::kCorner;
        b.side = true;
        b.part = PartKind::Stash, b.look = static_cast<int>(StashLook::Link);
        const auto include = l.include;
        b.action = l.command ? l.command : [include] {
          include();
          return MenuCommand{};
        };
        m.buttons_.push_back(b);
        break;
      }
      case StashRailLine::Row: {
        if (l.command) {  // a section or subgroup, while editing the rail
          const double moveW = kRowBox, gap = kit::kSpacePiece;
          const double indent = l.subgroup ? 12 : 0;  // as the rail's Sub labels
          const double w = rowW - indent - 2 * (moveW + gap);
          const double grow = (l.lines - 1) * kLineGrow;
          Button& row = m.addSwitch(x + indent, y0, w, kRowBox + grow, l.label, l.state == 1, l.command);
          row.disabled = l.inactive;
          row.side = true;
          row.lines = l.lines;
          double mx = x + indent + w + gap;
          for (const auto& [move, icon, tip] :
               {std::tuple{l.up, Menu::Icon::MoveUp, t.tipMoveUp},
                std::tuple{l.down, Menu::Icon::MoveDown, t.tipMoveDown}}) {
            if (move) {
              Button& b = m.addRound(icon, tip, move, mx, y0 + grow / 2, moveW);
              b.panel = Menu::kBrowser;
              b.bare = true;
              b.radius = kit::kCorner;
              b.side = true;
              b.disabled = l.inactive;
            }
            mx += moveW + gap;
          }
          break;
        }
        const double w = l.exclude ? rowW - kExcludeW - kit::kSpacePiece : rowW;
        Button b = facetButton(m, l.label, l.count, l.state, l.include, x, y0, w, kRowBox);
        if (b.tooltip.empty()) b.tooltip = t.tipFilter;
        b.side = true;
        m.buttons_.push_back(b);
        if (l.exclude) {
          Button ex = excludeButton(m, l.state, l.exclude, x + w + kit::kSpacePiece, y0, kExcludeW,
                                    kRowBox);
          ex.side = true;
          m.buttons_.push_back(ex);
        }
        break;
      }
    }
  }
}

// As the file browser's: Stash's covers, and frames of the files without one;
// first a page's own pictures (StashCovers::wantFirst()).
void StashPages::wantThumbnails(Menu& m, int first, int end, int page, double pictureW) {
  if (!m.thumbnailer_ || (!m.view_.thumbnails && !m.stashCoversShown())) {
    if (m.thumbnailer_) m.thumbnailer_->want({}, 0);
    std::vector<StashImages::Request> pictures = m.stashCovers_->takeFirst();
    if (!pictures.empty()) m.stash_->images().want(pictures);
    return;
  }
  const StashCatalog* c = catalog(m);
  const std::vector<int>& scenes = state(m).result.scenes;
  const int count = static_cast<int>(scenes.size());
  std::vector<std::string> wanted;
  std::set<int> taken;
  auto add = [&](int i) {
    if (c && i >= 0 && i < count && taken.insert(i).second)
      wanted.push_back(stashSceneLocation(c->scenes()[scenes[i]], m.offline_));
  };
  for (int i = first; i < end; ++i) add(i);
  const size_t shown = wanted.size();
  for (int i = end; i < end + std::max(30, page * 3); ++i) add(i);
  for (int i = count - page * 2; i < count; ++i) add(i);
  for (int i = 0; i < page * 2; ++i) add(i);
  for (int i = first - 1; i >= first - page * 2; --i) add(i);
  if (m.wantedAlready(wanted, shown, pictureW)) return;
  m.wantStashImages(wanted, shown, pictureW);
  std::vector<std::string> frames;
  if (m.view_.thumbnails)
    for (const std::string& path : wanted)
      if (!m.stashCovers_->coverComing(path)) frames.push_back(path);
  m.thumbnailer_->want(frames, stashPixels(pictureW));
}

StashPages::Button StashPages::sceneCard(const Menu& m, const StashCatalog& c, const StashScene& s,
                                         int plays, float rating, double x, double y, double w) {
  Button b;
  b.panel = Menu::kBrowser;
  b.x = x;
  b.y = y;
  b.w = w;
  b.h = cardHeight(w);
  b.radius = kCardCorner;
  b.card = true;
  b.label = title(m, s);
  b.byline = performers(m, s);
  // The studio's mark on the picture names it.
  b.detail = stashJoined({s.studio >= 0 && !m.stashCoversShown() ? c.studios()[s.studio].name : "",
                          s.date, s.duration > 0 ? formatTime(s.duration) : ""});
  const std::string location = stashSceneLocation(s, m.offline_);
  b.selected = location == m.view_.currentFile;
  ListItem item{};
  item.thumbPath = location;
  m.setThumbnailOrIcon(b, item);
  b.offlinePath = location;
  if (plays > 0) b.plays = stashNumber(plays);
  b.rating = rating;
  // A title longer than its line shows in full while pointed at.
  const double nameW = w - 2 * kit::kSpace - (b.selected ? kPlayMarkW : 0);
  if (m.shortened(b.label, kit::kRowSize, nameW, b.byline.empty() ? 2 : 1)) b.tooltip = b.label;
  return b;
}

double StashPages::cardHeight(double w) {
  return kCardIn + std::round((w - 2 * kCardIn) * 9 / 16) + kCardNameGap + 3 * kCardLine + kit::kSpace;
}

void StashPages::addSceneCard(Menu& m, const Button& card, const StashScene& s) {
  m.buttons_.push_back(card);
  m.addOfflineButtons(card, 0, card.y + card.h - kit::kSpace - kCardLine);
  m.buttons_.push_back(m.cardBadge(card, badge(s)));
  Button stars = m.cardRating(card.offlinePath, card.rating, card);
  stars.scrolls = card.scrolls;
  stars.side = card.side;
  m.buttons_.push_back(stars);
}

// Cards from `top` down, as the File explorer's: three columns beside the
// rail, four without it (fewer beside a performer's profile).
void StashPages::layoutGrid(Menu& m, double x0, double top) {
  StashMenu& st = state(m);
  const StashCatalog& c = *catalog(m);
  const int columns = gridColumns(x0, m.list_.right);
  const double cardW = m.gridCellWidth(x0, columns, kTileGap);
  const double rowH = cardHeight(cardW) + kTileGap;
  const std::vector<int>& scenes = st.result.scenes;
  const int count = static_cast<int>(scenes.size());
  m.list_.top = top;
  int first, end;
  m.gridRows(count, columns, rowH, first, end);
  const bool history = st.history.size() == c.scenes().size();
  for (int i = first; i < end; ++i) {
    const int cell = i - first, index = scenes[i];
    const StashScene& s = c.scenes()[index];
    Button b = sceneCard(m, c, s, history ? st.history[index].plays : 0,
                         history ? st.history[index].rating : 0,
                         x0 + (cell % columns) * (cardW + kTileGap),
                         top + (cell / columns - m.list_.fraction) * rowH, cardW);
    b.scrolls = true;
    b.action = [&m, path = b.offlinePath] { return open(m, path, false); };
    addSceneCard(m, b, s);
  }
  wantThumbnails(m, first, end, m.list_.rows * columns, cardW - 2 * kCardIn);
  m.addScrollbar();
}

// Rating, plays, release date and length; further apart where their
// headings need it (sceneApart()).
StashPages::SceneColumns StashPages::sceneColumns(const Menu& m, double x1) {
  const std::array<double, 3>& apart = sceneApart(m);
  SceneColumns c;
  c.length = x1 - kit::kSpace - kLengthColumnW;
  c.released = c.length - kColumnGap - apart[2] - kReleasedColumnW;
  c.plays = c.released - kColumnGap - apart[1] - kPlaysColumnW;
  c.rating = c.plays - kColumnGap - apart[0] - kRatingColumnW;
  c.status = c.rating - kStatusGap - kStatusColumnW;
  return c;
}

// Rating's and the release date's headings start at their columns, Plays'
// and Length's end at theirs (layoutList()); measured once a language.
const std::array<double, 3>& StashPages::sceneApart(const Menu& m) {
  auto& a = m.stashMenu_->sceneApart;
  if (a.strings == &strings()) return a.apart;
  const StashStrings& t = strings().stash;
  a.strings = &strings();
  a.apart = {};
  const SceneColumns c = sceneColumns(m, 0);
  a.apart = {m.headingsApart(t.columns[1], c.rating + 2, false, t.columns[2],
                             c.plays + kPlaysColumnW, true),
             m.headingsApart(t.columns[2], c.plays + kPlaysColumnW, true, t.columns[3], c.released,
                             false),
             m.headingsApart(t.columns[3], c.released, false, t.columns[4], c.length + kLengthColumnW,
                             true)};
  return a.apart;
}

// The column headings (sorting by theirs), then the rows: the File
// explorer's, their names up to the columns, which are drawn over them.
void StashPages::layoutList(Menu& m, double x0, double top) {
  StashMenu& st = state(m);
  const StashStrings& t = strings().stash;
  const StashCatalog& c = *catalog(m);
  const double rowW = m.list_.right - x0 - kScrollbarRoom;
  const SceneColumns cols = sceneColumns(m, x0 + rowW);
  auto heading = [&](int column, double x, double w, bool rightAligned, int sort) {
    const bool on = sort >= 0 && static_cast<int>(st.list.sort) == sort;
    std::function<MenuCommand()> by;
    if (sort >= 0)
      by = [&m, sort] {
        StashMenu& s = state(m);
        const StashSort sorted = static_cast<StashSort>(sort);
        if (s.list.sort == sorted)
          s.list.descending = !s.list.descending;
        else
          s.list.descending = sorted != StashSort::Title;
        s.list.sort = sorted;
        s.stale = true;
        m.list_.offset = 0;
        m.list_.fraction = 0;
        return MenuCommand{};
      };
    m.listHeading(t.columns[column], x, w, rightAligned, top, on, !st.list.descending,
                  sort >= 0 ? strings().tipSortColumn : "", std::move(by));
  };
  Button probe;
  probe.x = x0;
  probe.h = kFileRowH - kRowGap;
  double px, py, pw, ph;
  m.listPicture(probe, px, py, pw, ph);
  const std::vector<int>& scenes = st.result.scenes;
  if (!scenes.empty()) {  // none over an empty list
    heading(0, px + pw + kRowNameGap, 400, false, static_cast<int>(StashSort::Title));
    if (m.offline_) {  // the status column's: the Offline place
      Button& place = m.addRound(Menu::Icon::Download, strings().offline.nav,
                                 [&m] {
                                   m.showOffline();
                                   return MenuCommand{};
                                 },
                                 cols.status, top, kListHeaderH);
      place.panel = Menu::kBrowser;
      place.bare = true;
      place.iconSize = 22;
    }
    heading(1, cols.rating + 2, kRatingColumnW, false, static_cast<int>(StashSort::Rating));
    heading(2, cols.plays, kPlaysColumnW, true, static_cast<int>(StashSort::Plays));
    heading(3, cols.released, kReleasedColumnW, false, static_cast<int>(StashSort::Released));
    heading(4, cols.length, kLengthColumnW, true, static_cast<int>(StashSort::Duration));
  }
  m.list_.top = top + kListHeaderH + kit::kSpacePiece;
  m.list_.rowHeight = kFileRowH;
  m.list_.rows = std::max(1, static_cast<int>((m.list_.bottom - m.list_.top) / kFileRowH));
  m.endOnWholeRow();
  m.list_.count = static_cast<int>(scenes.size());
  int first, end;
  m.clampScroll(m.list_.count, m.list_.rows, first, end);
  const bool history = st.history.size() == c.scenes().size();
  for (int i = first; i < end; ++i) {
    const int index = scenes[i];
    const StashScene& s = c.scenes()[index];
    const std::string location = stashSceneLocation(s, m.offline_);
    Button b;
    b.panel = Menu::kBrowser;
    b.x = x0;
    b.y = m.list_.top + (i - m.list_.offset - m.list_.fraction) * kFileRowH;
    b.w = rowW;
    b.h = kFileRowH - kRowGap;
    b.radius = kit::kCorner;
    b.label = title(m, s);
    b.byline = performers(m, s);
    b.selected = location == m.view_.currentFile;
    b.listRow = true;
    b.scrolls = true;
    b.fontSize = kit::kRowSize;
    ListItem item{};
    item.thumbPath = location;
    m.setThumbnailOrIcon(b, item);
    b.inner = x0 + rowW - kit::kSpace - (cols.status - kColumnGap);  // the columns' room
    b.action = [&m, location] { return open(m, location, false); };
    b.tooltip = b.label + (b.byline.empty() ? "" : "  ·  " + b.byline);
    b.offlinePath = location;
    m.buttons_.push_back(b);
    m.addOfflineButtons(b, cols.status, 0);
    Button columns;
    columns.panel = Menu::kBrowser;
    columns.x = b.x;
    columns.y = b.y;
    columns.w = b.w;
    columns.h = b.h;
    columns.part = PartKind::Stash, columns.look = static_cast<int>(StashLook::SceneColumns);
    if (history && st.history[index].plays > 0) columns.plays = stashNumber(st.history[index].plays);
    columns.date = s.date;
    columns.caption = s.duration > 0 ? formatTime(s.duration) : "";
    columns.offlinePath = location;
    columns.scrolls = true;
    m.buttons_.push_back(columns);
    // Unrated, shown only while the row is pointed at (hoverLook()).
    Button stars = m.ratingButton(location, history ? st.history[index].rating : 0, Menu::kBrowser,
                                  cols.rating, b.y + kRowPictureIn, kRowStarW, b.h - 2 * kRowPictureIn);
    stars.scrolls = true;
    stars.iconSize = kRowStarSize;
    stars.part = PartKind::List, stars.look = static_cast<int>(ListLook::RowStars);
    m.buttons_.push_back(stars);
  }
  wantThumbnails(m, first, end, m.list_.rows, pw);
  m.addScrollbar();
}

namespace {

std::string fullTitle(const StashMenu& st, bool cards) {
  const StashStrings& t = strings().stash;
  if (st.full == StashFull::Pins) return t.pins;
  const int kind = st.full == StashFull::Performers ? 0
                   : st.full == StashFull::Tags     ? 1
                   : st.full == StashFull::Studios  ? 2
                                                    : -1;
  const std::string what = kind >= 0 ? t.sections[kind]
                           : st.full == StashFull::Look
                               ? stashLookHeading(static_cast<int>(st.fullLook))
                               : t.released;
  int n = 0;
  for (const auto& r : st.fullRows) n += r.state != 2;
  return stashPrinted(cards ? t.fullTitlePerformers : t.fullTitle, what, stashNumber(n));
}

}  // namespace

// A full list in place of the results: its tools row (Back, its title, its
// order and search), a row of letters while A–Z, then its rows in two
// columns.
void StashPages::layoutFull(Menu& m, double x0, double top) {
  StashMenu& st = state(m);
  const StashStrings& t = strings().stash;
  if (st.fullStale) st.buildFull(sources(m), listPage(m), st.performers);
  const double right = m.list_.right - kListRightPad;  // the page's, or a performer's profile's left
  const double h = kBrowserToolsH;
  const bool cards = m.page_ == Menu::Page::StashPerformers;  // in place of its cards
  Button back = actionButton(
      m, cards ? t.backToPerformers : t.back, StashIcon::Back,
      [&m, cards] {
        StashMenu& s = state(m);
        s.full = StashFull::None;
        m.editing_ = false;
        const double scroll = cards ? s.performers.scroll : s.scroll;
        m.list_.setPosition(scroll);
        return MenuCommand{};
      },
      x0, top, h);
  m.buttons_.push_back(back);
  st.fullTitleX = back.x + back.w + kit::kSpaceEdge;
  double listTop = top + h + kit::kSpace;
  const bool searchable = st.full != StashFull::Pins;
  st.fullTitleRight = right;
  if (searchable) {
    // Most first or A–Z in a well; years: one button, newest or oldest
    // first, that turns it around.
    const bool years = st.full == StashFull::Years;
    auto order = [&m, years](int byName) {
      return [&m, byName, years] {
        StashMenu& s = state(m);
        s.fullByName = years ? !s.fullByName : byName == 1;
        s.fullStale = true;
        m.list_.offset = 0;
        m.list_.fraction = 0;
        return MenuCommand{};
      };
    };
    std::vector<Menu::ToolPiece> orders;
    Button flip;
    double sortW = 0;
    if (years) {
      flip = actionButton(m, t.yearOrder[st.fullByName ? 1 : 0], StashIcon::None, order(1), 0, 0, h);
      flip.tooltip = t.tipYearSort;
      sortW = flip.w;
    } else {
      for (int byName = 0; byName <= 1; ++byName) {
        Menu::ToolPiece p;
        p.label = byName ? t.alphabetical : t.byCount;
        p.tooltip = t.tipFullSort[byName];
        p.chosen = st.fullByName == (byName == 1);
        p.action = order(byName);
        orders.push_back(std::move(p));
      }
      sortW = m.toolsWellWidth(orders);
    }
    double x = right - 420 - kColumnGap - sortW;
    double toolsY = top;
    st.fullTitleRight = x - kit::kSpaceEdge;
    // Allow long titles to ellipsize, but reserve enough space to read one.
    // On a narrow pane, the tools lead a second row and search fills it.
    const double titleW = std::min(240.0, m.textWidth(fullTitle(st, cards), kit::kRowSize, true));
    if (st.fullTitleRight - st.fullTitleX < titleW) {
      x = x0;
      toolsY = listTop;
      listTop += h + kit::kSpace;
      st.fullTitleRight = right;
    }
    if (years) {
      flip.x = x;
      flip.y = toolsY;
      m.buttons_.push_back(flip);
    } else {
      m.toolsWell(x, toolsY, orders);
    }

    // Its own search, as the tools row's (which has the page's search).
    const int kind = st.full == StashFull::Performers ? 0
                     : st.full == StashFull::Tags     ? 1
                     : st.full == StashFull::Studios  ? 2
                                                      : 3;
    const char* hint = st.full == StashFull::Look ? t.findLooks[static_cast<int>(st.fullLook)]
                                                  : t.find[kind];
    m.toolsSearch(
        x + sortW + kColumnGap, toolsY, right, st.fullSearch, hint,
        m.editing_ && st.typing == StashMenu::Typing::FullSearch,
        [&m] {
          state(m).typing = StashMenu::Typing::FullSearch;
          m.editing_ = true;
          return MenuCommand{};
        },
        [&m] {
          state(m).typing = StashMenu::Typing::FullSearch;
          setText(m, "");
          return MenuCommand{};
        });
  }
  const int columns = 2;
  // A–Z: the letters the list has, in a well; one scrolls to its first row.
  if (st.fullByName && st.full != StashFull::Years && st.full != StashFull::Pins) {
    std::vector<std::pair<std::string, int>> letters;  // letter, first row
    for (size_t i = 0; i < st.fullRows.size(); ++i) {
      if (st.fullRows[i].state) continue;
      const unsigned char first = st.fullRows[i].label.empty() ? '#' : st.fullRows[i].label[0];
      const std::string letter =
          std::isalpha(first) ? std::string(1, static_cast<char>(std::toupper(first))) : "#";
      if (letters.empty() || letters.back().first != letter)
        if (std::none_of(letters.begin(), letters.end(),
                         [&](const auto& l) { return l.first == letter; }))
          letters.emplace_back(letter, static_cast<int>(i));
    }
    constexpr double in = kit::kSpacePiece, wellH = 48;
    const size_t n = std::max<size_t>(1, letters.size());
    const double w = std::min(kViewPieceW, (right - x0 - in) / n - in);
    Button well;
    well.panel = Menu::kBrowser;
    well.well = true;
    well.x = x0;
    well.y = listTop;
    well.w = in + n * (w + in);
    well.h = wellH;
    well.radius = kit::kCorner;
    m.buttons_.push_back(well);
    double lx = x0 + in;
    for (const auto& [letter, row] : letters) {
      Button b = m.textButton(letter, Menu::Icon::None, [&m, row = row] {
        m.list_.offset = row / columns;
        m.list_.fraction = 0;
        return MenuCommand{};
      });
      b.panel = Menu::kBrowser;
      b.piece = true;
      b.x = lx;
      b.y = listTop + in;
      b.w = w;
      b.h = wellH - 2 * in;
      b.radius = kit::kPieceCorner;
      b.fontSize = kit::kWellSize;
      b.tooltip = t.tipJumpLetter;
      m.buttons_.push_back(b);
      lx += w + in;
    }
    listTop += wellH + kit::kSpace;
  }

  const std::vector<StashMenu::FullRow>& rows = st.fullRows;
  m.list_.top = listTop;
  int first, end;
  m.gridRows(static_cast<int>(rows.size()), columns, kRowStep, first, end);
  const double cellW = m.gridCellWidth(x0, columns, kit::kSpace);
  for (int i = first; i < end; ++i) {
    const StashMenu::FullRow& r = rows[i];
    const double cx = x0 + (i % columns) * (cellW + kit::kSpace);
    const double cy = listTop + (i / columns - m.list_.offset - m.list_.fraction) * kRowStep;
    const double w = r.exclude ? cellW - kExcludeW - kit::kSpacePiece : cellW;
    Button b = facetButton(m, r.label, r.count, r.state, r.include, cx, cy, w, kRowBox);
    if (st.full != StashFull::Pins && b.tooltip.empty()) b.tooltip = t.tipFilter;
    b.scrolls = true;
    m.buttons_.push_back(b);
    if (r.exclude) {
      Button ex = excludeButton(m, r.state, r.exclude, cx + w + kit::kSpacePiece, cy, kExcludeW, kRowBox);
      ex.scrolls = true;
      m.buttons_.push_back(ex);
    }
  }
  m.addScrollbar();
}

// The rail at the left, then in the rest the chips and the results (or a
// full list), the File explorer's list or cards.
void StashPages::layoutResults(Menu& m, double top, double right) {
  StashMenu& st = state(m);
  const bool editing = m.editingFilter();
  const double bottom = editing ? kKeyboardY - 16 : kListBottom;
  double resultsX = kContentLeft;
  if (st.railShown && catalog(m)) {
    layoutRail(m, st.rail, st.railHeight, top, bottom);
    resultsX = kContentLeft + kRailW + kRailGap;
  }
  m.list_.left = resultsX;
  m.list_.right = right + kListRightPad;
  m.listShowsFolder_ = false;
  m.listColumns_ = false;
  m.list_.bottom = bottom;
  m.list_.top = top;
  st.resultsX = resultsX;
  st.resultsTop = layoutChips(m, resultsX, right, top);
  double notHereY = st.resultsTop + 20;
  st.notHere = catalog(m) && st.full == StashFull::None &&
               layoutNotHere(m, resultsX + 24, notHereY, right - resultsX - 48);
  if (catalog(m) && !st.notHere) {
    if (st.placePending && st.full == StashFull::None) {
      st.placePending = false;
      m.scrollToPlace(st.place, resultPaths(m),
                      st.gallery ? gridColumns(resultsX, m.list_.right) : 1);
    }
    if (st.full != StashFull::None)
      layoutFull(m, resultsX, st.resultsTop);
    else if (st.gallery)
      layoutGrid(m, resultsX, st.resultsTop);
    else
      layoutList(m, resultsX, st.resultsTop);
  }
  // A page's own pictures the results didn't ask for with their covers (a
  // full list shows).
  std::vector<StashImages::Request> pictures = m.stashCovers_->takeFirst();
  if (!pictures.empty()) m.stash_->images().want(pictures);
  if (editing) m.layoutKeyboard();
}

// The header row, the tools row, then the rail and the results.
void StashPages::layoutScenes(Menu& m) {
  state(m).refresh(sources(m), listPage(m));
  layoutResults(m, kGridTop, kContentRight);
  layoutHeader(m, kContentLeft);
  layoutTools(m);
}

std::string StashPages::title(const Menu& m, const StashScene& s) {
  return m.view_.stash->titles ? s.displayTitle() : s.fileName();
}

std::string StashPages::performers(const Menu& m, const StashScene& s) {
  const StashCatalog* c = catalog(m);
  std::string names;
  if (!c) return names;
  for (const int p : s.performers) {
    const StashPerformer& performer = c->performers()[p];
    if (!stashShown(performer.gender, m.view_.stash->malePerformers)) continue;
    names += (names.empty() ? "" : ", ") + performer.name;
  }
  return names;
}

// "VR · 8K", "2D · 1080p": the resolution band the filter has it in.
std::string StashPages::badge(const StashScene& s) {
  const StashStrings& t = strings().stash;
  if (s.width <= 0) return s.vr ? t.vr : t.flat;
  const int smaller = std::min(s.width, s.height);
  const char* band;
  if (s.vr) {
    band = s.width >= 7600   ? "8K"
           : s.width >= 6900 ? "7K"
           : s.width >= 5700 ? "6K"
           : s.width >= 5000 ? "5K"
           : s.width >= 3800 ? "4K"
                             : "≤3K";
  } else {
    band = smaller >= 2100 ? "4K" : smaller >= 1000 ? "1080p" : smaller >= 700 ? "720p" : "SD";
  }
  return std::string(s.vr ? t.vr : t.flat) + " · " + band;
}

// Over everything but the buttons: the title and count, the full list's
// title, and what an empty page means.
void StashPages::drawScenesHeading(const Menu& m, cairo_t* cr) {
  const StashMenu& st = *m.stashMenu_;
  const StashStrings& t = strings().stash;
  const Palette& pal = m.palette();
  const StashCatalog* c = catalog(m);
  std::string heading = m.page_ == Menu::Page::StashPerformer ? performerTitle(m) : t.scenes;
  const int studio = c && fixedStudio(m) ? c->studioById(fixedStudio(m)) : -1;
  if (studio >= 0) heading = c->studios()[studio].name;
  const StashList list = scopedList(m);
  bool pinned = false;
  for (const StashPin& p : st.pins)
    if (p.list == list) {
      heading = p.name;
      pinned = true;
    }
  // A pinned list keeps its name while changed.
  if (!pinned && m.page_ == Menu::Page::StashScenes && st.ownScenes && st.pin >= 0 &&
      st.pin < static_cast<int>(st.pins.size()))
    heading = st.pins[st.pin].name;
  if (m.editing_ && st.typing == StashMenu::Typing::PinName)  // being renamed
    heading.insert(heading == m.filterText() ? m.cursorAt() : heading.size(), "▏");
  // The title as a page's, how many after it while there is room.
  constexpr int countSize = 23;
  std::string count;
  if (c) {
    const long shown = static_cast<long>(st.result.scenes.size());
    const long all = st.result.formats[static_cast<int>(st.list.format)];
    count = shown == all ? stashScenes(shown)
                         : stashPrinted(t.countOf, stashNumber(shown), stashNumber(all));
  }
  const double x = st.titleX, room = st.titleRight - x;
  const double countW = count.empty() ? 0 : m.textWidth(count, countSize, false) + kit::kSpaceEdge;
  const double titleW = std::min(m.textWidth(heading, kit::kTitleSize, true) + 4,
                                 std::max(std::min(room, 200.0), room - countW));
  m.drawText(cr, heading, x, kBrowserRowY, titleW, kBrowserRowH, kit::kTitleSize, true, false,
             pal.text.r, pal.text.g, pal.text.b);
  if (!count.empty() && titleW + countW <= room)
    m.drawText(cr, count, x + titleW + kit::kSpaceEdge, kBrowserRowY, countW, kBrowserRowH, countSize,
               false, false, pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a);
  if (!c)
    drawWaiting(m, cr, st.resultsX + 24, st.resultsTop + 20, kContentRight - st.resultsX - 48);
  else if (st.full == StashFull::None && st.result.scenes.empty() && !st.notHere)
    m.drawEmptyPage(cr, Menu::Icon::Film, t.noScenes);
  drawFullHeading(m, cr);
}

void StashPages::drawFullHeading(const Menu& m, cairo_t* cr) {
  const StashMenu& st = *m.stashMenu_;
  if (!catalog(m) || st.full == StashFull::None) return;
  const StashStrings& t = strings().stash;
  const Palette& pal = m.palette();
  const std::string title = fullTitle(st, m.page_ == Menu::Page::StashPerformers);
  m.drawText(cr, title, st.fullTitleX, st.resultsTop, st.fullTitleRight - st.fullTitleX,
             kBrowserToolsH, kit::kRowSize, true, false, pal.text.r, pal.text.g, pal.text.b);
  if (st.fullRows.empty() && !st.fullSearch.empty())
    m.drawEmptyPage(cr, Menu::Icon::Search, stashPrinted(t.noMatches, st.fullSearch));
}
