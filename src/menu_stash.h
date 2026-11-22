#pragma once

// The menu's Stash pages (docs/stash.md): the navigation's Stash section,
// Home, the Scenes page with its filter rail, chips and full lists, the
// Performers and Studios pages and a page for each, the strip's Scene tab,
// and the Settings group with the server's editor. Laid out and drawn by
// StashPages, nested in Menu, so the core menu has only a call here and
// there; its state is a StashMenu, kept for the session.

#include "menu.h"
#include "media_server.h"
#include "menu_stash_browse.h"
#include "stash_catalog.h"
#include "stash_sync.h"
#include "stash_write_back.h"

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

// How a Stash button is drawn (Menu::Button::look of a PartKind::Stash).
enum class StashLook {
  None,
  // Over a scene's list row, not clickable: its plays, studio, release
  // date and length (`plays`, `size`, `date`, `caption`) in their columns,
  // and the downloaded tick.
  SceneColumns,
  // A rail or full list row: name and count, flush; included in the accent
  // (latched), left out red with ⊘ on a faint red (dim).
  Facet,
  Exclude,  // its ⊘ target beside it: flush, red while pointed; red while on
  Chip,     // an active filter above the results: included latched, left out red with ⊘
  Link,     // "All tags …": text in the accent on a row
  Action,   // a button with an icon before its label (or only a label), its padding by its height
  Field,    // a text field (a search's icon first), its text from the left; ringed while typed into (on)
  // The Scene tab (menu_stash_scene.cpp): the strip's tab with Stash's mark;
  // a tag chip (label and size the count; latched chosen, dim a link such
  // as "+N more"); a wide button with an icon, its label and size a count
  // on the right.
  Tab,
  TagChip,
  Go,
  PerformerCard,  // a performer's card: portrait (2:3), name, then two lines; played count on it
  CoStar,         // a co-star on the performer page: round portrait, name, count; chosen blue (on)
  Studio,  // a studio's card: its logo (stashId), name and two lines
  Shelf,   // a Home shelf's heading: title, a dim note, and "See all" at the right
  Warning,  // a problem to act on: a red warning sign, then its label
};
enum class StashIcon { None, Stash, Scenes, Performers, Studios, Pin, Shuffle, Filters, Ban,
                       Undo, Back };

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

// The Performers page's state: its model's, where its grid and rail were
// left, the Scenes page's list while the performer page shows her scenes in
// its place, and where her profile is.
struct StashPerformersState : StashPerformersBrowse {
  double scroll = 0, railPosition = 0;  // the grid's position (rows) and the rail's, while away
  std::optional<StashSavedList> scenes, her;
  // The performer page's profile: where it is, and whether a picture of it
  // is still coming (it is redrawn when one arrives).
  double profileX = 0, profileTop = 0, profileBottom = 0, coStarsTop = -1;
  bool waiting = false;
};

// The Studios page's own state and a studio page's (menu_stash_studios.cpp).
struct StashStudiosPage;

// The server editor's sign-in fields, after the address and the API key.
constexpr int kStashUser = 2, kStashPassword = 3;

// The Stash pages' state for the session: what they browse (StashBrowse),
// and how they show it.
struct StashMenu : StashBrowse {
  int64_t performer = 0, studio = 0;  // whose page shows, by Stash id
  bool railShown = true;
  bool gallery = true;
  double scroll = 0;  // the results' position (rows), kept while away or in a full list
  // ... and the results' place, put back once they are laid out again
  // (placePending).
  ListPlace place;
  bool placePending = false;
  // What the keyboard types into.
  enum class Typing { Search, FullSearch, PinName, Server, Performers, Studios, Tags } typing =
      Typing::Search;
  // Server: which of the fields, and their text: the URL and the API key
  // (as SetStashServer sends them), then the user name and password to
  // sign in with (never kept).
  int field = 0;
  std::array<std::string, 4> draft;
  bool notHere = false;  // the results say Stash's videos aren't here (layoutNotHere())
  // The kind of server the fields are for: the one in use, or one chosen
  // that has none yet.
  ServerKind draftKind = ServerKind::Stash;
  bool showPassword = false;
  // Signing in (StashClient::signIn()) on Save: the result once it's done,
  // and why the last one failed.
  using SignIn = StashSignIn;
  std::future<SignIn> signingIn;
  std::string signInError;
  // The last Save found another kind at the address than its fields were
  // for, and that one was set up: its name, said in Settings until the
  // section is left or changed.
  std::string foundOther;
  // The host whose certificate the fields accept without verifying it (as
  // StashSettings::unverifiedHost).
  std::string draftUnverified;
  // A pinned list shown in the Scenes page's place (openPin()): the page's
  // own list meanwhile, and which pin it is (-1 once unpinned).
  std::optional<StashSavedList> ownScenes;
  int pin = -1;
  bool pinsLoaded = false;
  std::future<std::vector<StashPin>> pendingPins;
  // The kind of server all of this was chosen on (attach()): the pins are
  // its own, as are the ids in every list.
  ServerKind kind = ServerKind::Stash;

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
  StashStatus status;
  long minute = -1;
  int renaming = -1;  // the pinned list whose name is typed

  // "Send my ratings and plays" (menu_stash_settings.cpp): open in place of
  // the Settings page, its dry run listed; what it last showed of the
  // write-back, to redraw when that changes.
  bool sendOpen = false, sendList = false;
  bool shownWriteBack = false;  // there was one: its status, and its send's state and error
  StashWriteBackStatus shownWriteBackStatus;
  StashSendPlan::State shownSendState = StashSendPlan::State::None;
  std::string shownSendError;

  // Where the last layout put what the heading draws.
  double titleX = 0, titleRight = 0, resultsX = 0, resultsTop = 0, fullTitleX = 0, fullTitleRight = 0;
  // The scene list's columns: how much further apart their headings need
  // them, in this language (StashPages::sceneApart()).
  struct {
    const Strings* strings = nullptr;
    std::array<double, 3> apart{};
  } sceneApart;
};

// What Settings' Stash section shows (StashPages::settingsShown()), kept
// apart from its layout so that the debug fit check can show every row.
struct StashSettingsShown {
  bool enabled = false;
  int chosen = 0;  // the choice of server: 0 none, else 1 + the kind's place in mediaServers()
  std::string foundOther;  // the last Save set up another kind than was chosen
  bool connected = false;  // Stash runs (Menu::stash_); else only its server
  ServerAbilities abilities;  // what the server set up can do: rows it can't use aren't shown
  std::string url, apiKey;
  // A row of Stash's library folders: its field, what was found about it,
  // and the other places its videos are in, each with Use.
  struct Use {
    std::string text, tooltip, folder;
    StashFolderMap map;
    bool first = false;  // where they are, not where they are too: the one to take
  };
  struct Folder {
    std::string folder;  // as Stash has it
    std::string name;    // ... as said above its field; empty: nothing is
    std::string here;    // where its videos play from here; empty: from Stash
    std::string fromServer;  // what its field says then
    std::string tooltip;
    std::vector<std::string> lines;
    std::vector<Use> uses;
  };
  // None before the first sync has listed them.
  std::vector<Folder> folders;
  std::string looking;  // the search for them under way
  // The connection: its lamp (Lamp, menu_parts.h), what it says, and what
  // the copy in use holds.
  int lamp = 0;
  std::string state, counts;
  bool syncing = false, signIn = false, certificate = false, firstSync = false;
  bool covers = false, titles = false, malePerformers = false;
  bool writeBack = false;
  std::string writeBackLine;  // what waits or was sent, and what went wrong
};

struct Menu::StashPages {
  using Button = Menu::Button;
  using ListItem = Menu::ListItem;

  // Menu hooks.
  static void attach(Menu& m);  // setStash(): Stash is on
  // ... anew: what the Home and Studios pages made from the last one's
  // catalogs is made again.
  static void staleHome(Menu& m);
  static void staleStudios(Menu& m);
  static void update(Menu& m);  // each frame
  static void layoutNavigation(Menu& m, double y, double bottom);
  static void layoutCard(Menu& m, double x, double w, double bottom);  // "Use Stash?"
  // Settings' section (menu_stash_settings.cpp): which kind of server is
  // used, then with one its server and its state, its folders, and sending
  // to it.
  static StashSettingsShown settingsShown(Menu& m);
  static void layoutSettings(Menu& m, Menu::PageLayout& page, const StashSettingsShown& shown);
#ifndef NDEBUG
  static StashSettingsShown fullestSettings();  // every row it can have, for the fit check
#endif
  static void layoutServerEditor(Menu& m);
  // Its certificate isn't trusted: Don't connect, or Connect without
  // verification (Dialog::StashCertificate, the host in dialogPath_).
  static void layoutCertificateDialog(Menu& m, std::vector<Menu::Button>& row);
  // A kind's server fields, with the keyboard: the one in use, or `kind`.
  static void editServer(Menu& m, int field);
  static void editServer(Menu& m, int field, ServerKind kind);
  // The section's choice of server: none (-1), or a kind by its place in
  // mediaServers().
  static MenuCommand chooseServer(Menu& m, int index);
  // The server's editor or the one-off send in place of the Settings page;
  // false if neither is open.
  static bool layoutSettingsPanel(Menu& m);
  static void layoutSendPanel(Menu& m);
  // Settings' section left, or another server chosen in it: a Save under
  // way saves nothing, and what the last one found is no longer said.
  static void leaveSettings(Menu& m);
  // Menu::showListPage(): a Stash list page shown again as it was left;
  // false for none of Stash's.
  static bool showAgain(Menu& m, Menu::Page page);
  static void drawButton(const Menu& m, cairo_t* cr, const Button& b, const double ink[4]);
  static void setText(Menu& m, const std::string& text);
  static MenuCommand submitText(Menu& m);
  static MenuCommand saveServer(Menu& m);     // Save and sync
  // ... the fields as SetStashServer takes them; `other`: found to be
  // another kind than they were for.
  static MenuCommand serverCommand(Menu& m, bool other = false);
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
  static void layoutSceneTab(Menu& m, Menu::StripLayout& strip);
  static void showSceneDetails(Menu& m);
  static void leaveSceneDetails(Menu& m);
  static void updateSceneDetails(Menu& m);
  static void layoutSceneDetails(Menu& m);
  struct SceneDetails;  // its parts
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
  static void layoutHomeHeader(Menu& m, const StashHomePage& p, const StashCatalog* c);
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
  // A change of the Performers page's choices.
  static void changePerformers(Menu& m, const std::function<void(StashPerformerChoice&)>& f);
  static void layoutPerformerGrid(Menu& m, double x0, double top);
  static void layoutProfile(Menu& m, int performer, double x, double top, double bottom);
  static void drawProfile(const Menu& m, cairo_t* cr, const StashPerformer& p);
  static void drawPerformerButton(const Menu& m, cairo_t* cr, const Button& b, const double ink[4]);
  static double performerCardHeight(double w, int lines = 3);  // a card w wide (StashLook::PerformerCard)
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
  // What the browsing (StashBrowse) is made from, and for which page.
  static StashBrowse::Sources sources(const Menu& m);
  static StashBrowse::Page listPage(const Menu& m);
  // What its rows do (attach()).
  static StashBrowse::Host browseHost(Menu& m);
  // menu_stash_folders.cpp
  // The Folders rows Settings shows (settingsShown()).
  static void shownFolders(Menu& m, StashSettingsShown& shown);
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
  static void changeList(Menu& m, const std::function<void(StashList&)>& change);
  static void editSections(Menu& m, bool on);
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
  // A scene's card, the lists' own (Menu::addGallery()'s): its picture with
  // `plays` and `rating` on it, its title, performers, then studio · date ·
  // length (no studio where its mark is on the picture); w wide and
  // cardHeight(w) high, its action still to be set.
  static Button sceneCard(const Menu& m, const StashCatalog& c, const StashScene& s, int plays,
                          float rating, double x, double y, double w);
  static double cardHeight(double w);
  // The card and what goes on it: its download, its badge ("VR · 8K") and
  // its stars.
  static void addSceneCard(Menu& m, const Button& card, const StashScene& s);
  // Where the scene list's columns start, for a row ending at x1.
  struct SceneColumns {
    double status, rating, plays, released, length;
  };
  static SceneColumns sceneColumns(const Menu& m, double x1);
  static const std::array<double, 3>& sceneApart(const Menu& m);
  static constexpr double kReleasedColumnW = 130, kLengthColumnW = 90;
  // A button with an icon before its label (StashLook::Action), by its
  // height: its ends, its icon and the gap after it.
  struct ActionSpace {
    double pad, icon, gap;
  };
  static ActionSpace actionSpace(double h) {
    return h < 48 ? ActionSpace{16, 22, 8} : ActionSpace{20, 28, 12};
  }
  // A scene's lines: its title (Stash's with the titles setting), its shown
  // performers, "studio · date", and its resolution badge.
  static std::string title(const Menu& m, const StashScene& s);
  static std::string performers(const Menu& m, const StashScene& s);
  static std::string badge(const StashScene& s);
  static void drawIcon(cairo_t* cr, StashIcon icon, double cx, double cy, double s,
                       const double rgba[4]);
};
using StashPages = Menu::StashPages;  // as its own files name it

// "2 min ago", "3 h ago", "4 days ago".
std::string stashAgo(int64_t when);
// What a sync under way is doing: "Downloading your Stash … 3.4 MB".
std::string stashSyncStep(const StashStatus& status);
// "1 play", "96 plays"; none for none.
std::string stashPlays(int plays);
// "1 scene", "96 scenes".
std::string stashScenes(long scenes);
// Texture pixels for `w` layout pixels: how wide to ask images() for a picture.
inline int stashPixels(double w) {
  return static_cast<int>(std::lround(w * menu_geometry::kRasterScale));
}
// The parts that aren't empty, with `between` between them.
std::string stashJoined(const std::vector<std::string>& parts, const char* between = " · ");
