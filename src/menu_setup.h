#pragma once

// First-run setup's panel (docs/alpha/first-run.md): Welcome, then the steps
// with their dots, Back and Set up later; and the cues shown while it is out
// of view. Laid out and drawn by SetupPages, a friend of Menu; its state is a
// SetupMenu, from Menu::startSetup() to Menu::endSetup().

#include "cairo_canvas.h"
#include "menu.h"
#include "setup_places.h"

#include <map>
#include <set>

class StashClient;

// SteamVR's glyphs on setup's pages, in layout pixels.
constexpr int kSetupGlyph = 56;

// Add folders while it shows (menu_setup_folders.cpp): the places on the
// left, the chosen one's folder tree on the right as far as it is opened,
// and the page after Add.
struct SetupFolders {
  enum class Page { Pick, Added };
  Page page = Page::Pick;
  struct Place {
    enum class Kind { Home, Card, Usb, Drive, Computer };
    Kind kind = Kind::Home;
    std::string root;  // a folder; a computer's smb://host
    std::string name;
  };
  std::vector<Place> places;  // as laid out
  std::string shown;          // the place shown (its root); empty: Add a computer
  std::vector<std::string> hosts;  // computers added by their name or address (remote::root())
  std::string computer;            // ... typed
  // ... an SFTP one: its port and user name, and the field typed into (0
  // the name, 1 the port, 2 the user name).
  bool sftp = false;
  std::string port = "22", user;
  int field = 0;
  setup_places::Async<std::vector<RemovableDrives::Drive>> finding;
  std::vector<RemovableDrives::Drive> drives;
  // The folders read or being read, by path; those open.
  struct Folder {
    setup_places::Async<setup_places::Folder> reading;
    bool read = false;
    setup_places::Folder listing;
  };
  std::map<std::string, Folder> folders;
  std::set<std::string> open;
  std::string reveal;  // opened: scrolled into view once read
  // The tree's rows as laid out, in its own pixels from its top (drawn
  // scrolled by `scroll`): a folder, or a line under an opened one.
  struct Row {
    std::string path, name, line;
    int depth = 0;
    bool folder = true, opened = false, chevron = false, failed = false;
    // A card's or drive's own first row (ticked as a whole, opens nothing);
    // File system's, which can't be ticked.
    bool whole = false, tickable = true;
    double y = 0, h = 0;
  };
  std::vector<Row> rows;
  double height = 0;  // all of them
  // A list that scrolls, by the stick or dragged (the tree; after Add, what
  // was added), in layout pixels; its box.
  double scroll = 0, scrollMax = 0;
  double drawnScroll = -1;     // ... as the texture has it, -1 not yet drawn
  double scrollRemainder = 0;  // less than a texture pixel, kept for the next move
  double top = 0, bottom = 0, left = 0, right = 0;
  std::map<std::string, double> scrolled;  // each place's tree, while another shows
  struct Drag {
    bool pressed = false, moving = false;
    double pressY = 0, lastY = 0;
    std::function<MenuCommand()> action;  // the row pressed: clicked if let go unmoved
  } drag;
};

// Do you use Stash?, its address and its sign-in (menu_setup_stash.cpp).
struct SetupStash {
  enum class Page { Ask, Address, SignIn };
  Page page = Page::Ask;
  // Typed: the address, the user name and the password.
  enum Field { Address, User, Password };
  std::string text[3];
  int field = Address;
  bool showPassword = false;
  // Connect's request, then Sign in's, on a thread of their own; Back drops
  // it (and stops Connect's).
  struct Reply {
    int answer = 0;  // a StashClient::Answer
    std::string url;  // Connect's: where Stash answered
    std::string apiKey, error;
  };
  setup_places::Async<Reply> asking;
  std::shared_ptr<StashClient> client;
  std::string error;  // under the fields
};

// Your controllers (menu_setup_controls.cpp): the bindings, SteamVR's
// pictures for them (drawn on a thread of their own) and the labels as laid
// out.
struct SetupControls {
  ControllerBindings bindings;
  std::shared_ptr<const ControllerArt> art;
  setup_places::Async<std::shared_ptr<const ControllerArt>> loading;
  uint32_t held = 0;  // heldBit()s
  struct Point {
    int hand;
    double x, y;  // on the panel
  };
  struct Label {
    int hands = 0;    // a bit per hand
    int control = 0;  // its heldBit() control
    int column = 0;   // 0 the left controller's, 1 either's, 2 the right one's
    std::string glyph, title, line;
    double x = 0, y = 0, w = 0, h = 0;
    double want = 0;  // the height its title would have, beside its points
    std::vector<Point> points;
    bool lit = false;
  };
  std::vector<Label> labels;
  bool picture = false;  // with SteamVR's drawings, else a list
  double drawX[2] = {0, 0}, drawY[2] = {0, 0};
  ControllerArt::Image ring;  // round a held control, in the accent
  std::array<double, 3> ringAccent{};
};

struct SetupMenu {
  enum class Step { Welcome, Controls, Folders, Stash };
  Step step = Step::Welcome;
  bool controller = false;  // one is tracked
  SetupControls controls;
  bool holdWrites = false;  // --first-run: logins remembered are saved at the end
  std::vector<std::string> places;  // ticked: pinned once it ends
  std::vector<std::string> logins;  // servers to remember at the end (holdWrites; remote::root())
  std::unique_ptr<SetupFolders> folders;  // while Add folders shows
  SetupStash stash;
  CairoCanvas canvas;       // the panel, and its keyboard under it
  // Parts to draw again (the keyboard's keys, a field typed into), in
  // layout pixels; the whole of it with Menu::dirty_.
  struct Box {
    int x0, y0, x1, y1;
  };
  std::vector<Box> damage;
  // A whole page is drawn into the canvas's spare image a band of rows a
  // frame, the one before shown until it is done: the next band's first row
  // (-1 when none), and the rows it draws. The rows the image shown has (the
  // keyboard's too while typing).
  int row = -1, rowsEnd = 0;
  int rowsShown = Menu::kSetupHeight;
  CairoCanvas arrow, label, hint, mirror;  // Menu::SetupCue
  bool cuesDrawn = false;   // ... in the language shown
  // Welcome's intro (setup_intro.h, Menu::setupIntroPiece()): due until it
  // plays, once a run; its layers drawn once, before it, and dropped after.
  struct Intro {
    bool due = true;
    std::unique_ptr<CairoCanvas> backdrop;  // the panel with nothing on it
    std::unique_ptr<CairoCanvas> pieces;    // the icon's parts and the name, side by side
    std::unique_ptr<CairoCanvas> black;
  } intro;
};

// The hints after setup (menu_hints.cpp): SteamVR's glyphs for the bound
// controls, read on a thread of their own, and the line shown.
struct MenuHints {
  ControllerBindings bindings;
  std::shared_ptr<const ControllerArt> art;
  setup_places::Async<std::shared_ptr<const ControllerArt>> loading;
  CairoCanvas canvas;
  bool ready = false;  // the canvas
};

struct SetupPages {
  // Lays the page out and starts drawing all of it (it changes only on
  // clicks), or draws the parts damaged.
  static void redraw(Menu& m);
  // The next band of a whole page, every frame until it is shown: 128 rows,
  // 2–6 ms on the Steam Frame, where all of it took 25.
  static void drawBand(Menu& m, int rows = 128);
  static void drawCues(Menu& m);
  // Every frame: Add folders' drives and listings, the Stash step's answer.
  static void update(Menu& m);
  static void drivesChanged(Menu& m, const std::vector<std::string>& removed);
  // The keyboard's Done, and a sign-in that went through or didn't.
  static MenuCommand submit(Menu& m);
  // The field typed into (Menu::setupText()), and typing in it.
  static std::string* typed(Menu& m);
  static void setText(Menu& m, const std::string& text);
  static void signedIn(Menu& m);
  static void signInFailed(Menu& m);
  // An SFTP computer's connect or sign-in ended (Menu::sftpDone()) with a
  // form to show: in its pane, or Add a computer's (Menu::SmbForm::Kind::
  // AddServer) with what was typed and why it failed.
  static void showSftp(Menu& m);
  // Menu::browserChanged() while setup shows: only that part is drawn again.
  static void damaged(Menu& m, double y0, double y1, double x0, double x1);
  // A list on the page that scrolls (Add folders' tree, what was added): the
  // stick, and a press on it that drags it or, let go unmoved, clicks its row.
  static bool scrolls(const Menu& m);
  static void scroll(Menu& m, double rows);
  static bool press(Menu& m);
  static MenuCommand drag(Menu& m, bool held);
  // Your controllers: SteamVR's pictures for the bindings, read again; a
  // label lit by what is held.
  static void loadControls(Menu& m);
  static bool lit(const SetupControls::Label& label, uint32_t held);
  // A one-line hint as a pill centred in w × h: each glyph (SteamVR's
  // image, else a ring with its letter, else the trigger's icon; nothing
  // without either), then the line.
  struct PillGlyph {
    cairo_surface_t* image = nullptr;
    std::string letter;
    bool trigger = false;
  };
  static void drawPill(Menu& m, cairo_t* cr, double w, double h, const std::vector<PillGlyph>& glyphs,
                       const std::string& line);
  // The glyph of the first of `sources` (`fallback`, a glyph's name, if
  // none): SteamVR's, else its letter or the trigger.
  static PillGlyph pillGlyph(const ControllerArt* art, const std::vector<ControllerBindings::Source>& sources,
                             ControlAction action, bool upDown, const std::string& fallback);

 private:
  static void layout(Menu& m);
  static void draw(Menu& m, cairo_t* cr);
  static void setStep(Menu& m, SetupMenu::Step step);
  static void drawHead(Menu& m, cairo_t* cr, const char* title, const std::string& line,
                       double width = 1100);
  static void drawDots(Menu& m, cairo_t* cr);
  // Whether a box (with some room around it) reaches into the part being
  // drawn: text out of it isn't laid out at all.
  static bool shows(cairo_t* cr, double x, double y, double w, double h);
  // Your controllers (menu_setup_controls.cpp).
  static void updateControls(Menu& m);
  static void layoutControls(Menu& m);
  static void drawControls(Menu& m, cairo_t* cr);
  // SteamVR's glyph `name` centred at cx, cy (kGlyph units); false if it
  // has none.
  static bool drawGlyph(Menu& m, cairo_t* cr, const std::string& name, double cx, double cy);
  // Do you use Stash? (menu_setup_stash.cpp).
  static void layoutStash(Menu& m);
  static void drawStash(Menu& m, cairo_t* cr);
  static void updateStash(Menu& m);
  static void openStashPage(Menu& m, SetupStash::Page page);
  static void askStash(Menu& m);
  static void stopAsking(Menu& m);
  static MenuCommand stashAccepted(Menu& m, const std::string& apiKey);
  static void drawKeyboardPanel(Menu& m, cairo_t* cr);
  // Add folders (menu_setup_folders.cpp).
  static void startFolders(Menu& m);
  static void layoutFolders(Menu& m, const Menu::Button& later);
  static void drawFolders(Menu& m, cairo_t* cr);
  static void layoutPick(Menu& m, const Menu::Button& later);
  static void drawPick(Menu& m, cairo_t* cr);
  static void treeRows(Menu& m);
  static void layoutTree(Menu& m);
  static void drawTree(Menu& m, cairo_t* cr);
  static void layoutSignIn(Menu& m);
  static void drawSignIn(Menu& m, cairo_t* cr);
  // An SFTP computer's pane: its sign-in, key, changed key or refused key;
  // where their parts go, from the pane's top.
  struct SftpRows {
    double line = 0, choice = -1, user = 0, password = 0, once = -1, key = 0, keyH = 0, hint = -1,
           error = -1, errorHint = -1, go = 0;
    double run = -1, command = 0, commandH = 0, status = 0;  // KeyOnly's while its key is served
    int keySize = 23;
  };
  static SftpRows sftpRows(Menu& m);
  static void layoutSftp(Menu& m);
  static void drawSftp(Menu& m, cairo_t* cr);
  // The place `root` needs a login: SMB's form, or SFTP's connect job.
  static void askLogin(Menu& m, const std::string& root);
  static void layoutAddComputer(Menu& m);
  static void drawAddComputer(Menu& m, cairo_t* cr);
  static void layoutAdded(Menu& m);
  static void drawAdded(Menu& m, cairo_t* cr);
  static void showPlace(Menu& m, const std::string& root);
  static void readFolder(Menu& m, const std::string& path);
  static void toggleOpen(Menu& m, const std::string& path);
  static void toggleTick(Menu& m, const std::string& path);
  static void addComputer(Menu& m);
  static void splitComputer(SetupFolders& f);  // "alex@nas:2222" into its fields
  static void ticksChanged(Menu& m);
  static void scrollTo(Menu& m, double to);
  static void treeChanged(Menu& m, double y0);
  static std::vector<std::string> ticked(Menu& m);
  static std::string placeOf(Menu& m, const std::string& path);
  static Menu::Icon placeIcon(SetupFolders::Place::Kind kind);
  static void backToPicker(Menu& m);
  // Shared by the pages.
  static Menu::Button& add(Menu& m, Menu::Button b, double x, double y, double w, double h);
  static void addFooter(Menu& m, std::function<MenuCommand()> back, const std::string& primary = "",
                        std::function<MenuCommand()> go = nullptr, bool selected = true,
                        bool disabled = false);
};
