#include "menu_style.h"

#include "menu_list_parts.h"
#include "menu_stash.h"
#include "menu_stash_covers.h"
#include "stash.h"
#include "stash_text.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>

// The Performers page and a performer's page (docs/stash.md). Performers:
// the shown performers with scenes here as 2:3 portrait cards, filtered by
// their looks and the app's history (the rail, counted in performers),
// sorted, searched by name and alias, with a row of letters. Her page: the
// Scenes page's machinery (rail, chips, tiles or list, full lists) with her
// scenes in its list's place, and her profile in a column on the right:
// portrait, facts, her history in the app, and co-star chips that narrow
// her scenes to those with them too ("A with B").

namespace {

constexpr double kRailW = 400, kRailGap = kit::kSpace;  // as the Scenes page's
constexpr double kTileGap = kGalleryGap;
constexpr int kColumns = 6;
constexpr double kLettersH = 48;
constexpr double kCardsTop = kGridTop + kLettersH + kit::kSpace;  // under the letters
constexpr double kProfileW = 400;
constexpr double kPortraitW = 192, kPortraitH = 288;
constexpr double kFactStep = 32;
constexpr double kCoStarH = 52, kCoStarGap = 10;
constexpr char kLetters[] = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ";

int64_t unixNow() { return static_cast<int64_t>(std::time(nullptr)); }
int32_t today() { return static_cast<int32_t>(unixNow() / 86400); }

// The letter a name is listed under: A–Z, else '#'.
char letterOf(const std::string& name) {
  const unsigned char c = name.empty() ? 0 : static_cast<unsigned char>(name[0]);
  return c < 0x80 && std::isalpha(c) ? static_cast<char>(std::toupper(c)) : '#';
}

// What Stash knows of her, a line each.
std::vector<std::string> factsOf(const StashPerformer& p) {
  const StashStrings& t = strings().stash;
  std::vector<std::string> lines;
  std::vector<std::string> aliases;
  const std::string name = stashFold(p.name);
  for (const std::string& a : p.aliases)
    if (!a.empty() && stashFold(a) != name) aliases.push_back(a);
  if (!p.disambiguation.empty()) lines.push_back("(" + p.disambiguation + ")");
  if (!aliases.empty()) lines.push_back(stashPrinted(t.alsoKnownAs, stashJoined(aliases, ", ")));
  const int age = stashAge(p.born, today());
  lines.push_back(stashJoined({age > 0 ? stashPrinted(t.yearsOld, age) : "",
                               p.birthdate.empty() ? "" : stashPrinted(t.born, p.birthdate)}));
  lines.push_back(stashJoined({p.country, p.ethnicity}));
  lines.push_back(stashJoined({p.hairColor.empty() ? "" : stashPrinted(t.hairChip, p.hairColor),
                               p.eyeColor.empty() ? "" : stashPrinted(t.eyes, p.eyeColor)}));
  lines.push_back(
      stashJoined({p.heightCm > 0 ? stashPrinted(t.centimeters, p.heightCm) : "", p.measurements,
                   p.look(StashLookKind::Breasts) >= 0
                       ? std::string(t.breastKinds[p.look(StashLookKind::Breasts)])
                       : ""}));
  if (!p.careerStart.empty())
    lines.push_back(p.careerEnd.empty() ? stashPrinted(t.activeSince, p.careerStart)
                                        : stashPrinted(t.activeFromTo, p.careerStart, p.careerEnd));
  lines.erase(std::remove(lines.begin(), lines.end(), std::string()), lines.end());
  return lines;
}

}  // namespace

// ---- both pages ----

const std::vector<std::array<StashPerformerStats, 3>>& StashPages::performerStats(Menu& m) {
  StashMenu& st = state(m);
  StashPerformersState& ps = st.performers;
  const StashCatalog* c = catalog(m);
  if (!c) {
    ps.stats.clear();
    return ps.stats;
  }
  takeChanges(m);
  if (ps.statsCatalog == st.catalogGeneration && ps.statsLibrary == st.libraryGeneration &&
      ps.statsAway == st.awayGeneration && ps.stats.size() == c->performers().size())
    return ps.stats;
  ps.statsCatalog = st.catalogGeneration;
  ps.statsLibrary = st.libraryGeneration;
  ps.statsAway = st.awayGeneration;
  ps.stats.assign(c->performers().size(), {});
  const bool history = st.history.size() == c->scenes().size();
  for (size_t i = 0; i < c->scenes().size(); ++i) {
    if (history && st.history[i].hidden) continue;
    const StashScene& s = c->scenes()[i];
    bool vr = s.vr;
    if (history && st.history[i].vr >= 0) vr = st.history[i].vr == 1;
    const int kind = vr ? static_cast<int>(StashFormat::VR) : static_cast<int>(StashFormat::Flat);
    for (const int p : s.performers)
      for (const int f : {0, kind}) {
        StashPerformerStats& stats = ps.stats[p][f];
        ++stats.scenes;
        const StashAge age = stashAgeBand(stashAge(c->performers()[p].born, s.day));
        if (age != StashAge::kCount) stats.ages |= stashBit(age);
        stats.newest = std::max(stats.newest, s.day);
        if (!history) continue;
        const StashHistory& h = st.history[i];
        if (h.plays > 0) {
          ++stats.played;
          stats.plays += h.plays;
        }
        if (h.rating > 0) {
          ++stats.rated;
          stats.ratingSum += h.rating;
        }
        stats.lastPlayed = std::max(stats.lastPlayed, h.lastPlayedAt);
      }
  }
  return ps.stats;
}

void StashPages::drawPortrait(const Menu& m, cairo_t* cr, const StashPerformer& p, double x,
                              double y, double w, double h, double radius) {
  cairo_surface_t* image = nullptr;
  if (!p.image.empty()) {
    const StashImages::Request request = Stash::portraitRequest(p, stashPixels(w));
    image = m.stash_->images().find(request);
    if (!image && !m.stash_->images().failed(request)) m.stashMenu_->performers.waiting = true;
  }
  roundedRect(cr, x, y, w, h, radius);
  if (!image) {  // her initials
    cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
    cairo_fill(cr);
    std::string initials;  // the first character of her first two names
    const std::string& name = p.name;
    for (size_t i = 0, n = 0; i < name.size() && n < 2; ++i) {
      if (name[i] == ' ' || (i > 0 && name[i - 1] != ' ')) continue;
      size_t length = 1;
      while (i + length < name.size() && (name[i + length] & 0xC0) == 0x80) ++length;
      initials += letterOf(name.substr(i, 1)) == '#' ? name.substr(i, length)
                                                      : std::string(1, letterOf(name.substr(i, 1)));
      ++n;
    }
    m.drawText(cr, initials, x, y, w, h, static_cast<int>(std::max(14.0, w * 0.26)), true, true,
               1, 1, 1, 0.45);
    return;
  }
  // Filling the box; a tall portrait keeps its top (the face), as Stash does.
  const double sw = cairo_image_surface_get_width(image);
  const double sh = cairo_image_surface_get_height(image);
  const double s = std::max(w / sw, h / sh);
  cairo_save(cr);
  cairo_clip(cr);
  cairo_translate(cr, x + (w - sw * s) / 2, y + (h - sh * s) * 0.15);
  cairo_scale(cr, s, s);
  cairo_set_source_surface(cr, image, 0, 0);
  cairo_paint(cr);
  cairo_restore(cr);
}

// A card, as the lists' (Menu::addGallery()'s) with a 2:3 portrait: her
// scenes played on it, her name and two lines under it. A co-star: a round
// portrait, the name and the scenes with her; latched while chosen.
void StashPages::drawPerformerButton(const Menu& m, cairo_t* cr, const Button& b,
                                     const double ink[4]) {
  const Palette& pal = m.palette();
  const double dim[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a};
  const StashCatalog* c = catalog(m);
  const int index = c ? c->performerById(b.stashId) : -1;
  if (b.stashLook == static_cast<int>(StashLook::PerformerCard)) {
    const double pw = b.w - 2 * kCardIn, ph = std::round(pw * 1.5), px = b.x + kCardIn,
                 py = b.y + kCardIn;
    if (index >= 0)
      drawPortrait(m, cr, c->performers()[index], px, py, pw, ph, kit::kPieceCorner);
    else
      m.drawFinish(cr, Finish::Sunk, px, py, pw, ph, kit::kPieceCorner);
    m.drawCoverStrip(cr, b, px, py, pw, ph);  // her scenes played
    const double x = b.x + kit::kSpace, w = b.w - 2 * kit::kSpace, y = py + ph + kCardNameGap;
    m.drawLinesIn(cr, b.label, x, y, w, kit::kRowSize, 1, kCardLine, ink);
    m.drawLinesIn(cr, b.detail, x, y + kCardLine, w, kHintSize, 1, kCardLine, dim);
    m.drawLinesIn(cr, b.date, x, y + 2 * kCardLine, w, kHintSize, 1, kCardLine, dim);
    return;
  }
  // A co-star.
  const double d = b.h - 2 * kit::kSpacePiece - 4;
  if (index >= 0)
    drawPortrait(m, cr, c->performers()[index], b.x + kit::kSpacePiece + 2, b.y + (b.h - d) / 2, d, d,
                 d / 2);
  const double countW = b.size.empty() ? 0 : m.textWidth(b.size, kCountSize, false) + kit::kSpaceTight;
  const double nameX = b.x + d + kit::kSpace;
  m.drawText(cr, b.label, nameX, b.y, b.x + b.w - kit::kSpace - countW - nameX, b.h, kHintSize + 2,
             false, false, ink[0], ink[1], ink[2]);
  if (!b.size.empty()) {
    const double* n = b.latched ? ink : dim;
    m.drawText(cr, b.size, b.x + b.w - kit::kSpace - countW + kit::kSpaceTight, b.y, countW, b.h,
               kCountSize, false, false, n[0], n[1], n[2]);
  }
}

double StashPages::performerCardHeight(double w, int lines) {
  return kCardIn + std::round((w - 2 * kCardIn) * 1.5) + kCardNameGap + lines * kCardLine + kit::kSpace;
}

// ---- the Performers page ----

void StashPages::showPerformers(Menu& m) {
  StashPerformersState& ps = state(m).performers;
  m.showPage(Menu::Page::StashPerformers);
  m.scrollOffset_ = static_cast<int>(std::floor(ps.scroll));
  m.scrollFraction_ = ps.scroll - m.scrollOffset_;
  m.side_.position = ps.railPosition;
}

void StashPages::leavePerformers(Menu& m) {
  StashMenu& st = state(m);
  StashPerformersState& ps = st.performers;
  if (st.full == StashFull::None) ps.scroll = m.scrollOffset_ + m.scrollFraction_;
  ps.railPosition = m.side_.position;
  st.full = StashFull::None;  // a look's full list in place of the cards
  st.fullSearch.clear();
}

std::function<void()> StashPages::changePerformers(Menu& m,
                                                   std::function<void(StashPerformerChoice&)> f) {
  return [&m, f] {
    StashMenu& st = state(m);
    StashPerformersState& s = st.performers;
    f(s.choice);
    s.stale = true;
    if (st.full == StashFull::None) {
      m.scrollOffset_ = 0;
      m.scrollFraction_ = 0;
    } else {
      s.scroll = 0;
    }
    m.dirty_ = true;
  };
}

// The performers the choices and the search give, in order, the rail's
// counts (each kind with its own choices left out, so a count never leads
// to nothing) and the scenes with the chosen looks; when they, the catalog
// or the history changed.
void StashPages::buildPerformers(Menu& m) {
  StashMenu& st = state(m);
  StashPerformersState& ps = st.performers;
  const StashStrings& t = strings().stash;
  const std::vector<std::array<StashPerformerStats, 3>>& stats = performerStats(m);
  const StashFormat shownIn = shownFormat(m);
  if (ps.catalogGeneration != st.catalogGeneration ||
      ps.libraryGeneration != st.libraryGeneration || ps.awayGeneration != st.awayGeneration ||
      ps.format != shownIn) {
    ps.catalogGeneration = st.catalogGeneration;
    ps.libraryGeneration = st.libraryGeneration;
    ps.awayGeneration = st.awayGeneration;
    ps.format = shownIn;
    ps.stale = true;
  }
  if (!ps.stale) return;
  ps.stale = false;
  ps.shown.clear();
  ps.rail.clear();
  ps.all = 0;
  ps.formats = {};
  ps.theirScenes = -1;
  const StashCatalog* c = catalog(m);
  if (!c || stats.size() != c->performers().size()) {
    ps.railHeight = 0;
    return;
  }
  const int format = static_cast<int>(shownIn);
  const bool male = m.view_.stash.malePerformers;
  const StashPerformerChoice& ch = ps.choice;
  // The looks chosen, as the catalog's filter has them: per kind, each
  // value's choice (1 included, 2 left out) and whether any is included.
  StashFilter chosenLooks;
  ch.looks.addTo(chosenLooks, *c);
  std::array<std::vector<uint8_t>, kStashLookKinds> chosen;
  std::array<bool, kStashLookKinds> including{};
  for (int k = 0; k < kStashLookKinds; ++k) {
    chosen[k].assign(k < kStashNamedLooks ? c->lookNames(static_cast<StashLookKind>(k)).size()
                                          : size_t(kStashLookBuckets[k - kStashNamedLooks]),
                     0);
    for (const int v : chosenLooks.looks[k].exclude) chosen[k][v] = 2;
    for (const int v : chosenLooks.looks[k].include) chosen[k][v] = 1;
    including[k] = !chosenLooks.looks[k].include.empty();
  }
  constexpr int kAge = static_cast<int>(StashLookKind::Age);
  std::vector<std::string> words;  // all of them in her name or an alias
  {
    const std::string folded = stashFold(ps.text);
    size_t at = 0;
    while (at < folded.size()) {
      const size_t end = std::min(folded.find(' ', at), folded.size());
      if (end > at) words.push_back(folded.substr(at, end - at));
      at = end + 1;
    }
  }
  // What the app's history says of her scenes in a format, as bits.
  auto historyOf = [](const StashPerformerStats& s) {
    return stashBit(s.played ? StashPerformerHistory::Played : StashPerformerHistory::Unplayed) |
           (s.rated && s.average() >= 4 ? stashBit(StashPerformerHistory::Rated4) : 0u);
  };
  // The kinds of choice she misses with her scenes in a format, as bits: a
  // look included she hasn't (bit k; her age: in none of those scenes), one
  // left out she has (bit 16 + k), the history (bit 15).
  constexpr int kHistory = 15;
  auto misses = [&](int i, int f) {
    const StashPerformer& p = c->performers()[i];
    unsigned miss = 0;
    for (int k = 0; k < kStashLookKinds; ++k) {
      if (k == kAge) {
        if (including[k] && !(ch.looks.mask(StashLookKind::Age) & stats[i][f].ages))
          miss |= 1u << k;
        continue;
      }
      uint8_t choices = 0;
      stashEachLookValue(static_cast<StashLookKind>(k), p.looks[k], [&](int v) {
        choices |= chosen[k][v];
      });
      if (including[k] && !(choices & 1)) miss |= 1u << k;
      if (choices & 2) miss |= 1u << (16 + k);
    }
    if (ch.history && !(ch.history & historyOf(stats[i][f]))) miss |= 1u << kHistory;
    return miss;
  };
  std::array<std::vector<int>, kStashLookKinds> counts;
  for (int k = 0; k < kStashLookKinds; ++k) counts[k].assign(chosen[k].size(), 0);
  std::array<int, size_t(StashPerformerHistory::kCount)> history{};
  for (size_t i = 0; i < c->performers().size(); ++i) {
    const StashPerformer& p = c->performers()[i];
    if (!stashShown(p.gender, male)) continue;
    const bool listed = stats[i][format].scenes > 0;
    ps.all += listed;
    if (!words.empty()) {
      std::string haystack = stashFold(p.name);
      for (const std::string& a : p.aliases) haystack += "\n" + stashFold(a);
      if (!std::all_of(words.begin(), words.end(), [&](const std::string& w) {
            return haystack.find(w) != std::string::npos;
          }))
        continue;
    }
    for (int f = 0; f < 3; ++f)
      ps.formats[f] += stats[i][f].scenes > 0 && !misses(static_cast<int>(i), f);
    if (!listed) continue;
    // Each kind counted with the others chosen kept.
    const unsigned miss = misses(static_cast<int>(i), format);
    auto fitsBut = [miss](int k) { return !(miss & ~(1u << k)); };
    for (int k = 0; k < kStashLookKinds; ++k) {
      if (!fitsBut(k)) continue;
      if (k == kAge) {
        for (size_t b = 0; b < counts[k].size(); ++b) counts[k][b] += (stats[i][format].ages >> b) & 1;
      } else {
        stashEachLookValue(static_cast<StashLookKind>(k), p.looks[k], [&](int v) { ++counts[k][v]; });
      }
    }
    if (fitsBut(kHistory)) {
      const unsigned mine = historyOf(stats[i][format]);
      for (size_t k = 0; k < history.size(); ++k) history[k] += (mine >> k) & 1;
    }
    if (!miss) ps.shown.push_back(static_cast<int>(i));
  }

  // Sorted; ties (and those without the value, last either way) by scenes,
  // then by name: the catalog's own order.
  const bool down = ps.descending;
  auto key = [&](int i) -> double {
    const StashPerformerStats& s = stats[i][format];
    switch (ps.sort) {
      case StashPerformerSort::Scenes: return s.scenes;
      case StashPerformerSort::Plays: return s.plays;
      case StashPerformerSort::Rating: return s.rated ? s.average() : -1;
      case StashPerformerSort::Newest: return s.newest == kStashNoDay ? -1 : s.newest;
      case StashPerformerSort::Name: break;
    }
    return 0;
  };
  if (ps.sort == StashPerformerSort::Name) {
    if (down) std::reverse(ps.shown.begin(), ps.shown.end());
  } else {
    const bool known = ps.sort == StashPerformerSort::Rating || ps.sort == StashPerformerSort::Newest;
    std::stable_sort(ps.shown.begin(), ps.shown.end(), [&](int a, int b) {
      const double ka = key(a), kb = key(b);
      if (known && (ka < 0) != (kb < 0)) return kb < 0;
      if (ka != kb) return down ? ka > kb : ka < kb;
      return stats[a][format].scenes > stats[b][format].scenes;
    });
  }

  // Their scenes: the Scenes page with these looks (the same filter there:
  // scenes in which one of them had them all, her age then).
  if (ch.looks.active() && !ch.history && words.empty()) {
    StashList list;
    list.format = shownIn;
    list.looks = ch.looks;
    ps.theirScenes = static_cast<int>(
        c->query(stashFilterOf(list, *c, male, unixNow()), {}, &st.history, false).scenes.size());
  }

  // The rail: what is chosen first, then what occurs, most first; never a
  // choice in every one listed (it narrows nothing) or in none.
  const int total = static_cast<int>(ps.shown.size());
  auto change = [&m](std::function<void(StashPerformerChoice&)> f) {
    return changePerformers(m, std::move(f));
  };
  auto changeLooks = [&change](std::function<void(StashLooks&)> f) {
    return change([f](StashPerformerChoice& s) { f(s.looks); });
  };
  auto line = [&](StashRailLine::Kind kind, const std::string& label) {
    StashRailLine l;
    l.kind = kind;
    l.label = label;
    ps.rail.push_back(std::move(l));
  };
  if (ch.active()) {
    StashRailLine l;
    l.kind = StashRailLine::Link;
    l.label = t.clearAll;
    l.include = change([](StashPerformerChoice& s) { s = StashPerformerChoice{}; });
    ps.rail.push_back(std::move(l));
    line(StashRailLine::Gap, "");
  }
  std::array<std::vector<StashFacet>, kStashLookKinds> facets;
  for (int k = 0; k < kStashLookKinds; ++k) {
    for (size_t v = 0; v < counts[k].size(); ++v)
      if (counts[k][v] > 0) facets[k].push_back({static_cast<int>(v), counts[k][v]});
    std::stable_sort(facets[k].begin(), facets[k].end(),
                     [](const StashFacet& a, const StashFacet& b) { return a.count > b.count; });
  }
  ps.looks = facets;
  st.fullStale = true;  // a look's full list, from these
  auto more = [&m](StashLookKind look) { showFull(m, StashFull::Look, look); };
  using K = StashLookKind;
  lookRail(ps.rail, *c, ch.looks, facets, total, {K::Hair, K::Cup, K::Breasts}, true, changeLooks,
           more);
  {
    const size_t at = ps.rail.size();
    line(StashRailLine::Heading, t.sections[7]);
    for (int i = 0; i < 3; ++i) {
      const bool on = ch.history >> i & 1;
      if (on || (history[i] > 0 && (history[i] < total || ch.history))) {
        StashRailLine l;
        l.label = t.performerHistory[i];
        l.count = history[i] > 0 ? stashNumber(history[i]) : "";
        l.state = on ? 1 : 0;
        l.include = change([i](StashPerformerChoice& s) { s.history ^= 1u << i; });
        ps.rail.push_back(std::move(l));
      }
    }
    if (ps.rail.size() == at + 1)
      ps.rail.pop_back();
    else
      line(StashRailLine::Gap, "");
  }
  lookRail(ps.rail, *c, ch.looks, facets, total,
           {K::Age, K::Ethnicity, K::Country, K::Height, K::Eyes, K::Shape, K::Tattoos}, true,
           changeLooks, more);
  if (ps.rail.empty()) line(StashRailLine::Note, t.nothingToNarrow);
  ps.railHeight = railHeights(ps.rail);
}

// Cards from `top` down: six columns beside the rail.
void StashPages::layoutPerformerGrid(Menu& m, double x0, double top) {
  StashPerformersState& ps = state(m).performers;
  const StashCatalog& c = *catalog(m);
  const StashStrings& t = strings().stash;
  const double cardW = (m.listRight_ - 30 - x0 - (kColumns - 1) * kTileGap) / kColumns;
  const double pictureW = cardW - 2 * kCardIn;
  const double rowH = performerCardHeight(cardW) + kTileGap;
  const int count = static_cast<int>(ps.shown.size());
  m.listTop_ = top;
  m.rowHeight_ = rowH;
  m.visibleRows_ = std::max(1, static_cast<int>((m.listBottom_ - top) / rowH));
  m.listCount_ = (count + kColumns - 1) / kColumns;
  int firstRow, endRow;
  m.clampScroll(m.listCount_, m.visibleRows_, firstRow, endRow);
  const int first = firstRow * kColumns, end = std::min(count, endRow * kColumns);
  const int32_t now = today();
  for (int i = first; i < end; ++i) {
    const int cell = i - first, index = ps.shown[i];
    const StashPerformer& p = c.performers()[index];
    const StashPerformerStats& s = ps.stats[index][static_cast<int>(ps.format)];
    Button b;
    b.panel = Menu::kBrowser;
    b.x = x0 + (cell % kColumns) * (cardW + kTileGap);
    b.y = top + (cell / kColumns - m.scrollFraction_) * rowH;
    b.w = cardW;
    b.h = rowH - kTileGap;
    b.label = p.name;
    std::string scenes = stashScenes(s.scenes);
    if (ps.sort == StashPerformerSort::Plays && s.plays > 0)
      scenes = stashPlays(s.plays) + " · " + scenes;
    b.detail = stashJoined({scenes, s.rated ? stashPrinted(t.stars, s.average()) : ""});
    const int age = stashAge(p.born, now);
    b.date = stashJoined({p.country, age > 0 ? std::to_string(age) : ""});
    if (s.played > 0) b.plays = std::to_string(s.played);
    b.card = true;
    b.radius = kCardCorner;
    b.scrolls = true;
    b.stashLook = static_cast<int>(StashLook::PerformerCard);
    b.stashId = p.id;
    b.action = [&m, id = p.id] {
      showPerformer(m, id);
      return MenuCommand{};
    };
    if (m.textWidth(b.label, kit::kRowSize, false) > b.w - 2 * kit::kSpace) b.tooltip = b.label;
    m.buttons_.push_back(b);
  }
  // Portraits: those shown, then the next screens and the row before.
  std::vector<StashImages::Request> wanted;
  auto want = [&](int i) {
    if (i < 0 || i >= count) return;
    const StashPerformer& p = c.performers()[ps.shown[i]];
    if (!p.image.empty()) wanted.push_back(Stash::portraitRequest(p, stashPixels(pictureW)));
  };
  for (int i = first; i < end; ++i) want(i);
  for (int i = end; i < end + (m.visibleRows_ + 1) * kColumns * 3; ++i) want(i);
  for (int i = first - 1; i >= first - kColumns * 2; --i) want(i);
  m.stash_->images().want(wanted);
  m.addScrollbar();
}

// The header row (the title and count, Their scenes), the tools row, the
// letters, then the rail and the cards (or a look's full list).
void StashPages::layoutPerformers(Menu& m) {
  StashMenu& st = state(m);
  StashPerformersState& ps = st.performers;
  const StashStrings& t = strings().stash;
  buildPerformers(m);
  const StashCatalog* c = catalog(m);
  const bool editing = m.editingFilter();
  const double bottom = editing ? kKeyboardY - 16 : kListBottom;
  const double x0 = kContentLeft, right = kContentRight;

  m.listLeft_ = x0 + kRailW + kRailGap;
  m.listRight_ = right + kListRightPad;
  m.listShowsFolder_ = false;
  m.listColumns_ = false;
  m.listBottom_ = bottom;
  m.listTop_ = kCardsTop;
  const bool full = st.full != StashFull::None;  // a look's, in place of the letters and cards
  if (c) {
    layoutRail(m, ps.rail, ps.railHeight, kCardsTop, bottom);
    if (full) {
      st.resultsX = m.listLeft_;
      st.resultsTop = kGridTop;
      layoutFull(m, m.listLeft_, kGridTop);
    } else {
      layoutPerformerGrid(m, m.listLeft_, kCardsTop);
    }
  } else {
    m.listCount_ = 0;
    m.visibleRows_ = 1;
  }
  if (editing) m.layoutKeyboard();

  // Their scenes, at the header's right.
  if (ps.theirScenes > 0) {
    Button their = actionButton(
        m, std::string(t.theirScenes) + "  " + stashNumber(ps.theirScenes), StashIcon::Scenes,
        [&m] {
          StashMenu& s = state(m);
          StashList list;
          list.format = s.list.format;
          list.sort = s.list.sort;
          list.descending = s.list.descending;
          list.looks = s.performers.choice.looks;
          openScenes(m, list);
          return MenuCommand{};
        },
        0, kBrowserRowY, kBrowserRowH);
    their.x = right - their.w;
    their.tooltip = t.tipTheirScenes;
    m.buttons_.push_back(their);
  }

  // A–Z, a well of letters: one jumps to the first name with it (sorted by
  // name); those no name has are greyed.
  if (!full) {
    constexpr double in = kit::kSpacePiece;
    const int letters = static_cast<int>(sizeof(kLetters) - 1);
    const double lw = (right - x0 - in) / letters - in;
    Button well;
    well.panel = Menu::kBrowser;
    well.well = true;
    well.x = x0;
    well.y = kGridTop;
    well.w = right - x0;
    well.h = kLettersH;
    well.radius = kit::kCorner;
    m.buttons_.push_back(well);
    for (int i = 0; i < letters; ++i) {
      const char letter = kLetters[i];
      Button b = m.textButton(std::string(1, letter), Menu::Icon::None, [&m, letter] {
        StashPerformersState& s = state(m).performers;
        if (s.sort != StashPerformerSort::Name || s.descending) {
          s.sort = StashPerformerSort::Name;
          s.descending = false;
          s.stale = true;
          buildPerformers(m);
        }
        const StashCatalog* cat = catalog(m);
        for (size_t at = 0; cat && at < s.shown.size(); ++at)
          if (letterOf(cat->performers()[s.shown[at]].name) == letter) {
            m.scrollOffset_ = static_cast<int>(at) / kColumns;
            m.scrollFraction_ = 0;
            break;
          }
        m.dirty_ = true;
        return MenuCommand{MenuCommand::SetStashPerformersSort, double(s.descending),
                           stashSortName(s.sort)};
      });
      b.panel = Menu::kBrowser;
      b.piece = true;
      b.x = x0 + in + i * (lw + in);
      b.y = kGridTop + in;
      b.w = lw;
      b.h = kLettersH - 2 * in;
      b.tooltip = t.tipJumpLetter;
      b.radius = kit::kPieceCorner;
      b.fontSize = kit::kWellSize;
      b.disabled = !c || std::none_of(ps.shown.begin(), ps.shown.end(), [&](int p) {
        return letterOf(c->performers()[p].name) == letter;
      });
      m.buttons_.push_back(b);
    }
  }

  // The tools row: All / VR / 2D (the Scenes page's, with the performers
  // each lists), the order, the search, to the top / end.
  std::vector<Menu::ToolPiece> formats;
  for (int f = 0; f < 3; ++f) {
    Menu::ToolPiece p;
    p.label = t.formats[f];
    if (c) p.count = stashNumber(ps.formats[f]);
    p.chosen = static_cast<int>(ps.format) == f;
    p.disabled = c && ps.formats[f] == 0 && !p.chosen;
    p.action = [&m, f] { return setFormat(m, static_cast<StashFormat>(f)); };
    formats.push_back(std::move(p));
  }
  const double fx = m.toolsWell(x0, kBrowserToolsY, formats) + kColumnGap;
  const double x = layoutSort(
      m, fx, t.performerSorts, 5, static_cast<int>(ps.sort), ps.descending,
      [&m](int f) {
        StashPerformersState& s = state(m).performers;
        const StashPerformerSort sort = static_cast<StashPerformerSort>(f);
        // A–Z for names, else the most first.
        if (s.sort != sort) s.descending = sort != StashPerformerSort::Name;
        s.sort = sort;
        s.stale = true;
        return MenuCommand{MenuCommand::SetStashPerformersSort, double(s.descending),
                           stashSortName(s.sort)};
      },
      [&m] {
        StashPerformersState& s = state(m).performers;
        s.descending = !s.descending;
        s.stale = true;
        return MenuCommand{MenuCommand::SetStashPerformersSort, double(s.descending),
                           stashSortName(s.sort)};
      });
  layoutSearch(m, x, layoutJumps(m), ps.text, t.searchPerformers,
               StashMenu::Typing::Performers);
}

// The title and count as a page's (Menu::contentTitle()), and what an
// empty page means.
void StashPages::drawPerformersHeading(const Menu& m, cairo_t* cr) {
  const StashPerformersState& ps = m.stashMenu_->performers;
  const StashStrings& t = strings().stash;
  const Palette& pal = m.palette();
  const StashCatalog* c = catalog(m);
  const double x = kContentLeft + kit::kSpace;
  const double titleW = m.textWidth(t.performers, kit::kTitleSize, true) + 4;
  m.drawText(cr, t.performers, x, kBrowserRowY, titleW, kBrowserRowH, kit::kTitleSize, true, false,
             pal.text.r, pal.text.g, pal.text.b);
  if (c) {
    const long shown = static_cast<long>(ps.shown.size());
    const std::string count =
        shown == ps.all ? shown == 1 ? std::string(t.onePerformer)
                                     : stashPrinted(t.performersCount, stashNumber(shown))
            : stashPrinted(t.performersCountOf, stashNumber(shown), stashNumber(ps.all));
    m.drawText(cr, count, x + titleW + kit::kSpaceEdge, kBrowserRowY, 500, kBrowserRowH, 23, false,
               false, pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a);
  }
  if (!c)
    drawWaiting(m, cr, m.listLeft_ + kit::kSpace, kCardsTop + 20,
                kContentRight - m.listLeft_ - 2 * kit::kSpace);
  else if (ps.shown.empty() && m.stashMenu_->full == StashFull::None)
    m.drawEmptyPage(cr, Menu::Icon::People, t.noPerformers);
  drawFullHeading(m, cr);
}

// ---- a performer's page ----

int64_t StashPages::fixedPerformer(const Menu& m) {
  return m.page_ == Menu::Page::StashPerformer && m.stashMenu_ ? m.stashMenu_->performer : 0;
}

std::string StashPages::performerTitle(const Menu& m) {
  const StashStrings& t = strings().stash;
  const StashCatalog* c = catalog(m);
  const StashMenu& st = *m.stashMenu_;
  const int index = c ? c->performerById(st.performer) : -1;
  if (index < 0) return t.performers;
  std::vector<std::string> with;
  for (const int64_t id : st.list.performers) {
    const int other = id == st.performer ? -1 : c->performerById(id);
    if (other >= 0) with.push_back(c->performers()[other].name);
  }
  const std::string& name = c->performers()[index].name;
  return with.empty() ? name : stashPrinted(t.titleWith, name, stashJoined(with, t.and_));
}

void StashPages::showPerformer(Menu& m, int64_t id) {
  StashMenu& st = state(m);
  if (m.page_ == Menu::Page::StashPerformer && st.performer == id) return;
  m.showPage(Menu::Page::StashPerformer);  // leaves the page before
  st.performer = id;
  enterPerformer(m);
}

// Her scenes in the Scenes page's list: as she was left, if she was the
// last one shown; else all of them, newest first, in the format chosen (all
// formats if she has none in it).
void StashPages::enterPerformer(Menu& m) {
  StashMenu& st = state(m);
  StashPerformersState& ps = st.performers;
  if (ps.scenes || enterList(m, ps.scenes, ps.her, st.performer)) return;
  st.list.performers = {st.performer};
  const StashCatalog* c = catalog(m);
  if (c && st.list.format != StashFormat::All &&
      c->query(stashFilterOf(st.list, *c, m.view_.stash.malePerformers, unixNow()), {},
               &st.history, false)
          .scenes.empty())
    st.list.format = StashFormat::All;
}

// Hers kept for coming back (after playing one of them, say), the Scenes
// page's put back.
void StashPages::leavePerformer(Menu& m) {
  StashMenu& st = state(m);
  leaveList(m, st.performers.scenes, st.performers.her, st.performer);
}

// Her profile, a column at the page's right: portrait with her history in
// the app beside it, facts under it, then her co-stars in her scenes as
// they are filtered (women only unless the male performers setting is on):
// tapping one keeps her scenes with them too, again takes them away.
void StashPages::layoutProfile(Menu& m, int index, double x, double top, double bottom) {
  StashMenu& st = state(m);
  StashPerformersState& ps = st.performers;
  const StashStrings& t = strings().stash;
  const StashCatalog& c = *catalog(m);
  const StashPerformer& p = c.performers()[index];
  performerStats(m);
  ps.profileX = x;
  ps.profileTop = top;
  std::vector<StashImages::Request> pictures;
  if (!p.image.empty()) pictures.push_back(Stash::portraitRequest(p, stashPixels(kPortraitW)));
  double y = top + kPortraitH + 16 + factsOf(p).size() * kFactStep;
  ps.profileBottom = y;
  ps.coStarsTop = -1;

  // Those chosen first, then the most frequent.
  std::vector<std::pair<int, int>> coStars;  // performer, scenes with her here
  for (const int64_t id : st.list.performers) {
    const int other = id == p.id ? -1 : c.performerById(id);
    if (other < 0) continue;
    int count = 0;
    for (const StashFacet& f : st.result.performers)
      if (f.index == other) count = f.count;
    coStars.emplace_back(other, count);
  }
  const size_t chosen = coStars.size();
  for (const StashFacet& f : st.result.performers)
    if (f.index != index &&
        std::none_of(coStars.begin(), coStars.begin() + chosen,
                     [&](const auto& co) { return co.first == f.index; }))
      coStars.emplace_back(f.index, f.count);
  if (coStars.empty() || y + 12 + 40 + kCoStarH > bottom) {
    m.stashCovers_->wantFirst(std::move(pictures));
    return;
  }
  y += 12;
  ps.coStarsTop = y;
  y += 44;
  double cx = x;
  const double avatar = kCoStarH - 2 * kit::kSpacePiece - 4;
  for (size_t i = 0; i < coStars.size(); ++i) {
    const StashPerformer& co = c.performers()[coStars[i].first];
    const std::string count = coStars[i].second > 0 ? stashNumber(coStars[i].second) : "";
    const double w =
        std::min(kProfileW, avatar + 2 * kit::kSpace + 4 + m.textWidth(co.name, kHintSize + 2, false) +
                                (count.empty() ? 0 : m.textWidth(count, kCountSize, false) + kit::kSpaceTight));
    if (cx > x && cx + w > x + kProfileW) {
      cx = x;
      y += kCoStarH + kCoStarGap;
    }
    if (y + kCoStarH > bottom) break;
    Button b;
    b.panel = Menu::kBrowser;
    b.x = cx;
    b.y = y;
    b.w = w;
    b.h = kCoStarH;
    b.label = co.name;
    b.size = count;
    b.latched = i < chosen;
    b.radius = kCoStarH / 2;
    b.stashLook = static_cast<int>(StashLook::CoStar);
    b.stashId = co.id;
    b.tooltip = t.tipCoStar;
    b.action = [&m, id = co.id] {
      changeList(m, [id](StashList& l) {
        const auto found = std::find(l.performers.begin(), l.performers.end(), id);
        if (found == l.performers.end()) {
          l.performers.push_back(id);
          l.performersOut.erase(std::remove(l.performersOut.begin(), l.performersOut.end(), id),
                                l.performersOut.end());
        } else {
          l.performers.erase(found);
        }
        l.performersAll = true;  // her scenes with them too
      });
      return MenuCommand{};
    };
    m.buttons_.push_back(b);
    if (!co.image.empty()) pictures.push_back(Stash::portraitRequest(co, stashPixels(avatar)));
    cx += w + kCoStarGap;
    ps.profileBottom = y + kCoStarH;
  }
  m.stashCovers_->wantFirst(std::move(pictures));
}

void StashPages::layoutPerformer(Menu& m) {
  StashMenu& st = state(m);
  StashPerformersState& ps = st.performers;
  const StashStrings& t = strings().stash;
  enterPerformer(m);
  refresh(m);
  ps.waiting = false;  // until a picture drawn is not there yet
  const StashCatalog* c = catalog(m);
  const int index = c ? c->performerById(st.performer) : -1;
  const double bottom = m.editingFilter() ? kKeyboardY - 16 : kListBottom;
  const double x0 = kContentLeft;
  const double profileX = kContentRight - kProfileW;
  // Her profile first: its pictures go before the results' covers.
  ps.profileX = profileX;
  ps.profileTop = ps.profileBottom = kGridTop;
  ps.coStarsTop = -1;
  if (index >= 0) layoutProfile(m, index, profileX, kGridTop, bottom);
  layoutResults(m, kGridTop, profileX - kit::kSpaceEdge);
  Button back = actionButton(
      m, t.performers, StashIcon::Back,
      [&m] {
        showPerformers(m);
        return MenuCommand{};
      },
      x0, kBrowserRowY, kBrowserRowH);
  m.buttons_.push_back(back);
  layoutHeader(m, back.x + back.w + kit::kSpaceTight);
  layoutTools(m);
}

void StashPages::drawProfile(const Menu& m, cairo_t* cr, const StashPerformer& p) {
  const StashPerformersState& ps = m.stashMenu_->performers;
  const StashStrings& t = strings().stash;
  const Palette& pal = m.palette();
  const double x = ps.profileX, top = ps.profileTop, bottom = m.listBottom_;
  drawPortrait(m, cr, p, x, top, kPortraitW, kPortraitH, 12);

  // Her history in the app, beside the portrait.
  const StashCatalog* c = catalog(m);
  const int index = c ? c->performerById(p.id) : -1;
  StashPerformerStats s;
  if (index >= 0 && index < static_cast<int>(ps.stats.size()))
    s = ps.stats[index][static_cast<int>(m.stashMenu_->list.format)];  // her page's format
  const double sx = x + kPortraitW + 20, sw = kProfileW - kPortraitW - 20;
  double y = top;
  auto stat = [&](const std::string& big, const std::string& small) {
    m.drawText(cr, big, sx, y, sw, 40, 30, true, false, pal.text.r, pal.text.g, pal.text.b);
    m.drawText(cr, small, sx, y + 38, sw, 28, 22, false, false, pal.textDim.r, pal.textDim.g,
               pal.textDim.b, pal.textDim.a);
    y += 76;
  };
  stat(stashNumber(s.scenes), t.scenesHere);
  stat(stashNumber(s.played),
       s.plays > 0 ? std::string(t.played) + " · " + stashPlays(s.plays) : t.played);
  stat(s.rated ? stashPrinted(t.stars, s.average()) : "–",
       s.rated ? stashPrinted(t.averageOf, s.rated) : t.noneRated);
  const double dim[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a};
  m.drawTextLines(
      cr, s.lastPlayed > 0 ? stashPrinted(t.lastPlayed, stashAgo(s.lastPlayed)) : t.neverPlayed, sx,
      y, sw, 22, 2, dim, false);

  // What Stash knows of her.
  y = top + kPortraitH + 16;
  for (const std::string& fact : factsOf(p)) {
    if (y + kFactStep > bottom) break;
    m.drawText(cr, fact, x, y, kProfileW, kFactStep, 24, false, false, pal.text.r, pal.text.g,
               pal.text.b);
    y += kFactStep;
  }
  if (ps.coStarsTop >= 0)  // as a group's title
    m.drawText(cr, t.oftenWith, x, ps.coStarsTop, kProfileW, 40, kit::kGroupTitleSize, true, false,
               pal.textDim.r, pal.textDim.g, pal.textDim.b);
}

void StashPages::drawPerformerHeading(const Menu& m, cairo_t* cr) {
  drawScenesHeading(m, cr);  // her name (with the co-stars chosen), the count, the notes
  const StashCatalog* c = catalog(m);
  const int index = c ? c->performerById(m.stashMenu_->performer) : -1;
  if (index >= 0) {
    drawProfile(m, cr, c->performers()[index]);
  } else if (c) {
    const Palette& pal = m.palette();
    const double dim[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a};
    m.drawTextLines(cr, strings().stash.performerGone, m.stashMenu_->performers.profileX,
                    kGridTop + 20, kProfileW, 27, 3, dim, false);
  }
}
