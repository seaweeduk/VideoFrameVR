#pragma once

#include <string>
#include <string_view>
#include <vector>

// What's new: the release notes built in from CHANGELOG.md (its format is in
// that file and cmake/changelog.cmake), and whether they open after an
// update. The notes stay English.

// One release's notes for this build's device.
struct ReleaseNotes {
  struct Section {
    std::string name;                // "New", "Improved", "Fixed" or "Removed"
    std::vector<std::string> lines;  // without "- " and the device's marker
  };
  std::string version;            // "0.2.0"
  std::string date;               // "9 November 2026"
  std::vector<Section> sections;  // in that order, only those with lines
};

// The releases of a changelog, newest first, with the lines for the Frame
// (frame) or else the desktop: those unmarked and those marked for it. A
// release with none isn't listed. What is out of format is left out.
std::vector<ReleaseNotes> parseReleaseNotes(std::string_view changelog, bool frame);
// <0, 0 or >0 as a is older, the same or newer, by number ("0.10.0" is
// newer than "0.9.0"); a suffix such as "-dev" doesn't count.
int compareVersions(std::string_view a, std::string_view b);
// A development build's version: any but a release's plain "0.2.0", such as
// "0.2.0-dev", "dev-<time>-<commit>" or "local-<time>-<commit>".
bool devVersion(std::string_view version);
// "2026-11-09" as "9 November 2026"; anything else as it is.
std::string releaseDate(std::string_view isoDate);
// Those newer than `after` (empty: all) and not newer than `upTo` (empty:
// no limit), newest first.
std::vector<ReleaseNotes> releasesBetween(const std::vector<ReleaseNotes>& releases,
                                          std::string_view after, std::string_view upTo);

// The built-in notes for this build's device (the Frame on kStandaloneHeadset).
const std::vector<ReleaseNotes>& builtInReleaseNotes();
// Settings → Help's What's new: every release up to this build's version (a
// development build: all). Empty: no notes for this device.
std::vector<ReleaseNotes> allReleaseNotes();
// Those newer than `after`, up to this build's version.
std::vector<ReleaseNotes> releaseNotesSince(std::string_view after);

// Decided at launch, from settings.conf's version_seen and show_whats_new.
struct WhatsNewLaunch {
  // To show, newest first, the first time the menu opens; empty: nothing
  // opens. Closing it counts this version as seen (markVersionSeen).
  std::vector<ReleaseNotes> releases;
  bool seenNow = false;  // nothing to show: this version counts as seen at once
};
// newProfile: no settings.conf and no library.db (first_run.h). seen: the
// highest version seen, empty if none was saved (0.1.x saved none: they get
// this version's notes).
WhatsNewLaunch whatsNewAtLaunch(const std::vector<ReleaseNotes>& releases, std::string_view version,
                                bool newProfile, std::string_view seen, bool showAfterUpdates);
// ... with the built-in notes and this build's version.
WhatsNewLaunch whatsNewAtLaunch(bool newProfile, std::string_view seen, bool showAfterUpdates);
// `seen` becomes `version` if that is newer; a development build's isn't
// kept. Whether it changed (and is to be saved).
bool markVersionSeen(std::string& seen, std::string_view version);
bool markVersionSeen(std::string& seen);  // this build's version
