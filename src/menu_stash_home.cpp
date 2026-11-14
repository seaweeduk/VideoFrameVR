#include "menu_style.h"

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

constexpr int kSceneColumns = 5, kPerformerColumns = 8;
constexpr double kTileGap = 16;
constexpr double kStep = 100;     // what the list counts as a row
constexpr double kHeadingH = 52;  // a shelf's heading row
constexpr double kPinH = 64;
constexpr double kShelfGap = 30;
constexpr int kNewDays = 30;

int64_t unixNow() { return static_cast<int64_t>(std::time(nullptr)); }

enum Shelf { kNew, kTopRated, kTopPerformers, kRandom, kShelves };

}  // namespace

struct StashHomePage {
  struct SceneShelf {
    StashList list;          // what See all opens
    std::vector<int> all;    // its scenes, for Previous / Next
    int recent = 0;          // new in Stash: added in the last kNewDays
  };
  struct Performer {
    int index = 0, plays = 0, played = 0, rated = 0;  // played: her scenes played
    double stars = 0;
    double score() const { return plays + stars; }
  };
  // What the shelves were made from.
  uint64_t catalog = ~0ull, seed = 0, visits = 0, away = ~0ull;
  StashFormat format = StashFormat::VR;
  bool male = false, history = false;
  std::vector<StashPin> pins;
  bool stale = true;
  double scroll = 0;  // where it was left

  std::array<SceneShelf, kShelves> shelves;
  std::vector<Performer> performers;
  std::vector<long> pinCounts;
  std::array<int, 3> formats{};  // scenes per StashFormat
};

namespace {

StashHomePage& pageOf(StashMenu& st) {
  if (!st.home) st.home = std::make_shared<StashHomePage>();
  return *st.home;
}

bool samePins(const std::vector<StashPin>& a, const std::vector<StashPin>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].name != b[i].name || a[i].list != b[i].list) return false;
  return true;
}

// `generation`: the catalog's (Stash::generation()); `male`: the male
// performers setting.
void rebuild(StashMenu& st, StashHomePage& p, const StashCatalog& c, uint64_t generation,
             StashFormat format, bool male) {
  const bool history = st.history.size() == c.scenes().size();
  if (!p.stale && p.catalog == generation && p.format == format && p.male == male &&
      p.history == history && p.away == st.awayGeneration && samePins(p.pins, st.pins))
    return;
  p.stale = false;
  p.catalog = generation;
  p.away = st.awayGeneration;
  p.format = format;
  p.male = male;
  p.history = history;
  p.pins = st.pins;
  const int64_t now = unixNow();
  const std::vector<StashHistory>* h = history ? &st.history : nullptr;
  auto run = [&](StashHomePage::SceneShelf& shelf, bool facets) {
    const StashResult r = c.query(stashFilterOf(shelf.list, c, male, now),
                                  stashOrderOf(shelf.list, p.seed), h, facets);
    shelf.all = r.scenes;
    return r;
  };
  for (StashHomePage::SceneShelf& shelf : p.shelves) shelf = {};

  StashHomePage::SceneShelf& fresh = p.shelves[kNew];
  fresh.list.format = format;
  fresh.list.sort = StashSort::Added;
  const StashResult all = run(fresh, true);
  p.formats = all.formats;
  for (const int i : fresh.all) {
    if (c.scenes()[i].createdAt < now - kNewDays * 86400) break;
    ++fresh.recent;
  }

  StashHomePage::SceneShelf& rated = p.shelves[kTopRated];
  rated.list.format = format;
  rated.list.history = stashBit(StashHistoryOption::Rated4);
  rated.list.sort = StashSort::Rating;
  if (history) run(rated, false);

  StashHomePage::SceneShelf& picks = p.shelves[kRandom];
  picks.list.format = format;
  picks.list.sort = StashSort::Random;
  run(picks, false);

  // Your top performers: the plays of their scenes and the stars you gave
  // them, over the scenes in the format.
  p.performers.clear();
  if (history) {
    std::vector<StashHomePage::Performer> by(c.performers().size());
    for (const int i : picks.all) {
      const StashHistory& s = st.history[i];
      if (s.plays <= 0 && s.rating <= 0) continue;
      for (const int k : c.scenes()[i].performers) {
        if (!stashShown(c.performers()[k].gender, male)) continue;
        by[k].plays += s.plays;
        by[k].played += s.plays > 0;
        if (s.rating > 0) {
          ++by[k].rated;
          by[k].stars += s.rating;
        }
      }
    }
    for (size_t k = 0; k < by.size(); ++k) {
      by[k].index = static_cast<int>(k);
      if (by[k].score() > 0) p.performers.push_back(by[k]);
    }
    std::stable_sort(p.performers.begin(), p.performers.end(),
                     [](const StashHomePage::Performer& a, const StashHomePage::Performer& b) {
                       return a.score() > b.score();
                     });
    if (p.performers.size() > static_cast<size_t>(kPerformerColumns))
      p.performers.resize(kPerformerColumns);
  }

  p.pinCounts.clear();
  for (const StashPin& pin : st.pins)
    p.pinCounts.push_back(static_cast<long>(
        c.query(stashFilterOf(pin.list, c, male, now), {}, h, false).scenes.size()));
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
  m.scrollOffset_ = static_cast<int>(std::floor(p.scroll));
  m.scrollFraction_ = p.scroll - m.scrollOffset_;
}

void StashPages::leaveHome(Menu& m) {
  pageOf(state(m)).scroll = m.scrollOffset_ + m.scrollFraction_;
}

// The heading row (its title, Search Stash and All / VR / 2D), then the
// shelves, scrolling under it.
void StashPages::layoutHome(Menu& m) {
  StashMenu& st = state(m);
  StashHomePage& p = pageOf(st);
  const StashStrings& t = strings().stash;
  takeChanges(m);  // a new catalog, the library's history
  const StashCatalog* c = catalog(m);
  if (c)
    rebuild(st, p, *c, m.stash_->generation(), shownFormat(m), m.view_.stash.malePerformers);
  const double x0 = Menu::kPageX + kPagePad, right = Menu::kPageX + Menu::kPageWidth - kPagePad;

  // Search Stash: the Scenes page, all of the format, typing into its search.
  const double y = kBrowserRowY, h = kBrowserRowH;
  Button search = m.textButton(t.searchStash, Menu::Icon::Search, [&m] {
    StashList list;
    list.format = shownFormat(m);
    openScenes(m, list);
    StashMenu& s = state(m);
    s.typing = StashMenu::Typing::Search;
    m.editing_ = true;
    return MenuCommand{};
  });
  search.panel = Menu::kBrowser;
  search.w = 300;
  search.x = right - search.w;
  search.y = y;
  search.h = h;
  search.fontSize = Menu::kTextSize;
  search.field = true;
  search.placeholder = true;
  search.radius = 12;
  m.buttons_.push_back(search);
  double x = search.x - 24;
  const StashFormat shown = shownFormat(m);
  for (int f = 2; f >= 0; --f) {
    std::string label = t.formats[f];
    if (c) label += "  " + stashNumber(p.formats[f]);
    Button b = m.textButton(
        label, Menu::Icon::None, [&m, f] { return setFormat(m, static_cast<StashFormat>(f)); },
        static_cast<int>(shown) == f);
    b.disabled = c && p.formats[f] == 0 && static_cast<int>(shown) != f;
    b.panel = Menu::kBrowser;
    b.w = m.textWidth(label, Menu::kTextSize, false) + 36;
    b.x = x - b.w;
    b.y = y;
    b.h = h;
    b.radius = h / 2;
    m.buttons_.push_back(b);
    x = b.x - 6;
  }

  m.listLeft_ = x0;
  m.listRight_ = right + 6;
  m.listShowsFolder_ = false;
  m.listColumns_ = false;
  m.listTop_ = kBrowserToolsY;
  m.listBottom_ = kListBottom;
  double notHereY = kBrowserToolsY + 20;
  if (!c || layoutNotHere(m, x0 + 24, notHereY, right - x0 - 48)) {
    m.listCount_ = 0;
    return;
  }

  // Where each part goes, from the top of the shelves down.
  const double w = m.listRight_ - 30 - x0;
  const double tileW = (w - (kSceneColumns - 1) * kTileGap) / kSceneColumns;
  const double tileH = std::round(8 + (tileW - 16) * 9 / 16 + 6 + 34 + 30 + 30 + 10);
  const double cardW = (w - (kPerformerColumns - 1) * kTileGap) / kPerformerColumns;
  // As the Performers page's cards: a 2:3 portrait inset 8, three lines under it.
  const double cardH = std::round(8 + (cardW - 16) * 1.5 + 6 + 34 + 28 + 28 + 10);
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
        py += kPinH + 12;
      }
      pins.push_back({px, py, bw});
      px += bw + 12;
    };
    for (size_t i = 0; i < st.pins.size(); ++i) {
      const std::string count = i < p.pinCounts.size() ? stashNumber(p.pinCounts[i]) : "";
      place(std::min(480.0, m.textWidth(st.pins[i].name, Menu::kTextSize, false) +
                                m.textWidth(count, 22, false) + 70));
    }
    if (st.pins.empty()) place(std::min(w - 400, m.textWidth(t.noPins, 24, false) + 24));
    place(m.textWidth(t.pinFromScenes, Menu::kTextSize, false) + 92);
    pinsH = py + kPinH;
  }
  const bool shelf[kShelves] = {!p.shelves[kNew].all.empty(), !p.shelves[kTopRated].all.empty(),
                                !p.performers.empty(), !p.shelves[kRandom].all.empty()};
  double shelfY[kShelves];
  double height = kHeadingH + 6 + pinsH + kShelfGap;
  for (const int s : {kNew, kTopRated, kTopPerformers, kRandom}) {
    shelfY[s] = height;
    if (shelf[s]) height += kHeadingH + 6 + (s == kTopPerformers ? cardH : tileH) + kShelfGap;
  }

  // The list: steps of kStep pixels, enough of them to reach the end.
  const double view = m.listBottom_ - m.listTop_;
  m.rowHeight_ = kStep;
  m.visibleRows_ = std::max(1, static_cast<int>(view / kStep));
  m.listCount_ = m.visibleRows_ + static_cast<int>(std::ceil(std::max(0.0, height - view) / kStep));
  int first, end;
  m.clampScroll(m.listCount_, m.visibleRows_, first, end);
  const double top = m.listTop_ - (m.scrollOffset_ + m.scrollFraction_) * kStep;
  auto shows = [&](double y0, double y1) {
    return top + y1 > m.listTop_ && top + y0 < m.listBottom_;
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
    b.radius = 14;
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
  const double pinsTop = kHeadingH + 6;
  for (size_t i = 0; i < pins.size(); ++i) {
    const Placed& at = pins[i];
    if (!shows(pinsTop + at.y, pinsTop + at.y + kPinH)) continue;
    if (i < st.pins.size()) {
      const std::string count = i < p.pinCounts.size() ? stashNumber(p.pinCounts[i]) : "";
      Button b = facetButton(m, st.pins[i].name, count, 0, [&m, i] { openPin(m, static_cast<int>(i)); },
                             at.x, top + pinsTop + at.y, at.w, kPinH);
      b.radius = 16;
      add(b);
    } else if (st.pins.empty() && i == 0) {
      Button& note = m.addLabel(at.x + 12, top + pinsTop + at.y, at.w - 12, kPinH, t.noPins, 24, true);
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
    const double ty = shelfY[s] + kHeadingH + 6;
    for (int i = 0; i < kSceneColumns && i < static_cast<int>(sh.all.size()); ++i) {
      const int index = sh.all[i];
      const StashScene& scene = c->scenes()[index];
      const std::string location = stashSceneLocation(scene, m.offline_);
      wanted.push_back(location);
      if (!shows(ty, ty + tileH)) continue;
      Button b = sceneTile(m, *c, scene, history ? st.history[index].plays : 0,
                           x0 + i * (tileW + kTileGap), top + ty, tileW, tileH);
      b.action = [&m, s, path = location] {
        StashMenu& now = state(m);
        StashHomePage& home = pageOf(now);
        if (const StashCatalog* latest = catalog(m))
          rebuild(now, home, *latest, m.stash_->generation(), shownFormat(m),
                  m.view_.stash.malePerformers);
        return play(m, home.shelves[s].all, path, false);
      };
      const Button stars = tileStars(m, b, location, history ? st.history[index].rating : 0);
      add(b);
      m.addOfflineButtons(m.buttons_.back(), 0, tileLastLine(m.buttons_.back()));
      add(stars);
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
    const double ty = shelfY[kTopPerformers] + kHeadingH + 6;
    for (size_t i = 0; i < p.performers.size(); ++i) {
      const StashHomePage::Performer& entry = p.performers[i];
      const StashPerformer& performer = c->performers()[entry.index];
      if (!performer.image.empty())
        portraits.push_back(Stash::portraitRequest(performer, stashPixels(cardW - 16)));
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
      b.stashLook = static_cast<int>(StashLook::PerformerCard);
      b.stashId = performer.id;
      b.action = [&m, id = performer.id] {
        showPerformer(m, id);
        return MenuCommand{};
      };
      if (m.textWidth(b.label, 26, false) > b.w - 28) b.tooltip = b.label;
      add(b);
    }
  }
  if (shelf[kRandom]) scenes(kRandom, t.shelfNotes[kRandom]);

  // Every shelf's covers, logos and portraits (a few dozen at most).
  m.stashCovers_->wantFirst(std::move(portraits));
  m.wantStashImages(wanted, wanted.size(), tileW - 16);
  if (m.thumbnailer_) {
    std::vector<std::string> frames;
    if (m.view_.thumbnails)
      for (const std::string& path : wanted)
        if (!m.stashCovers_->coverComing(path)) frames.push_back(path);
    m.thumbnailer_->want(frames, stashPixels(tileW - 16));
  }
  m.addScrollbar();
}

void StashPages::drawHomeHeading(const Menu& m, cairo_t* cr) {
  const StashStrings& t = strings().stash;
  const Palette& pal = m.palette();
  const double x = Menu::kPageX + kPagePad;
  const double titleW = m.textWidth(t.home, 30, true) + 4;
  m.drawText(cr, t.home, x, kBrowserRowY, titleW, kBrowserRowH, 30, true, false, pal.text.r,
             pal.text.g, pal.text.b);
  if (!catalog(m))
    drawWaiting(m, cr, x + 24, kBrowserToolsY + 20, Menu::kPageWidth - 2 * kPagePad - 48);
}

// Bold title, a dim note after it, and "See all 2,007 ›" in blue at the right.
void StashPages::drawShelf(const Menu& m, cairo_t* cr, const Button& b, const double ink[4]) {
  const Palette& pal = m.palette();
  const double moreW = b.size.empty() ? 0 : m.textWidth(b.size, 24, false) + 8;
  const double room = b.w - 8 - moreW - 24;
  const double titleW = std::min(room, m.textWidth(b.label, 27, true) + 4);
  m.drawText(cr, b.label, b.x + 4, b.y, titleW, b.h, 27, true, false, ink[0], ink[1], ink[2]);
  if (!b.detail.empty() && room - titleW > 80)
    m.drawText(cr, b.detail, b.x + 4 + titleW + 18, b.y, room - titleW - 18, b.h, 22, false, false,
               pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a);
  if (!b.size.empty())
    m.drawText(cr, b.size, b.x + b.w - 12 - moreW, b.y, moreW, b.h, 24, false, false,
               pal.textAccent.r, pal.textAccent.g, pal.textAccent.b);
}
