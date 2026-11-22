#include "menu_stash_browse.h"

#include "library.h"
#include "offline.h"
#include "stash.h"
#include "stash_text.h"
#include "strings.h"

#include <algorithm>
#include <ctime>

namespace {

constexpr int kRailTop[] = {5, 6, 5};  // performers, tags and studios the rail lists
constexpr int kRailYears = 6;
constexpr int kAddedDays[] = {7, 30, 90, 365};
constexpr StashResolution kResolutionOrder[] = {StashResolution::VR8K,    StashResolution::VR7K,
                                                StashResolution::VR6K,    StashResolution::VR5K,
                                                StashResolution::VR4K,    StashResolution::VRUpTo3K,
                                                StashResolution::Flat4K,  StashResolution::Flat1080,
                                                StashResolution::Flat720, StashResolution::FlatSD};
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

constexpr double kRowStep = kStashRowStep, kLineGrow = kStashLineGrow;
constexpr int kNewDays = 30;

int64_t unixNow() { return static_cast<int64_t>(std::time(nullptr)); }

}  // namespace

const StashCatalog* StashBrowse::Sources::catalog() const {
  return stash ? stash->catalog().get() : nullptr;
}

bool StashBrowse::takeChanges(const Sources& src) {
  StashBrowse& st = *this;
  bool changed = false;
  if (src.stash->generation() != st.catalogGeneration) {
    st.catalogGeneration = src.stash->generation();
    st.libraryGeneration = ~0ull;
    st.stale = true;
    changed = true;
  }
  if (src.library && src.library->generation() != st.libraryGeneration) {
    st.libraryGeneration = src.library->generation();
    std::vector<StashHistory> history = src.stash->history();
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
  const StashCatalog* c = src.stash->catalog().get();
  if (src.offline && c && st.history.size() == c->scenes().size() &&
      (changed || st.offlineShape != src.offlineShape)) {
    st.offlineShape = src.offlineShape;
    std::vector<bool> hidden(c->scenes().size());
    const bool away = !src.offline->downServers().empty() || src.offline->stashDown();
    if (away || src.stash->hidesScenes())
      for (size_t i = 0; i < hidden.size(); ++i) {
        const StashScene& s = c->scenes()[i];
        if (src.stash->hidden(s.id)) {
          hidden[i] = true;
        } else if (away) {
          const std::string at = stashSceneLocation(s, src.offline);
          hidden[i] = src.offline->serverDown(at) && src.offline->copyOf(at, s.oshash).empty();
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
void StashBrowse::refresh(const Sources& src, const Page& page) {
  StashBrowse& st = *this;
  const StashCatalog* c = src.catalog();
  if (!c) {
    st.result = StashResult{};
    st.rail.clear();
    st.railHeight = 0;
    st.fullRows.clear();
    return;
  }
  takeChanges(src);
  if (!st.stale) return;
  st.stale = false;
  st.filter = stashFilterOf(st.list, *c, page.settings.malePerformers, unixNow());
  const bool history = st.history.size() == c->scenes().size();
  // A studio's page: its own scenes; the rail offers no studios there.
  StashFilter filter = st.filter;
  const int64_t fixed = page.studio;
  const int scope = fixed ? c->studioById(fixed) : -1;
  if (scope >= 0) filter.studios = StashPick{{scope}, {}, false};
  const int64_t her = page.performer;
  if ((fixed && scope < 0) || (her && c->performerById(her) < 0))
    st.result = StashResult{};  // gone from Stash
  else
    st.result = c->query(filter, stashOrderOf(st.list, st.seed), history ? &st.history : nullptr);
  buildRail(*c, page);
  st.fullStale = true;
}

namespace {

// What each kind of choice does to the list: included (or left out) once
// more, it is taken away.
std::function<void(StashList&)> includeIn(const StashBrowse::Host& host, int kind, int64_t id) {
  return [&host, kind, id](StashList& l) {
    const StashCatalog* c = host.catalog();
    removeId(l.*kOut[kind], c, kind, id);
    if (!removeId(l.*kIn[kind], c, kind, id)) (l.*kIn[kind]).push_back(id);
  };
}

std::function<void(StashList&)> excludeIn(const StashBrowse::Host& host, int kind, int64_t id) {
  return [&host, kind, id](StashList& l) {
    const StashCatalog* c = host.catalog();
    removeId(l.*kIn[kind], c, kind, id);
    if (!removeId(l.*kOut[kind], c, kind, id)) (l.*kOut[kind]).push_back(id);
  };
}

// On a studio's page, studios are no choice: its scenes are all its own.
bool offered(const StashBrowse::Page& page, int kind) { return kind != kStudios || !page.studio; }

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

}  // namespace

std::vector<StashChip> StashBrowse::chips(const StashList& l, const StashCatalog& c,
                                         int64_t performer) const {
  const StashStrings& t = strings().stash;
  std::vector<StashChip> chips;
  const int64_t fixed = performer;  // her page: always her
  for (int kind : {kPerformers, kTags, kStudios}) {
    for (int out = 0; out < 2; ++out)
      for (const int64_t id : out ? l.*kOut[kind] : l.*kIn[kind]) {
        const int index = indexOf(c, kind, id);
        if (index < 0 || (kind == kPerformers && id == fixed)) continue;
        chips.push_back({nameOf(c, kind, index), out == 1,
                         out ? excludeIn(host, kind, id) : includeIn(host, kind, id)});
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

int StashBrowse::count(const StashList& list, const StashCatalog& c, bool male) const {
  return static_cast<int>(
      c.query(stashFilterOf(list, c, male, unixNow()), {}, &history, false).scenes.size());
}

// The rail's lines: each section lists what occurs in the results, most
// common first, never a choice that leaves nothing (one in every result
// would narrow nothing, and leaving it out would leave nothing); what is
// chosen first, even where it counts nothing now.
void StashBrowse::buildRail(const StashCatalog& c, const Page& page) {
  StashBrowse& st = *this;
  const StashResult& r = st.result;
  const StashStrings& t = strings().stash;
  const int total = static_cast<int>(r.scenes.size());
  std::vector<StashRailLine>& rail = st.rail;
  rail.clear();
  auto change = [this](std::function<void(StashList&)> f) {
    return [this, f] { host.changeList(f); };
  };
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
  const int fixed = c.performerById(page.performer);
  auto people = [&](int kind) {
    if (!offered(page, kind)) return;
    section(t.sections[kind], [&, kind] {
      const StashPick& pick = pickOf(st.filter, kind);
      const std::vector<StashFacet>& facet = facetOf(r, kind);
      const int skip = kind == kPerformers ? fixed : -1;
      if (kind != kStudios && pick.include.size() >= 2 && skip < 0) {
        rail.back().mode = pick.all ? 1 : 0;
        rail.back().setAll = [this, kind](bool all) {
          host.changeList([kind, all](StashList& l) {
            (kind == kTags ? l.tagsAll : l.performersAll) = all;
          });
        };
      }
      for (const int index : pick.include)
        if (index != skip)
          row(nameOf(c, kind, index), countIn(facet, index), 1,
              change(includeIn(host, kind, idOf(c, kind, index))),
              change(excludeIn(host, kind, idOf(c, kind, index))));
      for (const int index : pick.exclude)
        row(nameOf(c, kind, index), 0, 2, change(includeIn(host, kind, idOf(c, kind, index))),
            change(excludeIn(host, kind, idOf(c, kind, index))));
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
            change(includeIn(host, kind, idOf(c, kind, f.index))),
            change(excludeIn(host, kind, idOf(c, kind, f.index))));
        ++shown;
      }
      if (more) {
        StashRailLine l;
        l.kind = StashRailLine::Link;
        l.label = t.more[kind];
        const StashFull full = kind == kPerformers ? StashFull::Performers
                               : kind == kTags     ? StashFull::Tags
                                                   : StashFull::Studios;
        l.include = [this, full] { host.showFull(full, StashLookKind::Hair); };
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
    const StashSettings& s = page.settings;
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
             [this](std::function<void(StashLooks&)> f) {
               return [this, f] { host.changeList([f](StashList& l) { f(l.looks); }); };
             },
             [this](StashLookKind look) { host.showFull(StashFull::Look, look); });
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
      l.include = [this] { host.showFull(StashFull::Years, StashLookKind::Hair); };
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
  const StashSettings& chosen = page.settings;
  if (st.editSections) {
    editSectionsRail(page);
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
  edit.include = [this] { host.editSections(true); };
  rail.push_back(std::move(edit));

  st.railHeight = railHeights(rail);
}

// The rail while editing it: each section with its switch and its moves,
// its subgroups under it with theirs (moving within it), then back to the
// defaults, and Done.
void StashBrowse::editSectionsRail(const Page& page) {
  StashBrowse& st = *this;
  const StashStrings& t = strings().stash;
  const StashSettings& s = page.settings;
  std::vector<StashRailLine>& rail = st.rail;
  auto set = [](const StashSettings& changed) {
    return [text = stashSectionsText(changed)] {
      MenuCommand c;
      c.type = MenuCommand::SetStashSections;
      c.path = text;
      return c;
    };
  };
  auto lines = [this](const std::string& label, double indent) {
    return host.switchLines(label, indent);
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
  done.include = [this] { host.editSections(false); };
  rail.push_back(std::move(done));
  st.railHeight = railHeights(rail);
}

void StashBrowse::lookRail(std::vector<StashRailLine>& rail, const StashCatalog& c,
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

double StashBrowse::railHeights(std::vector<StashRailLine>& rail) {
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
void StashBrowse::buildFull(const Sources& src, const Page& page,
                            const StashPerformersBrowse& performers) {
  StashBrowse& st = *this;
  st.fullStale = false;
  st.fullRows.clear();
  const StashCatalog* c = src.catalog();
  if (!c) return;
  const StashResult& r = st.result;
  const int total = static_cast<int>(r.scenes.size());
  const std::string search = stashFold(st.fullSearch);
  auto change = [this](std::function<void(StashList&)> f) {
    return [this, f] { host.changeList(f); };
  };
  if (st.full == StashFull::Pins) {
    const std::vector<StashHistory>* history =
        st.history.size() == c->scenes().size() ? &st.history : nullptr;
    for (size_t i = 0; i < st.pins.size(); ++i) {
      const StashResult pin =
          c->query(stashFilterOf(st.pins[i].list, *c, page.settings.malePerformers, unixNow()), {},
                   history, false);
      FullRow row;
      row.label = st.pins[i].name;
      row.count = stashNumber(static_cast<long>(pin.scenes.size()));
      row.include = [this, i] { host.openPin(static_cast<int>(i)); };
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
    std::vector<FullRow> sorted(st.fullRows.begin(), st.fullRows.begin() + chosen);
    for (const auto& k : keys) sorted.push_back(std::move(st.fullRows[k.second]));
    st.fullRows = std::move(sorted);
  };
  if (st.full == StashFull::Look) {
    // As the rail lists them: the Performers page's counted in performers,
    // the scenes' otherwise.
    const int k = static_cast<int>(st.fullLook);
    const bool cards = page.cards;
    const StashLooks& looks = cards ? performers.choice.looks : st.list.looks;
    const std::vector<StashFacet>& facet = (cards ? performers.looks : r.looks)[k];
    const int all = cards ? static_cast<int>(performers.shown.size()) : total;
    auto changeLooks = [this, cards](std::function<void(StashLooks&)> f) -> std::function<void()> {
      if (cards) return changePerformers([f](StashPerformerChoice& s) { f(s.looks); });
      return [this, f] { host.changeList([f](StashList& l) { f(l.looks); }); };
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
  if (!offered(page, kind)) return;
  const StashPick& pick = pickOf(st.filter, kind);
  const std::vector<StashFacet>& facet = facetOf(r, kind);
  const int skip = kind == kPerformers ? c->performerById(page.performer) : -1;
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
                             change(includeIn(host, kind, idOf(*c, kind, index))),
                             change(excludeIn(host, kind, idOf(*c, kind, index)))});
  for (const int index : pick.exclude)
    st.fullRows.push_back({nameOf(*c, kind, index), "", 2,
                           change(includeIn(host, kind, idOf(*c, kind, index))),
                           change(excludeIn(host, kind, idOf(*c, kind, index)))});
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
                           change(includeIn(host, kind, idOf(*c, kind, f.index))),
                           change(excludeIn(host, kind, idOf(*c, kind, f.index)))});
  }
  sortByName();
}

const std::vector<std::array<StashPerformerStats, 3>>& StashBrowse::performerStats(
    StashPerformersBrowse& ps, const Sources& src) {
  StashBrowse& st = *this;
  const StashCatalog* c = src.catalog();
  if (!c) {
    ps.stats.clear();
    return ps.stats;
  }
  takeChanges(src);
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

std::function<void()> StashBrowse::changePerformers(std::function<void(StashPerformerChoice&)> f) {
  return [this, f] { host.changePerformers(f); };
}

// The performers the choices and the search give, in order, the rail's
// counts (each kind with its own choices left out, so a count never leads
// to nothing) and the scenes with the chosen looks; when they, the catalog
// or the history changed.
void StashBrowse::buildPerformers(StashPerformersBrowse& ps, const Sources& src, bool male) {
  StashBrowse& st = *this;
  const StashStrings& t = strings().stash;
  const std::vector<std::array<StashPerformerStats, 3>>& stats = performerStats(ps, src);
  const StashFormat shownIn = st.list.format;
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
  const StashCatalog* c = src.catalog();
  if (!c || stats.size() != c->performers().size()) {
    ps.railHeight = 0;
    return;
  }
  const int format = static_cast<int>(shownIn);
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
    ps.theirScenes = count(list, *c, male);
  }

  // The rail: what is chosen first, then what occurs, most first; never a
  // choice in every one listed (it narrows nothing) or in none.
  const int total = static_cast<int>(ps.shown.size());
  auto change = [this](std::function<void(StashPerformerChoice&)> f) {
    return changePerformers(std::move(f));
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
  auto more = [this](StashLookKind look) { host.showFull(StashFull::Look, look); };
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

namespace {

bool samePins(const std::vector<StashPin>& a, const std::vector<StashPin>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].name != b[i].name || a[i].list != b[i].list) return false;
  return true;
}

}  // namespace

void StashHomePage::rebuild(const StashBrowse& st, const StashCatalog& c, uint64_t generation,
                            StashFormat format, bool male) {
  StashHomePage& p = *this;
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
