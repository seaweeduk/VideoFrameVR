#include "menu_style.h"

#include "menu_list_parts.h"
#include "menu_stash.h"
#include "menu_stash_covers.h"
#include "stash.h"
#include "stash_text.h"

#include <algorithm>
#include <array>

// The Studios page (docs/stash.md): logo cards of the studios with scenes
// here, each on its own, sorted by the app's plays; and a studio's page:
// its logo and your history with it over its own scenes, listed with the
// Scenes page's rail, chips and results. The cards' numbers are counted
// once per catalog, library change, format or order, in one walk over the
// scenes; each layout adds buttons only for the cards shown.

namespace {

constexpr int kColumns = 5;
constexpr double kCardGap = kGalleryGap;
// A logo is fitted into this part of its box (a card's picture), as
// Stash's cards show them.
constexpr double kLogoFitW = 0.82, kLogoFitH = 0.7;
// A studio's page: its band of logo and facts under the heading row.
constexpr double kStudioLogoW = 260, kStudioBandH = 132;

}  // namespace

struct StashStudiosPage {
  // A studio's scenes in the chosen format and your history with them.
  struct Stats {
    int scenes = 0, plays = 0, rated = 0;
    double ratings = 0;
    int32_t newest = kStashNoDay;
    std::string newestDate;
    double average() const { return rated ? ratings / rated : 0; }
  };
  StudioSort sort = StudioSort::Plays;
  bool descending = true;
  double scroll = 0;  // the grid's position (rows), while away

  // Remade when the catalog, the library, the format, the order or the
  // search changes.
  uint64_t catalog = ~0ull, library = ~0ull, away = ~0ull;
  StashFormat format = StashFormat::VR;
  std::string search;
  bool stale = true;
  std::vector<Stats> stats;             // per catalog studio
  std::vector<std::array<int, 3>> any;  // scenes per StashFormat
  std::array<int, 3> counts{};          // cards per StashFormat
  std::vector<int> cards;               // the studios listed, in order

  std::optional<StashSavedList> scenes, studio;
};

namespace {

StashStudiosPage& pageOf(StashMenu& st) {
  if (!st.studios) {
    st.studios = std::make_shared<StashStudiosPage>();
    st.studios->sort = st.settings.studiosSort;
    st.studios->descending = st.settings.studiosDescending;
  }
  return *st.studios;
}

// The cards' numbers and order, when what they are made from changed.
// `generation`: the catalog's (Stash::generation()).
void rebuild(StashMenu& st, StashStudiosPage& p, const StashCatalog& c, uint64_t generation,
             StashFormat format, const std::string& search) {
  if (!p.stale && p.catalog == generation && p.library == st.libraryGeneration &&
      p.away == st.awayGeneration && p.format == format && p.search == search)
    return;
  p.stale = false;
  p.catalog = generation;
  p.library = st.libraryGeneration;
  p.away = st.awayGeneration;
  p.format = format;
  p.search = search;
  const size_t n = c.studios().size();
  p.stats.assign(n, {});
  p.any.assign(n, {});
  const bool history = st.history.size() == c.scenes().size();
  for (size_t i = 0; i < c.scenes().size(); ++i) {
    const StashScene& s = c.scenes()[i];
    if (s.studio < 0 || (history && st.history[i].hidden)) continue;
    bool vr = s.vr;
    if (history && st.history[i].vr >= 0) vr = st.history[i].vr == 1;
    const int kind = vr ? static_cast<int>(StashFormat::VR) : static_cast<int>(StashFormat::Flat);
    ++p.any[s.studio][0];
    ++p.any[s.studio][kind];
    if (format != StashFormat::All && static_cast<int>(format) != kind) continue;
    StashStudiosPage::Stats& t = p.stats[s.studio];
    ++t.scenes;
    if (history) {
      t.plays += st.history[i].plays;
      if (st.history[i].rating > 0) {
        ++t.rated;
        t.ratings += st.history[i].rating;
      }
    }
    if (s.day != kStashNoDay && (t.newest == kStashNoDay || s.day > t.newest)) {
      t.newest = s.day;
      t.newestDate = s.date;
    }
  }
  p.counts = {};
  for (size_t k = 0; k < n; ++k)
    for (int f = 0; f < 3; ++f) p.counts[f] += p.any[k][f] > 0;

  // The cards: those with scenes in the format (and matching the search),
  // in the chosen order.
  const std::string words = stashFold(search);
  p.cards.clear();
  for (size_t k = 0; k < n; ++k)
    if (p.any[k][static_cast<int>(format)] > 0 &&
        (words.empty() || stashFold(c.studios()[k].name).find(words) != std::string::npos))
      p.cards.push_back(static_cast<int>(k));
  std::vector<std::string> keys;
  if (p.sort == StudioSort::Name) {
    keys.resize(n);
    for (const int k : p.cards) keys[k] = stashCollateKey(c.studios()[k].name);
  }
  auto value = [&](int k) -> double {
    const StashStudiosPage::Stats& t = p.stats[k];
    switch (p.sort) {
      case StudioSort::Plays:
        return t.plays;
      case StudioSort::Scenes:
        return t.scenes;
      case StudioSort::Newest:
        return t.newest;
      case StudioSort::Rating:
        return t.average();
      case StudioSort::Name:
        break;
    }
    return 0;
  };
  // Ties (and studios without the value, last either way) by scenes, then name.
  std::stable_sort(p.cards.begin(), p.cards.end(), [&](int a, int b) {
    if (p.sort == StudioSort::Name) {
      if (keys[a] != keys[b]) return p.descending ? keys[b] < keys[a] : keys[a] < keys[b];
    } else {
      const bool knownA = p.sort == StudioSort::Newest ? p.stats[a].newest != kStashNoDay
                          : p.sort == StudioSort::Rating ? p.stats[a].rated > 0
                                                         : true;
      const bool knownB = p.sort == StudioSort::Newest ? p.stats[b].newest != kStashNoDay
                          : p.sort == StudioSort::Rating ? p.stats[b].rated > 0
                                                         : true;
      if (knownA != knownB) return knownA;
      const double va = value(a), vb = value(b);
      if (va != vb) return p.descending ? va > vb : va < vb;
    }
    if (p.stats[a].scenes != p.stats[b].scenes) return p.stats[a].scenes > p.stats[b].scenes;
    return c.studios()[a].name < c.studios()[b].name;
  });
}

}  // namespace

StashFormat StashPages::shownFormat(const Menu& m) { return m.stashMenu_->list.format; }

MenuCommand StashPages::setFormat(Menu& m, StashFormat format) {
  StashMenu& st = state(m);
  st.list.format = format;
  st.stale = true;
  m.list_.offset = 0;
  m.list_.fraction = 0;
  MenuCommand c;
  c.type = MenuCommand::SetStashFormat;
  c.value = static_cast<int>(format);
  return c;
}

int64_t StashPages::fixedStudio(const Menu& m) {
  return m.page_ == Menu::Page::StashStudio && m.stashMenu_ ? m.stashMenu_->studio : 0;
}

double StashPages::bandHeight(const Menu& m) {
  return m.page_ == Menu::Page::StashStudio ? kStudioBandH + kit::kSpace : 0;
}

StashList StashPages::scopedList(const Menu& m) {
  StashList list = m.stashMenu_->list;
  const int64_t fixed = fixedStudio(m);
  if (fixed) {
    list.studios = {fixed};
    list.studiosOut.clear();
  }
  return list;
}

void StashPages::staleStudios(Menu& m) {
  if (state(m).studios) state(m).studios->stale = true;
}

void StashPages::showStudios(Menu& m) {
  const double scroll = pageOf(state(m)).scroll;
  m.showPage(Menu::Page::StashStudios);
  m.list_.offset = static_cast<int>(std::floor(scroll));
  m.list_.fraction = scroll - m.list_.offset;
}

void StashPages::leaveStudios(Menu& m) {
  pageOf(state(m)).scroll = m.list_.offset + m.list_.fraction;
}

void StashPages::showStudio(Menu& m, int64_t id) {
  StashMenu& st = state(m);
  if (m.page_ == Menu::Page::StashStudio && st.studio == id) return;
  m.showPage(Menu::Page::StashStudio);  // leaves the page before
  st.studio = id;
  enterStudio(m);
}

// Its scenes in the Scenes page's list: as they were left, if it was the
// last studio shown; else all of them, newest first, in the format chosen.
void StashPages::enterStudio(Menu& m) {
  StashMenu& st = state(m);
  StashStudiosPage& p = pageOf(st);
  if (!p.scenes) enterList(m, p.scenes, p.studio, st.studio);
}

// Its list kept for coming back (after playing one of its scenes, say),
// the Scenes page's put back.
void StashPages::leaveStudio(Menu& m) {
  StashMenu& st = state(m);
  StashStudiosPage& p = pageOf(st);
  leaveList(m, p.scenes, p.studio, st.studio);
}

// On the texture's pixels, with its soft shadow; while it comes, nothing
// (a studio's page draws its band again once it is there).
void StashPages::drawLogo(const Menu& m, cairo_t* cr, const StashStudio& studio, double x,
                          double y, double w, double h, double alpha) {
  constexpr float scale = menu_geometry::kRasterScale;
  int pad = 0;
  bool none = false;
  cairo_surface_t* logo =
      m.stashCovers_->studioLogo(studio, stashPixels(w), stashPixels(h), pad, none, y - 8, y + h + 8);
  if (logo) {
    const double lw = (cairo_image_surface_get_width(logo) - 2 * pad) / scale;
    const double lh = (cairo_image_surface_get_height(logo) - 2 * pad) / scale;
    cairo_save(cr);
    cairo_translate(cr, onPixel(x + (w - lw) / 2 - pad / scale),
                    onPixel(y + (h - lh) / 2 - pad / scale));
    cairo_scale(cr, 1 / scale, 1 / scale);
    cairo_set_source_surface(cr, logo, 0, 0);
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
    return;
  }
  if (!none) {
    if (m.page_ == Menu::Page::StashStudio) m.stashMenu_->bandPending = true;
    return;
  }
  const Palette& pal = m.palette();
  const double rgba[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a};
  m.drawTextLines(cr, studio.name, x, y + h / 2 - 34, w, 28, 2, rgba, true, y + h);
}

// As the lists' cards (Menu::addGallery()'s): its logo on the picture's
// sunk plate, its name and two lines under it.
void StashPages::drawStudioCard(const Menu& m, cairo_t* cr, const Button& b, const double ink[4]) {
  const StashCatalog* c = catalog(m);
  const int index = c ? c->studioById(b.stashId) : -1;
  double px, py, pw, ph;
  m.listPicture(b, px, py, pw, ph);
  m.drawFinish(cr, Finish::Sunk, px, py, pw, ph, kit::kPieceCorner);
  if (index >= 0)
    drawLogo(m, cr, c->studios()[index], px + pw * (1 - kLogoFitW) / 2, py + ph * (1 - kLogoFitH) / 2,
             pw * kLogoFitW, ph * kLogoFitH, 1);
  const Palette& pal = m.palette();
  const double dim[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a};
  const double x = b.x + kit::kSpace, w = b.w - 2 * kit::kSpace, y = py + ph + kCardNameGap;
  m.drawLinesIn(cr, b.label, x, y, w, kit::kRowSize, 1, kCardLine, ink);
  m.drawLinesIn(cr, b.detail, x, y + kCardLine, w, kHintSize, 1, kCardLine, dim);
  m.drawLinesIn(cr, b.date, x, y + 2 * kCardLine, w, kHintSize, 1, kCardLine, dim);
}

// The Studios page: the heading row (its title and count), the tool row
// (All / VR / 2D, the order, the search, to the top / end), then the cards.
void StashPages::layoutStudios(Menu& m) {
  StashMenu& st = state(m);
  StashStudiosPage& p = pageOf(st);
  const StashStrings& t = strings().stash;
  st.takeChanges(sources(m));  // a new catalog, the library's history
  const StashCatalog* c = catalog(m);
  if (c) rebuild(st, p, *c, m.stash_->generation(), shownFormat(m), st.studioSearch);

  const double x0 = kContentLeft, right = kContentRight;
  const StashFormat shown = shownFormat(m);
  const bool editing = m.editing_ && st.typing == StashMenu::Typing::Studios;

  // The cards, five a row.
  const double bottomY = editing ? kKeyboardY - 16 : kListBottom;
  m.list_.left = x0;
  m.list_.right = right + kListRightPad;
  m.listShowsFolder_ = false;
  m.listColumns_ = false;
  m.list_.top = kGridTop;
  m.list_.bottom = bottomY;
  if (c) {
    const double cardW = m.gridCellWidth(x0, kColumns, kCardGap);
    const double rowH = cardHeight(cardW) + kCardGap;
    const int count = static_cast<int>(p.cards.size());
    int first, last;
    m.gridRows(count, kColumns, rowH, first, last);
    for (int i = first; i < last; ++i) {
      const int k = p.cards[i], cell = i - first;
      const StashStudio& studio = c->studios()[k];
      const StashStudiosPage::Stats& s = p.stats[k];
      Button b;
      b.panel = Menu::kBrowser;
      b.x = x0 + (cell % kColumns) * (cardW + kCardGap);
      b.y = kGridTop + (cell / kColumns - m.list_.fraction) * rowH;
      b.w = cardW;
      b.h = rowH - kCardGap;
      b.card = true;
      b.radius = kCardCorner;
      b.label = studio.name;
      b.detail = stashJoined(
          {stashScenes(s.scenes), stashPlays(s.plays)});
      b.date = stashJoined({s.newestDate.empty() ? "" : stashPrinted(t.newest, s.newestDate),
                            s.rated ? stashPrinted(t.stars, s.average()) : ""});
      b.scrolls = true;
      b.stashLook = static_cast<int>(StashLook::Studio);
      b.stashId = studio.id;
      b.action = [&m, id = studio.id] {
        showStudio(m, id);
        return MenuCommand{};
      };
      if (m.textWidth(b.label, kit::kRowSize, false) > b.w - 2 * kit::kSpace) b.tooltip = b.label;
      m.buttons_.push_back(b);
    }
    // Their logos, then those of the rows around them.
    const int logoW = stashPixels((cardW - 2 * kCardIn) * kLogoFitW);
    std::vector<StashImages::Request> requests;
    const int page = m.list_.rows * kColumns;
    auto add = [&](int from, int to) {
      for (int i = std::max(0, from); i < std::min(count, to); ++i)
        if (!c->studios()[p.cards[i]].image.empty())
          requests.push_back(Stash::logoRequest(c->studios()[p.cards[i]], logoW));
    };
    add(first, last);
    add(last, last + 2 * page);
    add(first - page, first);
    m.stash_->images().want(requests);
    m.addScrollbar();
  }
  if (editing) m.layoutKeyboard();

  // The tools row last, its sort fields opening over the cards: All / VR /
  // 2D with the studios each has, the order, the search, to the top / end.
  std::vector<Menu::ToolPiece> formats;
  for (int f = 0; f < 3; ++f) {
    Menu::ToolPiece piece;
    piece.label = t.formats[f];
    if (c) piece.count = stashNumber(p.counts[f]);
    piece.chosen = static_cast<int>(shown) == f;
    piece.disabled = c && p.counts[f] == 0 && !piece.chosen;
    piece.action = [&m, f] { return setFormat(m, static_cast<StashFormat>(f)); };
    formats.push_back(std::move(piece));
  }
  double x = m.toolsWell(x0, kBrowserToolsY, formats) + kColumnGap;
  x = layoutSort(
      m, x, t.studioSorts, 5, static_cast<int>(p.sort), p.descending,
      [&m](int f) {
        StashStudiosPage& page = pageOf(state(m));
        const StudioSort sort = static_cast<StudioSort>(f);
        // A–Z for names, else the most (newest, best …) first.
        if (page.sort != sort) page.descending = sort != StudioSort::Name;
        page.sort = sort;
        page.stale = true;
        return MenuCommand{MenuCommand::SetStashStudiosSort, double(page.descending),
                           stashSortName(page.sort)};
      },
      [&m] {
        StashStudiosPage& page = pageOf(state(m));
        page.descending = !page.descending;
        page.stale = true;
        return MenuCommand{MenuCommand::SetStashStudiosSort, double(page.descending),
                           stashSortName(page.sort)};
      });
  layoutSearch(m, x, layoutJumps(m), st.studioSearch, t.find[2], StashMenu::Typing::Studios);
}

// The title and count as a page's (Menu::contentTitle()), and what an
// empty page means.
void StashPages::drawStudiosHeading(const Menu& m, cairo_t* cr) {
  const StashMenu& st = *m.stashMenu_;
  const StashStrings& t = strings().stash;
  const Palette& pal = m.palette();
  const double x = kContentLeft + kit::kSpace;
  const double titleW = m.textWidth(t.studios, kit::kTitleSize, true) + 4;
  m.drawText(cr, t.studios, x, kBrowserRowY, titleW, kBrowserRowH, kit::kTitleSize, true, false,
             pal.text.r, pal.text.g, pal.text.b);
  const StashCatalog* c = catalog(m);
  if (!c) {
    drawWaiting(m, cr, x, kGridTop + 20, kContentRight - x - kit::kSpace);
  } else if (st.studios) {
    const StashStudiosPage& p = *st.studios;
    const long n = static_cast<long>(p.cards.size());
    m.drawText(cr, n == 1 ? std::string(t.oneStudio) : stashPrinted(t.studiosCount, stashNumber(n)),
               x + titleW + kit::kSpaceEdge,
               kBrowserRowY, 600, kBrowserRowH, 23, false, false, pal.textDim.r, pal.textDim.g,
               pal.textDim.b, pal.textDim.a);
    if (p.cards.empty())
      m.drawEmptyPage(cr, st.studioSearch.empty() ? Menu::Icon::Building : Menu::Icon::Search,
                      st.studioSearch.empty() ? std::string(t.noStudios)
                                              : stashPrinted(t.noMatches, st.studioSearch));
  }
}

// A studio's page: Back to the studios and the Scenes heading row; its band
// (logo, your history with it, the studio it is under); then its scenes as
// the Scenes page shows them.
void StashPages::layoutStudio(Menu& m) {
  StashMenu& st = state(m);
  const StashStrings& t = strings().stash;
  enterStudio(m);
  st.refresh(sources(m), listPage(m));
  const StashCatalog* c = catalog(m);
  StashStudiosPage& p = pageOf(st);
  if (c) rebuild(st, p, *c, m.stash_->generation(), st.list.format, st.studioSearch);
  const double x0 = kContentLeft, right = kContentRight;
  Button back = actionButton(
      m, t.studios, StashIcon::Back,
      [&m] {
        showStudios(m);
        return MenuCommand{};
      },
      x0, kBrowserRowY, kBrowserRowH);
  m.buttons_.push_back(back);

  const int index = c ? c->studioById(st.studio) : -1;
  std::vector<StashImages::Request> logo;
  if (index >= 0) {
    const StashStudio& studio = c->studios()[index];
    const StashStudiosPage::Stats& s = p.stats[index];
    if (!studio.image.empty())
      logo.push_back(Stash::logoRequest(studio, stashPixels(kStudioLogoW * kLogoFitW)));
    const double y = kBrowserToolsY, tx = x0 + kStudioLogoW + kit::kSpaceEdge;
    // Your history with it, and the studio Stash has it under, as text.
    const std::string facts =
        stashJoined({stashScenes(s.scenes),
                     s.newestDate.empty() ? "" : stashPrinted(t.newestIn, s.newestDate),
                     stashPlays(s.plays), s.rated ? stashPrinted(t.average, s.average()) : ""});
    m.addLabel(tx, y, right - tx, 40, facts, kit::kWellSize, false);
    if (studio.parent >= 0)
      m.addLabel(tx, y + 40, right - tx, 36,
                 stashPrinted(t.partOf, c->studios()[studio.parent].name), kit::kWellSize, true);
  }
  // Its scenes as the Scenes page shows them, under the band.
  m.stashCovers_->wantFirst(std::move(logo));
  layoutResults(m, kGridTop + bandHeight(m), right);
  layoutHeader(m, back.x + back.w + kit::kSpaceTight);
  layoutTools(m);
}

void StashPages::drawStudioHeading(const Menu& m, cairo_t* cr) {
  drawScenesHeading(m, cr);
  const StashCatalog* c = catalog(m);
  const int index = c ? c->studioById(m.stashMenu_->studio) : -1;
  if (index < 0) return;
  const double x = kContentLeft, y = kBrowserToolsY;
  m.drawFinish(cr, Finish::Sunk, x, y, kStudioLogoW, kStudioBandH, kit::kCorner);
  drawLogo(m, cr, c->studios()[index], x + kStudioLogoW * (1 - kLogoFitW) / 2,
           y + kStudioBandH * (1 - kLogoFitH) / 2, kStudioLogoW * kLogoFitW, kStudioBandH * kLogoFitH, 1);
}
