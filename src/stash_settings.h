#pragma once

#include <array>
#include <string>
#include <vector>

// The Scenes rail's sections (StashStrings::sections), and the order they
// show in until someone changes it.
enum StashSection {
  kSectionPerformers, kSectionTags, kSectionStudios, kSectionCast, kSectionLooks,
  kSectionReleased, kSectionFormat, kSectionHistory, kStashSections
};
constexpr std::array<int, kStashSections> kStashDefaultSections = {
    kSectionPerformers, kSectionTags,     kSectionStudios, kSectionCast,
    kSectionLooks,      kSectionReleased, kSectionFormat,  kSectionHistory};

// The subgroups of Cast, Looks, Released and Format, each section's
// together, and the order they show in within it until someone changes it
// (each section's own in its own place).
enum StashSubgroup {
  kSubWomen, kSubMen,
  kSubHair, kSubCup, kSubBreasts, kSubAge, kSubEthnicity, kSubCountry, kSubHeight, kSubEyes,
  kSubShape, kSubTattoos,
  kSubAdded, kSubYear,
  kSubResolution, kSubDuration, kSubFrameRate, kSubCodec, kSubFileSize,
  kStashSubgroups
};
constexpr std::array<int, kStashSubgroups> kStashDefaultSubgroups = {
    kSubWomen, kSubMen,
    kSubHair, kSubCup, kSubBreasts, kSubAge, kSubEthnicity, kSubCountry, kSubHeight, kSubEyes,
    kSubShape, kSubTattoos,
    kSubAdded, kSubYear,
    kSubResolution, kSubDuration, kSubFrameRate, kSubCodec, kSubFileSize};
constexpr int stashSectionOf(int subgroup) {
  return subgroup < kSubHair         ? kSectionCast
         : subgroup < kSubAdded      ? kSectionLooks
         : subgroup < kSubResolution ? kSectionReleased
                                     : kSectionFormat;
}

// Which scenes the Stash pages list: all, VR (and 360°) only, or flat 2D only.
enum class StashFormat { All, VR, Flat };

enum class StashSort { Released, Added, Title, Duration, Resolution, Rating, Plays, LastPlayed,
                       CastSize, Random, Modified, Size };
enum class StashPerformerSort { Scenes, Name, Plays, Rating, Newest };
enum class StudioSort { Plays, Scenes, Newest, Name, Rating };

inline constexpr const char* kStashSortNames[] = {"released", "added", "title", "duration", "resolution",
                                                "rating", "plays", "played", "cast", "random",
                                                "modified", "size"};
const char* stashSortName(StashPerformerSort sort);
const char* stashSortName(StudioSort sort);

// One folder mapping (docs/stash.md, "Folders"): Stash's paths that start
// with `from` start with `to` here.
struct StashFolderMap {
  std::string from;  // Stash's side, as Stash writes it: /media/x, D:\Videos, \\NAS\share
  std::string to;    // this device's: a local path or a remote locator (remote.h)
  bool operator==(const StashFolderMap& o) const { return from == o.from && to == o.to; }
};

// A Windows path: a drive's (D:\...) or a UNC one (\\server\share\...).
bool stashWindowsPath(const std::string& path);
// Whether Stash's `path` is `folder` or in it, at a folder boundary; a
// Windows one compared without regard to case and with \ and / alike.
// `rest`: what follows the folder, '/'-separated ("/VR/a.mp4"), "" for the
// folder itself.
bool stashUnder(const std::string& path, const std::string& folder, std::string* rest = nullptr);
// Where Stash's `folder` (its library folder, or any) is here by `map`, as
// for its files; "" if it isn't under map.from.
std::string stashMapped(const StashFolderMap& map, const std::string& path);
// `list` with `map` as the one for Stash's library folder `folder`: made
// that folder's own (from = folder) so it alone decides there, and the
// mappings inside the folder dropped. An empty `folder` (an older copy,
// without Stash's folders): `map` alone.
std::vector<StashFolderMap> stashFoldersWith(std::vector<StashFolderMap> list,
                                             const std::string& folder, StashFolderMap map);

// The Stash integration's preferences (docs/stash.md), kept in settings.conf
// as stash_* keys. Off by default: with it off no Stash code runs.
struct StashSettings {
  bool enabled = false;
  std::string url;     // the server, without /graphql; empty: not set up
  std::string apiKey;  // empty: the server needs none
  // The host (stashHost()) whose certificate the user chose to accept
  // without verifying it, when it wasn't trusted; empty: none. It applies
  // only while `url` names that host.
  std::string unverifiedHost;
  // Where Stash's folders are here, a file going by the mapping with the
  // longest `from` it is in. None: the same paths on both. The first is
  // stash_path_from / stash_path_to (as older builds had one), the others
  // stash_folder lines.
  std::vector<StashFolderMap> folders;
  bool covers = true;           // a Stash cover replaces the frame thumbnail
  bool titles = true;           // a Stash title replaces the file name
  bool malePerformers = false;  // men are listed too
  bool writeBack = false;       // ratings and plays are sent to Stash
  bool card = false;  // the menu's "Use Stash?" card (first-run setup's Not now), until used or dismissed
  StashFormat format = StashFormat::VR;
  // The Scenes rail: its sections in order, and those hidden (bit per
  // section). A hidden section's filter still applies and shows as a chip.
  std::array<int, kStashSections> sections = kStashDefaultSections;
  unsigned hiddenSections = 0;
  // ... and the subgroups the same way, each section's in order in its own
  // place (as in kStashDefaultSubgroups).
  std::array<int, kStashSubgroups> subgroups = kStashDefaultSubgroups;
  unsigned hiddenSubgroups = 0;
  StashSort scenesSort = StashSort::Released;
  StashPerformerSort performersSort = StashPerformerSort::Scenes;
  StudioSort studiosSort = StudioSort::Plays;
  bool scenesDescending = true, performersDescending = true, studiosDescending = true;

  // The local path of a Stash path, as its mapping gives it ("" if the path
  // is in none of them).
  std::string localPath(const std::string& stashPath) const;
  // The mapping a Stash path goes by (the longest `from` it is in), or null.
  const StashFolderMap* mappingFor(const std::string& stashPath) const;
  // Whether the server's certificate is accepted without being verified:
  // `url` names unverifiedHost.
  bool unverified() const;
};

// settings.conf: reads one stash_* key (false if the key is not one of
// them), and the lines that keep them.
bool readStashSetting(const std::string& key, const std::string& value, StashSettings& s);
// The Scenes rail's sections and subgroups as stash_filters= keeps them.
std::string stashSectionsText(const StashSettings& s);
std::string stashSettingLines(const StashSettings& s);
