#include "whats_new.h"

#include "changelog.h"
#include "platform.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>

namespace {

constexpr std::array<const char*, 4> kSections = {"New", "Improved", "Fixed", "Removed"};
constexpr std::string_view kDot = " \xC2\xB7 ";  // " · " between a release's version and date

bool startsWith(std::string_view text, std::string_view prefix) {
  return text.substr(0, prefix.size()) == prefix;
}

bool digits(std::string_view text, size_t count) {
  return text.size() == count &&
         std::all_of(text.begin(), text.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

// "0.2.0": numbers between dots.
bool releaseVersion(std::string_view text) {
  if (text.empty() || text.front() == '.' || text.back() == '.') return false;
  for (size_t i = 0; i < text.size(); ++i)
    if (!std::isdigit(static_cast<unsigned char>(text[i])) && !(text[i] == '.' && text[i - 1] != '.'))
      return false;
  return true;
}

std::vector<unsigned long> versionNumbers(std::string_view version) {
  std::vector<unsigned long> numbers;
  unsigned long n = 0;
  bool any = false;
  for (char c : version) {
    if (std::isdigit(static_cast<unsigned char>(c))) {
      n = std::min(n * 10 + static_cast<unsigned long>(c - '0'), 1000000000ul);
      any = true;
    } else if (c == '.' && any) {
      numbers.push_back(n);
      n = 0;
      any = false;
    } else {
      break;
    }
  }
  if (any) numbers.push_back(n);
  return numbers;
}

std::string_view thisVersion() { return VIDEOFRAMEVR_VERSION; }

}  // namespace

int compareVersions(std::string_view a, std::string_view b) {
  const auto x = versionNumbers(a), y = versionNumbers(b);
  for (size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
    const unsigned long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
    if (p != q) return p < q ? -1 : 1;
  }
  return 0;
}

bool devVersion(std::string_view version) { return version.find("-dev") != std::string_view::npos; }

std::string releaseDate(std::string_view iso) {
  static constexpr const char* kMonths[] = {"January", "February", "March",     "April",
                                            "May",     "June",     "July",      "August",
                                            "September", "October", "November", "December"};
  if (iso.size() != 10 || iso[4] != '-' || iso[7] != '-' || !digits(iso.substr(0, 4), 4) ||
      !digits(iso.substr(5, 2), 2) || !digits(iso.substr(8, 2), 2))
    return std::string(iso);
  const int month = (iso[5] - '0') * 10 + (iso[6] - '0');
  const int day = (iso[8] - '0') * 10 + (iso[9] - '0');
  if (month < 1 || month > 12 || day < 1 || day > 31) return std::string(iso);
  return std::to_string(day) + " " + kMonths[month - 1] + " " + std::string(iso.substr(0, 4));
}

std::vector<ReleaseNotes> parseReleaseNotes(std::string_view text, bool frame) {
  struct Release {
    std::string version, date;
    std::array<std::vector<std::string>, kSections.size()> lines;
  };
  std::vector<Release> releases;
  bool inRelease = false, comment = false;
  int section = -1;
  while (!text.empty()) {
    const size_t end = text.find('\n');
    std::string_view line = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

    if (comment) {
      comment = line.find("-->") == std::string_view::npos;
      continue;
    }
    if (startsWith(line, "<!--")) {
      comment = line.find("-->", 4) == std::string_view::npos;
      continue;
    }
    if (startsWith(line, "## ")) {
      const std::string_view heading = line.substr(3);
      const size_t dot = heading.find(kDot);
      inRelease = dot != std::string_view::npos && releaseVersion(heading.substr(0, dot));
      section = -1;
      if (inRelease)
        releases.push_back({std::string(heading.substr(0, dot)),
                            releaseDate(heading.substr(dot + kDot.size())), {}});
    } else if (startsWith(line, "### ")) {
      const auto found = std::find(kSections.begin(), kSections.end(), line.substr(4));
      section = inRelease && found != kSections.end() ? static_cast<int>(found - kSections.begin()) : -1;
    } else if (startsWith(line, "- ") && section >= 0) {
      std::string_view note = line.substr(2);
      if (startsWith(note, "[Frame] ")) {
        if (!frame) continue;
        note.remove_prefix(8);
      } else if (startsWith(note, "[Desktop] ")) {
        if (frame) continue;
        note.remove_prefix(10);
      } else if (startsWith(note, "[")) {
        continue;  // an unknown device
      }
      while (!note.empty() && std::isspace(static_cast<unsigned char>(note.back()))) note.remove_suffix(1);
      if (!note.empty() && !std::isspace(static_cast<unsigned char>(note.front())))
        releases.back().lines[section].emplace_back(note);
    } else if (startsWith(line, "#")) {
      inRelease = false;
      section = -1;
    }
  }

  std::vector<ReleaseNotes> out;
  for (Release& r : releases) {
    ReleaseNotes notes{std::move(r.version), std::move(r.date), {}};
    for (size_t i = 0; i < kSections.size(); ++i)
      if (!r.lines[i].empty()) notes.sections.push_back({kSections[i], std::move(r.lines[i])});
    if (!notes.sections.empty()) out.push_back(std::move(notes));
  }
  std::stable_sort(out.begin(), out.end(), [](const ReleaseNotes& a, const ReleaseNotes& b) {
    return compareVersions(a.version, b.version) > 0;
  });
  return out;
}

std::vector<ReleaseNotes> releasesBetween(const std::vector<ReleaseNotes>& releases,
                                          std::string_view after, std::string_view upTo) {
  std::vector<ReleaseNotes> out;
  for (const ReleaseNotes& r : releases)
    if ((after.empty() || compareVersions(r.version, after) > 0) &&
        (upTo.empty() || compareVersions(r.version, upTo) <= 0))
      out.push_back(r);
  return out;
}

const std::vector<ReleaseNotes>& builtInReleaseNotes() {
  static const std::vector<ReleaseNotes> notes = parseReleaseNotes(kChangelog, kStandaloneHeadset);
  return notes;
}

std::vector<ReleaseNotes> allReleaseNotes() {
  return releasesBetween(builtInReleaseNotes(), {}, devVersion(thisVersion()) ? std::string_view() : thisVersion());
}

std::vector<ReleaseNotes> releaseNotesSince(std::string_view after) {
  return releasesBetween(builtInReleaseNotes(), after, thisVersion());
}

WhatsNewLaunch whatsNewAtLaunch(const std::vector<ReleaseNotes>& releases, std::string_view version,
                                bool newProfile, std::string_view seen, bool showAfterUpdates) {
  WhatsNewLaunch out;
  if (devVersion(version)) return out;
  if (newProfile || (!seen.empty() && compareVersions(version, seen) <= 0) || !showAfterUpdates) {
    out.seenNow = newProfile || !showAfterUpdates;
    return out;
  }
  if (seen.empty()) {
    for (const ReleaseNotes& r : releases)
      if (compareVersions(r.version, version) == 0) out.releases.push_back(r);
  } else {
    out.releases = releasesBetween(releases, seen, version);
  }
  out.seenNow = out.releases.empty();
  return out;
}

WhatsNewLaunch whatsNewAtLaunch(bool newProfile, std::string_view seen, bool showAfterUpdates) {
  return whatsNewAtLaunch(builtInReleaseNotes(), thisVersion(), newProfile, seen, showAfterUpdates);
}

bool markVersionSeen(std::string& seen, std::string_view version) {
  if (devVersion(version) || (!seen.empty() && compareVersions(version, seen) <= 0)) return false;
  seen = version;
  return true;
}

bool markVersionSeen(std::string& seen) { return markVersionSeen(seen, thisVersion()); }
