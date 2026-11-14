#pragma once

// The menu's Stash pages (docs/stash.md): the navigation's Stash section,
// Home, the Scenes page with its filter rail, chips and full lists, the
// Performers and Studios pages and a page for each, the strip's Scene tab,
// and the Settings group with the server's editor. Laid out and drawn by
// StashPages, a friend of Menu, so the core menu has only a call here and
// there; its state is a StashMenu, kept for the session.

#include "menu.h"
#include "stash_catalog.h"
#include "stash_sync.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
std::vector<StashPin> loadStashPins();
void saveStashPins(const std::vector<StashPin>& pins);

// The catalog's filter and order for a list (its ids that the catalog has
// no longer are left out); `now` for the added-since span.
StashFilter stashFilterOf(const StashList& list, const StashCatalog& catalog, bool malePerformers,
                          int64_t now);
StashOrder stashOrderOf(const StashList& list, uint64_t seed);
// A pinned list's name, made from its filter: "VR · Emily Pink · anal".
std::string stashListName(const StashList& list, const StashCatalog* catalog);

// How a Stash button is drawn (Menu::Button::stashLook).
enum class StashLook {
  None,
  Tile,     // a scene in the grid: picture, stars and plays, title, studio · date, performers
  Row,      // a scene in the list: picture, title, performers, then its columns
  Facet,    // a rail or full list row: name and count; included blue (on), left out red (danger)
  Exclude,  // its ⊘ target beside it
  Chip,     // an active filter above the results: included blue, left out red-outlined with ⊘
  Link,     // "All tags …": blue text on a row
  Action,   // a header button: icon and label
  Nav,      // a navigation row with a Stash icon
  Field,    // a text field, its text from the left; outlined blue while typed into (on)
  // The Scene tab (menu_stash_scene.cpp): the strip's tab with Stash's mark;
  // a tag chip (label and size the count; on chosen, dim an outlined "+N
  // more"); a performer's portrait card (label her name, detail her age
  // then, stashId hers); a wide button with an icon, its label and size a
  // count on the right.
  Tab,
  TagChip,
  Portrait,
  Go,
  PerformerCard,  // a performer's card: portrait (2:3), name, then two lines; played count on it
  CoStar,         // a co-star on the performer page: round portrait, name, count; chosen blue (on)
  Studio,  // a studio's card: its logo (stashId), name and two lines
  Shelf,   // a Home shelf's heading: title, a dim note, and "See all" at the right
  Warning,  // a problem to act on: a red warning sign, then its label
};
enum class StashIcon { None, Stash, Home, Scenes, Performers, Studios, Pin, Shuffle, Filters, Ban,
                       Undo, Back };

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

// The Scene tab's page (menu_stash_scene.cpp): what it shows of the playing
// video, kept while the same video plays.
struct StashSceneTab {
  std::string path;  // the video the choices below are for
  int64_t scene = 0;  // ... and its Stash id (0: Stash doesn't know it)
  std::vector<int64_t> tags;  // chosen, by the first Stash id of each merged tag
  int tagOrder = 0;           // StashSceneStrings::tagOrders
  bool tagsOpen = false, detailsOpen = false, similar = false;
  double scroll = 0;     // the right column's position, kept while away
  double otherSide = 0;  // ... and the side column's of the page before (the rail)
  // Similar scenes, made when first shown (by catalog generation and scene,
  // and the scenes hidden away from home).
  uint64_t similarGeneration = ~0ull, similarAway = ~0ull;
  int64_t similarOf = 0;
  bool similarMale = false;
  std::vector<std::pair<int, std::string>> similarScenes;  // scene index, why
  // How many scenes the chosen tags give.
  uint64_t countGeneration = ~0ull;
  std::vector<int64_t> countTags;
  StashFormat countFormat = StashFormat::VR;
  int count = 0;
  // Edit tags (with write-back on), by the first Stash id of each merged
  // tag: those added, and those struck (to be removed, or an added one
  // taken back); nothing is sent until Save. `adding`: Add tags shows in
  // the column's place, `search` its find field's words.
  bool editing = false, adding = false, confirm = false, saving = false;
  std::vector<int64_t> added, struck;
  std::string summary;     // "2 added, 1 removed", as last laid out
  std::vector<int64_t> sentAdded, sentRemoved;  // the save under way
  std::string search;
  std::vector<int> found;  // Add tags' rows, tag indexes, made for:
  std::string foundFor;
  uint64_t foundGeneration = ~0ull;
  // Saved: shown over the catalog's tags until a sync after it, and said.
  int64_t savedScene = 0, savedAt = 0;
  std::vector<int64_t> savedAdded, savedRemoved;
  std::string savedNote;
  // What the page last showed, to redraw it when that changes.
  uint64_t shownCatalog = ~0ull, shownLibrary = ~0ull;
  int64_t shownSync = -1;
  int shownSave = -1;
  // Where the last layout put what its heading draws.
  double dividerY = 0, boxY = 0, boxH = 0;
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

// A performer's or a studio's page shows its scenes with the Scenes page's
// list, rail and results: the Scenes page's list is kept aside meanwhile,
// and the page's own while away.
struct StashSavedList {
  StashList list;
  std::optional<StashList> undo;
  double scroll = 0, rail = 0;
  bool fullByName = false;
  int64_t id = 0;  // the performer's or the studio's
  ListPlace place;
};

// The Performers page's state, and the Scenes page's while the performer
// page shows her scenes in its place.
struct StashPerformersState {
  StashPerformerChoice choice;
  std::string text;
  StashPerformerSort sort = StashPerformerSort::Scenes;
  bool descending = true;
  double scroll = 0, railPosition = 0;  // the grid's position (rows) and the rail's, while away
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
  std::optional<StashSavedList> scenes, her;
  // The performer page's profile: where it is, and whether a picture of it
  // is still coming (it is redrawn when one arrives).
  double profileX = 0, profileTop = 0, profileBottom = 0, coStarsTop = -1;
  bool waiting = false;
};

// The Home page's own state (menu_stash_home.cpp), and the Studios page's
// and a studio page's (menu_stash_studios.cpp).
struct StashHomePage;
struct StashStudiosPage;

// What the full list (More …) in place of the results lists: Look is a
// named look's values (StashMenu::fullLook), also in place of the
// Performers page's cards.
enum class StashFull { None, Performers, Tags, Studios, Years, Pins, Look };

// The server editor's sign-in fields, after the four StashSettings has.
constexpr int kStashUser = 4, kStashPassword = 5;

// The Stash pages' state for the session.
struct StashMenu {
  int64_t performer = 0, studio = 0;  // whose page shows, by Stash id
  StashList list;                 // what the Scenes page shows
  std::optional<StashList> undo;  // ... before the last change of its filters
  bool railShown = true;
  bool editSections = false;  // the rail lists its sections to show, hide and order
  bool gallery = true;
  double scroll = 0;  // the results' position (rows), kept while away or in a full list
  // ... and the results' place, put back once they are laid out again
  // (placePending).
  ListPlace place;
  bool placePending = false;
  StashFull full = StashFull::None;
  StashLookKind fullLook = StashLookKind::Hair;  // Look: which
  bool fullByName = false;  // the full list A–Z, else most first (years: oldest first, else newest)
  std::string fullSearch;
  // What the keyboard types into.
  enum class Typing { Search, FullSearch, PinName, Server, Performers, Studios, Tags } typing =
      Typing::Search;
  // Server: which of the fields, and their text: the URL, the API key,
  // Stash's folder and this computer's (as SetStashServer sends them), then
  // the user name and password to sign in with (never kept).
  int field = 0;
  std::array<std::string, 6> draft;
  bool notHere = false;  // the results say Stash's videos aren't here (layoutNotHere())
  int draftFolder = -1;  // the mapping (StashSettings::folders) drafts 2 and 3 are; -1: a new one
  bool showPassword = false;
  // Signing in (StashClient::signIn()) on Save: the result once it's done,
  // and why the last one failed.
  struct SignIn {
    bool ok = false, refused = false;
    bool untrusted = false;  // failed on a certificate that isn't trusted
    std::string host;        // ... the server's, as Save sent it
    std::string url;  // where Stash answered, if it did
    std::string apiKey, error;
  };
  std::future<SignIn> signingIn;
  std::string signInError;
  // The host whose certificate the fields accept without verifying it (as
  // StashSettings::unverifiedHost).
  std::string draftUnverified;
  std::vector<StashPin> pins;
  // A pinned list shown in the Scenes page's place (openPin()): the page's
  // own list meanwhile, and which pin it is (-1 once unpinned).
  std::optional<StashSavedList> ownScenes;
  int pin = -1;
  bool pinsLoaded = false;
  std::future<std::vector<StashPin>> pendingPins;
  uint64_t seed = 1;  // Random's order

  // The query and its rail, remade only when the list, the catalog or the
  // male performers setting changes; the history when the library's does.
  bool stale = true;
  uint64_t catalogGeneration = 0, libraryGeneration = ~0ull;
  std::vector<StashHistory> history;
  // Away from home: the scenes hidden (StashHistory::hidden), as of the
  // menu's offlineShapeGeneration_; counted as they change.
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

  StashSceneTab sceneTab;

  // What the pages last showed, to redraw them when that changes.
  uint64_t shownCatalog = ~0ull, shownLibrary = ~0ull;
  StashSettings settings;
  bool settingsTaken = false;  // the browsing format and sorts are taken once
  double railPosition = 0;   // the Scenes page's rail, while away
  StashPerformersState performers;
  std::string studioSearch;  // the Studios page's
  bool bandPending = false;  // a studio's logo was not there when its page was drawn
  std::shared_ptr<StashStudiosPage> studios;
  std::shared_ptr<StashHomePage> home;
  StashStatus status;
  long minute = -1;
  int renaming = -1;  // the pinned list whose name is typed

  // "Send my ratings and plays" (menu_stash_settings.cpp): open in place of
  // the Settings page, its dry run listed; what it last showed of the
  // write-back, to redraw when that changes.
  bool sendOpen = false, sendList = false;
  std::string shownWriteBack;

  // Where the last layout put what the heading draws.
  double titleX = 0, resultsX = 0, resultsTop = 0, fullTitleX = 0, fullTitleRight = 0;
};

struct StashPages {
  using Button = Menu::Button;
  using ListItem = Menu::ListItem;

  // Menu hooks.
  static void attach(Menu& m);  // setStash(): Stash is on
  // ... anew: what the Home and Studios pages made from the last one's
  // catalogs is made again.
  static void staleHome(Menu& m);
  static void staleStudios(Menu& m);
  static void update(Menu& m);  // each frame
  static void layoutNavigation(Menu& m, double x, double w, double y, double bottom);
  static void layoutCard(Menu& m, double x, double w, double bottom);  // "Use Stash?"
  static void layoutSettingsGroup(Menu& m, double top);
  static void layoutServerEditor(Menu& m);
  // Its certificate isn't trusted: Don't connect, or Connect without
  // verification (Dialog::StashCertificate, the host in dialogPath_).
  static void layoutCertificateDialog(Menu& m, std::vector<Menu::Button>& row);
  static void drawCertificateDialog(Menu& m, cairo_t* cr, double textX, double boxY);
  // The server's fields, with the keyboard; the folders' are those of the
  // mapping Stash's `folder` goes by (the first for "").
  static void editServer(Menu& m, int field, const std::string& folder = std::string());
  // The server's editor or the one-off send in place of the Settings page;
  // false if neither is open.
  static bool layoutSettingsPanel(Menu& m);
  static void layoutSendPanel(Menu& m);
  static void leavePage(Menu& m);  // showPage(): before another page shows
  // Menu::showListPage(): a Stash list page shown again as it was left;
  // false for none of Stash's.
  static bool showAgain(Menu& m, Menu::Page page);
  static void layoutPage(Menu& m);  // layoutBrowser(): the Stash page shown
  static void drawHeading(const Menu& m, cairo_t* cr);  // drawBrowser(): ... under its buttons
  static void drawButton(const Menu& m, cairo_t* cr, const Button& b, const double ink[4]);
  static void setText(Menu& m, const std::string& text);
  static MenuCommand submitText(Menu& m);
  static MenuCommand saveServer(Menu& m);     // Save and sync
  static MenuCommand serverCommand(Menu& m);  // ... the fields as SetStashServer takes them
  // Whether the server typed has its certificate accepted unverified.
  static bool draftUnverified(Menu& m);
  static void updateSignIn(Menu& m);  // each frame, while Stash is on (set up or not)
  static void stopEditing(Menu& m);
  // Where a page shows a Stash catalog that isn't there yet: what the sync
  // is doing (or that it failed), and while it runs that it runs in the
  // background, from (x, y) across w.
  static void drawWaiting(const Menu& m, cairo_t* cr, double x, double y, double w);
  static std::string playingTitle(const Menu& m, const std::string& fallback);

  // The strip's Scene tab and its page (menu_stash_scene.cpp).
  static void layoutSceneTab(Menu& m, double& x);
  static void showSceneDetails(Menu& m);
  static void leaveSceneDetails(Menu& m);
  static void updateSceneDetails(Menu& m);
  static void layoutSceneDetails(Menu& m);
  static void drawSceneDetailsHeading(const Menu& m, cairo_t* cr);
  static void drawSceneButton(const Menu& m, cairo_t* cr, const Button& b, const double ink[4]);

  // The Studios page and a studio's (menu_stash_studios.cpp).
  static void showStudios(Menu& m);
  static void leaveStudios(Menu& m);
  static void showStudio(Menu& m, int64_t id);
  static void layoutStudios(Menu& m);
  static void layoutStudio(Menu& m);
  static void drawStudiosHeading(const Menu& m, cairo_t* cr);
  static void drawStudioHeading(const Menu& m, cairo_t* cr);
  static void enterStudio(Menu& m);  // its scenes in the Scenes page's place
  static void leaveStudio(Menu& m);
  // The studio whose page shows (its scenes are always within it), else 0.
  static int64_t fixedStudio(const Menu& m);
  // How far its band moves the tool row, the rail and the results down.
  static double bandHeight(const Menu& m);
  // The list shown with its studio: what pinning keeps.
  static StashList scopedList(const Menu& m);
  // The format the Scenes page shows, and choosing it on another page.
  static StashFormat shownFormat(const Menu& m);
  static MenuCommand setFormat(Menu& m, StashFormat format);
  static void drawStudioCard(const Menu& m, cairo_t* cr, const Button& b, const double ink[4]);
  // A studio's logo fitted into the box (its name when it has none).
  static void drawLogo(const Menu& m, cairo_t* cr, const StashStudio& studio, double x, double y,
                       double w, double h, double alpha);

  // The Home page (menu_stash_home.cpp).
  static void showHome(Menu& m);
  static void leaveHome(Menu& m);
  static void layoutHome(Menu& m);
  static void drawHomeHeading(const Menu& m, cairo_t* cr);
  static void drawShelf(const Menu& m, cairo_t* cr, const Button& b, const double ink[4]);

  // The Performers and performer pages (menu_stash_performers.cpp).
  static void showPerformers(Menu& m);
  static void showPerformer(Menu& m, int64_t id);
  static void layoutPerformers(Menu& m);
  static void layoutPerformer(Menu& m);
  static void drawPerformersHeading(const Menu& m, cairo_t* cr);
  static void drawPerformerHeading(const Menu& m, cairo_t* cr);
  static void leavePerformers(Menu& m);
  static void enterPerformer(Menu& m);  // her scenes in the Scenes page's place
  static void leavePerformer(Menu& m);
  static const std::vector<std::array<StashPerformerStats, 3>>& performerStats(Menu& m);
  static void buildPerformers(Menu& m);
  // A change of the Performers page's choices, as a row's action.
  static std::function<void()> changePerformers(Menu& m,
                                                std::function<void(StashPerformerChoice&)> f);
  static void layoutPerformerGrid(Menu& m, double x0, double top);
  static void layoutProfile(Menu& m, int performer, double x, double top, double bottom);
  static void drawProfile(const Menu& m, cairo_t* cr, const StashPerformer& p);
  static void drawPerformerButton(const Menu& m, cairo_t* cr, const Button& b, const double ink[4]);
  // Her portrait filling the box (initials while none), top kept.
  static void drawPortrait(const Menu& m, cairo_t* cr, const StashPerformer& p, double x, double y,
                           double w, double h, double radius);
  // The performer whose page shows (her scenes always have her), else 0.
  static int64_t fixedPerformer(const Menu& m);
  // Her name, "with" the co-stars chosen.
  static std::string performerTitle(const Menu& m);

  // The Scenes page (menu_stash_scenes.cpp).
  static StashMenu& state(Menu& m) { return *m.stashMenu_; }
  static const StashCatalog* catalog(const Menu& m);
  // menu_stash_folders.cpp
  static double layoutFolders(Menu& m, double x, double y, double w, double labelW);
  static bool layoutNotHere(Menu& m, double x, double& y, double w);
  static void showScenes(Menu& m);
  // The Scenes page with its own list, in place of a pinned list's.
  static void showOwnScenes(Menu& m);
  static void leaveScenes(Menu& m);
  // The results' locations, and where they were left.
  static std::vector<std::string> resultPaths(Menu& m);
  static ListPlace resultsPlace(Menu& m);
  // The Scenes page showing `list` (Undo goes back to the one before), its
  // own list back first if a pinned list showed.
  static void openScenes(Menu& m, const StashList& list);
  static void layoutScenes(Menu& m);
  static void drawScenesHeading(const Menu& m, cairo_t* cr);
  // A performer's or a studio's scenes in the Scenes page's place: its list
  // put aside in `scenes`, and `own` brought back if it is the page's for
  // `id` (else a new list in the format chosen, and false); then put back.
  static bool enterList(Menu& m, std::optional<StashSavedList>& scenes,
                        const std::optional<StashSavedList>& own, int64_t id);
  static void leaveList(Menu& m, std::optional<StashSavedList>& scenes,
                        std::optional<StashSavedList>& own, int64_t id);
  // Takes a new catalog (the lists are stale) and the library's changed
  // history; true if either came.
  static bool takeChanges(Menu& m);
  static void refresh(Menu& m);
  static void changeList(Menu& m, const std::function<void(StashList&)>& change);
  static void buildRail(Menu& m);
  static void editSections(Menu& m, bool on);
  static void editSectionsRail(Menu& m);
  static void buildFull(Menu& m);
  static void layoutHeader(Menu& m, double x);
  static void layoutTools(Menu& m);
  // The rail, the chips and the results (or a full list) from `top` down,
  // between the page's left and `right`, and the keyboard while typing.
  static void layoutResults(Menu& m, double top, double right);
  // The tool row's parts, for the Performers page too.
  static double layoutSort(Menu& m, double x, const char* const* fields, int count, int chosen,
                           int descending, std::function<MenuCommand(int)> choose,
                           std::function<MenuCommand()> turn, const int* order = nullptr);
  static double layoutJumps(Menu& m);
  static void layoutSearch(Menu& m, double x, double right, const std::string& text,
                           const char* hint, StashMenu::Typing typing);
  static double layoutChips(Menu& m, double x, double right, double y);
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
  // The full list's title, over the page's heading.
  static void drawFullHeading(const Menu& m, cairo_t* cr);
  static void layoutRail(Menu& m, const std::vector<StashRailLine>& rail, double height,
                         double top, double bottom);
  static void layoutGrid(Menu& m, double x0, double top);
  static void layoutList(Menu& m, double x0, double top);
  static void layoutFull(Menu& m, double x0, double top);
  static void wantThumbnails(Menu& m, int first, int end, int page, double pictureW);
  static MenuCommand open(Menu& m, const std::string& path, bool shuffled);
  // Plays a scene's file with `scenes` as the list Previous and Next go through.
  static MenuCommand play(Menu& m, const std::vector<int>& scenes, const std::string& path,
                          bool shuffled);
  static int pinOfList(Menu& m);
  // The pinned list the Scenes page shows in place of its own, else -1.
  static int shownPin(Menu& m);
  static void openPin(Menu& m, int pin);
  // The Scenes page's own list back where it was left (the page shows).
  static void leavePin(Menu& m);
  static void showFull(Menu& m, StashFull full, StashLookKind look = StashLookKind::Hair);
  static Button facetButton(Menu& m, const std::string& label, const std::string& count, int state,
                            std::function<void()> include, double x, double y, double w, double h);
  static Button excludeButton(Menu& m, int state, std::function<void()> exclude, double x, double y,
                              double w, double h);
  static Button actionButton(Menu& m, const std::string& label, StashIcon icon,
                             std::function<MenuCommand()> action, double x, double y, double h);
  // A scene's tile, `plays` on its picture (its action still to be set);
  // and the stars over the picture's lower left corner (the tile is told
  // their rating, for its strip).
  static Button sceneTile(const Menu& m, const StashCatalog& c, const StashScene& s, int plays,
                          double x, double y, double w, double h);
  static Button tileStars(const Menu& m, Button& tile, const std::string& path, float rating);
  // Where the scene list's columns start, for a row from x0 to x1 (a narrow
  // one has no studio column).
  struct SceneColumns {
    double status, rating, plays, studio, released, length;
  };
  // Where a tile's last line is (its studio and date), for its tick.
  static double tileLastLine(const Button& tile) { return tile.y + 8 + (tile.w - 16) * 9 / 16 + 70; }
  static SceneColumns sceneColumns(double x0, double x1);
  // A scene's lines: its title (Stash's with the titles setting), its shown
  // performers, "studio · date", and its resolution badge.
  static std::string title(const Menu& m, const StashScene& s);
  static std::string performers(const Menu& m, const StashScene& s);
  static std::string studioDate(const StashCatalog& c, const StashScene& s);
  static std::string badge(const StashScene& s);
  // The picture of a scene's tile or row: its Stash cover with covers on,
  // else its frame thumbnail; on a tile, the studio's logo top right.
  static void drawPicture(const Menu& m, cairo_t* cr, const Button& b, double x, double y, double w,
                          double h, double radius);
  static void drawIcon(cairo_t* cr, StashIcon icon, double cx, double cy, double s,
                       const double rgba[4]);
  // On a card's picture: a badge from its top left corner ("VR · 8K"), and
  // the plays, a play mark and the count, to its lower right one.
  static void drawCaption(const Menu& m, cairo_t* cr, const std::string& caption, double x,
                          double y);
  static void drawPlays(const Menu& m, cairo_t* cr, const std::string& plays, double right,
                        double bottom);
};

// "3,454".
std::string stashNumber(long n);
// "2 min ago", "3 h ago", "4 days ago".
std::string stashAgo(int64_t when);
// What a sync under way is doing: "Downloading your Stash … 3.4 MB".
std::string stashSyncStep(const StashStatus& status);
// "1 play", "96 plays"; none for none.
std::string stashPlays(int plays);
// Texture pixels for `w` layout pixels: how wide to ask images() for a picture.
inline int stashPixels(double w) {
  return static_cast<int>(std::lround(w * menu_geometry::kRasterScale));
}
// The parts that aren't empty, with `between` between them.
std::string stashJoined(const std::vector<std::string>& parts, const char* between = " · ");

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
