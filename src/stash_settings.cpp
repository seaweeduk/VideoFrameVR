#include "stash_settings.h"

#include "stash_http.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace {

std::string withoutTrailingSlash(std::string path) {
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  return path;
}

// Stable names for settings.conf, never the translated labels.
const char* const kSectionIds[kStashSections] = {"performers", "tags",     "studios", "cast",
                                                  "looks",      "released", "format",  "history"};
const char* const kSubgroupIds[kStashSubgroups] = {
    "women", "men",
    "hair", "cup", "breasts", "age", "ethnicity", "country", "height", "eyes", "shape", "tattoos",
    "added", "year",
    "resolution", "duration", "framerate", "codec", "filesize"};
constexpr const char* kPerformerSortNames[] = {"scenes", "name", "plays", "rating", "newest"};
constexpr const char* kStudioSortNames[] = {"plays", "scenes", "newest", "name", "rating"};

template <typename Sort, size_t N>
Sort readSort(const std::string& value, const char* const (&names)[N], Sort fallback) {
  for (size_t i = 0; i < N; ++i)
    if (value == names[i]) return static_cast<Sort>(i);
  return fallback;
}

// "performers,tags,-studios,cast(women,-men),...": a leading '-' hides one,
// and a section's subgroups follow it in brackets. Unknown names are
// ignored; sections missing are added at the end, and subgroups missing at
// the end of their section, shown, so that new ones appear for everyone.
void readSections(const std::string& value, StashSettings& s) {
  std::array<bool, kStashSections> seen{};
  std::array<bool, kStashSubgroups> seenSub{};
  std::array<int, kStashSections> next{};  // where each section's next subgroup goes
  for (int g = kStashSubgroups; g-- > 0;) next[stashSectionOf(g)] = g;
  int n = 0;
  s.hiddenSections = s.hiddenSubgroups = 0;
  auto hidden = [](std::string& name) {
    const bool minus = !name.empty() && name[0] == '-';
    if (minus) name.erase(0, 1);
    return minus;
  };
  auto section = [&](std::string name) {
    const bool hide = hidden(name);
    for (int i = 0; i < kStashSections; ++i)
      if (name == kSectionIds[i] && !seen[i]) {
        seen[i] = true;
        s.sections[n++] = i;
        if (hide) s.hiddenSections |= 1u << i;
        return i;
      }
    return -1;
  };
  auto subgroup = [&](std::string name, int of) {
    const bool hide = hidden(name);
    for (int g = 0; g < kStashSubgroups; ++g)
      if (name == kSubgroupIds[g] && stashSectionOf(g) == of && !seenSub[g]) {
        seenSub[g] = true;
        s.subgroups[next[of]++] = g;
        if (hide) s.hiddenSubgroups |= 1u << g;
      }
  };
  std::istringstream in(value);
  int of = -1;
  bool brackets = false;
  for (std::string name; std::getline(in, name, ',');) {
    const bool close = !name.empty() && name.back() == ')';
    if (close) name.pop_back();
    const size_t open = name.find('(');
    if (open != std::string::npos) {
      of = section(name.substr(0, open));
      brackets = true;
      name.erase(0, open + 1);
    }
    if (brackets)
      subgroup(name, of);
    else
      section(name);
    if (close) brackets = false;
  }
  for (int i : kStashDefaultSections)
    if (!seen[i]) s.sections[n++] = i;
  for (int g : kStashDefaultSubgroups)
    if (!seenSub[g]) s.subgroups[next[stashSectionOf(g)]++] = g;
}

}  // namespace

const char* stashSortName(StashPerformerSort sort) {
  return kPerformerSortNames[static_cast<int>(sort)];
}

const char* stashSortName(StudioSort sort) {
  return kStudioSortNames[static_cast<int>(sort)];
}

bool stashWindowsPath(const std::string& path) {
  if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':')
    return path.size() == 2 || path[2] == '\\' || path[2] == '/';
  return path.size() >= 2 && path[0] == '\\' && path[1] == '\\';
}

bool stashUnder(const std::string& path, const std::string& folder, std::string* rest) {
  const bool windows = stashWindowsPath(folder) || stashWindowsPath(path);
  auto separator = [windows](char c) { return c == '/' || (windows && c == '\\'); };
  std::string f = folder;
  while (f.size() > 1 && separator(f.back())) f.pop_back();
  if (f.empty() || path.size() < f.size()) return false;
  if (!windows) {
    if (f == "/") {
      if (path[0] != '/') return false;
      if (rest) *rest = path == "/" ? std::string() : path;
      return true;
    }
    // Only at a folder boundary: "/media/raid/.p" is not the start of "/media/raid/.pics".
    if (path.compare(0, f.size(), f) != 0 || (path.size() > f.size() && path[f.size()] != '/'))
      return false;
  } else {
    for (size_t i = 0; i < f.size(); ++i) {
      const char a = path[i], b = f[i];
      if (separator(a) && separator(b)) continue;
      if (std::tolower(static_cast<unsigned char>(a)) != std::tolower(static_cast<unsigned char>(b)))
        return false;
    }
    if (path.size() > f.size() && !separator(path[f.size()])) return false;
  }
  if (rest) {
    *rest = path.substr(f.size());
    std::replace(rest->begin(), rest->end(), '\\', '/');
    if (*rest == "/") rest->clear();
  }
  return true;
}

std::string stashMapped(const StashFolderMap& map, const std::string& path) {
  std::string rest;
  if (map.from.empty() || !stashUnder(path, map.from, &rest)) return {};
  const std::string to = withoutTrailingSlash(map.to);
  if (to.empty()) return {};
  if (to == "/") return rest.empty() ? to : rest;
  return to + rest;
}

std::vector<StashFolderMap> stashFoldersWith(std::vector<StashFolderMap> list,
                                             const std::string& folder, StashFolderMap map) {
  if (folder.empty()) return {map};
  if (stashUnder(folder, map.from) && !stashMapped(map, folder).empty()) {
    map.to = stashMapped(map, folder);
    map.from = folder;
  }
  list.erase(std::remove_if(list.begin(), list.end(),
                            [&](const StashFolderMap& m) {
                              return m.from.empty() || stashUnder(m.from, map.from);
                            }),
             list.end());
  list.push_back(std::move(map));
  return list;
}

const StashFolderMap* StashSettings::mappingFor(const std::string& stashPath) const {
  const StashFolderMap* best = nullptr;
  for (const StashFolderMap& m : folders)
    if (!m.from.empty() && (!best || m.from.size() > best->from.size()) &&
        stashUnder(stashPath, m.from))
      best = &m;
  return best;
}

std::string StashSettings::localPath(const std::string& stashPath) const {
  // No mapping: the same paths on both.
  if (std::none_of(folders.begin(), folders.end(),
                   [](const StashFolderMap& m) { return !m.from.empty(); }))
    return stashPath;
  const StashFolderMap* m = mappingFor(stashPath);
  return m ? stashMapped(*m, stashPath) : std::string();
}

bool StashSettings::unverified() const {
  return !unverifiedHost.empty() && stashHost(url) == unverifiedHost;
}

bool readStashSetting(const std::string& key, const std::string& value, StashSettings& s) {
  if (key == "stash") s.enabled = value == "1";
  else if (key == "stash_url") s.url = value;
  else if (key == "stash_api_key") s.apiKey = value;
  else if (key == "stash_unverified_host") s.unverifiedHost = value;
  else if (key == "stash_path_from" || key == "stash_path_to") {
    if (s.folders.empty()) s.folders.emplace_back();
    (key == "stash_path_from" ? s.folders[0].from : s.folders[0].to) = value;
  } else if (key == "stash_folder") {  // "<Stash's folder>\t<here>", after the first
    const size_t tab = value.find('\t');
    if (tab == std::string::npos) return true;
    if (s.folders.empty()) s.folders.emplace_back();
    s.folders.push_back({value.substr(0, tab), value.substr(tab + 1)});
  }
  else if (key == "stash_covers") s.covers = value != "0";
  else if (key == "stash_titles") s.titles = value != "0";
  else if (key == "stash_male_performers") s.malePerformers = value == "1";
  else if (key == "stash_write_back") s.writeBack = value == "1";
  else if (key == "stash_card") s.card = value == "1";
  else if (key == "stash_filters") readSections(value, s);
  else if (key == "stash_scenes_sort")
    s.scenesSort = readSort(value, kStashSortNames, StashSort::Released);
  else if (key == "stash_performers_sort")
    s.performersSort = readSort(value, kPerformerSortNames, StashPerformerSort::Scenes);
  else if (key == "stash_studios_sort")
    s.studiosSort = readSort(value, kStudioSortNames, StudioSort::Plays);
  else if (key == "stash_scenes_descending") s.scenesDescending = value != "0";
  else if (key == "stash_performers_descending") s.performersDescending = value != "0";
  else if (key == "stash_studios_descending") s.studiosDescending = value != "0";
  else if (key == "stash_format")
    s.format = value == "all" ? StashFormat::All : value == "2d" ? StashFormat::Flat : StashFormat::VR;
  else return false;
  return true;
}

std::string stashSectionsText(const StashSettings& s) {
  std::string text;
  for (int i = 0; i < kStashSections; ++i) {
    const int section = s.sections[i];
    text += std::string(i ? "," : "") + (s.hiddenSections >> section & 1 ? "-" : "") +
            kSectionIds[section];
    const char* before = "(";
    for (const int g : s.subgroups)
      if (stashSectionOf(g) == section) {
        text += std::string(before) + (s.hiddenSubgroups >> g & 1 ? "-" : "") + kSubgroupIds[g];
        before = ",";
      }
    if (*before == ',') text += ')';
  }
  return text;
}

std::string stashSettingLines(const StashSettings& s) {
  std::ostringstream out;
  // The first mapping as older builds have it, the others after it.
  std::vector<StashFolderMap> folders;
  for (const StashFolderMap& m : s.folders)
    if (!m.from.empty() && m.from.find_first_of("\t\n") == std::string::npos &&
        m.to.find_first_of("\t\n") == std::string::npos)
      folders.push_back(m);
  const StashFolderMap first = folders.empty() ? StashFolderMap{} : folders.front();
  out << "stash=" << (s.enabled ? 1 : 0) << "\nstash_url=" << s.url
      << "\nstash_api_key=" << s.apiKey << "\nstash_path_from=" << first.from
      << "\nstash_path_to=" << first.to << "\n";
  for (size_t i = 1; i < folders.size(); ++i)
    out << "stash_folder=" << folders[i].from << '\t' << folders[i].to << "\n";
  if (!s.unverifiedHost.empty()) out << "stash_unverified_host=" << s.unverifiedHost << "\n";
  out << "stash_covers=" << (s.covers ? 1 : 0)
      << "\nstash_titles=" << (s.titles ? 1 : 0)
      << "\nstash_male_performers=" << (s.malePerformers ? 1 : 0)
      << "\nstash_write_back=" << (s.writeBack ? 1 : 0) << "\nstash_card=" << (s.card ? 1 : 0)
      << "\nstash_format="
      << (s.format == StashFormat::All ? "all" : s.format == StashFormat::Flat ? "2d" : "vr")
      << "\nstash_scenes_sort=" << kStashSortNames[static_cast<int>(s.scenesSort)]
      << "\nstash_scenes_descending=" << s.scenesDescending
      << "\nstash_performers_sort=" << stashSortName(s.performersSort)
      << "\nstash_performers_descending=" << s.performersDescending
      << "\nstash_studios_sort=" << stashSortName(s.studiosSort)
      << "\nstash_studios_descending=" << s.studiosDescending << "\n";
  // Only once changed, so a new default order reaches those who never did.
  if (s.sections != kStashDefaultSections || s.hiddenSections ||
      s.subgroups != kStashDefaultSubgroups || s.hiddenSubgroups)
    out << "stash_filters=" << stashSectionsText(s) << "\n";
  return out.str();
}
