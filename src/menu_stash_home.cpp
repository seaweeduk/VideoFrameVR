#include "menu_style.h"

#include "menu_list_parts.h"
#include "menu_stash.h"
#include "menu_stash_covers.h"
#include "stash.h"
#include "thumbnailer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>

// The Stash home (docs/stash.md): what "Stash" in the navigation opens.
// Shelves, in this order: the pinned lists, new in Stash, your top rated,
// your top performers and random picks; each opens the Scenes page with its
// list (a performer opens her page). The shelves are made when the page is
// opened (a new handful of random picks), and again for a new catalog, the
// format or changed pins, not when a rating changes: a tile rated here stays
// where it is. The page scrolls as a list does, by steps of kStep pixels.

namespace {

constexpr int kSceneColumns = 5, kPerformerColumns = StashHomePage::kPerformerColumns;
constexpr double kTileGap = kGalleryGap;
constexpr double kStep = 100;     // what the list counts as a row
constexpr double kHeadingH = 48;  // a shelf's heading row
constexpr double kPinH = kBrowserRowH;
constexpr double kShelfGap = kit::kSpaceWide;

int64_t unixNow() { return static_cast<int64_t>(std::time(nullptr)); }

constexpr int kNew = StashHomePage::kNew, kTopRated = StashHomePage::kTopRated,
              kTopPerformers = StashHomePage::kTopPerformers, kRandom = StashHomePage::kRandom,
              kShelves = StashHomePage::kShelves;

}  // namespace

namespace {

StashHomePage& pageOf(StashMenu& st) {
  if (!st.home) st.home = std::make_shared<StashHomePage>();
  return *st.home;
}

}  // namespace

void StashPages::staleHome(Menu& m) {
  if (state(m).home) state(m).home->stale = true;
}

void StashPages::showHome(Menu& m) {
  StashMenu& st = state(m);
  m.showPage(Menu::Page::StashHome);
  StashHomePage& p = pageOf(st);
  p.stale = true;
  p.seed = static_cast<uint64_t>(unixNow()) * 1000 + ++p.visits;  // new random picks
  m.list_.offset = static_cast<int>(std::floor(p.scroll));
  m.list_.fraction = p.scroll - m.list_.offset;
}

void StashPages::leaveHome(Menu& m) {
  pageOf(state(m)).scroll = m.list_.offset + m.list_.fraction;
}

// The header row: its title (drawHomeHeading()), then at the right All /
// VR / 2D with their counts, and Search Stash: the Scenes page, all of
// the format, typing into its search.
void StashPages::layoutHomeHeader(Menu& m, const StashHomePage& p, const StashCatalog* c) {
  const StashStrings& t = strings().stash;
  const double right = kContentRight;
  const double y = kBrowserRowY;
  const StashFormat shown = shownFormat(m);
  std::vector<Menu::ToolPiece> formats;
  for (int f = 0; f < 3; ++f) {
    Menu::ToolPiece piece;
    piece.label = t.formats[f];
    if (c) piece.count = stashNumber(p.formats[f]);
    piece.chosen = static_cast<int>(shown) == f;
    piece.disabled = c && p.formats[f] == 0 && !piece.chosen;
    piece.action = [&m, f] { return setFormat(m, static_cast<StashFormat>(f)); };
    formats.push_back(std::move(piece));
  }
  constexpr double kSearchW = 360;
  const double searchX = right - kSearchW;
  m.toolsWell(searchX - kColumnGap - m.toolsWellWidth(formats), y, formats);
  m.toolsSearch(searchX, y, right, "", t.searchStash, false, [&m] {
    StashList list;
    list.format = shownFormat(m);
    openScenes(m, list);
    StashMenu& s = state(m);
    s.typing = StashMenu::Typing::Search;
    m.editing_ = true;
    return MenuCommand{};
  }, nullptr);
}

// The heading row (its title, Search Stash and All / VR / 2D), then the
// shelves, scrolling under it.
void StashPages::layoutHome(Menu& m) {
  StashMenu& st = state(m);
  StashHomePage& p = pageOf(st);
  const StashStrings& t = strings().stash;
  st.takeChanges(sources(m));  // a new catalog, the library's history
  const StashCatalog* c = catalog(m);
  if (c)
    p.rebuild(st, *c, m.stash_->generation(), shownFormat(m), m.view_.stash.malePerformers);
  const double x0 = kContentLeft, right = kContentRight;

  layoutHomeHeader(m, p, c);

  m.list_.left = x0;
  m.list_.right = right + kListRightPad;
  m.listShowsFolder_ = false;
  m.listColumns_ = false;
  m.list_.top = kBrowserToolsY;
  m.list_.bottom = kListBottom;
  double notHereY = kBrowserToolsY + 20;
  if (!c || layoutNotHere(m, x0 + 24, notHereY, right - x0 - 48)) {
    m.list_.count = 0;
    return;
  }

  // Where each part goes, from the top of the shelves down.
  const double w = m.list_.right - kScrollbarRoom - x0;
  const double tileW = (w - (kSceneColumns - 1) * kTileGap) / kSceneColumns;
  const double tileH = cardHeight(tileW);
  const double cardW = (w - (kPerformerColumns - 1) * kTileGap) / kPerformerColumns;
  const double cardH = performerCardHeight(cardW);  // as the Performers page's
  // The pins' rows, as they wrap.
  struct Placed {
    double x, y, w;
  };
  std::vector<Placed> pins;
  double pinsH = 0;
  {
    double px = x0, py = 0;
    auto place = [&](double bw) {
      if (px > x0 && px + bw > x0 + w) {
        px = x0;
        py += kPinH + kit::kSpaceTight;
      }
      pins.push_back({px, py, bw});
      px += bw + kit::kSpaceTight;
    };
    for (size_t i = 0; i < st.pins.size(); ++i) {
      const std::string count = i < p.pinCounts.size() ? stashNumber(p.pinCounts[i]) : "";
      place(std::min(480.0, m.textWidth(st.pins[i].name, kit::kRowSize, false) +
                                m.textWidth(count, kCountSize, false) + 3 * kit::kSpace));
    }
    if (st.pins.empty())
      place(std::min(w - 400, m.textWidth(t.noPins, kHintSize, false) + kit::kSpace));
    place(actionButton(m, t.pinFromScenes, StashIcon::Pin, nullptr, 0, 0, kPinH).w);
    pinsH = py + kPinH;
  }
  const bool shelf[kShelves] = {!p.shelves[kNew].all.empty(), !p.shelves[kTopRated].all.empty(),
                                !p.performers.empty(), !p.shelves[kRandom].all.empty()};
  double shelfY[kShelves];
  double height = kHeadingH + kit::kSpaceTight + pinsH + kShelfGap;
  for (const int s : {kNew, kTopRated, kTopPerformers, kRandom}) {
    shelfY[s] = height;
    if (shelf[s]) height += kHeadingH + kit::kSpaceTight + (s == kTopPerformers ? cardH : tileH) + kShelfGap;
  }

  // The list: steps of kStep pixels, enough of them to reach the end.
  const double view = m.list_.bottom - m.list_.top;
  m.list_.rowHeight = kStep;
  m.list_.rows = std::max(1, static_cast<int>(view / kStep));
  m.list_.count = m.list_.rows + static_cast<int>(std::ceil(std::max(0.0, height - view) / kStep));
  int first, end;
  m.clampScroll(m.list_.count, m.list_.rows, first, end);
  const double top = m.list_.top - (m.list_.offset + m.list_.fraction) * kStep;
  auto shows = [&](double y0, double y1) {
    return top + y1 > m.list_.top && top + y0 < m.list_.bottom;
  };
  auto add = [&](Button b) {
    b.scrolls = true;
    m.buttons_.push_back(std::move(b));
  };
  auto heading = [&](double at, const std::string& title, const std::string& note,
                     const std::string& more, std::function<void()> open) {
    if (!shows(at, at + kHeadingH)) return;
    Button b;
    b.panel = Menu::kBrowser;
    b.x = x0;
    b.y = top + at;
    b.w = w;
    b.h = kHeadingH;
    b.label = title;
    b.detail = note;
    b.size = more;
    b.bare = true;
    b.radius = kit::kCorner;
    b.stashLook = static_cast<int>(StashLook::Shelf);
    b.action = [open] {
      open();
      return MenuCommand{};
    };
    add(b);
  };
  auto seeAll = [&](size_t n) {
    return stashPrinted(t.seeAll, stashNumber(static_cast<long>(n))) + " ›";
  };

  // Pinned lists: one tap each.
  // (Without any, its heading leads to the Scenes page, where they are made.)
  heading(0, t.pins, "", st.pins.empty() ? "" : seeAll(st.pins.size()), [&m] {
    if (state(m).pins.empty())
      showOwnScenes(m);
    else
      showFull(m, StashFull::Pins);
  });
  const double pinsTop = kHeadingH + kit::kSpaceTight;
  for (size_t i = 0; i < pins.size(); ++i) {
    const Placed& at = pins[i];
    if (!shows(pinsTop + at.y, pinsTop + at.y + kPinH)) continue;
    if (i < st.pins.size()) {
      const std::string count = i < p.pinCounts.size() ? stashNumber(p.pinCounts[i]) : "";
      // A raised button: the list's name, how many it has dim at its end.
      Button b = facetButton(m, st.pins[i].name, count, 0, [&m, i] { openPin(m, static_cast<int>(i)); },
                             at.x, top + pinsTop + at.y, at.w, kPinH);
      b.bare = false;
      add(b);
    } else if (st.pins.empty() && i == 0) {
      Button& note = m.addLabel(at.x + kit::kSpace, top + pinsTop + at.y, at.w - kit::kSpace, kPinH,
                                t.noPins, kHintSize, true);
      note.scrolls = true;
    } else {
      Button b = actionButton(
          m, t.pinFromScenes, StashIcon::Pin,
          [&m] {
            showOwnScenes(m);
            return MenuCommand{};
          },
          at.x, top + pinsTop + at.y, kPinH);
      b.bare = true;
      b.on = true;
      add(b);
    }
  }

  // The scene shelves: five tiles each, as the Scenes page's.
  std::vector<std::string> wanted;
  const bool history = st.history.size() == c->scenes().size();
  auto scenes = [&](int s, const std::string& note) {
    const StashHomePage::SceneShelf& sh = p.shelves[s];
    const StashList list = sh.list;
    const uint64_t seed = p.seed;
    heading(shelfY[s], t.shelves[s], note, seeAll(sh.all.size()), [&m, list, seed] {
      openScenes(m, list);
      if (list.sort == StashSort::Random) {  // the same picks first
        state(m).seed = seed;
        state(m).stale = true;
      }
    });
    const double ty = shelfY[s] + kHeadingH + kit::kSpaceTight;
    for (int i = 0; i < kSceneColumns && i < static_cast<int>(sh.all.size()); ++i) {
      const int index = sh.all[i];
      const StashScene& scene = c->scenes()[index];
      const std::string location = stashSceneLocation(scene, m.offline_);
      wanted.push_back(location);
      if (!shows(ty, ty + tileH)) continue;
      Button b = sceneCard(m, *c, scene, history ? st.history[index].plays : 0,
                           history ? st.history[index].rating : 0, x0 + i * (tileW + kTileGap),
                           top + ty, tileW);
      b.scrolls = true;
      b.action = [&m, s, path = location] {
        StashMenu& now = state(m);
        StashHomePage& home = pageOf(now);
        if (const StashCatalog* latest = catalog(m))
          home.rebuild(now, *latest, m.stash_->generation(), shownFormat(m),
                       m.view_.stash.malePerformers);
        return play(m, home.shelves[s].all, path, false);
      };
      addSceneCard(m, b, scene);
    }
  };
  if (shelf[kNew])
    scenes(kNew, p.shelves[kNew].recent
                     ? stashPrinted(t.shelfNotes[kNew], stashNumber(p.shelves[kNew].recent))
                     : "");
  if (shelf[kTopRated]) scenes(kTopRated, t.shelfNotes[kTopRated]);

  // Your top performers: their portraits; a tap opens her page.
  std::vector<StashImages::Request> portraits;
  if (shelf[kTopPerformers]) {
    heading(shelfY[kTopPerformers], t.shelves[kTopPerformers], t.shelfNotes[kTopPerformers],
            t.more[0], [&m] { showPerformers(m); });
    const double ty = shelfY[kTopPerformers] + kHeadingH + kit::kSpaceTight;
    for (size_t i = 0; i < p.performers.size(); ++i) {
      const StashHomePage::Performer& entry = p.performers[i];
      const StashPerformer& performer = c->performers()[entry.index];
      if (!performer.image.empty())
        portraits.push_back(Stash::portraitRequest(performer, stashPixels(cardW - 2 * kCardIn)));
      if (!shows(ty, ty + cardH)) continue;
      Button b;
      b.panel = Menu::kBrowser;
      b.x = x0 + i * (cardW + kTileGap);
      b.y = top + ty;
      b.w = cardW;
      b.h = cardH;
      b.label = performer.name;
      b.detail = stashJoined({stashPlays(entry.plays),
                              entry.rated ? stashPrinted(t.stars, entry.stars / entry.rated) : ""});
      if (!performer.country.empty()) b.date = performer.country;
      if (entry.played > 0) b.plays = std::to_string(entry.played);
      b.card = true;
      b.radius = kCardCorner;
      b.stashLook = static_cast<int>(StashLook::PerformerCard);
      b.stashId = performer.id;
      b.action = [&m, id = performer.id] {
        showPerformer(m, id);
        return MenuCommand{};
      };
      if (m.textWidth(b.label, kit::kRowSize, false) > b.w - 2 * kit::kSpace) b.tooltip = b.label;
      add(b);
    }
  }
  if (shelf[kRandom]) scenes(kRandom, t.shelfNotes[kRandom]);

  // Every shelf's covers, logos and portraits (a few dozen at most).
  m.stashCovers_->wantFirst(std::move(portraits));
  m.wantStashImages(wanted, wanted.size(), tileW - 2 * kCardIn);
  if (m.thumbnailer_) {
    std::vector<std::string> frames;
    if (m.view_.thumbnails)
      for (const std::string& path : wanted)
        if (!m.stashCovers_->coverComing(path)) frames.push_back(path);
    m.thumbnailer_->want(frames, stashPixels(tileW - 2 * kCardIn));
  }
  m.addScrollbar();
}

void StashPages::drawHomeHeading(const Menu& m, cairo_t* cr) {
  const StashStrings& t = strings().stash;
  const Palette& pal = m.palette();
  const double x = kContentLeft + kit::kSpace;  // as a page's title (Menu::contentTitle())
  m.drawText(cr, t.home, x, kBrowserRowY, m.textWidth(t.home, kit::kTitleSize, true) + 4, kBrowserRowH,
             kit::kTitleSize, true, false, pal.text.r, pal.text.g, pal.text.b);
  if (!catalog(m)) drawWaiting(m, cr, x, kBrowserToolsY + 20, kContentRight - x - kit::kSpace);
}

// A shelf's title (semibold), a dim note after it, and "See all 2,007 ›" in
// the accent at the right.
void StashPages::drawShelf(const Menu& m, cairo_t* cr, const Button& b, const double ink[4]) {
  const Palette& pal = m.palette();
  constexpr int size = kHintSize;
  const double x = b.x + kit::kSpace, right = b.x + b.w - kit::kSpace;
  const double moreW = b.size.empty() ? 0 : m.textWidth(b.size, size, false) + 4;
  const double room = right - x - moreW - kit::kSpaceEdge;
  const double titleW = std::min(room, m.textWidth(b.label, kit::kRowSize, true) + 4);
  m.drawText(cr, b.label, x, b.y, titleW, b.h, kit::kRowSize, true, false, ink[0], ink[1], ink[2]);
  if (!b.detail.empty() && room - titleW > 80)
    m.drawText(cr, b.detail, x + titleW + kit::kSpace, b.y, room - titleW - kit::kSpace, b.h, size,
               false, false, pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a);
  if (!b.size.empty())
    m.drawText(cr, b.size, right - moreW, b.y, moreW, b.h, size, false, false, pal.textAccent.r,
               pal.textAccent.g, pal.textAccent.b);
}
