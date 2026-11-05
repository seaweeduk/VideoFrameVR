#pragma once

#include "stash_data.h"
#include "stash_folders.h"
#include "stash_settings.h"
#include "video_format.h"

#include <array>
#include <climits>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Days since 1970-01-01, for dates; kStashNoDay where there is none.
constexpr int32_t kStashNoDay = INT32_MIN;
// A date "YYYY-MM-DD" (or "YYYY-MM", "YYYY": the 1st) in days; kStashNoDay
// if it can't be read.
int32_t stashDay(const std::string& date);
// Whole years from `born` to `day`; -1 if either is not known.
int stashAge(int32_t born, int32_t day);

// What Stash calls a performer's gender.
enum class StashGender : uint8_t { Unknown, Female, Male, TransFemale, TransMale, Intersex, NonBinary };
// Whether a performer is shown (in casts, lists, co-stars and facets): all
// but men, who are shown with the "male performers" setting on. Performers
// without a gender are shown.
bool stashShown(StashGender gender, bool malePerformers);

// The buckets the Scenes page filters by. Single-valued buckets use
// kStashNone when unknown. The values are bit numbers in saved masks
// (stashBit()); body shape returns a mask rather than a single bucket.
constexpr uint8_t kStashNone = 255;
// The cast, where every performer's gender is known (and one is listed):
// the women (every gender shown without the male performers setting but
// one without a gender) and the men credited. Men 2+ takes in 4+.
enum class StashWomen : uint8_t { One, Two, ThreePlus, kCount };
enum class StashMen : uint8_t { None, One, TwoPlus, FourPlus, kCount };
enum class StashCup : uint8_t { AB, C, D, DDPlus, kCount };              // from the measurements
enum class StashBreasts : uint8_t { Natural, Augmented, kCount };       // Stash's fake_tits
enum class StashAge : uint8_t { To22, To29, To39, From40, kCount };       // at the scene's date
enum class StashHeight : uint8_t { Under160, To169, To179, From180, kCount };  // cm
// Persisted bit numbers. Descriptors can overlap; Unclassified is an exclusive
// filter status for no supported descriptor, not another body type.
enum class StashShape : uint8_t { Hourglass = 0, Pear = 1, TopHeavy = 2, Straight = 3,
                                  Unclassified = 4, kCount };
enum class StashTattoos : uint8_t { Some, NoneRecorded, kCount };
// VR by the width of the frame (both eyes), 2D by its smaller side.
enum class StashResolution : uint8_t { VR8K, VR7K, VR6K, VR5K, VR4K, VRUpTo3K, Flat4K, Flat1080,
                                       Flat720, FlatSD, kCount };
enum class StashDuration : uint8_t { Under20, To40, To60, From60, kCount };  // minutes
enum class StashFrameRate : uint8_t { From55, From45, Under45, kCount };     // fps
enum class StashCodec : uint8_t { HEVC, H264, Other, kCount };
enum class StashFileSize : uint8_t { Under5, To10, To20, From20, kCount };  // GB
// The app's own history (all of those chosen).
enum class StashHistoryOption : uint8_t { Unplayed, Played, Rated4, Unrated, PlayedLongAgo, kCount };

template <typename E>
constexpr unsigned stashBit(E value) { return 1u << static_cast<unsigned>(value); }

StashAge stashAgeBand(int age);  // kCount for an age not known (or under 18)
// Estimated proportions of measurements such as "34D-24-36" (docs/stash.md).
// Returns a descriptor mask, possibly overlapping; 0 if none is supported.
// Does not return Unclassified: catalog construction assigns that filter status
// to 0 results. Missing, malformed and ambiguous measurements are not Straight.
unsigned stashShape(const std::string& measurements);

// The kinds of look the filters know (the Looks section and the Performers
// page). The first kStashNamedLooks are as Stash names them, each value an
// index into StashCatalog::lookNames(); the others are buckets: StashCup,
// StashBreasts, StashAge (at the scene's date), StashHeight, StashShape,
// StashTattoos. Stored Shape values are masks; filter choices remain indexes.
enum class StashLookKind : uint8_t { Hair, Ethnicity, Country, Eyes, Cup, Breasts, Age, Height,
                                     Shape, Tattoos, kCount };
constexpr int kStashLookKinds = static_cast<int>(StashLookKind::kCount);
constexpr int kStashNamedLooks = 4;
// How many buckets each kind after the named ones has.
constexpr int kStashLookBuckets[kStashLookKinds - kStashNamedLooks] = {
    4, 2, 4, 4, static_cast<int>(StashShape::kCount), 2};

// Stored looks are single indexes except Shape, which is a mask. Keep this
// distinction out of matching and counting, including overlapping shapes.
template <typename F>
void stashEachLookValue(StashLookKind kind, int stored, F use) {
  if (stored < 0) return;
  if (kind == StashLookKind::Shape) {
    for (int b = 0; b < static_cast<int>(StashShape::kCount); ++b)
      if ((stored >> b) & 1) use(b);
  } else {
    use(stored);
  }
}

struct StashPerformer {
  int64_t id = 0;
  std::string name, disambiguation;
  std::vector<std::string> aliases;
  StashGender gender = StashGender::Unknown;
  std::string birthdate;  // as Stash has it
  int32_t born = kStashNoDay;
  std::string country, ethnicity, hairColor, eyeColor;
  int heightCm = 0;  // 0 = not known
  std::string measurements, fakeTits;
  std::string tattoos, piercings, careerStart, careerEnd;
  // Per StashLookKind: the name's index or the bucket, except Shape is a
  // stashBit(StashShape) mask. -1 = not known; built Shape masks have either
  // supported descriptors or the exclusive Unclassified status, never 0.
  // Age, which depends on the scene, is always -1 here.
  std::array<int, kStashLookKinds> looks{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
  int look(StashLookKind kind) const { return looks[static_cast<size_t>(kind)]; }
  std::string image;  // URL, empty = none
  bool favorite = false;
  int scenes = 0;     // listed scenes with this performer
};

struct StashStudio {
  int64_t id = 0;
  std::string name;
  std::string image;          // logo URL, empty = none
  // The studio Stash has it under: named on its page, nothing more. Every
  // studio stands alone; its scenes are its own.
  int parent = -1;
  int scenes = 0;  // listed scenes
};

// A tag, or several Stash tags whose names differ only in case, spaces and
// punctuation ("Doggy Style", "doggystyle"), merged into one.
struct StashTag {
  std::string name;                // the most used one's
  std::vector<std::string> names;  // all of them, the most used first
  std::vector<int64_t> ids;        // Stash's, in the same order
  int scenes = 0;                  // listed scenes with any of them
};

struct StashGroup {
  int64_t id = 0;
  std::string name, date, image;
  int studio = -1;
};

// A Stash scene with a file on this computer, or played from Stash's stream.
struct StashScene {
  int64_t id = 0;
  std::string title, code, details, director;
  std::string date;           // release date, "YYYY-MM-DD"
  int32_t day = kStashNoDay;  // ... in days
  int64_t createdAt = 0;      // added to Stash, Unix seconds
  int studio = -1;
  std::vector<int> performers;  // all, by name; stashShown() picks those to show
  std::vector<int> tags;        // StashTag indexes, by name
  std::vector<std::pair<int, int>> groups;  // group, the scene's number in it (0 = none)
  std::vector<std::string> urls;
  std::string cover;  // URL, empty = none
  // Its file here: the first of its files (Stash's primary first) that is,
  // one on this device before one on a server. Without one, `path` is empty and
  // the rest is Stash's first file, which its stream serves.
  std::string path, stashPath, oshash;
  uint64_t size = 0;
  int64_t modified = 0;  // Unix seconds, as Stash last saw it
  int width = 0, height = 0;
  double duration = 0, fps = 0;
  std::string codec;
  std::vector<std::string> oshashes;  // of all its files
  // VR or 2D (docs/stash.md), by the first answer of: the format chosen in
  // the player, the frame's shape, the file name, Stash's VR tag.
  enum class VrBy : uint8_t { None, Own, Shape, Name, Tag };
  bool vr = false;
  VrBy vrBy = VrBy::None;

  std::string fileName() const {
    return path.empty() ? stashPath.substr(stashPath.find_last_of("/\\") + 1)
                        : path.substr(path.find_last_of('/') + 1);
  }
  // Stash's title, else the file name.
  std::string displayTitle() const { return title.empty() ? fileName() : title; }
};

// Included and excluded tags, performers or studios (indexes). A scene
// matches with all (or any) of `include` and none of `exclude`.
struct StashPick {
  std::vector<int> include, exclude;
  bool all = true;  // All of, else Any of (studios: always any of, a scene has one)
  bool active() const { return !include.empty() || !exclude.empty(); }
};

// What the Scenes page shows. Empty parts don't filter. Bucket masks
// (stashBit()) and lists are any of within their kind; kinds combine.
struct StashFilter {
  StashFormat format = StashFormat::All;
  StashPick tags, performers, studios;
  unsigned women = 0, men = 0;  // StashWomen, StashMen
  // Looks, per StashLookKind (names' indexes or buckets): a scene matches
  // if one performer of it who is not a man has all those chosen at once,
  // any of each kind's `include` and none of its `exclude`.
  std::array<StashPick, kStashLookKinds> looks;
  // Release date (days, inclusive; kStashNoDay = open on that side) and
  // added to Stash (Unix seconds, inclusive; 0 = open): a range leaves out
  // scenes without a date.
  int32_t releasedFrom = kStashNoDay, releasedTo = kStashNoDay;
  int64_t addedFrom = 0, addedTo = 0;
  unsigned resolutions = 0, durations = 0, frameRates = 0, codecs = 0, sizes = 0;
  unsigned history = 0;  // StashHistoryOption, all of; needs the history
  int64_t longAgo = 0;   // PlayedLongAgo: last played before this (Unix seconds)
  std::string text;      // all words in the title, file name, performers or studio
  // Facets: whether the performers facet lists men (and those without a
  // gender); and the times to count scenes added since.
  bool malePerformers = false;
  std::vector<int64_t> addedSince;
};

struct StashOrder {
  StashSort by = StashSort::Released;
  bool descending = true;  // newest, longest, largest, best, most first; Title: Z to A
  uint64_t seed = 0;       // Random: the same seed, the same order
};

// The app's own history of a scene's file (Library, made on the main
// thread), for the history filters and sorts; indexed like scenes().
struct StashHistory {
  float rating = 0;  // 0 = unrated
  int plays = 0;
  int64_t lastPlayedAt = 0;
  int8_t vr = -1;  // the format chosen in the player since the catalog was made: 1 VR, 0 2D, -1 none
  bool hidden = false;  // its server doesn't answer and there is no copy on this device
};

struct StashFacet {
  int index = 0;  // tag, performer, studio, a look's name or bucket
  int count = 0;
};

// A query's scenes in order, and per kind of filter how many scenes each
// choice would give: counted with that kind's own choices left out (Any of)
// or kept (All of, which only narrows), so a choice with a count never
// leads to nothing.
struct StashResult {
  std::vector<int> scenes;
  std::array<int, 3> formats{};  // StashFormat: All, VR, Flat
  std::vector<StashFacet> tags, performers, studios;  // most first, none with 0
  std::array<int, size_t(StashWomen::kCount)> women{};
  std::array<int, size_t(StashMen::kCount)> men{};
  std::array<std::vector<StashFacet>, kStashLookKinds> looks;  // per StashLookKind, most first
  std::vector<std::pair<int, int>> years;  // release year, scenes; newest first
  int undated = 0;
  std::vector<int> addedSince;  // per StashFilter::addedSince
  std::array<int, size_t(StashResolution::kCount)> resolutions{};
  std::array<int, size_t(StashDuration::kCount)> durations{};
  std::array<int, size_t(StashFrameRate::kCount)> frameRates{};
  std::array<int, size_t(StashCodec::kCount)> codecs{};
  std::array<int, size_t(StashFileSize::kCount)> sizes{};
  std::array<int, size_t(StashHistoryOption::kCount)> history{};
};

// The library's fingerprinted videos by oshash, with the format chosen for
// each in the player, if one was (Library::formatsByHash()).
using StashLibraryFormats = std::unordered_map<std::string, std::optional<VideoFormat>>;

// One of Stash's library folders as a build found it: Settings' folder
// rows, and what the folder finder looks for.
struct StashCatalogFolder {
  std::string folder;  // as Stash has it; "" for files in none of its folders (or not listed)
  std::vector<StashFolderFinder::File> samples;  // a few of its files, across its subfolders
  int scenes = 0;      // scenes whose first file is in it
  int listed = 0;      // ... with a file here
  int streamed = 0;    // ... listed without one, from Stash's stream
};

// What a build found, for the Settings page and --stash-sync.
struct StashCatalogStats {
  int stashScenes = 0;   // in Stash
  int listed = 0;        // ... with a file here
  int streamed = 0;      // ... listed without one, from Stash's stream
  int vr = 0;            // listed VR scenes
  std::array<int, 5> vrBy{};  // ... by StashScene::VrBy
  int stashTags = 0;     // before merging
  int libraryVideos = 0;  // fingerprinted videos of the library
  int libraryMatched = 0;  // ... with a file in Stash
  double checkMs = 0;    // looking for the files
  double buildMs = 0;    // the rest
};

// The Stash data the app browses: immutable, built on a worker thread from
// a sync or the local copy and handed to the main thread whole. Indexes
// (scenes, performers, studios, tags, groups) are its own, in name order
// (scenes by Stash id), valid for this catalog only. query() works on
// in-memory bit sets: a filter, its facet counts and its order over every
// scene take well under a millisecond.
class StashCatalog {
 public:
  // Looks for the scenes' files (Stash's paths mapped by `settings`) and
  // builds the catalog. Reads folders: worker threads only.
  static std::shared_ptr<const StashCatalog> build(const StashData& data,
                                                   const StashSettings& settings,
                                                   const StashLibraryFormats& library);

  const std::vector<StashScene>& scenes() const { return scenes_; }
  // Stash's library folders with scenes, in its order, then "" for scenes
  // in none of them (all of them when the copy has no folders listed).
  const std::vector<StashCatalogFolder>& folders() const { return folders_; }
  const std::vector<StashPerformer>& performers() const { return performers_; }
  const std::vector<StashStudio>& studios() const { return studios_; }
  const std::vector<StashTag>& tags() const { return tags_; }
  const std::vector<StashGroup>& groups() const { return groups_; }
  // The names a named kind of look has (StashPerformer::looks indexes),
  // each spelled as first found; those equal but for case, spaces and
  // punctuation are one.
  const std::vector<std::string>& lookNames(StashLookKind kind) const {
    return lookNames_[static_cast<size_t>(kind)];
  }
  const StashCatalogStats& stats() const { return stats_; }
  const std::string& server() const { return server_; }
  const std::string& version() const { return version_; }
  int64_t syncedAt() const { return syncedAt_; }

  // -1 if there is none.
  int sceneById(int64_t id) const;
  int sceneByOshash(const std::string& oshash) const;
  int sceneByPath(const std::string& path) const;  // any of its files' local paths
  int performerById(int64_t id) const;
  int studioById(int64_t id) const;
  int tagById(int64_t id) const;  // the merged tag the Stash tag is in

  // Stash's oshash for a local file, if Stash has a file at that path with
  // exactly this size and modification time (seconds); else empty. For the
  // library, which then need not read the file.
  std::string oshashFor(const std::string& path, uint64_t size, int64_t modified) const;
  // Stash's path of the file at a local path, for deleting it
  // (docs/alpha/delete-file.md); empty if none, or if Stash has it more
  // than once (several scenes, or two of its paths here).
  std::string stashPathOf(const std::string& path) const;

  // The scenes matching `filter` in `order`, with the facet counts (unless
  // `facets` is false). `history`, if given, must have one entry per scene.
  StashResult query(const StashFilter& filter, const StashOrder& order,
                    const std::vector<StashHistory>* history = nullptr,
                    bool facets = true) const;

  // Improved v1: weighted tag overlap, credited cast counts, small identity
  // and studio bonuses. Same format only, duplicate groups collapsed.
  std::vector<int> similarScenes(int scene, bool malePerformers, size_t limit = 12) const;

 private:
  StashCatalog() = default;
  void index();
  void indexSimilarity();  // worker-only, after index() has built the tag bit sets

  struct LocalFile {
    int scene = -1;
    uint64_t size = 0;
    int64_t modified = 0;
    std::string oshash;
    std::string stashPath;  // Stash's path of it
    bool shared = false;    // Stash has it more than once (several scenes): `scene` the last
  };
  // A performer of a scene as the Looks filter sees her: her looks, her
  // age then among them; -1 = not known. Shape is a mask, as in StashPerformer.
  struct Look {
    std::array<int16_t, kStashLookKinds> values;
  };

  std::vector<StashScene> scenes_;
  std::vector<StashCatalogFolder> folders_;
  std::vector<StashPerformer> performers_;
  std::vector<StashStudio> studios_;
  std::vector<StashTag> tags_;
  std::vector<StashGroup> groups_;
  std::array<std::vector<std::string>, kStashNamedLooks> lookNames_;
  StashCatalogStats stats_;
  std::string server_, version_;
  int64_t syncedAt_ = 0;

  std::unordered_map<int64_t, int> sceneById_, performerById_, studioById_, tagById_;
  std::unordered_map<std::string, int> sceneByOshash_;
  std::unordered_map<std::string, LocalFile> files_;  // by local path

  // For query(): a bit per scene, per tag, performer and studio and for VR;
  // each scene's tags, performers and studio in flat lists, for counting;
  // each scene's
  // buckets; its performers who are not men, with their looks; the text a
  // search looks through; and the scenes in each fixed order (those without
  // the value last).
  size_t words_ = 0;  // 64-bit words per bit set
  std::vector<uint64_t> tagBits_, performerBits_, studioBits_, vrBits_;
  struct Lists {
    std::vector<uint32_t> start;  // per scene, into items, and one past the last
    std::vector<int32_t> items;
  };
  Lists sceneTags_, scenePerformers_, sceneStudios_;
  std::vector<uint8_t> hidden_;  // per performer: a man
  // The cast's women and men (counted up to 3 and 4), kStashNone where a
  // gender is not known or nobody is listed.
  std::vector<uint8_t> women_, men_, duration_, frameRate_, codec_, size_;
  // Its resolution as a 2D and as a VR video: a format chosen in the player
  // picks one.
  std::array<std::vector<uint8_t>, 2> resolutions_;
  std::vector<int16_t> year_;  // 0 = no date
  int16_t firstYear_ = 0, lastYear_ = 0;
  std::vector<Look> looks_;
  std::vector<uint32_t> lookStart_;  // per scene, into looks_, and one past the last
  std::vector<std::string> haystack_;
  std::vector<int> titleRank_;
  std::array<std::vector<double>, 2> similarityWeights_;  // 2D, VR
  std::vector<double> similarityMass_;  // sum of each scene's tag weights
  std::vector<int> similarityGroup_;    // duplicate group, for recommendations only
  struct Order {
    std::vector<int> scenes;
    size_t known = 0;  // those with the value come first
  };
  std::array<Order, 8> orders_;  // Released, Added, Title, Duration, Resolution, CastSize, Modified, Size
};
