#include "menu_stash.h"

#include "file_util.h"
#include "settings.h"  // configDir

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

// Scenes lists: their filters by Stash's ids, turned into a catalog's
// filter, named, and kept in stash_lists.conf when pinned.

bool StashLooks::active() const {
  for (int k = 0; k < kStashNamedLooks; ++k)
    if (!in[k].empty() || !out[k].empty()) return true;
  for (const unsigned m : masks)
    if (m) return true;
  return false;
}

namespace {

template <typename T>
bool takeAway(std::vector<T>& values, const T& value) {
  const auto found = std::find(values.begin(), values.end(), value);
  if (found == values.end()) return false;
  values.erase(found);
  return true;
}

}  // namespace

void StashLooks::include(int kind, const std::string& name) {
  takeAway(out[kind], name);
  if (!takeAway(in[kind], name)) in[kind].push_back(name);
}

void StashLooks::exclude(int kind, const std::string& name) {
  takeAway(in[kind], name);
  if (!takeAway(out[kind], name)) out[kind].push_back(name);
}

void StashLooks::addTo(StashFilter& f, const StashCatalog& c) const {
  for (int k = 0; k < kStashNamedLooks; ++k) {
    const std::vector<std::string>& names = c.lookNames(static_cast<StashLookKind>(k));
    for (int o = 0; o < 2; ++o)
      for (const std::string& name : o ? out[k] : in[k]) {
        const auto found = std::find(names.begin(), names.end(), name);
        if (found != names.end())
          (o ? f.looks[k].exclude : f.looks[k].include)
              .push_back(static_cast<int>(found - names.begin()));
      }
    f.looks[k].all = false;
  }
  for (int k = kStashNamedLooks; k < kStashLookKinds; ++k) {
    for (int b = 0; b < kStashLookBuckets[k - kStashNamedLooks]; ++b)
      if (masks[k - kStashNamedLooks] >> b & 1) f.looks[k].include.push_back(b);
    f.looks[k].all = false;
  }
}

namespace {

const char* const* bucketLabels(int kind) {
  const StashStrings& t = strings().stash;
  switch (static_cast<StashLookKind>(kind)) {
    case StashLookKind::Cup: return t.cups;
    case StashLookKind::Breasts: return t.breastKinds;
    case StashLookKind::Age: return t.ages;
    case StashLookKind::Height: return t.heights;
    case StashLookKind::Shape: return t.shapes;
    default: return t.tattooKinds;
  }
}

}  // namespace

const char* stashLookHeading(int kind) {
  const StashStrings& t = strings().stash;
  const char* headings[kStashLookKinds] = {t.hair, t.ethnicity, t.country, t.eyeColour, t.cup,
                                           t.breasts, t.age, t.height, t.shape, t.tattoos};
  return headings[kind];
}

std::string stashLookRow(int kind, const std::string& name, int bucket) {
  const StashStrings& t = strings().stash;
  switch (static_cast<StashLookKind>(kind)) {
    case StashLookKind::Hair: return stashPrinted(t.hairChip, name);
    case StashLookKind::Eyes: return stashPrinted(t.eyes, name);
    case StashLookKind::Cup: return stashPrinted(t.cupChip, t.cups[bucket]);
    default: return kind < kStashNamedLooks ? name : bucketLabels(kind)[bucket];
  }
}

std::string stashLookChip(int kind, const std::string& name, int bucket) {
  if (kind == static_cast<int>(StashLookKind::Age))
    return stashPrinted(strings().stash.ageChip, strings().stash.ages[bucket]);
  return stashLookRow(kind, name, bucket);
}

bool StashList::operator==(const StashList& o) const {
  return format == o.format && tags == o.tags && tagsOut == o.tagsOut &&
         performers == o.performers && performersOut == o.performersOut && studios == o.studios &&
         studiosOut == o.studiosOut && tagsAll == o.tagsAll && performersAll == o.performersAll &&
         women == o.women && men == o.men && looks == o.looks && year == o.year &&
         addedDays == o.addedDays && resolutions == o.resolutions && durations == o.durations &&
         frameRates == o.frameRates && codecs == o.codecs && sizes == o.sizes &&
         history == o.history && text == o.text && sort == o.sort && descending == o.descending;
}

namespace {

// The looks' keys: names included, left out (with "_out"); buckets.
constexpr const char* kLookKeys[kStashLookKinds] = {"hair",    "ethnicity", "country", "eyes",
                                                    "cups",    "breasts",   "ages",    "heights",
                                                    "shapes",  "tattoos"};

std::vector<std::string> splitNames(const std::string& s) {
  std::vector<std::string> names;
  std::stringstream in(s);
  std::string name;
  while (std::getline(in, name, '|'))
    if (!name.empty()) names.push_back(name);
  return names;
}

std::string joinNames(const std::vector<std::string>& names) {
  std::string s;
  for (const std::string& name : names) s += (s.empty() ? "" : "|") + name;
  return s;
}

std::string joinIds(const std::vector<int64_t>& ids) {
  std::string s;
  for (const int64_t id : ids) s += (s.empty() ? "" : ",") + std::to_string(id);
  return s;
}

std::vector<int64_t> splitIds(const std::string& s) {
  std::vector<int64_t> ids;
  std::stringstream in(s);
  std::string part;
  while (std::getline(in, part, ','))
    if (!part.empty()) ids.push_back(std::atoll(part.c_str()));
  return ids;
}

fs::path pinsPath() { return configDir() / "stash_lists.conf"; }

// The catalog's indexes of the ids it still has.
template <typename Lookup>
std::vector<int> indexes(const std::vector<int64_t>& ids, Lookup lookup) {
  std::vector<int> out;
  for (const int64_t id : ids) {
    const int i = lookup(id);
    if (i >= 0 && std::find(out.begin(), out.end(), i) == out.end()) out.push_back(i);
  }
  return out;
}

}  // namespace

// One list after another, each from its name line on; key=value lines,
// id lists by commas, names by '|'. The cast's sizes and mixes of older
// files ("cast", "mix") become the women and men they mean, where they do:
// Solo one woman and no men, Women only no men.
std::vector<StashPin> loadStashPins() {
  std::vector<StashPin> pins;
  std::ifstream in(pinsPath());
  std::string line;
  while (std::getline(in, line)) {
    const size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    if (key == "name") {
      pins.push_back({value, {}});
      continue;
    }
    if (pins.empty()) continue;
    StashList& l = pins.back().list;
    const unsigned mask = static_cast<unsigned>(std::strtoul(value.c_str(), nullptr, 10));
    if (key == "format")
      l.format = value == "all"  ? StashFormat::All
                 : value == "2d" ? StashFormat::Flat
                                 : StashFormat::VR;
    else if (key == "tags")
      l.tags = splitIds(value);
    else if (key == "tags_out")
      l.tagsOut = splitIds(value);
    else if (key == "tags_any")
      l.tagsAll = value != "1";
    else if (key == "performers")
      l.performers = splitIds(value);
    else if (key == "performers_out")
      l.performersOut = splitIds(value);
    else if (key == "performers_any")
      l.performersAll = value != "1";
    else if (key == "studios")
      l.studios = splitIds(value);
    else if (key == "studios_out")
      l.studiosOut = splitIds(value);
    else if (key == "women")
      l.women = mask;
    else if (key == "men")
      l.men = mask;
    else if (key == "cast" && (mask & 2)) {  // solo
      l.women |= stashBit(StashWomen::One);
      l.men |= stashBit(StashMen::None);
    } else if (key == "mix" && (mask & 1))  // women only
      l.men |= stashBit(StashMen::None);
    else if (key == "year")
      l.year = std::atoi(value.c_str());
    else if (key == "added_days")
      l.addedDays = std::atoi(value.c_str());
    else if (key == "resolutions")
      l.resolutions = mask;
    else if (key == "durations")
      l.durations = mask;
    else if (key == "frame_rates")
      l.frameRates = mask;
    else if (key == "codecs")
      l.codecs = mask;
    else if (key == "sizes")
      l.sizes = mask;
    else if (key == "history")
      l.history = mask;
    else if (key == "text")
      l.text = value;
    else if (key == "descending")
      l.descending = value != "0";
    else if (key == "sort")
      for (int i = 0; i < static_cast<int>(std::size(kStashSortNames)); ++i)
        if (value == kStashSortNames[i]) l.sort = static_cast<StashSort>(i);
    for (int k = 0; k < kStashLookKinds; ++k) {
      if (k < kStashNamedLooks && key == kLookKeys[k]) l.looks.in[k] = splitNames(value);
      if (k < kStashNamedLooks && key == std::string(kLookKeys[k]) + "_out")
        l.looks.out[k] = splitNames(value);
      if (k >= kStashNamedLooks && key == kLookKeys[k]) l.looks.masks[k - kStashNamedLooks] = mask;
    }
  }
  return pins;
}

static void writeStashPins(const std::vector<StashPin>& pins) {
  const fs::path path = pinsPath();
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path);
  if (!out) {
    std::fprintf(stderr, "Could not save the pinned Stash lists\n");
    return;
  }
  out << "# VideoFrameVR's pinned Stash lists: Stash's ids of tags, performers and studios\n";
  for (const StashPin& p : pins) {
    const StashList& l = p.list;
    out << "\nname=" << p.name << "\nformat="
        << (l.format == StashFormat::All    ? "all"
            : l.format == StashFormat::Flat ? "2d"
                                            : "vr")
        << "\ntags=" << joinIds(l.tags) << "\ntags_out=" << joinIds(l.tagsOut)
        << "\ntags_any=" << (l.tagsAll ? 0 : 1) << "\nperformers=" << joinIds(l.performers)
        << "\nperformers_out=" << joinIds(l.performersOut)
        << "\nperformers_any=" << (l.performersAll ? 0 : 1) << "\nstudios=" << joinIds(l.studios)
        << "\nstudios_out=" << joinIds(l.studiosOut) << "\nwomen=" << l.women
        << "\nmen=" << l.men;
    for (int k = 0; k < kStashNamedLooks; ++k)
      out << "\n" << kLookKeys[k] << "=" << joinNames(l.looks.in[k]) << "\n" << kLookKeys[k]
          << "_out=" << joinNames(l.looks.out[k]);
    for (int k = kStashNamedLooks; k < kStashLookKinds; ++k)
      out << "\n" << kLookKeys[k] << "=" << l.looks.masks[k - kStashNamedLooks];
    out << "\nyear=" << l.year << "\nadded_days=" << l.addedDays
        << "\nresolutions=" << l.resolutions << "\ndurations=" << l.durations
        << "\nframe_rates=" << l.frameRates << "\ncodecs=" << l.codecs << "\nsizes=" << l.sizes
        << "\nhistory=" << l.history << "\ntext=" << l.text
        << "\nsort=" << kStashSortNames[static_cast<int>(l.sort)]
        << "\ndescending=" << (l.descending ? 1 : 0) << "\n";
  }
}

void saveStashPins(const std::vector<StashPin>& pins) {
  queueConfigWork([pins] { writeStashPins(pins); });
}

StashFilter stashFilterOf(const StashList& l, const StashCatalog& c, bool malePerformers,
                          int64_t now) {
  StashFilter f;
  f.format = l.format;
  auto tag = [&](int64_t id) { return c.tagById(id); };
  auto performer = [&](int64_t id) { return c.performerById(id); };
  auto studio = [&](int64_t id) { return c.studioById(id); };
  f.tags.include = indexes(l.tags, tag);
  f.tags.exclude = indexes(l.tagsOut, tag);
  f.tags.all = l.tagsAll;
  f.performers.include = indexes(l.performers, performer);
  f.performers.exclude = indexes(l.performersOut, performer);
  f.performers.all = l.performersAll;
  f.studios.include = indexes(l.studios, studio);
  f.studios.exclude = indexes(l.studiosOut, studio);
  f.studios.all = false;
  f.women = l.women;
  f.men = l.men;
  l.looks.addTo(f, c);
  if (l.year > 0) {
    char from[32], to[32];
    std::snprintf(from, sizeof(from), "%04d-01-01", l.year);
    std::snprintf(to, sizeof(to), "%04d-12-31", l.year);
    f.releasedFrom = stashDay(from);
    f.releasedTo = stashDay(to);
  }
  if (l.addedDays > 0) f.addedFrom = now - static_cast<int64_t>(l.addedDays) * 86400;
  f.resolutions = l.resolutions;
  f.durations = l.durations;
  f.frameRates = l.frameRates;
  f.codecs = l.codecs;
  f.sizes = l.sizes;
  f.history = l.history;
  f.longAgo = now - 90 * 86400;
  f.text = l.text;
  f.malePerformers = malePerformers;
  for (const int days : {7, 30, 90, 365}) f.addedSince.push_back(now - days * 86400);
  return f;
}

StashOrder stashOrderOf(const StashList& l, uint64_t seed) {
  StashOrder o;
  o.by = l.sort;
  o.descending = l.descending;
  o.seed = seed;
  return o;
}

// The format, then who and what it is about, as the chips show them; at
// most five parts.
std::string stashListName(const StashList& l, const StashCatalog* c) {
  const StashStrings& t = strings().stash;
  std::vector<std::string> parts;
  if (l.format != StashFormat::All) parts.push_back(t.formats[static_cast<int>(l.format)]);
  auto names = [&](const std::vector<int64_t>& ids, int kind, bool out) {
    for (const int64_t id : ids) {
      std::string name;
      if (c && kind == 0 && c->performerById(id) >= 0)
        name = c->performers()[c->performerById(id)].name;
      if (c && kind == 1 && c->tagById(id) >= 0) name = c->tags()[c->tagById(id)].name;
      if (c && kind == 2 && c->studioById(id) >= 0) name = c->studios()[c->studioById(id)].name;
      if (name.empty()) continue;
      if (out) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), t.without, name.c_str());
        name = buf;
      }
      parts.push_back(name);
    }
  };
  names(l.performers, 0, false);
  names(l.studios, 2, false);
  names(l.tags, 1, false);
  auto bits = [&](unsigned mask, const char* const* labels, int count) {
    for (int i = 0; i < count; ++i)
      if (mask & (1u << i)) parts.push_back(labels[i]);
  };
  bits(l.women, t.womenCounts, 3);
  bits(l.men, t.menCounts, 4);
  for (int k = 0; k < kStashNamedLooks; ++k)
    for (const std::string& name : l.looks.in[k]) parts.push_back(stashLookChip(k, name));
  bits(l.resolutions, t.resolutions, 10);
  bits(l.history, t.history, 5);
  if (l.year) parts.push_back(std::to_string(l.year));
  if (!l.text.empty()) parts.push_back("“" + l.text + "”");
  names(l.performersOut, 0, true);
  names(l.tagsOut, 1, true);
  names(l.studiosOut, 2, true);
  if (parts.empty()) parts.push_back(t.scenes);
  std::string name;
  for (size_t i = 0; i < parts.size() && i < 5; ++i) name += (i ? " · " : "") + parts[i];
  if (parts.size() > 5) name += " …";
  return name;
}
