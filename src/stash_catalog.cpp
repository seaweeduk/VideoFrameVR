#include "stash_catalog.h"

#include "stash_text.h"
#include "remote.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <map>
#include <thread>
#include <unordered_set>

#include <dirent.h>

namespace {

double msSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// Which of `paths` exist, a folder at a time (one listing per folder, not a
// stat per file: on a network share that is many times faster), several
// folders at once. A folder that can't be read for another reason than not
// being there (a share that is down) counts as having them all, so its
// scenes stay listed.
std::vector<char> existing(const std::vector<std::string>& paths) {
  constexpr size_t kThreads = 8;
  std::vector<char> there(paths.size(), 0);
  std::unordered_map<std::string, std::vector<size_t>> folders;
  for (size_t i = 0; i < paths.size(); ++i) {
    const size_t slash = paths[i].find_last_of('/');
    if (paths[i].empty() || slash == std::string::npos) continue;
    folders[paths[i].substr(0, slash ? slash : 1)].push_back(i);
  }
  std::vector<const std::pair<const std::string, std::vector<size_t>>*> list;
  for (const auto& folder : folders) list.push_back(&folder);
  std::atomic<size_t> next{0};
  auto work = [&] {
    for (size_t k; (k = next++) < list.size();) {
      const auto& [folder, files] = *list[k];
      std::unordered_set<std::string> names;
      if (remote::isRemote(folder)) {
        std::vector<remote::Entry> entries;
        const auto status = remote::list(folder, entries);
        if (status != remote::Status::Ok) {
          if (status != remote::Status::Missing)
            for (const size_t i : files) there[i] = 1;
          continue;
        }
        for (const auto& entry : entries) names.insert(entry.name);
      } else {
        DIR* dir = ::opendir(folder.c_str());
        if (!dir) {
          if (errno != ENOENT && errno != ENOTDIR)
            for (const size_t i : files) there[i] = 1;
          continue;
        }
        while (const dirent* entry = ::readdir(dir)) names.insert(entry->d_name);
        ::closedir(dir);
      }
      for (const size_t i : files)
        there[i] = names.count(paths[i].substr(paths[i].find_last_of('/') + 1)) > 0;
    }
  };
  std::vector<std::thread> threads;
  for (size_t t = 1; t < std::min(kThreads, list.size()); ++t) threads.emplace_back(work);
  work();
  for (std::thread& t : threads) t.join();
  return there;
}

StashGender gender(const std::string& g) {
  if (g == "FEMALE") return StashGender::Female;
  if (g == "MALE") return StashGender::Male;
  if (g == "TRANSGENDER_FEMALE") return StashGender::TransFemale;
  if (g == "TRANSGENDER_MALE") return StashGender::TransMale;
  if (g == "INTERSEX") return StashGender::Intersex;
  if (g == "NON_BINARY") return StashGender::NonBinary;
  return StashGender::Unknown;
}

// The cup of measurements such as "34D-24-36" or "32DD": A or B, C, D, or
// bigger.
uint8_t cup(const std::string& measurements) {
  size_t at = 0;
  while (at < measurements.size() && (std::isdigit(static_cast<unsigned char>(measurements[at])) ||
                                      measurements[at] == ' '))
    ++at;
  std::string letters;
  while (at < measurements.size() && std::isalpha(static_cast<unsigned char>(measurements[at])))
    letters += static_cast<char>(std::toupper(static_cast<unsigned char>(measurements[at++])));
  if (letters.empty()) return kStashNone;
  if (letters == "AA" || letters == "A" || letters == "B") return uint8_t(StashCup::AB);
  if (letters == "C") return uint8_t(StashCup::C);
  if (letters == "D") return uint8_t(StashCup::D);
  return uint8_t(StashCup::DDPlus);
}

uint8_t breasts(const std::string& fakeTits) {
  const std::string v = stashFold(fakeTits);
  if (v == "natural" || v == "no") return uint8_t(StashBreasts::Natural);
  if (v == "fake" || v == "yes" || v == "augmented") return uint8_t(StashBreasts::Augmented);
  return kStashNone;  // also Stash's "Na"
}

uint8_t height(int cm) {
  if (cm <= 0) return kStashNone;
  return uint8_t(cm < 160 ? StashHeight::Under160 : cm < 170 ? StashHeight::To169
                 : cm < 180 ? StashHeight::To179 : StashHeight::From180);
}

// Tattoos are recorded unless the text is empty or says there are none.
uint8_t tattoos(const std::string& text) {
  const std::string key = stashMergeKey(text);
  for (const char* none : {"", "none", "no", "na", "nope", "unknown", "notattoos", "noneknown",
                           "novisible", "nonevisible"})
    if (key == none) return uint8_t(StashTattoos::NoneRecorded);
  return uint8_t(StashTattoos::Some);
}

uint8_t codec(const std::string& name) {
  const std::string v = stashMergeKey(name);
  if (v.empty()) return kStashNone;
  if (v == "hevc" || v == "h265") return uint8_t(StashCodec::HEVC);
  if (v == "h264" || v == "avc" || v == "avc1") return uint8_t(StashCodec::H264);
  return uint8_t(StashCodec::Other);
}

uint8_t fileSize(uint64_t bytes) {
  if (!bytes) return kStashNone;
  const double gb = bytes / 1e9;
  return uint8_t(gb < 5 ? StashFileSize::Under5 : gb < 10 ? StashFileSize::To10
                 : gb < 20 ? StashFileSize::To20 : StashFileSize::From20);
}

// The calendar date of a day since 1970.
struct Civil {
  int year, month, day;
};
Civil civil(int32_t days) {
  const int64_t z = static_cast<int64_t>(days) + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  return {static_cast<int>(yoe + era * 400 + (month <= 2)), month,
          static_cast<int>(doy - (153 * mp + 2) / 5 + 1)};
}

// Sorts `items` by name (as shown, ignoring case), ties by id; the order of
// their indexes.
template <typename T, typename Name>
std::vector<size_t> byName(const std::vector<T>& items, Name name) {
  std::vector<std::string> keys(items.size());
  for (size_t i = 0; i < items.size(); ++i) keys[i] = stashCollateKey(name(items[i]));
  std::vector<size_t> order(items.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return keys[a] != keys[b] ? keys[a] < keys[b] : items[a].id < items[b].id;
  });
  return order;
}

}  // namespace

int32_t stashDay(const std::string& date) {
  int y = 0, m = 1, d = 1;
  if (date.size() < 4 || std::sscanf(date.c_str(), "%4d-%2d-%2d", &y, &m, &d) < 1) return kStashNoDay;
  if (y <= 1 || m < 1 || m > 12 || d < 1 || d > 31) return kStashNoDay;  // 0001-01-01: none
  const int64_t yy = y - (m <= 2);
  const int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
  const int64_t yoe = yy - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  return static_cast<int32_t>(era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468);
}

int stashAge(int32_t born, int32_t day) {
  if (born == kStashNoDay || day == kStashNoDay || day < born) return -1;
  // The years between, less one before the birthday.
  const Civil b = civil(born), d = civil(day);
  const bool before = d.month < b.month || (d.month == b.month && d.day < b.day);
  return d.year - b.year - (before ? 1 : 0);
}

StashAge stashAgeBand(int age) {
  if (age < 18) return StashAge::kCount;
  return age <= 22 ? StashAge::To22 : age <= 29 ? StashAge::To29 : age <= 39 ? StashAge::To39
                                                                            : StashAge::From40;
}

namespace {

enum class MeasureUnit { Unset, Inches, Cm };

// Strict triple parser, not a search for three numbers in arbitrary text.
// No locale-dependent conversion, exponents, signs or partial numbers.
struct ShapeMeasurements {
  double values[3]{};
  MeasureUnit units[3]{};
  std::string cup;

  bool parse(const std::string& text) {
    if (text.size() > 128) return false;
    std::string s;
    for (size_t at = 0; at < text.size();) {
      bool replaced = false;
      for (const char* dash : {"‐", "‑", "‒", "–", "—", "−"}) {
        const std::string d(dash);
        if (text.compare(at, d.size(), d) != 0) continue;
        s += '-';
        at += d.size();
        replaced = true;
        break;
      }
      if (replaced) continue;
      for (const char* space : {"\u00a0", "\u2009", "\u202f"}) {
        const std::string sp(space);
        if (text.compare(at, sp.size(), sp) != 0) continue;
        s += ' ';
        at += sp.size();
        replaced = true;
        break;
      }
      if (!replaced) s += static_cast<char>(std::tolower(static_cast<unsigned char>(text[at++])));
    }
    size_t at = 0;
    auto spaces = [&] {
      const size_t start = at;
      while (at < s.size() && std::isspace(static_cast<unsigned char>(s[at]))) ++at;
      return at != start;
    };
    auto digit = [&] { return at < s.size() && s[at] >= '0' && s[at] <= '9'; };
    // Read an optional suffix without eating a whitespace-only separator.
    auto word = [&] {
      const size_t start = at;
      spaces();
      const size_t begin = at;
      if (at < s.size() && s[at] == '"') ++at;
      else while (at < s.size() && s[at] >= 'a' && s[at] <= 'z') ++at;
      if (at == begin) {
        at = start;
        return std::string();
      }
      return s.substr(begin, at - begin);
    };
    auto unit = [](const std::string& word) {
      if (word == "cm") return MeasureUnit::Cm;
      if (word == "in" || word == "inch" || word == "inches" || word == "\"")
        return MeasureUnit::Inches;
      return MeasureUnit::Unset;
    };
    spaces();
    for (int i = 0; i < 3; ++i) {
      if (!digit()) return false;
      double value = 0;
      while (digit()) {
        value = value * 10 + (s[at++] - '0');
        if (value > 1000) return false;
      }
      if (at < s.size() && s[at] == '.') {
        ++at;
        if (!digit()) return false;
        double place = .1;
        while (digit()) {
          value += (s[at++] - '0') * place;
          place *= .1;
        }
      }
      values[i] = value;
      std::string suffix = word();
      units[i] = unit(suffix);
      if (!suffix.empty() && units[i] == MeasureUnit::Unset) {
        if (i != 0) return false;
        cup = suffix;
        suffix = word();
        units[i] = unit(suffix);
        if (!suffix.empty() && units[i] == MeasureUnit::Unset) return false;
      }
      const bool gap = spaces();
      if (i == 2) return at == s.size();
      if (at < s.size() && s[at] == '-') {
        ++at;
        spaces();
      } else if (!gap) {
        return false;
      }
    }
    return false;
  }
};

// Approximate band-to-bust differences in inches. With no sizing-system
// metadata, single letters E and up cover common US and UK interpretations.
// Repeated cups are whole names, not their first letter. This is an estimate
// from recorded bra labels, not a conversion from a fitted circumference.
bool cupRange(const std::string& cup, double& low, double& high) {
  struct Cup { const char* name; double low, high; };
  static constexpr Cup cups[] = {
      {"aa", 0, 0}, {"a", 1, 1}, {"b", 2, 2}, {"c", 3, 3}, {"d", 4, 4},
      {"dd", 5, 5}, {"ddd", 6, 6}, {"e", 5, 6}, {"f", 6, 7}, {"ff", 8, 8},
      {"g", 7, 9}, {"gg", 10, 10}, {"h", 8, 11}, {"hh", 12, 12}, {"i", 9, 9},
      {"j", 10, 13}, {"jj", 14, 14}, {"k", 11, 15}, {"kk", 16, 16},
      {"l", 12, 17}, {"ll", 18, 18}, {"m", 13, 19}, {"mm", 20, 20},
      {"n", 14, 21}, {"nn", 22, 22}, {"o", 15, 23}, {"oo", 24, 24},
      {"p", 16, 25}, {"pp", 26, 26}};
  for (const Cup& c : cups)
    if (cup == c.name) {
      low = c.low;
      high = c.high;
      return true;
    }
  return false;
}

bool plausibleMeasurements(double bust, double waist, double hips) {
  // Broad metadata typo guards, not classification thresholds. In
  // particular, a 40-inch waist is not evidence that the triple is metric.
  return std::isfinite(bust) && std::isfinite(waist) && std::isfinite(hips) &&
         bust >= 24 && bust <= 80 && waist >= 16 && waist <= 65 && hips >= 24 && hips <= 85 &&
         waist <= hips;
}

}  // namespace

unsigned stashShape(const std::string& measurements) {
  ShapeMeasurements m;
  if (!m.parse(measurements)) return 0;
  double cupLow = 0, cupHigh = 0;
  if (!m.cup.empty() && !cupRange(m.cup, cupLow, cupHigh)) return 0;
  auto plausible = [&](MeasureUnit inferred) {
    const double bust = m.values[0] / (inferred == MeasureUnit::Cm ? 2.54 : 1);
    const double waist = m.values[1] / (inferred == MeasureUnit::Cm ? 2.54 : 1);
    const double hips = m.values[2] / (inferred == MeasureUnit::Cm ? 2.54 : 1);
    if (inferred == MeasureUnit::Inches && !m.cup.empty()) {
      if (m.values[0] < 24 || m.values[0] > 60) return false;
      return plausibleMeasurements(bust + cupLow, waist, hips) &&
             plausibleMeasurements(bust + cupHigh, waist, hips);
    }
    return plausibleMeasurements(bust, waist, hips);
  };
  MeasureUnit common = MeasureUnit::Unset;
  bool mixed = false;
  for (const MeasureUnit unit : m.units) {
    if (unit == MeasureUnit::Unset) continue;
    if (common != MeasureUnit::Unset && common != unit) mixed = true;
    common = unit;
  }
  if (common == MeasureUnit::Unset) {
    const bool inches = plausible(MeasureUnit::Inches), cm = plausible(MeasureUnit::Cm);
    if (inches == cm) return 0;  // neither plausible, or genuinely ambiguous
    common = inches ? MeasureUnit::Inches : MeasureUnit::Cm;
  }
  for (int i = 0; i < 3; ++i) {
    if (m.units[i] == MeasureUnit::Unset) {
      if (mixed) return 0;  // mixed units need an explicit unit for every value
      m.units[i] = common;
    }
    if (m.units[i] == MeasureUnit::Cm) m.values[i] /= 2.54;
  }
  // A metric cup suffix might describe a bust circumference or an EU bra
  // band. There is not enough information to choose or add a cup again.
  if (!m.cup.empty() && (m.units[0] != MeasureUnit::Inches ||
                         m.values[0] < 24 || m.values[0] > 60)) return 0;
  const double bustLow = m.values[0] + cupLow, bustHigh = m.values[0] + cupHigh;
  const double waist = m.values[1], hips = m.values[2];
  if (!plausibleMeasurements(bustLow, waist, hips) ||
      !plausibleMeasurements(bustHigh, waist, hips)) return 0;

  // Product heuristics for metadata, not validated anatomical categories.
  // Hourglass needs both waist definition and reasonably balanced outer
  // measurements; a narrow waist alone does not make every top-heavy figure
  // Hourglass. Intermediate proportions need not receive any label. All
  // checks are symmetric on swapping bust and hips and must hold across the
  // whole plausible cup range.
  constexpr double kDefinedWaistRatio = .70;
  constexpr double kHourglassBalanceRatio = 1.15;
  constexpr double kStraightWaistRatio = .80;
  constexpr double kHeavySideRatio = 1.10;
  constexpr double kRatioEpsilon = 1e-9;  // equivalent inch/cm boundaries
  unsigned shapes = 0;
  if (waist <= kDefinedWaistRatio * bustLow + kRatioEpsilon &&
      waist <= kDefinedWaistRatio * hips + kRatioEpsilon &&
      bustHigh <= kHourglassBalanceRatio * hips + kRatioEpsilon &&
      hips <= kHourglassBalanceRatio * bustLow + kRatioEpsilon)
    shapes |= stashBit(StashShape::Hourglass);
  if (waist + kRatioEpsilon >= kStraightWaistRatio * bustHigh &&
      waist + kRatioEpsilon >= kStraightWaistRatio * hips)
    shapes |= stashBit(StashShape::Straight);
  if (hips + kRatioEpsilon >= kHeavySideRatio * bustHigh) shapes |= stashBit(StashShape::Pear);
  if (bustLow + kRatioEpsilon >= kHeavySideRatio * hips) shapes |= stashBit(StashShape::TopHeavy);
  return shapes;
}

bool stashShown(StashGender gender, bool malePerformers) {
  return malePerformers || gender != StashGender::Male;
}

std::shared_ptr<const StashCatalog> StashCatalog::build(const StashData& data,
                                                        const StashSettings& settings,
                                                        const StashLibraryFormats& library) {
  const auto start = std::chrono::steady_clock::now();
  std::shared_ptr<StashCatalog> c(new StashCatalog);
  StashCatalogStats& stats = c->stats_;
  c->server_ = data.server;
  c->version_ = data.version;
  c->syncedAt_ = data.syncedAt;
  stats.stashScenes = static_cast<int>(data.scenes.size());
  stats.stashTags = static_cast<int>(data.tags.size());

  // Each file's library folder (the longest of Stash's it is in, else the
  // last, ""), and a few files of each, spread across its subfolders.
  const size_t libraries = data.libraryFolders.size();
  std::vector<StashCatalogFolder> folders(libraries + 1);
  for (size_t i = 0; i < libraries; ++i) folders[i].folder = data.libraryFolders[i];
  auto folderOf = [&](const std::string& path) {
    size_t best = libraries;
    for (size_t i = 0; i < libraries; ++i)
      if ((best == libraries || data.libraryFolders[i].size() > data.libraryFolders[best].size()) &&
          stashUnder(path, data.libraryFolders[i]))
        best = i;
    return best;
  };
  using SampleFiles = std::map<std::string, uint64_t>;
  std::vector<std::map<std::string, SampleFiles>> sampleFolders(libraries + 1);
  std::vector<size_t> sceneFolder(data.scenes.size(), libraries);
  for (size_t i = 0; i < data.scenes.size(); ++i)
    for (size_t f = 0; f < data.scenes[i].files.size(); ++f) {
      const std::string& path = data.scenes[i].files[f].path;
      const bool windows = stashWindowsPath(path);
      if (path.empty() || (path.front() != '/' && !windows)) continue;
      const size_t folder = folderOf(path);
      if (f == 0) {
        sceneFolder[i] = folder;
        ++folders[folder].scenes;
      }
      const size_t slash = path.find_last_of(windows ? "/\\" : "/");
      if (slash == std::string::npos || slash + 1 == path.size()) continue;
      auto [it, added] = sampleFolders[folder][path.substr(0, slash)].emplace(
          path, data.scenes[i].files[f].size);
      if (!added) it->second = std::min(it->second, data.scenes[i].files[f].size);
    }
  for (size_t k = 0; k <= libraries; ++k) {
    std::vector<StashFolderFinder::File>& samples = folders[k].samples;
    std::vector<const SampleFiles*> parents;
    for (const auto& [parent, files] : sampleFolders[k]) parents.push_back(&files);
    const size_t count = std::min(size_t(5), parents.size());
    for (size_t i = 0; i < count; ++i) {
      const auto& files = *parents[count > 1 ? i * (parents.size() - 1) / (count - 1) : 0];
      samples.push_back({files.begin()->first, files.begin()->second});
    }
    if (count < 5)
      for (const auto& [parent, files] : sampleFolders[k])
        for (auto it = std::next(files.begin()); it != files.end() && samples.size() < 5; ++it)
          samples.push_back({it->first, it->second});
  }

  std::vector<std::string> local;
  for (const StashData::Scene& s : data.scenes)
    for (const StashData::File& f : s.files) local.push_back(settings.localPath(f.path));
  const std::vector<char> there = existing(local);
  stats.checkMs = msSince(start);
  const auto buildStart = std::chrono::steady_clock::now();

  // Performers, by name.
  std::array<std::unordered_map<std::string, int>, kStashNamedLooks> namedByKey;
  for (const size_t i : byName(data.performers, [](const auto& p) { return p.name; })) {
    const StashData::Performer& d = data.performers[i];
    StashPerformer p;
    p.id = d.id;
    p.name = d.name;
    p.disambiguation = d.disambiguation;
    p.aliases = d.aliases;
    p.gender = gender(d.gender);
    p.birthdate = d.birthdate;
    p.born = stashDay(d.birthdate);
    p.country = d.country;
    p.ethnicity = d.ethnicity;
    p.hairColor = d.hairColor;
    p.eyeColor = d.eyeColor;
    p.heightCm = d.heightCm;
    p.measurements = d.measurements;
    p.fakeTits = d.fakeTits;
    p.tattoos = d.tattoos;
    p.looks.fill(-1);
    const std::string* named[kStashNamedLooks] = {&d.hairColor, &d.ethnicity, &d.country,
                                                  &d.eyeColor};
    for (int k = 0; k < kStashNamedLooks; ++k) {
      const std::string key = stashMergeKey(*named[k]);
      if (key.empty()) continue;
      std::vector<std::string>& names = c->lookNames_[k];
      const auto [it, added] = namedByKey[k].emplace(key, static_cast<int>(names.size()));
      if (added) names.push_back(*named[k]);
      p.looks[k] = it->second;
    }
    auto bucket = [&p](StashLookKind kind, uint8_t value) {
      p.looks[static_cast<size_t>(kind)] = value == kStashNone ? -1 : value;
    };
    bucket(StashLookKind::Cup, cup(d.measurements));
    bucket(StashLookKind::Breasts, breasts(d.fakeTits));
    bucket(StashLookKind::Height, height(d.heightCm));
    const unsigned shapes = stashShape(d.measurements);
    p.looks[static_cast<size_t>(StashLookKind::Shape)] =
        static_cast<int>(shapes ? shapes : stashBit(StashShape::Unclassified));
    bucket(StashLookKind::Tattoos, tattoos(d.tattoos));
    p.piercings = d.piercings;
    p.careerStart = d.careerStart;
    p.careerEnd = d.careerEnd;
    p.image = d.image;
    p.favorite = d.favorite;
    c->performerById_[p.id] = static_cast<int>(c->performers_.size());
    c->performers_.push_back(std::move(p));
  }

  // Studios, by name, each with the one it is under.
  for (const size_t i : byName(data.studios, [](const auto& s) { return s.name; })) {
    const StashData::Studio& d = data.studios[i];
    c->studioById_[d.id] = static_cast<int>(c->studios_.size());
    c->studios_.push_back({d.id, d.name, d.image, -1, 0});
  }
  for (const StashData::Studio& d : data.studios) {
    const auto parent = c->studioById_.find(d.parent);
    if (parent != c->studioById_.end() && d.parent != d.id)
      c->studios_[c->studioById_.at(d.id)].parent = parent->second;
  }

  // Tags, duplicates merged: names equal but for case, spaces and
  // punctuation. The most used one names the merged tag.
  std::unordered_map<int64_t, int> uses;
  for (const StashData::Scene& s : data.scenes)
    for (const int64_t t : s.tags) ++uses[t];
  std::vector<const StashData::Tag*> tags;
  for (const StashData::Tag& t : data.tags) tags.push_back(&t);
  std::sort(tags.begin(), tags.end(), [&](const StashData::Tag* a, const StashData::Tag* b) {
    const int ua = uses[a->id], ub = uses[b->id];
    return ua != ub ? ua > ub : a->id < b->id;
  });
  std::unordered_map<std::string, size_t> mergedByKey;
  std::vector<StashTag> merged;
  for (const StashData::Tag* t : tags) {
    std::string key = stashMergeKey(t->name);
    if (key.empty()) key = t->name;  // all punctuation: only itself
    const auto [it, added] = mergedByKey.emplace(key, merged.size());
    if (added) merged.push_back({t->name, {}, {}, 0});
    merged[it->second].names.push_back(t->name);
    merged[it->second].ids.push_back(t->id);
  }
  struct Named {
    int64_t id;
    std::string name;
  };
  std::vector<Named> mergedNames;
  for (size_t i = 0; i < merged.size(); ++i) mergedNames.push_back({static_cast<int64_t>(i), merged[i].name});
  for (const size_t i : byName(mergedNames, [](const Named& n) { return n.name; })) {
    for (const int64_t id : merged[i].ids) c->tagById_[id] = static_cast<int>(c->tags_.size());
    c->tags_.push_back(std::move(merged[i]));
  }
  std::unordered_set<int64_t> vrTags;
  const std::string vrTag = stashFold(data.vrTag);
  for (const StashData::Tag& t : data.tags)
    if (!vrTag.empty() && stashFold(t.name) == vrTag) vrTags.insert(t.id);

  // Groups, by name.
  std::unordered_map<int64_t, int> groupById;
  for (const size_t i : byName(data.groups, [](const auto& g) { return g.name; })) {
    const StashData::Group& d = data.groups[i];
    const auto studio = c->studioById_.find(d.studio);
    groupById[d.id] = static_cast<int>(c->groups_.size());
    c->groups_.push_back(
        {d.id, d.name, d.date, d.image, studio != c->studioById_.end() ? studio->second : -1});
  }

  // Scenes with a file here, by id.
  std::vector<size_t> sceneOrder(data.scenes.size());
  std::vector<size_t> firstFile(data.scenes.size());
  for (size_t i = 0, f = 0; i < data.scenes.size(); f += data.scenes[i].files.size(), ++i) {
    sceneOrder[i] = i;
    firstFile[i] = f;
  }
  std::sort(sceneOrder.begin(), sceneOrder.end(),
            [&](size_t a, size_t b) { return data.scenes[a].id < data.scenes[b].id; });
  std::unordered_set<std::string> stashHashes;
  for (const size_t i : sceneOrder) {
    const StashData::Scene& d = data.scenes[i];
    for (const StashData::File& f : d.files)
      if (!f.oshash.empty()) stashHashes.insert(f.oshash);
    // A file here, one on this device before one on a server; else Stash's
    // stream, which serves its first file.
    int chosen = -1;
    for (const bool remoteToo : {false, true})
      for (size_t f = 0; f < d.files.size() && chosen < 0; ++f)
        if (there[firstFile[i] + f] && (remoteToo || !remote::isRemote(local[firstFile[i] + f])))
          chosen = static_cast<int>(f);
    const bool streamed = chosen < 0;
    if (streamed) {
      if (d.files.empty() || d.files[0].oshash.empty()) continue;
      chosen = 0;
      ++stats.streamed;
      ++folders[sceneFolder[i]].streamed;
    } else {
      ++folders[sceneFolder[i]].listed;
    }
    const int index = static_cast<int>(c->scenes_.size());
    const StashData::File& file = d.files[chosen];
    StashScene s;
    s.id = d.id;
    s.title = d.title;
    s.code = d.code;
    s.details = d.details;
    s.director = d.director;
    s.date = d.date;
    s.day = stashDay(d.date);
    s.createdAt = d.createdAt;
    if (const auto it = c->studioById_.find(d.studio); it != c->studioById_.end()) s.studio = it->second;
    for (const int64_t id : d.performers)
      if (const auto it = c->performerById_.find(id); it != c->performerById_.end())
        s.performers.push_back(it->second);
    for (const int64_t id : d.tags)
      if (const auto it = c->tagById_.find(id); it != c->tagById_.end()) s.tags.push_back(it->second);
    for (auto list : {&s.performers, &s.tags}) {
      std::sort(list->begin(), list->end());
      list->erase(std::unique(list->begin(), list->end()), list->end());
    }
    for (const auto& [id, number] : d.groups)
      if (const auto it = groupById.find(id); it != groupById.end()) s.groups.emplace_back(it->second, number);
    s.urls = d.urls;
    s.cover = d.cover;
    if (!streamed) s.path = local[firstFile[i] + chosen];
    s.stashPath = file.path;
    s.oshash = file.oshash;
    s.size = file.size;
    s.modified = file.modified;
    s.width = file.width;
    s.height = file.height;
    s.duration = file.duration;
    s.fps = file.fps;
    s.codec = file.codec;
    for (size_t f = 0; f < d.files.size(); ++f) {
      const StashData::File& each = d.files[f];
      if (!each.oshash.empty()) {
        s.oshashes.push_back(each.oshash);
        c->sceneByOshash_.emplace(each.oshash, index);
      }
      if (there[firstFile[i] + f]) {
        LocalFile& at = c->files_[local[firstFile[i] + f]];
        at = {index, each.size, each.modified, each.oshash, each.path, at.scene >= 0};
      }
    }

    // VR or 2D: the first answer wins.
    std::optional<VideoFormat> own;
    for (const std::string& hash : s.oshashes) {
      const auto it = library.find(hash);
      if (it != library.end() && it->second && (hash == s.oshash || !own)) own = it->second;
    }
    using VrBy = StashScene::VrBy;
    if (own) s.vrBy = own->projection != Projection::Flat ? VrBy::Own : VrBy::None;
    else if (isVrFrame(s.width, s.height)) s.vrBy = VrBy::Shape;
    else if (hasVrNameTag(s.stashPath)) s.vrBy = VrBy::Name;
    else if (std::any_of(d.tags.begin(), d.tags.end(), [&](int64_t t) { return vrTags.count(t) > 0; }))
      s.vrBy = VrBy::Tag;
    s.vr = s.vrBy != VrBy::None;
    stats.vr += s.vr;
    ++stats.vrBy[static_cast<size_t>(s.vrBy)];

    c->sceneById_[s.id] = index;
    c->scenes_.push_back(std::move(s));
  }
  stats.listed = static_cast<int>(c->scenes_.size()) - stats.streamed;
  for (StashCatalogFolder& f : folders)
    if (f.scenes > 0) c->folders_.push_back(std::move(f));
  stats.libraryVideos = static_cast<int>(library.size());
  for (const auto& entry : library) stats.libraryMatched += stashHashes.count(entry.first) > 0;

  c->index();
  c->indexSimilarity();
  stats.buildMs = msSince(buildStart);
  return c;
}

// What query() works with, worked out once.
void StashCatalog::index() {
  const size_t n = scenes_.size();
  words_ = (n + 63) / 64;
  tagBits_.assign(tags_.size() * words_, 0);
  performerBits_.assign(performers_.size() * words_, 0);
  studioBits_.assign(studios_.size() * words_, 0);
  vrBits_.assign(words_, 0);
  women_.assign(n, kStashNone);
  men_.assign(n, kStashNone);
  for (auto& resolution : resolutions_) resolution.assign(n, kStashNone);
  duration_.assign(n, kStashNone);
  frameRate_.assign(n, kStashNone);
  codec_.assign(n, kStashNone);
  size_.assign(n, kStashNone);
  year_.assign(n, 0);
  looks_.clear();
  lookStart_.assign(n + 1, 0);
  haystack_.assign(n, std::string());

  for (Lists* lists : {&sceneTags_, &scenePerformers_, &sceneStudios_}) {
    lists->start.assign(n + 1, 0);
    lists->items.clear();
  }
  hidden_.resize(performers_.size());
  for (size_t p = 0; p < performers_.size(); ++p) hidden_[p] = !stashShown(performers_[p].gender, false);
  firstYear_ = lastYear_ = 0;

  auto set = [this](std::vector<uint64_t>& bits, size_t row, size_t i) {
    bits[row * words_ + i / 64] |= uint64_t(1) << (i % 64);
  };
  for (size_t i = 0; i < n; ++i) {
    const StashScene& s = scenes_[i];
    if (s.vr) set(vrBits_, 0, i);
    sceneTags_.start[i] = static_cast<uint32_t>(sceneTags_.items.size());
    scenePerformers_.start[i] = static_cast<uint32_t>(scenePerformers_.items.size());
    sceneStudios_.start[i] = static_cast<uint32_t>(sceneStudios_.items.size());
    for (const int t : s.tags) {
      set(tagBits_, t, i);
      sceneTags_.items.push_back(t);
      ++tags_[t].scenes;
    }
    int women = 0, men = 0, unknown = 0;
    std::string text = s.displayTitle() + ' ' + s.fileName();
    lookStart_[i] = static_cast<uint32_t>(looks_.size());
    for (const int p : s.performers) {
      set(performerBits_, p, i);
      scenePerformers_.items.push_back(p);
      StashPerformer& performer = performers_[p];
      ++performer.scenes;
      text += ' ' + performer.name;
      const StashGender g = performer.gender;
      if (g == StashGender::Male) ++men;
      else if (g == StashGender::Unknown) ++unknown;
      else ++women;
      if (g == StashGender::Male) continue;
      Look look;
      for (int k = 0; k < kStashLookKinds; ++k) look.values[k] = static_cast<int16_t>(performer.looks[k]);
      const StashAge age = stashAgeBand(stashAge(performer.born, s.day));
      look.values[static_cast<size_t>(StashLookKind::Age)] =
          age == StashAge::kCount ? int16_t(-1) : int16_t(age);
      looks_.push_back(look);
    }
    if (s.studio >= 0) {
      set(studioBits_, s.studio, i);
      sceneStudios_.items.push_back(s.studio);
      ++studios_[s.studio].scenes;
      text += ' ' + studios_[s.studio].name;
    }
    haystack_[i] = stashFold(text);

    if (!s.performers.empty() && !unknown) {
      women_[i] = static_cast<uint8_t>(std::min(women, 3));
      men_[i] = static_cast<uint8_t>(std::min(men, 4));
    }
    codec_[i] = codec(s.codec);
    size_[i] = fileSize(s.size);
    if (s.width > 0 && s.height > 0) {
      const int w = s.width, side = std::min(s.width, s.height);
      const StashResolution vr =
          w >= 7600 ? StashResolution::VR8K : w >= 6900 ? StashResolution::VR7K
          : w >= 5700 ? StashResolution::VR6K : w >= 5000 ? StashResolution::VR5K
          : w >= 3800 ? StashResolution::VR4K : StashResolution::VRUpTo3K;
      const StashResolution flat =
          side >= 2100 ? StashResolution::Flat4K : side >= 1000 ? StashResolution::Flat1080
          : side >= 700 ? StashResolution::Flat720 : StashResolution::FlatSD;
      resolutions_[0][i] = uint8_t(flat);
      resolutions_[1][i] = uint8_t(vr);
    }
    if (s.duration > 0)
      duration_[i] = uint8_t(s.duration < 1200 ? StashDuration::Under20 : s.duration < 2400
                             ? StashDuration::To40 : s.duration < 3600 ? StashDuration::To60
                                                   : StashDuration::From60);
    if (s.fps > 0)
      frameRate_[i] = uint8_t(s.fps >= 55 ? StashFrameRate::From55 : s.fps >= 45
                              ? StashFrameRate::From45 : StashFrameRate::Under45);
    if (s.day != kStashNoDay) {
      year_[i] = static_cast<int16_t>(civil(s.day).year);
      if (!firstYear_ || year_[i] < firstYear_) firstYear_ = year_[i];
      lastYear_ = std::max(lastYear_, year_[i]);
    }
  }
  lookStart_[n] = static_cast<uint32_t>(looks_.size());
  for (Lists* lists : {&sceneTags_, &scenePerformers_, &sceneStudios_})
    lists->start[n] = static_cast<uint32_t>(lists->items.size());

  // The fixed orders, ascending, the scenes without the value last; ties
  // by title.
  std::vector<std::string> titleKeys(n);
  for (size_t i = 0; i < n; ++i) titleKeys[i] = stashCollateKey(scenes_[i].displayTitle());
  std::vector<int> byTitle(n);
  for (size_t i = 0; i < n; ++i) byTitle[i] = static_cast<int>(i);
  std::sort(byTitle.begin(), byTitle.end(), [&](int a, int b) {
    return titleKeys[a] != titleKeys[b] ? titleKeys[a] < titleKeys[b] : scenes_[a].id < scenes_[b].id;
  });
  titleRank_.assign(n, 0);
  for (size_t r = 0; r < n; ++r) titleRank_[byTitle[r]] = static_cast<int>(r);

  auto makeOrder = [&](Order& order, auto key) {  // key: 0 = none
    order.scenes = byTitle;
    std::stable_sort(order.scenes.begin(), order.scenes.end(), [&](int a, int b) {
      const auto ka = key(scenes_[a]), kb = key(scenes_[b]);
      const bool na = ka == 0, nb = kb == 0;
      return na != nb ? nb : ka < kb;
    });
    order.known = static_cast<size_t>(
        std::count_if(scenes_.begin(), scenes_.end(), [&](const StashScene& s) { return key(s) != 0; }));
  };
  makeOrder(orders_[0], [](const StashScene& s) {  // undated ones by the day they were added
    const int64_t day = s.day != kStashNoDay ? s.day : s.createdAt ? s.createdAt / 86400 : kStashNoDay;
    return day == kStashNoDay ? int64_t(0) : day - int64_t(kStashNoDay);
  });
  makeOrder(orders_[1], [](const StashScene& s) { return s.createdAt; });
  orders_[2].scenes = byTitle;
  orders_[2].known = n;
  makeOrder(orders_[3], [](const StashScene& s) { return s.duration; });
  makeOrder(orders_[4], [](const StashScene& s) { return int64_t(s.width) * s.height; });
  makeOrder(orders_[5], [](const StashScene& s) { return s.performers.size(); });
  makeOrder(orders_[6], [](const StashScene& s) { return s.modified; });
  makeOrder(orders_[7], [](const StashScene& s) { return s.size; });
}

int StashCatalog::sceneById(int64_t id) const {
  const auto it = sceneById_.find(id);
  return it != sceneById_.end() ? it->second : -1;
}

int StashCatalog::sceneByOshash(const std::string& oshash) const {
  const auto it = sceneByOshash_.find(oshash);
  return it != sceneByOshash_.end() ? it->second : -1;
}

int StashCatalog::sceneByPath(const std::string& path) const {
  const auto it = files_.find(path);
  return it != files_.end() ? it->second.scene : -1;
}

int StashCatalog::performerById(int64_t id) const {
  const auto it = performerById_.find(id);
  return it != performerById_.end() ? it->second : -1;
}

int StashCatalog::studioById(int64_t id) const {
  const auto it = studioById_.find(id);
  return it != studioById_.end() ? it->second : -1;
}

int StashCatalog::tagById(int64_t id) const {
  const auto it = tagById_.find(id);
  return it != tagById_.end() ? it->second : -1;
}

std::string StashCatalog::stashPathOf(const std::string& path) const {
  const auto it = files_.find(path);
  return it == files_.end() || it->second.shared ? std::string() : it->second.stashPath;
}

std::string StashCatalog::oshashFor(const std::string& path, uint64_t size, int64_t modified) const {
  const auto it = files_.find(path);
  if (it == files_.end() || it->second.size != size || it->second.modified != modified) return {};
  return it->second.oshash;
}
