#include "menu_style.h"

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

constexpr double kRailW = 400, kRailGap = 16;
constexpr double kRowStep = 54, kRowBox = 50;  // facet rows (rail and full lists)
constexpr double kLineGrow = 30;               // ... and each wrapped line, editing the rail
constexpr double kExcludeW = 60;               // ... and their ⊘ target
constexpr double kChipH = 48;
constexpr double kResultsGap = 12;  // under the chips
constexpr double kTileGap = 16;
constexpr int kRailTop[] = {5, 6, 5};  // performers, tags and studios the rail lists
constexpr int kRailYears = 6;
constexpr int kAddedDays[] = {7, 30, 90, 365};
constexpr StashResolution kResolutionOrder[] = {StashResolution::VR8K,    StashResolution::VR7K,
                                                StashResolution::VR6K,    StashResolution::VR5K,
                                                StashResolution::VR4K,    StashResolution::VRUpTo3K,
                                                StashResolution::Flat4K,  StashResolution::Flat1080,
                                                StashResolution::Flat720, StashResolution::FlatSD};
// The Sort by fields, in the dropdown's order: the dates together.
constexpr int kSortOrder[] = {int(StashSort::Released),   int(StashSort::Added),
                              int(StashSort::Modified),   int(StashSort::LastPlayed),
                              int(StashSort::Rating),     int(StashSort::Plays),
                              int(StashSort::Title),      int(StashSort::Duration),
                              int(StashSort::Resolution), int(StashSort::Size),
                              int(StashSort::CastSize),   int(StashSort::Random)};

enum Kind { kPerformers, kTags, kStudios };

// The Looks section's kinds, as its subgroups list them from kSubHair on.
const std::vector<StashLookKind> kSceneLooks = {
    StashLookKind::Hair,    StashLookKind::Cup,   StashLookKind::Breasts, StashLookKind::Age,
    StashLookKind::Ethnicity, StashLookKind::Country, StashLookKind::Height, StashLookKind::Eyes,
    StashLookKind::Shape,   StashLookKind::Tattoos};

std::vector<int64_t> StashList::* const kIn[] = {&StashList::performers, &StashList::tags,
                                                 &StashList::studios};
std::vector<int64_t> StashList::* const kOut[] = {&StashList::performersOut, &StashList::tagsOut,
                                                  &StashList::studiosOut};

int64_t idOf(const StashCatalog& c, int kind, int index) {
  if (kind == kPerformers) return c.performers()[index].id;
  if (kind == kTags) return c.tags()[index].ids.empty() ? 0 : c.tags()[index].ids.front();
  return c.studios()[index].id;
}

int indexOf(const StashCatalog& c, int kind, int64_t id) {
  return kind == kPerformers ? c.performerById(id)
         : kind == kTags     ? c.tagById(id)
                             : c.studioById(id);
}

const std::string& nameOf(const StashCatalog& c, int kind, int index) {
  if (kind == kPerformers) return c.performers()[index].name;
  if (kind == kTags) return c.tags()[index].name;
  return c.studios()[index].name;
}

const std::vector<StashFacet>& facetOf(const StashResult& r, int kind) {
  return kind == kPerformers ? r.performers : kind == kTags ? r.tags : r.studios;
}

const StashPick& pickOf(const StashFilter& f, int kind) {
  return kind == kPerformers ? f.performers : kind == kTags ? f.tags : f.studios;
}

// Whether the kind's counts are taken in the results (none included, or
// All of), so one as many as the results is in every one of them; Any of
// counts them without its own choices.
bool inResults(const StashPick& pick) { return pick.all || pick.include.empty(); }

// Removes the id from the list, and the other ids of the same merged tag.
// By ids, not indexes: a click may come after a new catalog replaced the
// one the page was laid out with, before it is laid out again.
bool removeId(std::vector<int64_t>& ids, const StashCatalog* c, int kind, int64_t id) {
  const int index = c ? indexOf(*c, kind, id) : -1;
  const size_t before = ids.size();
  ids.erase(std::remove_if(ids.begin(), ids.end(),
                           [&](int64_t other) {
                             return other == id ||
                                    (index >= 0 && indexOf(*c, kind, other) == index);
                           }),
            ids.end());
  return ids.size() != before;
}

int countIn(const std::vector<StashFacet>& facet, int index) {
  for (const StashFacet& f : facet)
    if (f.index == index) return f.count;
  return 0;
}

int64_t unixNow() { return static_cast<int64_t>(std::time(nullptr)); }

// The grid's tiles across, from x0 to the list's right: about 400 wide.
int gridColumns(double x0, double right) {
  return std::max(1, static_cast<int>((right - 30 - x0 + kTileGap) / (400 + kTileGap)));
}

// An active filter above the results; tapping it takes it away.
struct Chip {
  std::string label;
  bool out = false;
  std::function<void(StashList&)> remove;
};

}  // namespace

bool StashPages::takeChanges(Menu& m) {
  StashMenu& st = state(m);
  bool changed = false;
  if (m.stash_->generation() != st.catalogGeneration) {
    st.catalogGeneration = m.stash_->generation();
    st.libraryGeneration = ~0ull;
    st.stale = true;
    changed = true;
  }
  if (m.library_ && m.library_->generation() != st.libraryGeneration) {
    st.libraryGeneration = m.library_->generation();
    std::vector<StashHistory> history = m.stash_->history();
    // A format chosen in the player moves its scene between VR and 2D: query
    // again then (a rating alone doesn't).
    if (history.size() != st.history.size()) {
      st.stale = true;
    } else {
      for (size_t i = 0; i < history.size() && !st.stale; ++i)
        st.stale = history[i].vr != st.history[i].vr;
    }
    st.history = std::move(history);
    for (size_t i = 0; i < st.history.size() && i < st.hidden.size(); ++i)
      st.history[i].hidden = st.hidden[i];
    changed = true;
  }
  // Away from home: the scenes on a server that doesn't answer (SMB, or
  // Stash for those played from its stream), but for those with a copy on
  // this device; and those whose only file was deleted (Stash::hideScene()).
  const StashCatalog* c = m.stash_->catalog().get();
  if (m.offline_ && c && st.history.size() == c->scenes().size() &&
      (changed || st.offlineShape != m.offlineShapeGeneration_)) {
    st.offlineShape = m.offlineShapeGeneration_;
    std::vector<bool> hidden(c->scenes().size());
    const bool away = !m.offline_->downServers().empty() || m.offline_->stashDown();
    if (away || m.stash_->hidesScenes())
      for (size_t i = 0; i < hidden.size(); ++i) {
        const StashScene& s = c->scenes()[i];
        if (m.stash_->hidden(s.id)) {
          hidden[i] = true;
        } else if (away) {
          const std::string at = stashSceneLocation(s, m.offline_);
          hidden[i] = m.offline_->serverDown(at) && m.offline_->copyOf(at, s.oshash).empty();
        }
      }
    if (hidden != st.hidden) {
      st.hidden = std::move(hidden);
      for (size_t i = 0; i < st.history.size(); ++i) st.history[i].hidden = st.hidden[i];
      ++st.awayGeneration;
      st.stale = true;
      changed = true;
    }
  }
  return changed;
}

// The query, when the list or the catalog changed; the history (ratings
// and plays) when the library's did, without querying again: a rating
// changed meanwhile doesn't move its tile away from the pointer.
void StashPages::refresh(Menu& m) {
  StashMenu& st = state(m);
  const StashCatalog* c = catalog(m);
  if (!c) {
    st.result = StashResult{};
    st.rail.clear();
    st.railHeight = 0;
    st.fullRows.clear();
    return;
  }
  takeChanges(m);
  if (!st.stale) return;
  st.stale = false;
  st.filter = stashFilterOf(st.list, *c, m.view_.stash.malePerformers, unixNow());
  const bool history = st.history.size() == c->scenes().size();
  // A studio's page: its own scenes; the rail offers no studios there.
  StashFilter filter = st.filter;
  const int64_t fixed = fixedStudio(m);
  const int scope = fixed ? c->studioById(fixed) : -1;
  if (scope >= 0) filter.studios = StashPick{{scope}, {}, false};
  const int64_t her = fixedPerformer(m);
  if ((fixed && scope < 0) || (her && c->performerById(her) < 0))
    st.result = StashResult{};  // gone from Stash
  else
    st.result = c->query(filter, stashOrderOf(st.list, st.seed), history ? &st.history : nullptr);
  buildRail(m);
  st.fullStale = true;
}

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
    m.scrollOffset_ = 0;
    m.scrollFraction_ = 0;
  } else {
    st.scroll = 0;
  }
  m.dirty_ = true;
}

namespace {

// What each kind of choice does to the list: included (or left out) once
// more, it is taken away.
std::function<void(StashList&)> includeIn(Menu& m, int kind, int64_t id) {
  return [&m, kind, id](StashList& l) {
    const StashCatalog* c = StashPages::catalog(m);
    removeId(l.*kOut[kind], c, kind, id);
    if (!removeId(l.*kIn[kind], c, kind, id)) (l.*kIn[kind]).push_back(id);
  };
}

std::function<void(StashList&)> excludeIn(Menu& m, int kind, int64_t id) {
  return [&m, kind, id](StashList& l) {
    const StashCatalog* c = StashPages::catalog(m);
    removeId(l.*kIn[kind], c, kind, id);
    if (!removeId(l.*kOut[kind], c, kind, id)) (l.*kOut[kind]).push_back(id);
  };
}

// On a studio's page, studios are no choice: its scenes are all its own.
bool offered(const Menu& m, int kind) { return kind != kStudios || !StashPages::fixedStudio(m); }

std::function<void(StashList&)> toggleBit(unsigned StashList::* mask, unsigned bit) {
  return [mask, bit](StashList& l) { l.*mask ^= 1u << bit; };
}

const char* subgroupLabel(int subgroup) {
  const StashStrings& t = strings().stash;
  switch (subgroup) {
    case kSubWomen: return t.women;
    case kSubMen: return t.men;
    case kSubAdded: return t.added;
    case kSubYear: return t.released;
    case kSubResolution: return t.resolution;
    case kSubDuration: return t.duration;
    case kSubFrameRate: return t.frameRate;
    case kSubCodec: return t.codec;
    case kSubFileSize: return t.fileSize;
    default: return stashLookHeading(static_cast<int>(kSceneLooks[subgroup - kSubHair]));
  }
}

// The chips of a list, in the rail's order.
std::vector<Chip> chipsOf(Menu& m, const StashList& l, const StashCatalog& c) {
  const StashStrings& t = strings().stash;
  std::vector<Chip> chips;
  const int64_t fixed = StashPages::fixedPerformer(m);  // her page: always her
  for (int kind : {kPerformers, kTags, kStudios}) {
    for (int out = 0; out < 2; ++out)
      for (const int64_t id : out ? l.*kOut[kind] : l.*kIn[kind]) {
        const int index = indexOf(c, kind, id);
        if (index < 0 || (kind == kPerformers && id == fixed)) continue;
        chips.push_back({nameOf(c, kind, index), out == 1,
                         out ? excludeIn(m, kind, id) : includeIn(m, kind, id)});
      }
  }
  auto bits = [&](unsigned StashList::* mask, const char* const* labels, int count,
                  const char* pattern = nullptr) {
    for (int i = 0; i < count; ++i)
      if (l.*mask & (1u << i))
        chips.push_back({pattern ? stashPrinted(pattern, labels[i]) : labels[i], false,
                         toggleBit(mask, static_cast<unsigned>(i))});
  };
  bits(&StashList::women, t.womenCounts, 3);
  bits(&StashList::men, t.menCounts, 4);
  for (const StashLookKind look : kSceneLooks) {
    const int k = static_cast<int>(look);
    if (k < kStashNamedLooks) {
      for (int out = 0; out < 2; ++out)
        for (const std::string& name : out ? l.looks.out[k] : l.looks.in[k])
          chips.push_back({stashLookChip(k, name), out == 1, [k, out, name](StashList& list) {
                             out ? list.looks.exclude(k, name) : list.looks.include(k, name);
                           }});
      continue;
    }
    for (int b = 0; b < kStashLookBuckets[k - kStashNamedLooks]; ++b)
      if (l.looks.mask(look) >> b & 1)
        chips.push_back({stashLookChip(k, "", b), false,
                         [look, b](StashList& list) { list.looks.mask(look) ^= 1u << b; }});
  }
  for (int i = 0; i < 4; ++i)
    if (l.addedDays == kAddedDays[i])
      chips.push_back({t.addedSpans[i], false, [](StashList& list) { list.addedDays = 0; }});
  if (l.year)
    chips.push_back(
        {stashPrinted(t.releasedChip, l.year), false, [](StashList& list) { list.year = 0; }});
  bits(&StashList::resolutions, t.resolutions, 10);
  bits(&StashList::durations, t.durations, 4);
  bits(&StashList::frameRates, t.frameRates, 3);
  bits(&StashList::codecs, t.codecs, 3);
  bits(&StashList::sizes, t.fileSizes, 4);
  bits(&StashList::history, t.history, 5);
  if (!l.text.empty())
    chips.push_back({"“" + l.text + "”", false, [](StashList& list) { list.text.clear(); }});
  return chips;
}

}  // namespace

// The rail's lines: each section lists what occurs in the results, most
// common first, never a choice that leaves nothing (one in every result
// would narrow nothing, and leaving it out would leave nothing); what is
// chosen first, even where it counts nothing now.
void StashPages::buildRail(Menu& m) {
  StashMenu& st = state(m);
  const StashCatalog& c = *catalog(m);
  const StashResult& r = st.result;
  const StashStrings& t = strings().stash;
  const int total = static_cast<int>(r.scenes.size());
  std::vector<StashRailLine>& rail = st.rail;
  rail.clear();
  auto change = [&m](std::function<void(StashList&)> f) { return [&m, f] { changeList(m, f); }; };
  auto heading = [&](const char* label) {
    StashRailLine l;
    l.kind = StashRailLine::Heading;
    l.label = label;
    rail.push_back(std::move(l));
  };
  auto sub = [&](const std::string& label) {
    StashRailLine l;
    l.kind = StashRailLine::Sub;
    l.label = label;
    rail.push_back(std::move(l));
  };
  auto row = [&](const std::string& label, int count, int state, std::function<void()> include,
                 std::function<void()> exclude = nullptr) {
    StashRailLine l;
    l.label = label;
    l.count = count > 0 ? stashNumber(count) : "";
    l.state = state;
    l.include = std::move(include);
    l.exclude = std::move(exclude);
    rail.push_back(std::move(l));
  };
  auto gap = [&] {
    StashRailLine l;
    l.kind = StashRailLine::Gap;
    rail.push_back(std::move(l));
  };
  // A section's heading goes in only if it gets a row; take it out otherwise.
  auto section = [&](const char* label, const std::function<void()>& rows) {
    const size_t at = rail.size();
    heading(label);
    rows();
    if (rail.size() == at + 1)
      rail.pop_back();
    else
      gap();
  };

  // Performers, tags and studios: chosen first, then the most common. On a
  // performer's page, her scenes are always hers, with all of the others.
  const int fixed = c.performerById(fixedPerformer(m));
  auto people = [&](int kind) {
    if (!offered(m, kind)) return;
    section(t.sections[kind], [&, kind] {
      const StashPick& pick = pickOf(st.filter, kind);
      const std::vector<StashFacet>& facet = facetOf(r, kind);
      const int skip = kind == kPerformers ? fixed : -1;
      if (kind != kStudios && pick.include.size() >= 2 && skip < 0) {
        rail.back().mode = pick.all ? 1 : 0;
        rail.back().setAll = [&m, kind](bool all) {
          changeList(m, [kind, all](StashList& l) {
            (kind == kTags ? l.tagsAll : l.performersAll) = all;
          });
        };
      }
      for (const int index : pick.include)
        if (index != skip)
          row(nameOf(c, kind, index), countIn(facet, index), 1,
              change(includeIn(m, kind, idOf(c, kind, index))),
              change(excludeIn(m, kind, idOf(c, kind, index))));
      for (const int index : pick.exclude)
        row(nameOf(c, kind, index), 0, 2, change(includeIn(m, kind, idOf(c, kind, index))),
            change(excludeIn(m, kind, idOf(c, kind, index))));
      int shown = 0, more = 0;
      for (const StashFacet& f : facet) {
        if (std::find(pick.include.begin(), pick.include.end(), f.index) != pick.include.end() ||
            (inResults(pick) && f.count >= total) || f.index == skip)
          continue;
        if (shown == kRailTop[kind]) {
          ++more;
          break;
        }
        row(nameOf(c, kind, f.index), f.count, 0,
            change(includeIn(m, kind, idOf(c, kind, f.index))),
            change(excludeIn(m, kind, idOf(c, kind, f.index))));
        ++shown;
      }
      if (more) {
        StashRailLine l;
        l.kind = StashRailLine::Link;
        l.label = t.more[kind];
        const StashFull full = kind == kPerformers ? StashFull::Performers
                               : kind == kTags     ? StashFull::Tags
                                                   : StashFull::Studios;
        l.include = [&m, full] { showFull(m, full); };
        rail.push_back(std::move(l));
      }
    });
  };

  // Buckets: tapping one includes it (any of within its kind).
  auto bucket = [&](unsigned StashList::* mask, unsigned chosen, int i, const char* label,
                    int count) {
    const bool on = chosen & (1u << i);
    if (on || (count > 0 && count < total) || (count > 0 && chosen))
      row(label, count, on ? 1 : 0, change(toggleBit(mask, static_cast<unsigned>(i))));
  };
  // A section's subgroups in the order chosen (stash_filters), without
  // those hidden; one that gets no row goes out.
  auto subgroups = [&](int section, const std::function<void(int)>& rows) {
    const StashSettings& s = m.view_.stash;
    for (const int g : s.subgroups) {
      if (stashSectionOf(g) != section || s.hiddenSubgroups >> g & 1) continue;
      const size_t at = rail.size();
      rows(g);
      if (rail.size() == at + 1) rail.pop_back();
    }
  };
  auto cast = [&] { section(t.sections[3], [&] { subgroups(kSectionCast, [&](int g) {
    if (g == kSubWomen) {
      sub(t.women);
      for (int i = 0; i < 3; ++i)
        bucket(&StashList::women, st.list.women, i, t.womenCounts[i], r.women[i]);
      return;
    }
    sub(t.men);
    for (int i = 0; i < 4; ++i) bucket(&StashList::men, st.list.men, i, t.menCounts[i], r.men[i]);
  }); }); };
  auto looks = [&] { section(t.sections[4], [&] {
    std::vector<StashLookKind> kinds;
    subgroups(kSectionLooks, [&](int g) { kinds.push_back(kSceneLooks[g - kSubHair]); });
    lookRail(rail, c, st.list.looks, r.looks, total, kinds, false,
             [&m](std::function<void(StashLooks&)> f) {
               return [&m, f] { changeList(m, [f](StashList& l) { f(l.looks); }); };
             },
             [&m](StashLookKind look) { showFull(m, StashFull::Look, look); });
  }); };
  // A span or a year chosen: the others are counted without it, so each
  // stays to switch to.
  auto released = [&] { section(t.sections[5], [&] { subgroups(kSectionReleased, [&](int g) {
    if (g == kSubAdded) {
      sub(t.added);
      for (int i = 0; i < 4 && i < static_cast<int>(r.addedSince.size()); ++i) {
        const int days = kAddedDays[i], count = r.addedSince[i];
        const bool on = st.list.addedDays == days;
        if (on || (count > 0 && (count < total || st.list.addedDays)))
          row(t.addedSpans[i], count, on ? 1 : 0,
              change([days](StashList& l) { l.addedDays = l.addedDays == days ? 0 : days; }));
      }
      return;
    }
    sub(t.released);
    int shown = 0;
    for (const auto& [year, count] : r.years) {
      const bool on = st.list.year == year;
      if (!on && ((count >= total && !st.list.year) || shown >= kRailYears)) continue;
      row(std::to_string(year), count, on ? 1 : 0,
          change([year = year](StashList& l) { l.year = l.year == year ? 0 : year; }));
      shown += !on;
    }
    if (r.years.size() > static_cast<size_t>(kRailYears)) {
      StashRailLine l;
      l.kind = StashRailLine::Link;
      l.label = t.more[3];
      l.include = [&m] { showFull(m, StashFull::Years); };
      rail.push_back(std::move(l));
    }
  }); }); };
  auto format = [&] { section(t.sections[6], [&] { subgroups(kSectionFormat, [&](int g) {
    switch (g) {
      case kSubResolution:
        sub(t.resolution);
        for (const StashResolution res : kResolutionOrder) {
          const int i = static_cast<int>(res);
          bucket(&StashList::resolutions, st.list.resolutions, i,
                 (std::string(i >= static_cast<int>(StashResolution::Flat4K) ? t.flat : t.vr) +
                  " · " + t.resolutions[i])
                     .c_str(),
                 r.resolutions[i]);
        }
        break;
      case kSubDuration:
        sub(t.duration);
        for (int i = 0; i < 4; ++i)
          bucket(&StashList::durations, st.list.durations, i, t.durations[i], r.durations[i]);
        break;
      case kSubFrameRate:
        sub(t.frameRate);
        for (int i = 0; i < 3; ++i)
          bucket(&StashList::frameRates, st.list.frameRates, i, t.frameRates[i], r.frameRates[i]);
        break;
      case kSubCodec:
        sub(t.codec);
        for (int i = 0; i < 3; ++i)
          bucket(&StashList::codecs, st.list.codecs, i, t.codecs[i], r.codecs[i]);
        break;
      case kSubFileSize:
        sub(t.fileSize);
        for (int i = 0; i < 4; ++i)
          bucket(&StashList::sizes, st.list.sizes, i, t.fileSizes[i], r.sizes[i]);
        break;
    }
  }); }); };
  auto history = [&] { section(t.sections[7], [&] {
    for (int i = 0; i < 5; ++i)
      bucket(&StashList::history, st.list.history, i, t.history[i], r.history[i]);
  }); };

  // In the order chosen (stash_filters), without those hidden.
  const StashSettings& chosen = m.view_.stash;
  if (st.editSections) {
    editSectionsRail(m);
    return;
  }
  for (const int s : chosen.sections) {
    if (chosen.hiddenSections >> s & 1) continue;
    switch (s) {
      case kSectionPerformers: people(kPerformers); break;
      case kSectionTags: people(kTags); break;
      case kSectionStudios: people(kStudios); break;
      case kSectionCast: cast(); break;
      case kSectionLooks: looks(); break;
      case kSectionReleased: released(); break;
      case kSectionFormat: format(); break;
      case kSectionHistory: history(); break;
    }
  }
  if (rail.empty()) {
    StashRailLine l;
    l.kind = StashRailLine::Note;
    l.label = t.nothingToNarrow;
    rail.push_back(std::move(l));
  }
  StashRailLine edit;
  edit.kind = StashRailLine::Link;
  edit.label = t.editFilters;
  edit.include = [&m] { editSections(m, true); };
  rail.push_back(std::move(edit));

  st.railHeight = railHeights(rail);
}

void StashPages::editSections(Menu& m, bool on) {
  StashMenu& st = state(m);
  st.editSections = on;
  st.stale = true;
  m.side_.position = 0;
  m.dirty_ = true;
}

// The rail while editing it: each section with its switch and its moves,
// its subgroups under it with theirs (moving within it), then back to the
// defaults, and Done.
void StashPages::editSectionsRail(Menu& m) {
  StashMenu& st = state(m);
  const StashStrings& t = strings().stash;
  const StashSettings& s = m.view_.stash;
  std::vector<StashRailLine>& rail = st.rail;
  auto set = [](const StashSettings& changed) {
    return [text = stashSectionsText(changed)] {
      MenuCommand c;
      c.type = MenuCommand::SetStashSections;
      c.path = text;
      return c;
    };
  };
  // Two lines for a label too wide beside its switch and moves (as layoutRail
  // and the switch row lay them out).
  auto lines = [&m](const std::string& label, double indent) {
    const double w = kRailW - 14 - indent - 2 * (kRowBox + 6) - 88;
    return m.textWidth(label, Menu::kTextSize, false) > w ? 2 : 1;
  };
  StashRailLine heading;
  heading.kind = StashRailLine::Heading;
  heading.label = t.filtersShown;
  rail.push_back(std::move(heading));
  for (int i = 0; i < kStashSections; ++i) {
    const int section = s.sections[i];
    StashRailLine l;
    l.label = t.sections[section];
    l.lines = lines(l.label, 0);
    l.state = s.hiddenSections >> section & 1 ? 0 : 1;
    StashSettings changed = s;
    changed.hiddenSections ^= 1u << section;
    l.command = set(changed);
    for (const int to : {i - 1, i + 1}) {
      if (to < 0 || to >= kStashSections) continue;
      changed = s;
      std::swap(changed.sections[i], changed.sections[to]);
      (to < i ? l.up : l.down) = set(changed);
    }
    rail.push_back(std::move(l));
    for (int k = 0; k < kStashSubgroups; ++k) {
      const int g = s.subgroups[k];
      if (stashSectionOf(g) != section) continue;
      StashRailLine sub;
      sub.label = subgroupLabel(g);
      sub.lines = lines(sub.label, 12);
      sub.state = s.hiddenSubgroups >> g & 1 ? 0 : 1;
      sub.subgroup = true;
      sub.inactive = s.hiddenSections >> section & 1;
      changed = s;
      changed.hiddenSubgroups ^= 1u << g;
      sub.command = set(changed);
      for (const int to : {k - 1, k + 1}) {
        if (to < 0 || to >= kStashSubgroups || stashSectionOf(s.subgroups[to]) != section) continue;
        changed = s;
        std::swap(changed.subgroups[k], changed.subgroups[to]);
        (to < k ? sub.up : sub.down) = set(changed);
      }
      rail.push_back(std::move(sub));
    }
  }
  StashRailLine gap;
  gap.kind = StashRailLine::Gap;
  rail.push_back(std::move(gap));
  StashRailLine reset;
  reset.kind = StashRailLine::Link;
  reset.label = t.resetFilters;
  reset.command = set(StashSettings{});
  rail.push_back(std::move(reset));
  StashRailLine done;
  done.kind = StashRailLine::Link;
  done.label = t.doneFilters;
  done.include = [&m] { editSections(m, false); };
  rail.push_back(std::move(done));
  st.railHeight = railHeights(rail);
}

void StashPages::lookRail(std::vector<StashRailLine>& rail, const StashCatalog& c,
                          const StashLooks& looks,
                          const std::array<std::vector<StashFacet>, kStashLookKinds>& counts,
                          int total, const std::vector<StashLookKind>& kinds, bool headings,
                          const std::function<std::function<void()>(
                              std::function<void(StashLooks&)>)>& change,
                          const std::function<void(StashLookKind)>& more) {
  constexpr int kTop = 6;  // a named kind's values listed besides those chosen
  auto line = [&](StashRailLine::Kind kind, const std::string& label) {
    StashRailLine l;
    l.kind = kind;
    l.label = label;
    rail.push_back(std::move(l));
  };
  auto row = [&](const std::string& label, int count, int state, std::function<void()> include,
                 std::function<void()> exclude = nullptr) {
    StashRailLine l;
    l.label = label;
    l.count = count > 0 ? stashNumber(count) : "";
    l.state = state;
    l.include = std::move(include);
    l.exclude = std::move(exclude);
    rail.push_back(std::move(l));
  };
  for (const StashLookKind look : kinds) {
    const int k = static_cast<int>(look);
    const size_t at = rail.size();
    line(headings ? StashRailLine::Heading : StashRailLine::Sub,
         headings && look == StashLookKind::Age ? strings().stash.ageInScenes
                                                : stashLookHeading(k));
    const std::vector<StashFacet>& facet = counts[k];
    if (k < kStashNamedLooks) {
      const std::vector<std::string>& names = c.lookNames(look);
      auto countOf = [&](const std::string& name) {
        const auto found = std::find(names.begin(), names.end(), name);
        return found == names.end() ? 0 : countIn(facet, static_cast<int>(found - names.begin()));
      };
      auto rowFor = [&](const std::string& name, int count, int state) {
        row(stashLookRow(k, name), count, state,
            change([k, name](StashLooks& l) { l.include(k, name); }),
            change([k, name](StashLooks& l) { l.exclude(k, name); }));
      };
      for (const std::string& name : looks.in[k]) rowFor(name, countOf(name), 1);
      for (const std::string& name : looks.out[k]) rowFor(name, 0, 2);
      const bool chosen = !looks.in[k].empty() || !looks.out[k].empty();
      int shown = 0;
      for (const StashFacet& f : facet) {
        const std::string& name = names[f.index];
        if (std::find(looks.in[k].begin(), looks.in[k].end(), name) != looks.in[k].end() ||
            (!chosen && f.count >= total))
          continue;
        if (shown++ == kTop) {
          StashRailLine l;
          l.kind = StashRailLine::Link;
          l.label = strings().stash.moreLooks[k];
          l.include = [more, look] { more(look); };
          rail.push_back(std::move(l));
          break;
        }
        rowFor(name, f.count, 0);
      }
    } else {
      const unsigned mask = looks.mask(look);
      for (int b = 0; b < kStashLookBuckets[k - kStashNamedLooks]; ++b) {
        const bool on = mask >> b & 1;
        const int count = countIn(facet, b);
        if (on || (count > 0 && (count < total || mask)))
          row(stashLookRow(k, "", b), count, on ? 1 : 0,
              change([look, b](StashLooks& l) { l.mask(look) ^= 1u << b; }));
      }
    }
    if (rail.size() == at + 1)
      rail.pop_back();
    else if (headings)
      line(StashRailLine::Gap, "");
  }
}

double StashPages::railHeights(std::vector<StashRailLine>& rail) {
  double total = 0;
  for (StashRailLine& l : rail) {
    switch (l.kind) {
      case StashRailLine::Heading:
        l.height = 56;
        break;
      case StashRailLine::Sub:
        l.height = 36;
        break;
      case StashRailLine::Gap:
        l.height = 14;
        break;
      case StashRailLine::Note:
        l.height = 60;
        break;
      default:
        l.height = kRowStep + (l.lines - 1) * kLineGrow;
        break;
    }
    total += l.height;
  }
  return total;
}

// The full list's rows: what is chosen first, then everything in the
// results (matching its search), most first or A–Z.
void StashPages::buildFull(Menu& m) {
  StashMenu& st = state(m);
  st.fullStale = false;
  st.fullRows.clear();
  const StashCatalog* c = catalog(m);
  if (!c) return;
  const StashResult& r = st.result;
  const int total = static_cast<int>(r.scenes.size());
  const std::string search = stashFold(st.fullSearch);
  auto change = [&m](std::function<void(StashList&)> f) { return [&m, f] { changeList(m, f); }; };
  if (st.full == StashFull::Pins) {
    const std::vector<StashHistory>* history =
        st.history.size() == c->scenes().size() ? &st.history : nullptr;
    for (size_t i = 0; i < st.pins.size(); ++i) {
      const StashResult pin =
          c->query(stashFilterOf(st.pins[i].list, *c, m.view_.stash.malePerformers, unixNow()), {},
                   history, false);
      StashMenu::FullRow row;
      row.label = st.pins[i].name;
      row.count = stashNumber(static_cast<long>(pin.scenes.size()));
      row.include = [&m, i] { openPin(m, static_cast<int>(i)); };
      st.fullRows.push_back(std::move(row));
    }
    return;
  }
  if (st.full == StashFull::Years) {
    for (const auto& [year, count] : r.years) {
      const bool on = st.list.year == year;
      if (!on && count >= total && !st.list.year) continue;
      const std::string label = std::to_string(year);
      if (!search.empty() && label.find(search) == std::string::npos) continue;
      st.fullRows.push_back(
          {label, stashNumber(count), on ? 1 : 0,
           change([year = year](StashList& l) { l.year = l.year == year ? 0 : year; }), nullptr});
    }
    // Newest first as the catalog has them; fullByName: oldest first.
    if (st.fullByName) std::reverse(st.fullRows.begin(), st.fullRows.end());
    return;
  }
  // A–Z: what is chosen, then the rest by their collation keys.
  size_t chosen = 0;
  std::vector<std::pair<std::string, size_t>> keys;
  auto sortByName = [&] {
    if (!st.fullByName) return;
    std::stable_sort(keys.begin(), keys.end());
    std::vector<StashMenu::FullRow> sorted(st.fullRows.begin(), st.fullRows.begin() + chosen);
    for (const auto& k : keys) sorted.push_back(std::move(st.fullRows[k.second]));
    st.fullRows = std::move(sorted);
  };
  if (st.full == StashFull::Look) {
    // As the rail lists them: the Performers page's counted in performers,
    // the scenes' otherwise.
    const int k = static_cast<int>(st.fullLook);
    const bool cards = m.page_ == Menu::Page::StashPerformers;
    const StashLooks& looks = cards ? st.performers.choice.looks : st.list.looks;
    const std::vector<StashFacet>& facet = (cards ? st.performers.looks : r.looks)[k];
    const int all = cards ? static_cast<int>(st.performers.shown.size()) : total;
    auto changeLooks = [&m, cards](std::function<void(StashLooks&)> f) -> std::function<void()> {
      if (cards) return changePerformers(m, [f](StashPerformerChoice& s) { f(s.looks); });
      return [&m, f] { changeList(m, [f](StashList& l) { f(l.looks); }); };
    };
    const std::vector<std::string>& names = c->lookNames(st.fullLook);
    auto add = [&](const std::string& name, int count, int state) {
      if (!state && st.fullByName) keys.emplace_back(stashCollateKey(name), st.fullRows.size());
      st.fullRows.push_back({stashLookRow(k, name), count > 0 ? stashNumber(count) : "", state,
                             changeLooks([k, name](StashLooks& l) { l.include(k, name); }),
                             changeLooks([k, name](StashLooks& l) { l.exclude(k, name); })});
    };
    for (const std::string& name : looks.in[k]) {
      const auto found = std::find(names.begin(), names.end(), name);
      const int count =
          found == names.end() ? 0 : countIn(facet, static_cast<int>(found - names.begin()));
      add(name, count, 1);
    }
    for (const std::string& name : looks.out[k]) add(name, 0, 2);
    chosen = st.fullRows.size();
    const bool any = !looks.in[k].empty() || !looks.out[k].empty();
    for (const StashFacet& f : facet) {
      const std::string& name = names[f.index];
      if (std::find(looks.in[k].begin(), looks.in[k].end(), name) != looks.in[k].end() ||
          (!any && f.count >= all) ||
          (!search.empty() && stashFold(name).find(search) == std::string::npos))
        continue;
      add(name, f.count, 0);
    }
    sortByName();
    return;
  }
  const int kind = st.full == StashFull::Performers ? kPerformers
                   : st.full == StashFull::Tags     ? kTags
                                                    : kStudios;
  if (!offered(m, kind)) return;
  const StashPick& pick = pickOf(st.filter, kind);
  const std::vector<StashFacet>& facet = facetOf(r, kind);
  const int skip = kind == kPerformers ? c->performerById(fixedPerformer(m)) : -1;
  auto matches = [&](int index) {
    if (search.empty()) return true;
    if (stashFold(nameOf(*c, kind, index)).find(search) != std::string::npos) return true;
    if (kind == kTags)
      for (const std::string& name : c->tags()[index].names)
        if (stashFold(name).find(search) != std::string::npos) return true;
    return false;
  };
  for (const int index : pick.include)
    if (index != skip)
      st.fullRows.push_back({nameOf(*c, kind, index), stashNumber(countIn(facet, index)), 1,
                             change(includeIn(m, kind, idOf(*c, kind, index))),
                             change(excludeIn(m, kind, idOf(*c, kind, index)))});
  for (const int index : pick.exclude)
    st.fullRows.push_back({nameOf(*c, kind, index), "", 2,
                           change(includeIn(m, kind, idOf(*c, kind, index))),
                           change(excludeIn(m, kind, idOf(*c, kind, index)))});
  chosen = st.fullRows.size();
  for (const StashFacet& f : facet) {
    if (std::find(pick.include.begin(), pick.include.end(), f.index) != pick.include.end() ||
        (inResults(pick) && f.count >= total) || f.index == skip || !matches(f.index))
      continue;
    std::string label = nameOf(*c, kind, f.index);
    if (kind == kTags && c->tags()[f.index].names.size() > 1)  // merged: all its names
      for (size_t i = 1; i < c->tags()[f.index].names.size(); ++i)
        label += " · " + c->tags()[f.index].names[i];
    if (st.fullByName)
      keys.emplace_back(stashCollateKey(nameOf(*c, kind, f.index)), st.fullRows.size());
    st.fullRows.push_back({label, stashNumber(f.count), 0,
                           change(includeIn(m, kind, idOf(*c, kind, f.index))),
                           change(excludeIn(m, kind, idOf(*c, kind, f.index)))});
  }
  sortByName();
}

void StashPages::showScenes(Menu& m) {
  StashMenu& st = state(m);
  m.showPage(Menu::Page::StashScenes);
  st.full = StashFull::None;
  m.scrollOffset_ = static_cast<int>(std::floor(st.scroll));
  m.scrollFraction_ = st.scroll - m.scrollOffset_;
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
    st.scroll = m.scrollOffset_ + m.scrollFraction_;
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
  return m.listPlace(resultPaths(m), st.gallery ? gridColumns(st.resultsX, m.listRight_) : 1);
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
  m.scrollOffset_ = 0;
  m.scrollFraction_ = 0;
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
  m.scrollOffset_ = static_cast<int>(std::floor(scroll));
  m.scrollFraction_ = scroll - m.scrollOffset_;
  m.dirty_ = true;
  return kept;
}

void StashPages::leaveList(Menu& m, std::optional<StashSavedList>& scenes,
                           std::optional<StashSavedList>& own, int64_t id) {
  StashMenu& st = state(m);
  if (!scenes) return;
  const double scroll =
      st.full == StashFull::None ? m.scrollOffset_ + m.scrollFraction_ : st.scroll;
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
    (cards ? st.performers.scroll : st.scroll) = m.scrollOffset_ + m.scrollFraction_;
    if (!cards && !st.placePending) st.place = resultsPlace(m);  // else as left, not laid out yet
  }
  st.full = full;
  st.fullLook = look;
  st.fullSearch.clear();
  st.fullStale = true;
  m.editing_ = false;
  m.scrollOffset_ = 0;
  m.scrollFraction_ = 0;
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
                                  listed ? m.scrollOffset_ + m.scrollFraction_ : st.scroll,
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
  m.scrollOffset_ = 0;
  m.scrollFraction_ = 0;
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
  m.scrollOffset_ = static_cast<int>(std::floor(own.scroll));
  m.scrollFraction_ = own.scroll - m.scrollOffset_;
  m.side_.position = own.rail;
  m.dirty_ = true;
}

// Plays a scene's file, with the results as the list Previous and Next go
// through (or all of them in a random order).
MenuCommand StashPages::open(Menu& m, const std::string& path, bool shuffled) {
  refresh(m);  // the results of this catalog, should a new one have come since the layout
  return play(m, state(m).result.scenes, path, shuffled);
}

MenuCommand StashPages::play(Menu& m, const std::vector<int>& scenes, const std::string& path,
                             bool shuffled) {
  StashMenu& st = state(m);
  const StashCatalog* c = catalog(m);
  if (!c) return {};
  m.playList_.clear();
  for (const int i : scenes) m.playList_.push_back(stashSceneLocation(c->scenes()[i], m.offline_));
  if (shuffled) {
    std::mt19937_64 random(static_cast<uint64_t>(unixNow()) ^ st.seed++);
    std::shuffle(m.playList_.begin(), m.playList_.end(), random);
  }
  if (m.playList_.empty()) return {};
  m.playAt_ = shuffled ? m.playList_.front() : path;
  return m.openVideo(m.playAt_);
}

bool Menu::listNeighbor(const std::string& path, int step, std::string& next) {
  next.clear();
  if (playList_.empty() || path != playAt_) {  // since opened from elsewhere
    playList_.clear();
    playAt_.clear();
    return false;
  }
  const auto at = std::find(playList_.begin(), playList_.end(), path);
  const long i = (at - playList_.begin()) + step;
  if (at == playList_.end() || i < 0 || i >= static_cast<long>(playList_.size())) return true;
  next = playAt_ = playList_[i];
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
  b.on = state == 1;
  b.danger = false;
  b.dim = state == 2;  // left out
  b.bare = true;
  b.radius = 12;
  b.stashLook = static_cast<int>(StashLook::Facet);
  b.action = [include] {
    include();
    return MenuCommand{};
  };
  if (m.textWidth(label, Menu::kTextSize, false) > w - 40 - m.textWidth(count, 22, false))
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
  b.radius = 12;
  b.stashLook = static_cast<int>(StashLook::Exclude);
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
  Button b;
  b.panel = Menu::kBrowser;
  b.x = x;
  b.y = y;
  b.h = h;
  b.label = label;
  b.w = m.textWidth(label, Menu::kTextSize, false) + (icon != StashIcon::None ? 92 : 50);
  b.fontSize = Menu::kTextSize;
  b.stashLook = static_cast<int>(StashLook::Action);
  b.stashIcon = static_cast<int>(icon);
  b.action = std::move(action);
  return b;
}

// The heading row from x: Filters (the rail on or off, with how many are
// active), the title, and at the right Shuffle play and pinning.
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
  filters.selected = st.railShown;
  filters.tooltip = t.tipFilters;
  const StashCatalog* c = catalog(m);
  const size_t active = c ? chipsOf(m, st.list, *c).size() : 0;
  if (active && !st.railShown) filters.badge = std::to_string(active);
  m.buttons_.push_back(filters);
  st.titleX = filters.x + filters.w + 24;

  double right = Menu::kPageX + Menu::kPageWidth - kPagePad;
  auto add = [&](Button b) {
    right -= b.w;
    b.x = right;
    right -= kGap;
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
          saveStashPins(s.pins);
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
          saveStashPins(s.pins);
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
  st.fullTitleRight = right;
}

// The tool row: list or tiles, All / VR / 2D, the order, the search and to
// the top / end, as the file browser's.
void StashPages::layoutTools(Menu& m) {
  StashMenu& st = state(m);
  const StashStrings& t = strings().stash;
  const Strings& base = strings();
  const double y = kBrowserToolsY + bandHeight(m), h = kBrowserToolsH;
  double x = Menu::kPageX + kPagePad;
  auto icon = [&](Menu::Icon i, const char* tip, bool on, std::function<MenuCommand()> action) {
    Button& b = m.addRound(i, tip, std::move(action), x, y, h);
    b.panel = Menu::kBrowser;
    b.bare = true;
    b.on = on;
    x += h + 8;
  };
  auto view = [&m](bool gallery) {
    return [&m, gallery] {
      StashMenu& s = state(m);
      if (s.gallery == gallery) return MenuCommand{};
      s.gallery = gallery;
      m.scrollOffset_ = 0;
      m.scrollFraction_ = 0;
      return MenuCommand{};
    };
  };
  icon(Menu::Icon::List, base.tipList, !st.gallery, view(false));
  icon(Menu::Icon::Grid, base.tipGallery, st.gallery, view(true));
  x += 8;
  // All / VR / 2D with their counts, the chosen one blue; kept in the settings.
  for (int f = 0; f < 3; ++f) {
    std::string label = t.formats[f];
    if (catalog(m)) label += "  " + stashNumber(st.result.formats[f]);
    Button b = m.textButton(
        label, Menu::Icon::None,
        [&m, f] {
          StashMenu& s = state(m);
          s.list.format = static_cast<StashFormat>(f);
          s.stale = true;
          if (s.full == StashFull::None) {
            m.scrollOffset_ = 0;
            m.scrollFraction_ = 0;
          }
          MenuCommand c;
          c.type = MenuCommand::SetStashFormat;
          c.value = f;
          return c;
        },
        static_cast<int>(st.list.format) == f);
    // Never a choice that shows nothing.
    b.disabled = catalog(m) && st.result.formats[f] == 0 && static_cast<int>(st.list.format) != f;
    b.panel = Menu::kBrowser;
    b.x = x;
    b.y = y;
    b.h = h;
    b.w = m.textWidth(label, Menu::kTextSize, false) + 36;
    b.radius = h / 2;
    m.buttons_.push_back(b);
    x += b.w + 6;
  }
  x += 16;
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

// "Sort by  Release date ▾  ▼" on the tool row from x, and its fields under
// it while open, in `order` (null: as listed); `descending` -1 offers no
// direction. Either starts the list at the top. Returns where the next part goes.
double StashPages::layoutSort(Menu& m, double x, const char* const* fields, int count, int chosen,
                              int descending, std::function<MenuCommand(int)> choose,
                              std::function<MenuCommand()> turn, const int* order) {
  const StashStrings& t = strings().stash;
  const Strings& base = strings();
  const double y = kBrowserToolsY + bandHeight(m), h = kBrowserToolsH;
  Button& label =
      m.addLabel(x, y, m.textWidth(base.sortBy, 26, false) + 8, h, base.sortBy, 26, true);
  x += label.w + 8;
  Button value = m.textButton(std::string(fields[chosen]) + " ▾", Menu::Icon::None, [&m] {
    m.dropdown_ = m.dropdown_ == Menu::Dropdown::Sort ? Menu::Dropdown::None : Menu::Dropdown::Sort;
    return MenuCommand{};
  });
  value.panel = Menu::kBrowser;
  value.bare = true;
  value.on = true;
  value.x = x;
  value.y = y;
  value.h = h;
  value.fontSize = 26;
  value.w = m.textWidth(value.label, 26, false) + 40;
  m.buttons_.push_back(value);
  x += value.w + 4;
  if (descending >= 0) {
    Button direction = m.textButton(descending ? "▼" : "▲", Menu::Icon::None, [&m, turn] {
      m.scrollOffset_ = 0;
      m.scrollFraction_ = 0;
      return turn();
    });
    direction.panel = Menu::kBrowser;
    direction.bare = true;
    direction.on = true;
    direction.x = x;
    direction.y = y;
    direction.w = direction.h = h;
    direction.radius = h / 2;
    direction.fontSize = 26;
    direction.tooltip = t.tipTurnOrder;
    m.buttons_.push_back(direction);
  }
  x += h + 16;

  if (m.dropdown_ == Menu::Dropdown::Sort) {  // the fields under it, the chosen one blue
    std::vector<Button> entries;
    for (int i = 0; i < count; ++i) {
      const int f = order ? order[i] : i;
      entries.push_back(m.textButton(
          fields[f], Menu::Icon::None,
          [&m, f, choose] {
            m.dropdown_ = Menu::Dropdown::None;
            m.scrollOffset_ = 0;
            m.scrollFraction_ = 0;
            return choose(f);
          },
          f == chosen));
    }
    m.addDropdownBelow(value, std::move(entries));
  }
  return x;
}

// To the top / end at the tool row's right end; returns where they start.
double StashPages::layoutJumps(Menu& m) {
  const Strings& base = strings();
  const double y = kBrowserToolsY + bandHeight(m), h = kBrowserToolsH;
  double right = Menu::kPageX + Menu::kPageWidth - kPagePad - h;
  Button& bottom = m.addRound(
      Menu::Icon::JumpBottom, base.tipBottom,
      [&m] {
        m.scrollOffset_ = 1 << 30;  // clamped to the last page by the layout
        return MenuCommand{};
      },
      right, y, h);
  bottom.panel = Menu::kBrowser;
  bottom.bare = true;
  right -= h + 8;
  Button& top = m.addRound(
      Menu::Icon::JumpTop, base.tipTop,
      [&m] {
        m.scrollOffset_ = 0;
        m.scrollFraction_ = 0;
        return MenuCommand{};
      },
      right, y, h);
  top.panel = Menu::kBrowser;
  top.bare = true;
  return right - 16;
}

// A search on the tool row from x to right, found as you type with the
// keyboard: `typing` says what it types into.
void StashPages::layoutSearch(Menu& m, double x, double right, const std::string& text,
                              const char* hint, StashMenu::Typing typing) {
  const Strings& base = strings();
  const double y = kBrowserToolsY + bandHeight(m), h = kBrowserToolsH;
  const bool editing = m.editing_ && state(m).typing == typing;
  Button search = m.textButton(
      text.empty() && !editing ? std::string(hint) : text,
      Menu::Icon::Search, [&m, typing] {
        state(m).typing = typing;
        m.editing_ = true;
        return MenuCommand{};
      });
  search.panel = Menu::kBrowser;
  search.x = x;
  search.y = y;
  search.w = right - x;
  search.h = h;
  search.fontSize = Menu::kTextSize;
  search.field = true;
  search.on = editing;
  search.caret = editing ? m.caret() : -1;
  search.placeholder = text.empty() && !editing;
  search.tooltip = hint;
  search.radius = 12;
  m.buttons_.push_back(search);
  if (!text.empty()) {
    Button clear = m.textButton(base.clear, Menu::Icon::None, [&m, typing] {
      state(m).typing = typing;
      setText(m, "");
      return MenuCommand{};
    });
    clear.panel = Menu::kBrowser;
    clear.bare = true;
    clear.on = true;
    clear.fontSize = 26;
    clear.w = m.textWidth(base.clear, 26, false) + 40;
    clear.x = right - clear.w - 6;
    clear.y = y + 4;
    clear.h = h - 8;
    clear.radius = clear.h / 2;
    m.buttons_.push_back(clear);
  }
}

// The active filters, included blue and left out red with ⊘: tapping one
// takes it away; Undo brings back what the last change took, Clear all
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
    undo.radius = h / 2;
    m.buttons_.push_back(undo);
    x += undo.w + 12;
  }
  const std::vector<Chip> chips = c ? chipsOf(m, st.list, *c) : std::vector<Chip>{};
  if (chips.empty()) {
    m.addLabel(x + 8, y, right - x - 8, h, t.noFilters, 24, true);
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
  clear.radius = h / 2;
  clear.x = right - clear.w;
  m.buttons_.push_back(clear);
  const double end = clear.x - 12;
  for (size_t i = 0; i < chips.size(); ++i) {
    const Chip& chip = chips[i];
    const double w = std::min(420.0, m.textWidth(chip.label, 24, false) + (chip.out ? 72 : 40));
    const std::string more = stashPrinted(t.moreChips, static_cast<int>(chips.size() - i));
    const double moreW = i + 1 < chips.size() ? m.textWidth(more, 24, false) + 16 : 0;
    if (x + w + moreW > end) {
      m.addLabel(x + 4, y, end - x, h, more, 24, true);
      break;
    }
    Button b;
    b.panel = Menu::kBrowser;
    b.x = x;
    b.y = y;
    b.w = w;
    b.h = h;
    b.label = chip.label;
    b.dim = chip.out;
    b.bare = true;
    b.radius = h / 2;
    b.stashLook = static_cast<int>(StashLook::Chip);
    const auto remove = chip.remove;
    b.action = [&m, remove] {
      changeList(m, remove);
      return MenuCommand{};
    };
    b.tooltip = stashPrinted(t.tipRemoveFilter, chip.label);
    m.buttons_.push_back(b);
    x += w + 10;
  }
  return y + h + kResultsGap;
}

// The rail's lines that show, in the side column (which scrolls on its own).
void StashPages::layoutRail(Menu& m, const std::vector<StashRailLine>& rail, double height,
                            double top, double bottom) {
  const StashStrings& t = strings().stash;
  const double x = Menu::kPageX + kPagePad;
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
        Button& b = m.addLabel(x + 4, y0 + 4, rowW, 48, l.label, 27, false);
        b.side = true;
        b.staticText = false;  // drawn as a list heading: bold, dimmer
        b.label = l.label;
        if (l.mode >= 0) {
          const char* names[] = {t.anyOf, t.allOf};
          double right = x + rowW;
          for (int all = 0; all < 2; ++all) {
            const auto setAll = l.setAll;
            Button mode = m.textButton(
                names[all], Menu::Icon::None,
                [setAll, all] {
                  setAll(all == 1);
                  return MenuCommand{};
                },
                l.mode == all);
            mode.panel = Menu::kBrowser;
            mode.w = m.textWidth(names[all], 24, false) + 32;
            mode.h = 48;
            mode.x = right - mode.w;
            mode.y = y0 + 4;
            mode.fontSize = 24;
            mode.radius = 24;
            mode.side = true;
            mode.tooltip = t.tipMatch[all];
            right = mode.x - 6;
            m.buttons_.push_back(mode);
          }
        }
        break;
      }
      case StashRailLine::Sub:
        m.addLabel(x + 12, y0 + 4, rowW - 12, l.height - 4, l.label, kHintSize, true).side = true;
        break;
      case StashRailLine::Note:
        m.addLabel(x + 12, y0 + 6, rowW - 12, 0, l.label, kHintSize, true, 2).side = true;
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
        b.radius = 12;
        b.side = true;
        b.stashLook = static_cast<int>(StashLook::Link);
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
          const double moveW = kRowBox;
          const double indent = l.subgroup ? 12 : 0;  // as the rail's Sub labels
          const double w = rowW - indent - 2 * (moveW + 6);
          const double grow = (l.lines - 1) * kLineGrow;
          Button& row = m.addToggleRow(x + indent, y0 + (kRowBox - kSettingRowH) / 2, w, l.label,
                                       l.state == 1, l.command, l.inactive);
          row.side = true;
          row.h += grow;
          row.lines = l.lines;
          double mx = x + indent + w + 6;
          for (const auto& [move, icon, tip] :
               {std::tuple{l.up, Menu::Icon::MoveUp, t.tipMoveUp},
                std::tuple{l.down, Menu::Icon::MoveDown, t.tipMoveDown}}) {
            if (move) {
              Button& b = m.addRound(icon, tip, move, mx, y0 + grow / 2, moveW);
              b.panel = Menu::kBrowser;
              b.bare = true;
              b.side = true;
              b.disabled = l.inactive;
            }
            mx += moveW + 6;
          }
          break;
        }
        const double w = l.exclude ? rowW - kExcludeW - 6 : rowW;
        Button b = facetButton(m, l.label, l.count, l.state, l.include, x, y0, w, kRowBox);
        if (b.tooltip.empty()) b.tooltip = t.tipFilter;
        b.side = true;
        m.buttons_.push_back(b);
        if (l.exclude) {
          Button ex = excludeButton(m, l.state, l.exclude, x + w + 6, y0, kExcludeW, kRowBox);
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

StashPages::Button StashPages::sceneTile(const Menu& m, const StashCatalog& c, const StashScene& s,
                                         int plays, double x, double y, double w, double h) {
  Button b;
  b.panel = Menu::kBrowser;
  b.x = x;
  b.y = y;
  b.w = w;
  b.h = h;
  b.label = title(m, s);
  b.detail = performers(m, s);
  b.date = studioDate(c, s);
  b.caption = badge(s);
  const std::string location = stashSceneLocation(s, m.offline_);
  b.selected = location == m.view_.currentFile;
  b.card = true;
  b.stashLook = static_cast<int>(StashLook::Tile);
  b.offlinePath = location;
  if (m.view_.thumbnails || m.stashCoversShown())
    b.thumbPath = location;
  else
    b.icon = Menu::Icon::Video;
  if (plays > 0) b.plays = std::to_string(plays);
  if (m.textWidth(b.label, 26, false) > b.w - 28) b.tooltip = b.label;
  return b;
}

StashPages::Button StashPages::tileStars(const Menu& m, Button& tile, const std::string& path,
                                         float rating) {
  tile.rating = rating;
  return m.cardRating(path, rating, tile);
}

// Tiles from `top` down, about the gallery's size: three columns beside the
// rail, four without it (fewer beside a performer's profile).
void StashPages::layoutGrid(Menu& m, double x0, double top) {
  StashMenu& st = state(m);
  const StashCatalog& c = *catalog(m);
  const int columns = gridColumns(x0, m.listRight_);
  const double cardW = (m.listRight_ - 30 - x0 - (columns - 1) * kTileGap) / columns;
  const double thumbH = (cardW - 16) * 9 / 16;
  const double rowH = std::round(8 + thumbH + 6 + 34 + 30 + 30 + 10 + kTileGap);
  const std::vector<int>& scenes = st.result.scenes;
  const int count = static_cast<int>(scenes.size());
  m.listTop_ = top;
  m.rowHeight_ = rowH;
  m.visibleRows_ = std::max(1, static_cast<int>((m.listBottom_ - top) / rowH));
  m.listCount_ = (count + columns - 1) / columns;
  int firstRow, endRow;
  m.clampScroll(m.listCount_, m.visibleRows_, firstRow, endRow);
  const int first = firstRow * columns, end = std::min(count, endRow * columns);
  const bool history = st.history.size() == c.scenes().size();
  for (int i = first; i < end; ++i) {
    const int cell = i - first, index = scenes[i];
    const StashScene& s = c.scenes()[index];
    Button b = sceneTile(m, c, s, history ? st.history[index].plays : 0,
                         x0 + (cell % columns) * (cardW + kTileGap),
                         top + (cell / columns - m.scrollFraction_) * rowH, cardW, rowH - kTileGap);
    b.scrolls = true;
    b.action = [&m, path = b.offlinePath] { return open(m, path, false); };
    Button stars = tileStars(m, b, b.offlinePath, history ? st.history[index].rating : 0);
    stars.scrolls = true;
    m.buttons_.push_back(b);
    m.addOfflineButtons(b, 0, tileLastLine(b));
    m.buttons_.push_back(stars);
  }
  wantThumbnails(m, first, end, m.visibleRows_ * columns, cardW - 16);
  m.addScrollbar();
}

namespace {

constexpr double kStudioW = 230, kReleasedW = 130, kLengthW = 90;

}  // namespace

// Rating, plays, studio, release date and length.
StashPages::SceneColumns StashPages::sceneColumns(double x0, double x1) {
  SceneColumns c;
  c.length = x1 - 16 - kLengthW;
  c.released = c.length - 20 - kReleasedW;
  c.studio = x1 - x0 < 1100 ? c.released : c.released - 20 - kStudioW;
  c.plays = c.studio - 16 - kPlaysColumnW;
  c.rating = c.plays - 16 - kRatingColumnW;
  c.status = c.rating - 8 - kStatusColumnW;
  return c;
}

// Rows from under their column headings down (the headings sort).
void StashPages::layoutList(Menu& m, double x0, double top) {
  StashMenu& st = state(m);
  const StashStrings& t = strings().stash;
  const StashCatalog& c = *catalog(m);
  const double rowW = m.listRight_ - x0 - 30;
  const SceneColumns cols = sceneColumns(x0, x0 + rowW);
  auto heading = [&](int column, double x, double w, bool rightAligned, int sort) {
    const bool on = sort >= 0 && static_cast<int>(st.list.sort) == sort;
    std::string label = t.columns[column];
    if (on) label += st.list.descending ? "  ▼" : "  ▲";
    Button b = m.textButton(label, Menu::Icon::None, [&m, sort] {
      StashMenu& s = state(m);
      const StashSort by = static_cast<StashSort>(sort);
      if (s.list.sort == by)
        s.list.descending = !s.list.descending;
      else
        s.list.descending = by != StashSort::Title;
      s.list.sort = by;
      s.stale = true;
      m.scrollOffset_ = 0;
      m.scrollFraction_ = 0;
      return MenuCommand{};
    });
    if (sort < 0) b.action = nullptr;  // a heading only
    b.panel = Menu::kBrowser;
    b.bare = true;
    b.on = on;
    b.fontSize = 24;
    b.w = m.textWidth(label, 24, false) + 24;
    b.x = rightAligned ? x + w - b.w + 12 : x - 12;
    b.y = top;
    b.h = kListHeaderH;
    b.radius = 10;
    b.tooltip = strings().tipSortColumn;
    if (sort >= 0)
      m.buttons_.push_back(b);
    else
      m.addLabel(b.x + 12, b.y, b.w, b.h, label, 24, true);
  };
  Button probe;
  probe.h = kFileRowH - kRowGap;
  heading(0, x0 + 10 + m.listThumbW(probe) + 16, 400, false, static_cast<int>(StashSort::Title));
  if (m.offline_) {  // the status column's: the Offline place
    Button& place = m.addRound(Menu::Icon::Download, strings().offline.nav,
                               [&m] {
                                 m.showOffline();
                                 return MenuCommand{};
                               },
                               cols.status, top + 2, kStatusColumnW);
    place.panel = Menu::kBrowser;
    place.bare = true;
  }
  heading(1, cols.rating + 6, kRatingColumnW, false,
          static_cast<int>(StashSort::Rating));
  heading(2, cols.plays, kPlaysColumnW, true, static_cast<int>(StashSort::Plays));
  if (cols.studio < cols.released) heading(3, cols.studio, kStudioW, false, -1);
  heading(4, cols.released, kReleasedW, false, static_cast<int>(StashSort::Released));
  heading(5, cols.length, kLengthW, true, static_cast<int>(StashSort::Duration));

  const std::vector<int>& scenes = st.result.scenes;
  m.listTop_ = top + kListHeaderH + 6;
  m.rowHeight_ = kFileRowH;
  m.visibleRows_ = std::max(1, static_cast<int>((m.listBottom_ - m.listTop_) / kFileRowH));
  m.listCount_ = static_cast<int>(scenes.size());
  int first, end;
  m.clampScroll(m.listCount_, m.visibleRows_, first, end);
  const bool history = st.history.size() == c.scenes().size();
  for (int i = first; i < end; ++i) {
    const int index = scenes[i];
    const StashScene& s = c.scenes()[index];
    Button b;
    b.panel = Menu::kBrowser;
    b.x = x0;
    b.y = m.listTop_ + (i - m.scrollOffset_ - m.scrollFraction_) * kFileRowH;
    b.w = rowW;
    b.h = kFileRowH - kRowGap;
    b.label = title(m, s);
    b.detail = performers(m, s);
    b.size = s.studio >= 0 ? c.studios()[s.studio].name : "";
    b.date = s.date;
    b.caption = s.duration > 0 ? formatTime(s.duration) : "";
    const std::string location = stashSceneLocation(s, m.offline_);
    b.selected = location == m.view_.currentFile;
    b.listRow = true;
    b.scrolls = true;
    b.fontSize = Menu::kTextSize;
    b.stashLook = static_cast<int>(StashLook::Row);
    if (m.view_.thumbnails || m.stashCoversShown())
      b.thumbPath = location;
    else
      b.icon = Menu::Icon::Video;
    if (history && st.history[index].plays > 0) b.plays = std::to_string(st.history[index].plays);
    b.action = [&m, location] { return open(m, location, false); };
    b.tooltip = b.label + (b.detail.empty() ? "" : "  ·  " + b.detail);
    b.offlinePath = location;
    m.buttons_.push_back(b);
    m.addOfflineButtons(b, cols.status, 0);
    Button stars = m.ratingButton(location, history ? st.history[index].rating : 0, Menu::kBrowser,
                                  cols.rating, b.y + 2, kRowStarW, b.h - 4);
    stars.scrolls = true;
    m.buttons_.push_back(stars);
  }
  wantThumbnails(m, first, end, m.visibleRows_, m.listThumbW(probe));
  m.addScrollbar();
}

// A full list in place of the results: Back, its order and search, a row of
// letters while A–Z, then its rows in two columns.
void StashPages::layoutFull(Menu& m, double x0, double top) {
  StashMenu& st = state(m);
  const StashStrings& t = strings().stash;
  if (st.fullStale) buildFull(m);
  const double right = m.listRight_ - 6;  // the page's, or a performer's profile's left
  const double h = 56;
  const bool cards = m.page_ == Menu::Page::StashPerformers;  // in place of its cards
  Button back = actionButton(
      m, cards ? t.backToPerformers : t.back, StashIcon::Back,
      [&m, cards] {
        StashMenu& s = state(m);
        s.full = StashFull::None;
        m.editing_ = false;
        const double scroll = cards ? s.performers.scroll : s.scroll;
        m.scrollOffset_ = static_cast<int>(std::floor(scroll));
        m.scrollFraction_ = scroll - m.scrollOffset_;
        return MenuCommand{};
      },
      x0, top, h);
  m.buttons_.push_back(back);
  st.fullTitleX = back.x + back.w + 20;
  double x = right;
  const bool searchable = st.full != StashFull::Pins;
  if (searchable) {
    const int kind = st.full == StashFull::Performers ? 0
                     : st.full == StashFull::Tags     ? 1
                     : st.full == StashFull::Studios  ? 2
                                                      : 3;
    const char* hint = st.full == StashFull::Look ? t.findLooks[static_cast<int>(st.fullLook)]
                                                  : t.find[kind];
    const bool editing = m.editing_ && st.typing == StashMenu::Typing::FullSearch;
    const std::string& text = st.fullSearch;
    Button search = m.textButton(
        text.empty() && !editing ? std::string(hint) : text,
        Menu::Icon::Search, [&m] {
          StashMenu& s = state(m);
          s.typing = StashMenu::Typing::FullSearch;
          m.editing_ = true;
          return MenuCommand{};
        });
    search.panel = Menu::kBrowser;
    search.w = 420;
    search.x = right - search.w;
    search.y = top;
    search.h = h;
    search.field = true;
    search.on = editing;
    search.caret = editing ? m.caret() : -1;
    search.placeholder = text.empty() && !editing;
    search.tooltip = hint;
    search.radius = 12;
    m.buttons_.push_back(search);
    x = search.x - 16;
    if (!text.empty()) {
      Button clear = m.textButton(strings().clear, Menu::Icon::None, [&m] {
        StashMenu& s = state(m);
        s.typing = StashMenu::Typing::FullSearch;
        setText(m, "");
        return MenuCommand{};
      });
      clear.panel = Menu::kBrowser;
      clear.bare = true;
      clear.on = true;
      clear.fontSize = 26;
      clear.w = m.textWidth(strings().clear, 26, false) + 40;
      clear.x = right - clear.w - 6;
      clear.y = top + 4;
      clear.h = h - 8;
      clear.radius = clear.h / 2;
      m.buttons_.push_back(clear);
    }
    // Most first or A–Z; years: one button, newest or oldest first, that flips.
    const bool years = st.full == StashFull::Years;
    for (int byName = 1; byName >= (years ? 1 : 0); --byName) {
      const char* label = years ? t.yearOrder[st.fullByName ? 1 : 0]
                          : byName ? t.alphabetical
                                   : t.byCount;
      Button b = m.textButton(
          label, Menu::Icon::None,
          [&m, byName, years] {
            StashMenu& s = state(m);
            s.fullByName = years ? !s.fullByName : byName == 1;
            s.fullStale = true;
            m.scrollOffset_ = 0;
            m.scrollFraction_ = 0;
            return MenuCommand{};
          },
          !years && st.fullByName == (byName == 1));
      b.panel = Menu::kBrowser;
      b.w = m.textWidth(label, Menu::kTextSize, false) + 40;
      b.x = x - b.w;
      b.y = top;
      b.h = h;
      b.radius = h / 2;
      b.tooltip = years ? t.tipYearSort : t.tipFullSort[byName];
      m.buttons_.push_back(b);
      x = b.x - 6;
    }
  }
  st.fullTitleRight = x - 12;

  double listTop = top + h + 10;
  const int columns = 2;
  // A–Z: a row of the letters the list has; one scrolls to its first row.
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
    const double w = std::min(56.0, (right - x0 - 30) / std::max<size_t>(1, letters.size()) - 4);
    double lx = x0;
    for (const auto& [letter, row] : letters) {
      Button b = m.textButton(letter, Menu::Icon::None, [&m, row = row, columns] {
        m.scrollOffset_ = row / columns;
        m.scrollFraction_ = 0;
        return MenuCommand{};
      });
      b.panel = Menu::kBrowser;
      b.x = lx;
      b.y = listTop;
      b.w = w;
      b.h = 48;
      b.radius = 10;
      b.fontSize = 24;
      b.tooltip = t.tipJumpLetter;
      m.buttons_.push_back(b);
      lx += w + 4;
    }
    listTop += 58;
  }

  const std::vector<StashMenu::FullRow>& rows = st.fullRows;
  m.listTop_ = listTop;
  m.rowHeight_ = kRowStep;
  m.visibleRows_ = std::max(1, static_cast<int>((m.listBottom_ - listTop) / kRowStep));
  m.listCount_ = (static_cast<int>(rows.size()) + columns - 1) / columns;
  int firstRow, endRow;
  m.clampScroll(m.listCount_, m.visibleRows_, firstRow, endRow);
  const double cellW = (m.listRight_ - 30 - x0 - 16) / columns;
  const int end = std::min(static_cast<int>(rows.size()), endRow * columns);
  for (int i = firstRow * columns; i < end; ++i) {
    const StashMenu::FullRow& r = rows[i];
    const double cx = x0 + (i % columns) * (cellW + 16);
    const double cy = listTop + (i / columns - m.scrollOffset_ - m.scrollFraction_) * kRowStep;
    const double w = r.exclude ? cellW - kExcludeW - 6 : cellW;
    Button b = facetButton(m, r.label, r.count, r.state, r.include, cx, cy, w, kRowBox);
    if (st.full != StashFull::Pins && b.tooltip.empty()) b.tooltip = t.tipFilter;
    b.scrolls = true;
    m.buttons_.push_back(b);
    if (r.exclude) {
      Button ex = excludeButton(m, r.state, r.exclude, cx + w + 6, cy, kExcludeW, kRowBox);
      ex.scrolls = true;
      m.buttons_.push_back(ex);
    }
  }
  m.addScrollbar();
}

void StashPages::layoutResults(Menu& m, double top, double right) {
  StashMenu& st = state(m);
  const bool editing = m.editingFilter();
  const double bottom = editing ? kKeyboardY - 16 : kListBottom;
  const double x0 = Menu::kPageX + kPagePad;
  double resultsX = x0;
  if (st.railShown && catalog(m)) {
    layoutRail(m, st.rail, st.railHeight, top, bottom);
    resultsX = x0 + kRailW + kRailGap;
  }
  m.listLeft_ = resultsX;
  m.listRight_ = right + 6;
  m.listShowsFolder_ = false;
  m.listColumns_ = false;
  m.listBottom_ = bottom;
  m.listTop_ = top;
  st.resultsX = resultsX;
  st.resultsTop = layoutChips(m, resultsX, right, top);
  double notHereY = st.resultsTop + 20;
  st.notHere = catalog(m) && st.full == StashFull::None &&
               layoutNotHere(m, resultsX + 24, notHereY, right - resultsX - 48);
  if (catalog(m) && !st.notHere) {
    if (st.placePending && st.full == StashFull::None) {
      st.placePending = false;
      m.scrollToPlace(st.place, resultPaths(m),
                      st.gallery ? gridColumns(resultsX, m.listRight_) : 1);
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

void StashPages::layoutScenes(Menu& m) {
  refresh(m);
  layoutResults(m, kGridTop, Menu::kPageX + Menu::kPageWidth - kPagePad);
  layoutHeader(m, Menu::kPageX + kPagePad);
  layoutTools(m);
}

std::string StashPages::title(const Menu& m, const StashScene& s) {
  return m.view_.stash.titles ? s.displayTitle() : s.fileName();
}

std::string StashPages::performers(const Menu& m, const StashScene& s) {
  const StashCatalog* c = catalog(m);
  std::string names;
  if (!c) return names;
  for (const int p : s.performers) {
    const StashPerformer& performer = c->performers()[p];
    if (!stashShown(performer.gender, m.view_.stash.malePerformers)) continue;
    names += (names.empty() ? "" : ", ") + performer.name;
  }
  return names;
}

std::string StashPages::studioDate(const StashCatalog& c, const StashScene& s) {
  return stashJoined({s.studio >= 0 ? c.studios()[s.studio].name : "", s.date,
                      s.duration > 0 ? formatTime(s.duration) : ""});
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
  const double x = st.titleX;
  // Shorter on a performer's or studio's page, where Back comes first.
  const double titleW = std::min(m.textWidth(heading, 30, true) + 4,
                                 m.page_ == Menu::Page::StashPerformer ||
                                         m.page_ == Menu::Page::StashStudio
                                     ? 520.0
                                     : 700.0);
  m.drawText(cr, heading, x, kBrowserRowY, titleW, kBrowserRowH, 30, true, false, pal.text.r,
             pal.text.g, pal.text.b);
  if (c) {
    const long shown = static_cast<long>(st.result.scenes.size());
    const long all = st.result.formats[static_cast<int>(st.list.format)];
    const std::string count = shown == all
                                  ? stashPrinted(t.count, stashNumber(shown))
                                  : stashPrinted(t.countOf, stashNumber(shown), stashNumber(all));
    m.drawText(cr, count, x + titleW + 20, kBrowserRowY, 360, kBrowserRowH, 24, false, false,
               pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a);
  }
  const char* empty = nullptr;
  if (!c)
    drawWaiting(m, cr, st.resultsX + 24, st.resultsTop + 20,
                Menu::kPageX + Menu::kPageWidth - kPagePad - st.resultsX - 48);
  else if (st.full == StashFull::None && st.result.scenes.empty() && !st.notHere)
    empty = t.noScenes;
  if (empty)
    m.drawText(cr, empty, st.resultsX + 24, st.resultsTop + 20,
               Menu::kPageX + Menu::kPageWidth - kPagePad - st.resultsX - 48, kFileRowH, 27, false,
               false, pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a);
  drawFullHeading(m, cr);
}

void StashPages::drawFullHeading(const Menu& m, cairo_t* cr) {
  const StashMenu& st = *m.stashMenu_;
  if (!catalog(m) || st.full == StashFull::None) return;
  const StashStrings& t = strings().stash;
  const Palette& pal = m.palette();
  const int kind = st.full == StashFull::Performers ? 0
                   : st.full == StashFull::Tags     ? 1
                   : st.full == StashFull::Studios  ? 2
                                                    : -1;
  std::string title;
  if (st.full == StashFull::Pins) {
    title = t.pins;
  } else {
    std::string what = t.released;
    if (kind >= 0)
      what = t.sections[kind];
    else if (st.full == StashFull::Look)
      what = stashLookHeading(static_cast<int>(st.fullLook));
    int n = 0;
    for (const auto& r : st.fullRows) n += r.state != 2;
    const bool cards = m.page_ == Menu::Page::StashPerformers;
    title = stashPrinted(cards ? t.fullTitlePerformers : t.fullTitle, what, stashNumber(n));
  }
  m.drawText(cr, title, st.fullTitleX, st.resultsTop, st.fullTitleRight - st.fullTitleX, 56, 27,
             true, false, pal.text.r, pal.text.g, pal.text.b);
  if (st.fullRows.empty() && !st.fullSearch.empty())
    m.drawText(cr, stashPrinted(t.noMatches, st.fullSearch), st.resultsX + 24, m.listTop_, 900,
               kFileRowH, 27, false, false, pal.textDim.r, pal.textDim.g, pal.textDim.b,
               pal.textDim.a);
}
