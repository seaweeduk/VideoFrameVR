#pragma once

// The Stash pages' browsing (docs/stash.md), apart from the menu that
// shows it: the Scenes page's list (the query, its filter rail, the active
// filters as chips, a full list's rows), which the performer's and the
// studio's pages share; the Performers page's statistics, cards and rail;
// and Home's shelves. Made from the catalog, the app's history and the
// scenes hidden away from home, and made again only when one of them or a
// choice changes. What a row does is the menu's (StashBrowse::Host).

#include "menu_command.h"
#include "server_kind.h"
#include "stash_catalog.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Library;
class Offline;
class Stash;

// "3,454".
std::string stashNumber(long n);
// A printf pattern filled in, std::string arguments as their text.
inline const char* stashArg(const std::string& s) { return s.c_str(); }
template <typename T>
T stashArg(T value) {
  return value;
}
template <typename... Args>
std::string stashPrinted(const char* pattern, const Args&... args) {
  char buf[512];
  std::snprintf(buf, sizeof(buf), pattern, stashArg(args)...);
  return buf;
}

// Looks chosen (StashLookKind), on the Scenes page and the Performers page:
// the named kinds as Stash names them, included (any of) or left out; the
// buckets as stashBit() masks, any of within each kind.
struct StashLooks {
  std::array<std::vector<std::string>, kStashNamedLooks> in, out;
  std::array<unsigned, kStashLookKinds - kStashNamedLooks> masks{};

  bool operator==(const StashLooks& o) const { return in == o.in && out == o.out && masks == o.masks; }
  bool active() const;
  unsigned& mask(StashLookKind kind) { return masks[static_cast<size_t>(kind) - kStashNamedLooks]; }
  unsigned mask(StashLookKind kind) const {
    return masks[static_cast<size_t>(kind) - kStashNamedLooks];
  }
  // A name included, or left out; again, taken away.
  void include(int kind, const std::string& name);
  void exclude(int kind, const std::string& name);
  // The catalog's filter for them.
  void addTo(StashFilter& filter, const StashCatalog& catalog) const;
};

// A look's text: its heading ("Hair"), a value's rail row ("Blonde hair",
// "Cup D", "18–22") and its chip ("Blonde hair", "Cup D", "Age 18–22"): a
// named kind's `name`, another kind's `bucket`.
const char* stashLookHeading(int kind);
std::string stashLookRow(int kind, const std::string& name, int bucket = -1);
std::string stashLookChip(int kind, const std::string& name, int bucket = -1);

// A Scenes list: what it shows and in which order, by Stash's ids and names
// (a catalog's indexes are its own, and the catalog changes with each sync),
// so it lasts the session and can be pinned. Empty parts don't filter.
struct StashList {
  StashFormat format = StashFormat::VR;
  // Included and left out; tags by any of the Stash ids of a merged tag.
  std::vector<int64_t> tags, tagsOut, performers, performersOut, studios, studiosOut;
  bool tagsAll = true, performersAll = true;  // All of, else Any of
  unsigned women = 0, men = 0;  // stashBit() masks, as StashFilter's
  StashLooks looks;
  int year = 0;       // released that year, 0 = any
  int addedDays = 0;  // added to Stash in the last so many days, 0 = any
  unsigned resolutions = 0, durations = 0, frameRates = 0, codecs = 0, sizes = 0, history = 0;
  std::string text;  // the search's words
  StashSort sort = StashSort::Released;
  bool descending = true;

  bool operator==(const StashList& o) const;
  bool operator!=(const StashList& o) const { return !(*this == o); }
};

struct StashPin {
  std::string name;
  StashList list;
};

// configDir()/stash_lists.conf: the pinned lists (user data, kept as made).
// Each kind of server has its own (their ids are the server's): Stash's in
// stash_lists.conf as ever, another's in <its id>_lists.conf.
std::vector<StashPin> loadStashPins(ServerKind kind);
void saveStashPins(const std::vector<StashPin>& pins, ServerKind kind);

// The catalog's filter and order for a list (its ids that the catalog has
// no longer are left out); `now` for the added-since span.
StashFilter stashFilterOf(const StashList& list, const StashCatalog& catalog, bool malePerformers,
                          int64_t now);
StashOrder stashOrderOf(const StashList& list, uint64_t seed);
// A pinned list's name, made from its filter: "VR · Emily Pink · anal".
std::string stashListName(const StashList& list, const StashCatalog* catalog);

// One line of the filter rail, made when the results change; laid out only
// where it shows.
struct StashRailLine {
  enum Kind { Heading, Sub, Row, Link, Note, Gap } kind = Row;
  std::string label, count;
  int state = 0;                           // Row: 1 included, 2 left out
  std::function<void()> include, exclude;  // exclude empty: none offered
  // Heading of performers or tags with two or more included: All of / Any of.
  int mode = -1;  // -1 none, else 1 = All of chosen, 0 = Any of
  std::function<void(bool)> setAll;
  // Editing the rail's sections (StashMenu::editSections): a Row is a
  // switch, `command` its action (and a Link's), up and down its moves. A
  // subgroup's is indented under its section, inactive while that's hidden.
  std::function<MenuCommand()> command, up, down;
  bool subgroup = false, inactive = false;
  int lines = 1;  // the switch's label, wrapped when it doesn't fit beside the moves
  double height = 0;
};

// An active filter above the results; tapping it takes it away.
struct StashChip {
  std::string label;
  bool out = false;
  std::function<void(StashList&)> remove;
};

// The Performers page's choices (docs/stash.md): looks (her age: in one of
// her scenes) and the app's history, any of within each kind, and a search
// of names and aliases.
enum class StashPerformerHistory : uint8_t { Played, Unplayed, Rated4, kCount };
struct StashPerformerChoice {
  StashLooks looks;
  unsigned history = 0;  // stashBit() mask
  bool active() const { return looks.active() || history; }
};

// What the app's history says of a performer: over her scenes here in a
// format.
struct StashPerformerStats {
  int scenes = 0;
  unsigned ages = 0;  // stashBit(StashAge) of her ages in them
  int played = 0, plays = 0, rated = 0;  // scenes played, plays, scenes rated
  double ratingSum = 0;
  int32_t newest = kStashNoDay;  // her newest scene's release date
  int64_t lastPlayed = 0;
  double average() const { return rated ? ratingSum / rated : 0; }
};

// What the Performers page lists, and the Scenes page while the performer
// page shows her scenes in its place (StashPerformersState).
struct StashPerformersBrowse {
  StashPerformerChoice choice;
  std::string text;
  StashPerformerSort sort = StashPerformerSort::Scenes;
  bool descending = true;
  // Remade when the choices change, or the catalog, the library or the
  // format (the Scenes page's) does.
  bool stale = true;
  uint64_t catalogGeneration = ~0ull, libraryGeneration = ~0ull, awayGeneration = ~0ull;
  StashFormat format = StashFormat::VR;
  uint64_t statsCatalog = ~0ull, statsLibrary = ~0ull, statsAway = ~0ull;
  // Per catalog performer, per StashFormat.
  std::vector<std::array<StashPerformerStats, 3>> stats;
  std::vector<int> shown;       // the performers listed, in order
  int all = 0;                  // ... and without the choices and search
  std::array<int, 3> formats{};  // performers listed per StashFormat, with them
  int theirScenes = -1;                    // scenes with the chosen looks, -1 = not offered
  std::array<std::vector<StashFacet>, kStashLookKinds> looks;  // the rail's counts, most first
  std::vector<StashRailLine> rail;
  double railHeight = 0;
};

// What the full list (More …) in place of the results lists: Look is a
// named look's values (StashMenu::fullLook), also in place of the
// Performers page's cards.
enum class StashFull { None, Performers, Tags, Studios, Years, Pins, Look };

struct StashHomePage;

// What the Scenes page lists, with its choices; StashMenu is one.
struct StashBrowse {
  // What it is made from: Stash (its catalog, history and hidden scenes),
  // the library (its generation) and what is away from home (Offline, as
  // of the menu's downloads_.shapeGeneration).
  struct Sources {
    const Stash* stash = nullptr;
    const Library* library = nullptr;
    const Offline* offline = nullptr;
    uint64_t offlineShape = 0;
    const StashCatalog* catalog() const;
  };
  // The page it is made for: the settings, whose page shows (a performer's
  // or a studio's, by Stash id; else 0), and whether it is the Performers
  // page (a look's full list in place of its cards).
  struct Page {
    const StashSettings& settings;
    int64_t performer = 0, studio = 0;
    bool cards = false;
  };
  // What the rows do, set by the menu with Stash (StashPages::attach()): the
  // catalog when a row is tapped (by ids: it may be newer than the one
  // laid out), a change of the list or of the Performers page's choices,
  // a full list or the rail's sections shown, a pinned list opened; and
  // how many lines a switch's label takes on the rail being edited.
  struct Host {
    std::function<const StashCatalog*()> catalog;
    std::function<void(const std::function<void(StashList&)>&)> changeList;
    std::function<void(const std::function<void(StashPerformerChoice&)>&)> changePerformers;
    std::function<void(StashFull, StashLookKind)> showFull;
    std::function<void(bool)> editSections;
    std::function<void(int)> openPin;
    std::function<int(const std::string& label, double indent)> switchLines;
  };
  Host host;

  StashList list;                 // what the Scenes page shows
  std::optional<StashList> undo;  // ... before the last change of its filters
  bool editSections = false;  // the rail lists its sections to show, hide and order
  StashFull full = StashFull::None;
  StashLookKind fullLook = StashLookKind::Hair;  // Look: which
  bool fullByName = false;  // the full list A–Z, else most first (years: oldest first, else newest)
  std::string fullSearch;
  std::vector<StashPin> pins;
  uint64_t seed = 1;  // Random's order

  // The query and its rail, remade only when the list, the catalog or the
  // male performers setting changes; the history when the library's does.
  bool stale = true;
  uint64_t catalogGeneration = 0, libraryGeneration = ~0ull;
  std::vector<StashHistory> history;
  // Away from home: the scenes hidden (StashHistory::hidden), as of the
  // menu's downloads_.shapeGeneration; counted as they change.
  std::vector<bool> hidden;
  uint64_t offlineShape = ~0ull, awayGeneration = 0;
  StashFilter filter;
  StashResult result;
  std::vector<StashRailLine> rail;
  double railHeight = 0;
  // The full list's rows (indexes into the facet), remade with it.
  struct FullRow {
    std::string label, count;
    int state = 0;
    std::function<void()> include, exclude;
  };
  std::vector<FullRow> fullRows;
  bool fullStale = true;
  std::shared_ptr<StashHomePage> home;

  // Takes a new catalog (the lists are stale) and the library's changed
  // history; true if either came.
  bool takeChanges(const Sources& src);
  void refresh(const Sources& src, const Page& page);
  void buildRail(const StashCatalog& c, const Page& page);
  void editSectionsRail(const Page& page);
  void buildFull(const Sources& src, const Page& page, const StashPerformersBrowse& performers);
  // The chips of a list, in the rail's order; `performer`: her page's (always her).
  std::vector<StashChip> chips(const StashList& l, const StashCatalog& c, int64_t performer) const;
  // How many scenes the list gives.
  int count(const StashList& list, const StashCatalog& c, bool male) const;
  const std::vector<std::array<StashPerformerStats, 3>>& performerStats(StashPerformersBrowse& ps,
                                                                        const Sources& src);
  void buildPerformers(StashPerformersBrowse& ps, const Sources& src, bool male);
  // A change of the Performers page's choices, as a row's action.
  std::function<void()> changePerformers(std::function<void(StashPerformerChoice&)> f);
  // The heights of a rail's lines; their sum.
  static double railHeights(std::vector<StashRailLine>& rail);
  // The rail's rows for some kinds of look, each under a heading (or a
  // sub-heading): what is chosen first, then the values `counts` has (most
  // first; buckets in their order), never one in every one of `total` or in
  // none; a named kind with more than it lists ends in a link to them all.
  // `change` makes a row's action from a change of the looks.
  static void lookRail(std::vector<StashRailLine>& rail, const StashCatalog& c,
                       const StashLooks& looks,
                       const std::array<std::vector<StashFacet>, kStashLookKinds>& counts,
                       int total, const std::vector<StashLookKind>& kinds, bool headings,
                       const std::function<std::function<void()>(
                           std::function<void(StashLooks&)>)>& change,
                       const std::function<void(StashLookKind)>& more);
};

// A rail's (and a full list's) row step, and how much a wrapped switch
// label adds to it.
constexpr double kStashRowStep = 54, kStashLineGrow = 30;

// The Home page's shelves (menu_stash_home.cpp), made when the page is
// opened and again for a new catalog, the format or changed pins.
struct StashHomePage {
  enum Shelf { kNew, kTopRated, kTopPerformers, kRandom, kShelves };
  static constexpr int kPerformerColumns = 8;  // your top performers
  struct SceneShelf {
    StashList list;          // what See all opens
    std::vector<int> all;    // its scenes, for Previous / Next
    int recent = 0;          // new in Stash: added in the last kNewDays
  };
  struct Performer {
    int index = 0, plays = 0, played = 0, rated = 0;  // played: her scenes played
    double stars = 0;
    double score() const { return plays + stars; }
  };
  // What the shelves were made from.
  uint64_t catalog = ~0ull, seed = 0, visits = 0, away = ~0ull;
  StashFormat format = StashFormat::VR;
  bool male = false, history = false;
  std::vector<StashPin> pins;
  bool stale = true;
  double scroll = 0;  // where it was left

  std::array<SceneShelf, kShelves> shelves;
  std::vector<Performer> performers;
  std::vector<long> pinCounts;
  std::array<int, 3> formats{};  // scenes per StashFormat

  // `generation`: the catalog's (Stash::generation()); `male`: the male
  // performers setting.
  void rebuild(const StashBrowse& st, const StashCatalog& c, uint64_t generation,
               StashFormat format, bool male);
};
