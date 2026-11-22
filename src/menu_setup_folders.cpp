#include "menu_setup.h"

#include "file_util.h"
#include "menu_list_parts.h"
#include "menu_stash.h"  // stashPrinted
#include "menu_style.h"
#include "platform.h"

#include <algorithm>
#include <cstdlib>

// Add folders (docs/alpha/first-run.md, step 3): the places on the left
// (this device's home folder, its cards and drives, the computers on the
// network), the chosen one's folders on the right as a tree opened in
// place, each read only when opened (setup_places::readFolder()), ticks at
// any depth; a computer's sign-in in the tree's place; then what was added.
// What it ticks is pinned when setup ends (Menu::endSetup()).

namespace {

using Place = SetupFolders::Place;
using Kind = SetupFolders::Place::Kind;

using setup::kFooterY;
using setup::kH;
using setup::kW;
// The places, as the browser's navigation rows, and the box the tree (or a
// sign-in) shows in, kit::kSpace right of them.
constexpr double kNavX = setup::kLeft, kNavW = 340, kNavH = kit::kNavRowH, kNavStep = kNavH + kit::kSpacePiece;
constexpr double kPaneX = kNavX + kNavW + kit::kSpace, kPaneY = setup::kTop, kPaneR = setup::kRight,
                 kPaneB = setup::kBottom;
constexpr double kPad = kit::kSpaceTight;
// The tree: a folder's row as a list's (its tick, then its folder's tile,
// name and chevron), each level kIndent further in; the line under an opened
// folder. Its scroll bar at the pane's right.
constexpr double kRowH = kit::kRowH, kRowStep = kRowH + kit::kSpacePiece, kIndent = 40, kTickW = 56;
constexpr double kLineH = 40, kLineStep = kLineH + kit::kSpacePiece, kTick = 32;
constexpr double kBarX = kScrollTrackW + kit::kSpaceTight;  // the bar from the list's right
// After Add: a tick, the title and its line over what was added, all
// centred between the panel's top and the footer. What was added is a list
// in a group's box as tall as its rows: each the place's tile, the folder's
// name over where it is, × at its right end.
constexpr double kAddedW = 920, kDone = 72, kItemH = 84, kItemTile = 64, kRemove = kRowRoundSize;
constexpr int kItemWhereSize = 24;  // a list's byline's

std::string homeDir() {
  const char* home = std::getenv("HOME");
  return home && *home ? home : "/";
}

std::string printed(const char* format, int value) {
  char line[256];
  std::snprintf(line, sizeof(line), format, value);
  return line;
}

std::string printed(const char* format, const std::string& value) {
  char line[256];
  std::snprintf(line, sizeof(line), format, value.c_str());
  return line;
}

// One "•" per character typed.
std::string hidden(const std::string& text) {
  std::string dots;
  for (char c : text)
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) dots += "•";
  return dots;
}

bool under(const std::string& path, const std::string& folder) {
  return path.size() > folder.size() && path.compare(0, folder.size(), folder) == 0 &&
         (path[folder.size()] == '/' || folder.back() == '/');
}

std::string lastPart(const std::string& path) { return path.substr(path.rfind('/') + 1); }

// The place `root` shows, as it was left (no folder read).
void showRoot(SetupFolders& f, const std::string& root) {
  if (f.shown == root) return;
  f.scrolled[f.shown] = f.scroll;
  f.shown = root;
  f.scroll = f.scrolled.count(root) ? f.scrolled[root] : 0;
  f.drag = {};
  f.reveal.clear();
}

// The tree's order: by name, hidden ones after the others (readFolder()).
bool nameBefore(const std::string& a, const std::string& b) {
  const bool hiddenA = !a.empty() && a[0] == '.', hiddenB = !b.empty() && b[0] == '.';
  if (hiddenA != hiddenB) return hiddenB;
  return fileNameLess(a, b);
}

}  // namespace

// A tick box: a sunk well, or ticked (`state` 2) filled in the accent with
// its tick, faded in a ticked folder (1).
void SetupPages::drawTick(const Menu& m, cairo_t* cr, double x, double y, int state) {
  constexpr double r = 8;
  if (state == 0) {  // its edge lit faintly, to be seen on the dark box
    m.drawFinish(cr, Finish::Sunk, x, y, kTick, kTick, r);
    roundedRect(cr, x + 0.75, y + 0.75, kTick - 1.5, kTick - 1.5, r - 0.75);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.3);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    return;
  }
  cairo_save(cr);
  cairo_rectangle(cr, x - 2, y - 2, kTick + 4, kTick + 6);
  cairo_clip(cr);
  cairo_push_group(cr);
  m.drawFinish(cr, Finish::Primary, x, y, kTick, kTick, r);
  Menu::drawIcon(cr, Menu::Icon::Check, x + kTick / 2, y + kTick / 2, 24);
  cairo_pop_group_to_source(cr);
  cairo_paint_with_alpha(cr, state == 2 ? 1.0 : 0.45);
  cairo_restore(cr);
}

// A list's scroll bar (the kit's): a sunk track, the shown part's thumb.
void SetupPages::drawBar(const Menu& m, cairo_t* cr, double x, double top, double bottom, double scroll, double max) {
  constexpr double w = kScrollTrackW;
  const double track = bottom - top;
  m.drawFinish(cr, Finish::Sunk, x, top, w, track, w / 2);
  const double h = std::max(kScrollThumbMin, track * track / (track + max));
  const double y = top + (track - h) * scroll / max;
  cairo_pattern_t* fill = cairo_pattern_create_linear(0, y, 0, y + h);
  cairo_pattern_add_color_stop_rgba(fill, 0, 1, 1, 1, 0.34);
  cairo_pattern_add_color_stop_rgba(fill, 1, 1, 1, 1, 0.22);
  roundedRect(cr, x, y, w, h, w / 2);
  cairo_set_source(cr, fill);
  cairo_fill(cr);
  cairo_pattern_destroy(fill);
}

// The places are looked for, this device's shown with its home folder
// open, and the network's computers looked for.
void SetupPages::startFolders(Menu& m) {
  SetupFolders& f = *(m.setup_->folders = std::make_unique<SetupFolders>());
  f.finding.start([] { return setup_places::findDrives(); });
  if (!m.network_.discovery.scanning()) m.network_.discovery.scan();
  f.open.insert(homeDir());
  readFolder(m, homeDir());
  showPlace(m, "/");
}

void SetupPages::readFolder(Menu& m, const std::string& path) {
  SetupFolders::Folder& folder = m.setup_->folders->folders[path];
  folder.read = false;
  folder.reading.start([path] { return setup_places::readFolder(path); });
}

// Every frame while setup runs: the drives found, folders read, Stash's
// answer, the controls held.
void SetupPages::update(Menu& m) {
  SetupMenu& s = *m.setup_;
  updateControls(m);
  if (s.step == SetupMenu::Step::Stash) updateStash(m);
  if (!s.folders) return;
  SetupFolders& f = *s.folders;
  if (f.finding.take(f.drives) && s.step == SetupMenu::Step::Folders) m.dirty_ = true;
  // A page being drawn shows the tree as it started; folders read join it,
  // and its list moves to where it was scrolled meanwhile, once it shows.
  if (s.row >= 0) return;
  if (f.scroll != f.drawnScroll && scrolls(m)) moveList(m);
  for (auto& [path, folder] : f.folders) {
    if (!folder.reading.take(folder.listing)) continue;
    folder.read = true;
    if (s.step != SetupMenu::Step::Folders || f.page != SetupFolders::Page::Pick || placeOf(m, path) != f.shown) continue;
    if (path == f.shown) {  // the place itself: its folders, or its sign-in
      const remote::Status status = folder.listing.status;
      if ((status == remote::Status::NeedsLogin ||
           (path.rfind("sftp://", 0) == 0 &&
            (status == remote::Status::HostKeyUnknown || status == remote::Status::HostKeyChanged))) &&
          m.network_.form.kind == Menu::SmbForm::Kind::None && !m.network_.form.signing)
        askLogin(m, remote::root(path));
      m.dirty_ = true;
      continue;
    }
    // Opened: as much of it as fits scrolled into view, or the rows from it down.
    const double before = f.scroll;
    double y = 0;
    for (const SetupFolders::Row& r : f.rows)
      if (r.path == path && r.folder) y = r.y;
    treeRows(m);
    if (f.reveal == path) {
      f.reveal.clear();
      double end = y + kRowH;
      for (const SetupFolders::Row& r : f.rows)
        if (r.path == path || under(r.path, path)) end = std::max(end, r.y + r.h);
      const double need = end + kPad - (f.bottom - f.top);
      if (need > f.scroll) f.scroll = std::clamp(std::min(y - kPad, need), 0.0, f.scrollMax);
    }
    treeChanged(m, f.scroll != before ? -1 : y);
  }
}

// A drive taken out goes with its ticks; the drives are looked for again
// for one put in.
void SetupPages::drivesChanged(Menu& m, const std::vector<std::string>& removed) {
  SetupMenu& s = *m.setup_;
  if (!s.folders) return;
  SetupFolders& f = *s.folders;
  const size_t before = s.places.size();
  for (const std::string& point : removed) {
    s.places.erase(std::remove_if(s.places.begin(), s.places.end(),
                                  [&](const std::string& p) { return p == point || under(p, point); }),
                   s.places.end());
    if (f.shown == point) showPlace(m, "/");
  }
  if (s.places.size() != before) ticksChanged(m);
  f.finding.start([] { return setup_places::findDrives(); });
  m.dirty_ = true;
}

Menu::Icon SetupPages::placeIcon(Kind kind) {
  switch (kind) {
    case Kind::Home: return kStandaloneHeadset ? Menu::Icon::Headset : Menu::Icon::Computer;
    case Kind::Card: return Menu::Icon::Card;
    case Kind::Usb:
    case Kind::Drive: return Menu::Icon::Storage;
    case Kind::Computer: return Menu::Icon::Computer;
  }
  return Menu::Icon::Folder;
}

// The place a folder is in (its root), "" if none.
std::string SetupPages::placeOf(Menu& m, const std::string& path) {
  std::string best;
  for (const Place& p : m.setup_->folders->places)
    if ((path == p.root || under(path, p.root)) && p.root.size() > best.size()) best = p.root;
  return best;
}

// The ticks in the order the places and their trees show them.
std::vector<std::string> SetupPages::ticked(Menu& m) {
  std::vector<std::string> list = m.setup_->places;
  const std::vector<Place>& places = m.setup_->folders->places;
  auto rank = [&](const std::string& path) {
    const std::string root = placeOf(m, path);
    for (size_t i = 0; i < places.size(); ++i)
      if (places[i].root == root) return i;
    return places.size();
  };
  auto parts = [](const std::string& path) {
    std::vector<std::string> out;
    for (size_t at = 0; at <= path.size();) {
      const size_t end = std::min(path.find('/', at), path.size());
      out.push_back(path.substr(at, end - at));
      at = end + 1;
    }
    return out;
  };
  std::stable_sort(list.begin(), list.end(), [&](const std::string& a, const std::string& b) {
    const size_t ra = rank(a), rb = rank(b);
    if (ra != rb) return ra < rb;
    const auto pa = parts(a), pb = parts(b);
    return std::lexicographical_compare(pa.begin(), pa.end(), pb.begin(), pb.end(), nameBefore);
  });
  return list;
}

// Kept with the marker by main.cpp (MenuCommand::SetupPlaces) and pinned
// when setup ends.
void SetupPages::ticksChanged(Menu& m) {
  SetupMenu& s = *m.setup_;
  s.places = ticked(m);
  MenuCommand c;
  c.type = MenuCommand::SetupPlaces;
  for (const std::string& path : s.places) c.path += (c.path.empty() ? "" : "\n") + path;
  m.command_ = c;
}

// --- The page -----------------------------------------------------------

void SetupPages::layoutFolders(Menu& m) {
  if (!m.setup_->folders) startFolders(m);
  if (m.setup_->folders->page == SetupFolders::Page::Added && m.setup_->places.empty()) m.setup_->folders->page = SetupFolders::Page::Pick;
  if (m.setup_->folders->page == SetupFolders::Page::Pick)
    layoutPick(m);
  else
    layoutAdded(m);
}

void SetupPages::drawFolders(Menu& m, cairo_t* cr) {
  if (m.setup_->folders->page == SetupFolders::Page::Pick)
    drawPick(m, cr);
  else
    drawAdded(m, cr);
  drawDots(m, cr);
}

void SetupPages::showPlace(Menu& m, const std::string& root) {
  SetupFolders& f = *m.setup_->folders;
  f.scrolled[f.shown] = f.scroll;
  f.shown = root;
  f.scroll = f.scrolled.count(root) ? f.scrolled[root] : 0;
  f.drag = {};
  f.reveal.clear();
  m.network_.form = {};
  m.editing_ = root.empty();  // Add a computer: the keyboard up at once
  if (root.empty()) {
    f.computer.clear();
    f.user.clear();
    f.port = "22";
    f.sftp = false;
    f.field = 0;
  } else if (!f.folders.count(root) || (!f.folders[root].read && !f.folders[root].reading.running()) ||
             (f.folders[root].read && f.folders[root].listing.status != remote::Status::Ok &&
              f.folders[root].listing.status != remote::Status::NeedsLogin)) {
    readFolder(m, root);
  } else if (f.folders[root].read && f.folders[root].listing.status == remote::Status::NeedsLogin) {
    askLogin(m, remote::root(root));
  }
  m.dirty_ = true;
}

// The places on the left (those with ticked folders show how many), the
// box with the place's tree or its form, then Set up later and Add or Skip.
void SetupPages::layoutPick(Menu& m) {
  using Icon = Menu::Icon;
  SetupMenu& s = *m.setup_;
  SetupFolders& f = *s.folders;
  const SetupStrings& t = m.text().setup;
  const DriveStrings& d = m.text().drives;
  f.places.clear();
  // This device: its whole file system, its tree starting at Home (treeRows()).
  f.places.push_back({Kind::Home, "/", kStandaloneHeadset ? t.thisHeadset : t.thisComputer});
  for (const RemovableDrives::Drive& drive : f.drives) {
    const Kind kind = drive.kind == RemovableDrives::Kind::Card ? Kind::Card
                      : drive.kind == RemovableDrives::Kind::Usb ? Kind::Usb
                                                                 : Kind::Drive;
    const char* name = kind == Kind::Card ? d.card : kind == Kind::Usb ? d.usbDrive : d.drive;
    // Two of a kind go by their labels.
    const bool twin = std::count_if(f.drives.begin(), f.drives.end(), [&](const RemovableDrives::Drive& o) {
                        return o.kind == drive.kind;
                      }) > 1;
    f.places.push_back({kind, drive.mountPoint,
                        twin ? (drive.label.empty() ? lastPart(drive.mountPoint) : drive.label) : name});
  }
  const size_t computers = f.places.size();
  auto addComputerPlace = [&](const std::string& root) {
    if (std::none_of(f.places.begin(), f.places.end(), [&](const Place& p) { return p.root == root; }))
      f.places.push_back({Kind::Computer, root, m.serverName(root)});
  };
  for (const remote::Server& server : m.networkServers()) addComputerPlace(server.root);
  for (const std::string& root : f.hosts) addComputerPlace(root);
  for (const std::string& tick : s.places)  // ticked before setup was left
    if (remote::isRemote(tick)) addComputerPlace(remote::root(tick));

  double y = kPaneY;
  auto row = [&](const std::string& label, Icon icon, bool on, std::function<MenuCommand()> action) -> Menu::Button& {
    Menu::Button b = m.textButton(label, icon, std::move(action));
    b.nav = true;
    b.on = on;
    b.listLook = static_cast<int>(ListLook::NavSection);
    b.fontSize = kit::kNavSize;
    b.radius = kit::kCorner;
    Menu::Button& added = add(m, b, kNavX, y, kNavW, kNavH);
    y += kNavStep;
    return added;
  };
  // "Network" over the computers, as a group's title (Looking … under it
  // while there are none yet).
  auto network = [&] {
    f.networkY = y;
    y += kit::kGroupTitle;
    if (m.network_.discovery.scanning() && f.places.size() == computers) y += kit::kGroupTitle;
  };
  for (size_t i = 0; i < f.places.size(); ++i) {
    if (i == computers) network();
    const Place& p = f.places[i];
    if (y + kNavH > kPaneB - (i >= computers ? kNavStep : 0)) continue;  // no room
    const int ticks = static_cast<int>(std::count_if(s.places.begin(), s.places.end(), [&](const std::string& path) {
      return placeOf(m, path) == p.root;
    }));
    Menu::Button& b = row(p.name, placeIcon(p.kind), p.root == f.shown, [&m, root = p.root] {
      showPlace(m, root);
      return MenuCommand{};
    });
    if (ticks > 0) b.badge = std::to_string(ticks);
  }
  if (f.places.size() == computers) network();
  Menu::Button& adding = row(t.addComputer, Icon::Plus, f.shown.empty(), [&m] {
    showPlace(m, "");
    return MenuCommand{};
  });
  adding.placeholder = !adding.on;

  f.top = kPaneY + kPad, f.bottom = kPaneB - kPad, f.left = kPaneX + kPad, f.right = kPaneR - kPad - kBarX;
  f.form = true;
  if (f.shown.empty()) {
    layoutAddComputer(m);
  } else if (m.network_.form.kind != Menu::SmbForm::Kind::None) {
    layoutSignIn(m);
  } else if (m.network_.form.signing && m.network_.form.root == f.shown) {  // connecting
    Menu::PageLayout page(m, Menu::PageLayout::UnderHeader{}, t.videosTitle);
    page.groupAt(0, kPaneX, kPaneY, kPaneR - kPaneX, m.serverName(f.shown)).status(Lamp::Busy, t.connecting);
  } else {
    f.form = false;
    layoutTree(m);
  }

  const int n = static_cast<int>(s.places.size());
  Menu::Action go{n == 0 ? std::string(t.skip) : n == 1 ? std::string(t.addOneFolder) : printed(t.addFolders, n), "",
                  [&m, n] {
                    SetupFolders& f = *m.setup_->folders;
                    m.network_.form = {};
                    m.editing_ = false;
                    if (n == 0) {
                      setStep(m, SetupMenu::Step::Stash);
                    } else {
                      f.scrolled[f.shown] = f.scroll;
                      f.page = SetupFolders::Page::Added;
                      f.scroll = 0;
                      f.drag = {};
                    }
                    return MenuCommand{};
                  }};
  go.primary = n > 0;
  addFooter(m, {t.setUpLater, "", command(MenuCommand::EndSetup, 0)}, Icon::None, {go});
  if (m.editing_) m.layoutKeyboard(Menu::kSetup, kW / 2, kH + 34, 90);
}

void SetupPages::drawPick(Menu& m, cairo_t* cr) {
  SetupFolders& f = *m.setup_->folders;
  const SetupStrings& t = m.text().setup;
  const Palette pal = m.palette();
  drawHead(m, cr, t.videosTitle, t.videosText);
  const Rgba& dim = pal.textDim;
  if (shows(cr, kNavX, f.networkY, kNavW, 2 * kit::kGroupTitle)) {
    m.drawText(cr, m.text().network.nav, kNavX + kit::kSpace, f.networkY, kNavW - kit::kSpace, kit::kGroupTitle,
               kit::kGroupTitleSize, true, false, dim.r, dim.g, dim.b);
    if (m.network_.discovery.scanning() && std::none_of(f.places.begin(), f.places.end(), [](const Place& p) {
          return p.kind == Kind::Computer;
        }))
      m.drawText(cr, t.looking, kNavX + kit::kSpace, f.networkY + kit::kGroupTitle, kNavW - kit::kSpace,
                 kit::kGroupTitle, kit::kWellSize, false, false, dim.r, dim.g, dim.b);
  }
  if (!f.form) {
    m.drawFinish(cr, Finish::Group, kPaneX, kPaneY, kPaneR - kPaneX, kPaneB - kPaneY, kit::kGroupCorner);
    drawTree(m, cr);
  } else if (m.network_.form.kind != Menu::SmbForm::Kind::None && m.network_.form.sftp) {
    drawSftp(m, cr);
  }
  drawKeyboardPanel(m, cr);
}

// --- The tree ------------------------------------------------------------

namespace {

bool isTicked(const std::vector<std::string>& ticks, const std::string& path) {
  return std::find(ticks.begin(), ticks.end(), path) != ticks.end();
}

// In a ticked folder: dimmed, ticked with it.
bool inherited(const std::vector<std::string>& ticks, const std::string& path) {
  return std::any_of(ticks.begin(), ticks.end(), [&](const std::string& t) { return under(path, t); });
}

}  // namespace

// The rows from the place's folders down through those open, each opened
// one ending with its line. This device's are Home and File system, opened
// in place like its folders; a card's or drive's start with the whole of it.
void SetupPages::treeRows(Menu& m) {
  SetupFolders& f = *m.setup_->folders;
  const SetupStrings& t = m.text().setup;
  f.rows.clear();
  double y = 0;
  auto line = [&](const std::string& path, int depth, const std::string& text, bool failed) {
    SetupFolders::Row r;
    r.path = path;
    r.depth = depth;
    r.folder = false;
    r.line = text;
    r.failed = failed;
    r.y = y;
    r.h = kLineH;
    f.rows.push_back(r);
    y += kLineStep;
  };
  // A folder's rows; `depth` is its subfolders'.
  std::function<void(const std::string&, int)> walk = [&](const std::string& path, int depth) {
    const auto it = f.folders.find(path);
    if (it == f.folders.end() || !it->second.read) {
      line(path, depth - 1, t.opening, false);
      return;
    }
    const setup_places::Folder& listing = it->second.listing;
    if (listing.status != remote::Status::Ok) {
      line(path, depth - 1, listing.status == remote::Status::Offline ? m.text().network.offline : t.cantOpen, true);
      return;
    }
    for (const setup_places::Folder::Sub& sub : listing.folders) {
      SetupFolders::Row r;
      r.path = path + (path.back() == '/' ? "" : "/") + sub.name;
      r.name = sub.name;
      r.depth = depth;
      r.opened = f.open.count(r.path) > 0;
      // Its chevron unless it is known to have no subfolders.
      const auto read = f.folders.find(r.path);
      r.chevron = read != f.folders.end() && read->second.read && read->second.listing.status == remote::Status::Ok
                      ? !read->second.listing.folders.empty()
                      : sub.folders != 0;
      r.y = y;
      r.h = kRowH;
      f.rows.push_back(r);
      y += kRowStep;
      if (r.opened) walk(r.path, depth + 1);
    }
    const bool folders = !listing.folders.empty();
    if (listing.videos > 0)
      line(path, depth - 1,
           folders ? (listing.videos == 1 ? t.oneVideoToo : printed(t.videosToo, stashNumber(listing.videos)))
                   : (listing.videos == 1 ? t.oneVideo : printed(t.videos, stashNumber(listing.videos))),
           false);
    else if (!folders)
      line(path, depth - 1, t.empty, false);
  };
  const auto place = std::find_if(f.places.begin(), f.places.end(), [&](const Place& p) { return p.root == f.shown; });
  const Kind kind = place != f.places.end() ? place->kind : Kind::Computer;
  auto top = [&](const std::string& path, const std::string& name, bool whole) {
    SetupFolders::Row r;
    r.path = path;
    r.name = name;
    r.whole = whole;
    r.tickable = path != "/";
    r.opened = !whole && f.open.count(path) > 0;
    r.chevron = !whole;
    r.y = y;
    r.h = kRowH;
    f.rows.push_back(r);
    y += kRowStep;
    if (r.opened) walk(path, 1);
  };
  const auto root = f.folders.find(f.shown);
  if (kind == Kind::Home) {
    top(homeDir(), t.home, false);
    top("/", t.fileSystem, false);
  } else {
    if (kind != Kind::Computer && root != f.folders.end() && root->second.read &&
        root->second.listing.status == remote::Status::Ok)
      top(f.shown, kind == Kind::Card ? t.wholeCard : t.wholeDrive, true);
    walk(f.shown, 0);
  }
  f.height = y;
  f.scrollMax = std::max(0.0, f.height - (f.bottom - f.top));
  f.scroll = std::clamp(f.scroll, 0.0, f.scrollMax);
}

// A button for each row's tick and name where it shows.
void SetupPages::layoutTree(Menu& m) {
  SetupMenu& s = *m.setup_;
  SetupFolders& f = *s.folders;
  const SetupStrings& t = m.text().setup;
  treeRows(m);
  // The place itself not read: Try again.
  const auto root = f.folders.find(f.shown);
  if (root != f.folders.end() && root->second.read && root->second.listing.status != remote::Status::Ok) {
    Menu::Button again = m.textButton(t.tryAgain, Menu::Icon::None, [&m] {
      readFolder(m, m.setup_->folders->shown);
      return MenuCommand{};
    });
    again.fontSize = kit::kRowSize;
    again.radius = kit::kCorner;
    add(m, again, f.left + kTickW + 4, f.top + kLineStep + kit::kSpaceTight,
        m.textWidth(t.tryAgain, kit::kRowSize, false) + 2 * 20, kit::kRowH);
  }
  for (const SetupFolders::Row& r : f.rows) {
    if (!r.folder) continue;
    const double y0 = f.top + r.y - f.scroll;
    const double top = std::max(y0, f.top), bottom = std::min(y0 + r.h, f.bottom);
    if (bottom - top < 1) continue;
    const double x0 = f.left + r.depth * kIndent;
    if (r.tickable) {
      Menu::Button tick = m.textButton("", Menu::Icon::None, [&m, path = r.path] {
        toggleTick(m, path);
        return MenuCommand{};
      });
      tick.bare = true;
      tick.radius = kit::kCorner;
      tick.disabled = inherited(s.places, r.path);
      add(m, tick, x0, top, kTickW, bottom - top);
    }
    Menu::Button name = m.textButton("", Menu::Icon::None, [&m, path = r.path, whole = r.whole] {
      if (whole)
        toggleTick(m, path);
      else
        toggleOpen(m, path);
      return MenuCommand{};
    });
    name.bare = true;
    name.radius = kit::kCorner;
    add(m, name, x0 + kTickW + 4, top, f.right - x0 - kTickW - 4, bottom - top);
  }
}

void SetupPages::drawTree(Menu& m, cairo_t* cr) {
  SetupMenu& s = *m.setup_;
  SetupFolders& f = *s.folders;
  const Palette pal = m.palette();
  double clipX0, clipY0, clipX1, clipY1;
  cairo_clip_extents(cr, &clipX0, &clipY0, &clipX1, &clipY1);
  cairo_save(cr);
  cairo_rectangle(cr, f.left - 4, f.top, f.right - f.left + 8, f.bottom - f.top);
  cairo_clip(cr);
  const Rgba warn = hexColour(0xff8a85);
  for (const SetupFolders::Row& r : f.rows) {
    const double y = f.top + r.y - f.scroll;
    if (y + r.h < std::max(clipY0, f.top) || y > std::min(clipY1, f.bottom)) continue;
    const double x0 = f.left + r.depth * kIndent;
    const double nameX = x0 + kTickW + 4;  // the name's button (layoutTree())
    // Its folder's tile, as a list row's picture.
    constexpr double tile = kRowH - 2 * kRowPictureIn;
    const double tileX = nameX + kRowPictureIn, textX = tileX + tile + kRowNameGap;
    if (!r.folder) {
      const Rgba& k = r.failed ? warn : pal.textDim;
      m.drawText(cr, r.line, textX + kIndent, y, f.right - textX - kIndent, r.h, kit::kNoteSize, false, false,
                 k.r, k.g, k.b);
      continue;
    }
    const bool on = isTicked(s.places, r.path), within = !on && inherited(s.places, r.path);
    if (on || within) {
      roundedRect(cr, x0, y, f.right - x0, r.h, kit::kCorner);
      setColor(cr, pal.accent, on ? 0.2 : 0.08);
      cairo_fill(cr);
    }
    // The tick box (File system has none).
    if (r.tickable) drawTick(m, cr, x0 + (kTickW - kTick) / 2, y + (r.h - kTick) / 2, on ? 2 : within ? 1 : 0);
    // The whole card or drive by its place's icon, File system as a drive.
    Menu::Icon icon = r.path == "/" ? Menu::Icon::Storage : Menu::Icon::Folder;
    for (const Place& p : f.places)
      if (r.whole && p.root == r.path) icon = placeIcon(p.kind);
    m.drawFinish(cr, Finish::Sunk, tileX, y + kRowPictureIn, tile, tile, kit::kPieceCorner);
    Menu::drawIcon(cr, icon, tileX + tile / 2, y + r.h / 2, std::round(tile * 0.62), pal.textDim.r,
                   pal.textDim.g, pal.textDim.b);
    const double chevronW = r.chevron ? 56 : kit::kSpace;
    const Rgba& ink = within ? pal.textDim : pal.text;
    m.drawText(cr, r.name, textX, y, f.right - chevronW - textX, r.h, kit::kRowSize, false, false, ink.r, ink.g,
               ink.b);
    if (r.chevron) {  // pointing down once opened
      cairo_save(cr);
      cairo_translate(cr, f.right - 28, y + r.h / 2);
      if (r.opened) cairo_rotate(cr, M_PI / 2);
      Menu::drawIcon(cr, Menu::Icon::Chevron, 0, 0, 24, pal.textDim.r, pal.textDim.g, pal.textDim.b);
      cairo_restore(cr);
    }
  }
  cairo_restore(cr);
  if (f.scrollMax > 0) drawBar(m, cr, f.right + kBarX - kScrollTrackW, f.top, f.bottom, f.scroll, f.scrollMax);
}

// The tree from tree pixel y down drawn again (all of it if y < 0: its
// scroll changed, and the texture will have it where it is now), its scroll
// bar too.
void SetupPages::treeChanged(Menu& m, double y) {
  SetupFolders& f = *m.setup_->folders;
  if (y < 0) f.drawnScroll = f.scroll;
  const double from = y < 0 ? f.top : std::clamp(f.top + y - f.scroll, f.top, f.bottom);
  damaged(m, from, f.bottom, f.left - 4, f.right + 4);
  damaged(m, f.top - 1, f.bottom + 1, f.right + kBarX - kScrollTrackW - 2, f.right + kBarX + 2);
}

// A ticked folder takes in those under it; ticking one clears the ticks
// inside it. Its rows, its place's count and the footer's button change.
void SetupPages::toggleTick(Menu& m, const std::string& path) {
  SetupMenu& s = *m.setup_;
  SetupFolders& f = *s.folders;
  if (inherited(s.places, path)) return;
  const auto it = std::find(s.places.begin(), s.places.end(), path);
  if (it != s.places.end()) {
    s.places.erase(it);
  } else {
    s.places.erase(std::remove_if(s.places.begin(), s.places.end(),
                                  [&](const std::string& p) { return under(p, path); }),
                   s.places.end());
    s.places.push_back(path);
  }
  ticksChanged(m);
  double y0 = -1, y1 = 0;
  for (const SetupFolders::Row& r : f.rows)
    if (r.path == path || under(r.path, path)) {
      if (y0 < 0) y0 = r.y;
      y1 = r.y + r.h;
    }
  if (y0 >= 0)
    damaged(m, std::max(f.top, f.top + y0 - f.scroll), std::min(f.bottom, f.top + y1 - f.scroll), f.left - 4,
            f.right + 4);
  // The places' badges, as one part.
  double navTop = kH, navBottom = 0;
  for (const Menu::Button& b : m.buttons_)
    if (b.nav && b.panel == Menu::kSetup) navTop = std::min(navTop, b.y), navBottom = std::max(navBottom, b.y + b.h);
  damaged(m, navTop - 2, navBottom + 2, kNavX + kNavW - 90, kNavX + kNavW + 4);
  // Add's shadow (its faint end beyond reach()) down to the panel's edge.
  damaged(m, kFooterY - MenuFinishes::reach(Finish::Primary), kH, kW - 560, kW);
}

// The name opens the folder in place (read the first time), or closes it.
void SetupPages::toggleOpen(Menu& m, const std::string& path) {
  SetupFolders& f = *m.setup_->folders;
  double y = 0;
  for (const SetupFolders::Row& r : f.rows)
    if (r.path == path && r.folder) y = r.y;
  if (f.open.erase(path) == 0) {
    f.open.insert(path);
    const auto it = f.folders.find(path);
    if (it == f.folders.end() || (!it->second.read && !it->second.reading.running()) ||
        (it->second.read && it->second.listing.status != remote::Status::Ok))
      readFolder(m, path);
    f.reveal = f.folders[path].read ? "" : path;
  }
  // Read already: in view at once.
  const double before = f.scroll;
  treeRows(m);
  if (f.open.count(path) && f.reveal.empty()) {
    double end = y + kRowH;
    for (const SetupFolders::Row& r : f.rows)
      if (under(r.path, path) || (r.path == path && !r.folder)) end = std::max(end, r.y + r.h);
    const double need = end + kPad - (f.bottom - f.top);
    if (need > f.scroll) f.scroll = std::clamp(std::min(y - kPad, need), 0.0, f.scrollMax);
  }
  treeChanged(m, f.scroll != before ? -1 : y);
}

// --- A computer's sign-in, and one added by name --------------------------

// The pane's forms (the kit's, as the Network page's): a group across it
// from its top, its rows the parts'.

// A sign-in's user name and password (Show inside its field).
void SetupPages::signInFields(Menu& m, Menu::GroupLayout& g) {
  const NetworkStrings& n = m.text().network;
  const Menu::SmbForm& form = m.network_.form;
  for (const int i : {1, 2}) {
    Menu::Field field;
    field.text = i == 2 && !form.showPassword ? hidden(form.text[i]) : form.text[i];
    field.typing = form.field == i && m.editing_;
    field.action = [&m, i] {
      m.network_.form.field = i;
      m.editing_ = true;
      return MenuCommand{};
    };
    if (i == 2) {
      field.inner = form.showPassword ? m.text().hide : n.show;
      field.innerAction = [&m] {
        m.network_.form.showPassword = !m.network_.form.showPassword;
        return MenuCommand{};
      };
    }
    g.field(n.fields[i], field);
  }
}

// Why it failed, and what to do about it.
void SetupPages::formError(Menu& m, Menu::GroupLayout& g) {
  const Menu::SmbForm& form = m.network_.form;
  if (!form.error.empty()) g.status(Lamp::Failed, form.error);
  if (!form.hint.empty()) g.note(form.hint, true);
}

// An SMB computer's sign-in: the fields, Sign in; the keyboard up on the
// user name (Menu::openSmbForm()), Done moving to the password, then signing
// in.
void SetupPages::layoutSignIn(Menu& m) {
  if (m.network_.form.sftp) {
    layoutSftp(m);
    return;
  }
  const Menu::SmbForm& form = m.network_.form;
  const NetworkStrings& n = m.text().network;
  const bool signing = form.signing != nullptr;
  Menu::PageLayout page(m, Menu::PageLayout::UnderHeader{}, m.text().setup.videosTitle);
  Menu::GroupLayout g = page.groupAt(0, kPaneX, kPaneY, kPaneR - kPaneX, printed(n.signInTitle, m.serverName(form.root)));
  g.labelsFor({n.fields[1], n.fields[2]});
  signInFields(m, g);
  formError(m, g);
  Menu::Action go{signing ? n.signingIn : n.signIn, "", [&m] {
                    m.network_.form.field = 2;  // signs in from either field
                    return m.submitSmbForm();
                  }};
  go.primary = true;
  go.disabled = signing;
  g.buttons({go});
}

// Add a computer: SMB or SFTP (what's typed may say which), its name or
// address; an SFTP one's port and user name; why it couldn't connect; Connect.
void SetupPages::layoutAddComputer(Menu& m) {
  SetupFolders& f = *m.setup_->folders;
  const SetupStrings& t = m.text().setup;
  const NetworkStrings& n = m.text().network;
  const Menu::SmbForm& form = m.network_.form;
  Menu::PageLayout page(m, Menu::PageLayout::UnderHeader{}, t.videosTitle);
  Menu::GroupLayout g = page.groupAt(0, kPaneX, kPaneY, kPaneR - kPaneX, t.addComputerTitle);
  g.labelsFor({n.type, t.computerField, n.port, n.fields[1]});
  g.choice(n.type, {n.types[0], n.types[1]}, f.sftp ? 1 : 0, [&m](int i) {
    m.setup_->folders->sftp = i == 1;
    m.setup_->folders->field = 0;
    m.network_.form.error.clear();
    m.network_.form.hint.clear();
    m.editing_ = true;
    return MenuCommand{};
  });
  g.note(t.typeHint);
  auto field = [&](int i, const std::string& label, const std::string& typed, const std::string& example) {
    Menu::Field fl;
    fl.text = typed;
    fl.example = example;
    fl.typing = m.editing_ && (f.sftp ? f.field : 0) == i;
    fl.action = [&m, i] {
      m.setup_->folders->field = i;
      m.editing_ = true;
      return MenuCommand{};
    };
    g.field(label, fl);
  };
  field(0, t.computerField, f.computer, f.sftp ? n.sftpServerHint : n.serverHint);
  if (f.sftp) {
    field(1, n.port, f.port, "");
    field(2, n.fields[1], f.user, t.userHint);
  }
  formError(m, g);
  const bool connecting = form.signing != nullptr;
  auto blank = [](const std::string& s) { return s.find_first_not_of(' ') == std::string::npos; };
  Menu::Action go{connecting ? t.connecting : t.connect, "", [&m] {
                    addComputer(m);
                    return MenuCommand{};
                  }};
  go.primary = true;
  go.disabled = connecting || blank(f.computer) || (f.sftp && blank(f.user) && f.computer.find('@') == std::string::npos);
  g.buttons({go});
}

void SetupPages::splitComputer(SetupFolders& f) {
  const Menu::TypedServer typed = Menu::splitServer(f.computer);
  if (!typed.user.empty()) f.user = typed.user;
  if (typed.port) f.port = std::to_string(std::max(typed.port, 0));
  f.computer = typed.host + (typed.path.empty() ? "" : "/" + typed.path);
}

// "smb://nas/media", "\\nas\media" and "nas" all name nas (as the
// browser's Add server does): it joins the places, shown. An SFTP one is
// connected to first (Menu::connectSftp()), and joins them then.
void SetupPages::addComputer(Menu& m) {
  SetupFolders& f = *m.setup_->folders;
  if (f.sftp) {
    splitComputer(f);
    const Menu::TypedServer typed = Menu::splitServer(f.computer);
    const int port = f.port.empty() ? 22 : std::atoi(f.port.c_str());
    if (typed.host.empty() || f.user.empty() || port < 1 || port > 65535 || m.network_.form.signing) return;
    m.network_.form = {};
    m.network_.form.root = Menu::sftpRoot(typed.host, port);
    m.network_.form.then = typed.path;
    m.network_.form.text = {f.computer, f.user, "", f.port};
    m.editing_ = false;
    m.connectSftp();
    return;
  }
  std::string host = f.computer;
  std::replace(host.begin(), host.end(), '\\', '/');
  if (host.rfind("smb://", 0) == 0) host.erase(0, 6);
  host.erase(0, std::min(host.size(), host.find_first_not_of("/ ")));
  host = host.substr(0, host.find('/'));
  while (!host.empty() && host.back() == ' ') host.pop_back();
  for (char& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (host.empty()) return;
  const std::string root = remote::root("smb://" + host);
  if (std::find(f.hosts.begin(), f.hosts.end(), root) == f.hosts.end()) f.hosts.push_back(root);
  showPlace(m, root);
}

// The field typed into: a computer's name (an SFTP one's port or user
// name), a sign-in's (Menu::network_.form), or Stash's.
std::string* SetupPages::typed(Menu& m) {
  SetupMenu& s = *m.setup_;
  if (s.step == SetupMenu::Step::Folders && s.folders && s.folders->page == SetupFolders::Page::Pick && s.folders->shown.empty()) {
    SetupFolders& f = *s.folders;
    return !f.sftp || f.field == 0 ? &f.computer : f.field == 1 ? &f.port : &f.user;
  }
  if (s.step != SetupMenu::Step::Stash || s.stash.page == SetupStash::Page::Ask) return nullptr;
  return &s.stash.text[s.stash.field];
}

// Signed in (Menu::updateNetwork()): the computer's shares are read again;
// an SFTP one added joins the places, shown, opened down to a folder typed
// after its name. Under --first-run a login to remember is saved only when
// setup ends (Menu::endSetup()).
void SetupPages::signedIn(Menu& m) {
  SetupMenu& s = *m.setup_;
  const std::string root = m.network_.form.root, then = m.network_.form.then;
  if (s.holdWrites && m.network_.form.remember &&
      std::find(s.logins.begin(), s.logins.end(), root) == s.logins.end())
    s.logins.push_back(root);
  m.network_.form = {};
  m.editing_ = false;
  m.dirty_ = true;
  if (!s.folders) return;
  SetupFolders& f = *s.folders;
  if (std::find(f.hosts.begin(), f.hosts.end(), root) == f.hosts.end()) f.hosts.push_back(root);
  showRoot(f, root);
  readFolder(m, root);
  std::string path = root;
  for (size_t at = 0; at < then.size();) {
    const size_t end = std::min(then.find('/', at), then.size());
    if (end > at) {
      path += "/" + then.substr(at, end - at);
      f.open.insert(path);
      readFolder(m, path);
    }
    at = end + 1;
  }
  if (path != root) f.reveal = path;
}

// See Menu::sftpDone().
void SetupPages::showSftp(Menu& m) {
  if (!m.setup_->folders) return;
  SetupFolders& f = *m.setup_->folders;
  const Menu::SmbForm form = m.network_.form;
  if (form.kind == Menu::SmbForm::Kind::AddServer) {
    showPlace(m, "");
    f.sftp = true;
    f.computer = form.text[0];
    f.port = form.text[3];
    f.user = form.text[1];
    f.field = f.user.empty() ? 2 : 0;
    m.network_.form.error = form.error;
    m.network_.form.hint = form.hint;
    return;
  }
  if (std::find(f.hosts.begin(), f.hosts.end(), form.root) == f.hosts.end()) f.hosts.push_back(form.root);
  showRoot(f, form.root);
  m.network_.form = form;
  m.editing_ = form.kind == Menu::SmbForm::Kind::SignIn;
  if (form.kind == Menu::SmbForm::Kind::KeyRefused) readFolder(m, form.root);
  m.dirty_ = true;
}

// An SMB computer's sign-in; an SFTP one is connected to (Menu::
// connectSftp()) with the user name known, else from Add a computer.
void SetupPages::askLogin(Menu& m, const std::string& root) {
  if (root.rfind("sftp://", 0) != 0) {
    m.openSmbForm(Menu::SmbForm::Kind::SignIn, root);
    return;
  }
  remote::Login login;
  bool remembered = false;
  const auto tried = m.network_.sftpUsers.find(root);
  m.network_.form = {};
  m.network_.form.sftp = true;
  m.network_.form.root = root;
  m.network_.form.text[1] = remote::knownLogin(root, login, remembered) ? login.user
                       : tried != m.network_.sftpUsers.end()               ? tried->second
                                                                   : "";
  if (!m.network_.form.text[1].empty()) {
    m.connectSftp();
    return;
  }
  m.network_.form.kind = Menu::SmbForm::Kind::AddServer;
  m.network_.form.text[0] = remote::host(root);
  m.network_.form.text[3] = std::to_string(remote::port(root));
  showSftp(m);
}

// --- An SFTP computer's pane ----------------------------------------------

// Its sign-in (how, where it offers both), its key to add, its key changed,
// or its key refused and the password to remember.
void SetupPages::layoutSftp(Menu& m) {
  using Kind = Menu::SmbForm::Kind;
  Menu::SmbForm& form = m.network_.form;
  const NetworkStrings& n = m.text().network;
  const SetupStrings& t = m.text().setup;
  const int device = kStandaloneHeadset ? 0 : 1;
  const bool signing = form.signing != nullptr;
  const std::string name = m.serverName(form.root), user = form.text[1];
  const std::string title = form.kind == Kind::Changed      ? stashPrinted(n.changedTitle, name)
                            : form.kind == Kind::KeyRefused ? std::string(n.keyRefusedTitle)
                                                            : stashPrinted(n.signInTitle, name);
  Menu::PageLayout page(m, Menu::PageLayout::UnderHeader{}, t.videosTitle);
  Menu::GroupLayout g = page.groupAt(0, kPaneX, kPaneY, kPaneR - kPaneX, title);
  // A key (or the command adding it) in a box as a field's (Menu::drawSmbForm()).
  auto box = [&](const std::string& text, double* to) {
    const double ink[4] = {};
    const double h = m.drawMono(nullptr, text, 0, 0, g.rowWidth() - 2 * kit::kSpace, 24, ink) + 24;
    const Menu::GroupLayout::Box room = g.room(h);
    to[0] = room.x, to[1] = room.y, to[2] = room.w, to[3] = room.h;
  };
  if (form.kind == Kind::SignIn) {
    g.labelsFor({n.signInWith, n.fields[1], n.fields[2]});
    if (form.keyChoice) {
      g.choice(n.signInWith, {t.methodKey[device], n.methods[1]}, form.byKey ? 0 : 1, [&m](int i) {
        m.network_.form.byKey = i == 0;
        return MenuCommand{};
      });
      g.note(form.byKey ? t.keyChoiceHint[device] : t.passwordChoiceHint[device]);
    }
    signInFields(m, g);
    if (form.byKey && form.error.empty()) g.note(stashPrinted(t.keyOnce[device], user));
    formError(m, g);
    Menu::Action go{signing ? (form.byKey ? n.settingUpKey : n.signingIn) : n.signIn, "", [&m] {
                      m.network_.form.field = 2;  // signs in from either field
                      return m.submitSmbForm();
                    }};
    go.primary = true;
    go.disabled = signing;
    g.buttons({go});
  } else if (form.kind == Kind::KeyOnly) {
    // With the command, what it's for goes without saying.
    g.note(form.command.empty() ? stashPrinted(t.keyOnly[device], name, user) : stashPrinted(n.keysOnly, name), true,
           3);
    if (!form.command.empty()) {
      g.note(stashPrinted(n.runCommand, name, user), true, 3);
      box(form.command, form.commandBox);
      if (!form.error.empty())
        g.status(Lamp::Failed, form.error);
      else if (m.keyFetched())
        g.status(Lamp::Ok, n.keyFetched);
      else
        g.status(Lamp::Busy, stashPrinted(n.waitingForKey, name));
    }
    g.note(t.ownKey[device], true, 1);
    box(form.publicKey, form.keyBox);
    g.note(stashPrinted(n.addKeyLine, name, user));
    if (form.command.empty()) formError(m, g);
    Menu::Action go{signing ? n.signingIn : n.tryAgain, "", [&m] { return m.submitSmbForm(); }};
    go.primary = true;
    go.disabled = signing;
    g.buttons({go});
  } else if (form.kind == Kind::Changed) {
    g.status(Lamp::Failed, n.changedText);
    // The key trusted and the one it has now, in rows of their own (drawSftp()).
    const Menu::GroupLayout::Box keys = g.room(2 * kit::kRowH);
    m.setup_->folders->keys = {keys.x, keys.y, keys.w};
    formError(m, g);
    // Don't connect: back to the address, to fix it.
    Menu::Action dont{n.dontConnect, "", [&m] {
                        Menu::SmbForm& f = m.network_.form;
                        f.kind = Kind::AddServer;
                        f.text[0] = remote::host(f.root) + (f.then.empty() ? "" : "/" + f.then);
                        f.text[3] = std::to_string(remote::port(f.root));
                        showSftp(m);
                        return MenuCommand{};
                      }};
    dont.primary = true;
    Menu::Action trust{n.trustNewKey, "", [&m] {
                         const std::string key = m.network_.form.fingerprint;
                         m.network_.form.kind = Kind::None;
                         m.connectSftp(key);
                         return MenuCommand{};
                       }};
    trust.disabled = signing;
    g.buttons({dont, trust}).control().dangerHover = true;
  } else {
    g.note(stashPrinted(t.keyRefusedText[device], name), true, 4);
    g.note(n.keyRefusedHint);
    formError(m, g);
    // Remember password: saved now, or under --first-run when setup ends.
    Menu::Action remember{n.remember, "", [&m] {
                            SetupMenu& s = *m.setup_;
                            const std::string root = m.network_.form.root;
                            if (!s.holdWrites)
                              remote::remember(root);
                            else if (std::find(s.logins.begin(), s.logins.end(), root) == s.logins.end())
                              s.logins.push_back(root);
                            m.network_.form = {};
                            m.dirty_ = true;
                            return MenuCommand{};
                          }};
    remember.primary = true;
    g.buttons({remember, {n.notNow, "", [&m] {
                            m.network_.form = {};
                            m.dirty_ = true;
                            return MenuCommand{};
                          }}});
  }
}

// What the parts don't draw: the key and the command in their boxes, a
// changed key's two fingerprints.
void SetupPages::drawSftp(Menu& m, cairo_t* cr) {
  const Menu::SmbForm& form = m.network_.form;
  m.drawSmbForm(cr);
  if (form.kind != Menu::SmbForm::Kind::Changed) return;
  const NetworkStrings& n = m.text().network;
  const Palette pal = m.palette();
  const double ink[4] = {pal.text.r, pal.text.g, pal.text.b, 1};
  const std::array<double, 3>& k = m.setup_->folders->keys;
  const double labelW =
      std::max(m.textWidth(n.trustedKey, kit::kRowSize, false), m.textWidth(n.nowKey, kit::kRowSize, false)) +
      kit::kSpaceEdge;
  double y = k[1];
  for (const auto& [label, key] : {std::pair{n.trustedKey, form.trusted}, {n.nowKey, form.fingerprint}}) {
    m.drawText(cr, label, k[0] + kit::kSpace, y, labelW, kit::kRowH, kit::kRowSize, false, false, pal.textDim.r,
               pal.textDim.g, pal.textDim.b);
    m.drawMono(cr, key, k[0] + kit::kSpace + labelW, y + 14, k[2] - 2 * kit::kSpace - labelW, 24, ink);
    y += kit::kRowH;
  }
}

// Refused: the password again, typed anew.
void SetupPages::signInFailed(Menu& m) {
  m.network_.form.text[2].clear();
  m.network_.form.field = 2;
  m.editing_ = true;
}

// The keyboard's Done: a sign-in's next field or Sign in, Connect for a
// computer added, Stash's.
MenuCommand SetupPages::submit(Menu& m) {
  SetupMenu& s = *m.setup_;
  if (s.step == SetupMenu::Step::Stash) {
    SetupStash& st = s.stash;
    if (st.page == SetupStash::Page::SignIn && st.field == SetupStash::User) {
      st.field = SetupStash::Password;
      m.dirty_ = true;
    } else {
      askStash(m);
    }
    return {};
  }
  if (m.network_.form.kind != Menu::SmbForm::Kind::None) return m.submitSmbForm();
  if (s.folders && s.folders->shown.empty()) {
    SetupFolders& f = *s.folders;
    if (f.sftp && f.field < 2) {  // on to the next field
      if (f.field == 0) splitComputer(f);
      ++f.field;
      m.dirty_ = true;
    } else {
      addComputer(m);
    }
    return {};
  }
  m.stopEditing();
  return {};
}

// --- After Add -------------------------------------------------------------

// The picker again, its tree scrolled as it was.
void SetupPages::backToPicker(Menu& m) {
  SetupFolders& f = *m.setup_->folders;
  f.page = SetupFolders::Page::Pick;
  f.scroll = f.scrolled.count(f.shown) ? f.scrolled[f.shown] : 0;
  f.drag = {};
}

namespace {

// Where the Added page's parts go for `items` folders: the tick's top, the
// title's, the line's and the list's box (x, y, w, h). The list shows every
// row while they fit, else whole rows and half of one (so it shows it
// scrolls).
struct AddedPage {
  double done, title, line, box[4];
};
constexpr double kAddedTitleGap = kit::kSpace, kAddedLineH = 36, kAddedListGap = kit::kSpaceWide;

AddedPage addedPage(int items) {
  constexpr double head = kDone + kAddedTitleGap + setup::kTitleH + kit::kSpaceTight + kAddedLineH + kAddedListGap;
  constexpr double room = setup::kBottom - setup::kTitleY;
  const double fit = (room - head - 2 * kPad + kRowGap) / kItemH;
  const double rows = items <= std::floor(fit) ? items : std::floor(fit - 0.5) + 0.5;
  const double boxH = rows * kItemH - (rows == std::floor(rows) ? kRowGap : 0) + 2 * kPad;
  AddedPage a;
  a.done = setup::kTitleY + std::round((room - head - boxH) / 2);
  a.title = a.done + kDone + kAddedTitleGap;
  a.line = a.title + setup::kTitleH + kit::kSpaceTight;
  const double box[4] = {(kW - kAddedW) / 2, a.line + kAddedLineH + kAddedListGap, kAddedW, boxH};
  std::copy(box, box + 4, a.box);
  return a;
}

}  // namespace

// What was added, each with × to take it back out (the last one back to
// the picker); Add more folders back with the ticks, and Continue.
void SetupPages::layoutAdded(Menu& m) {
  SetupMenu& s = *m.setup_;
  SetupFolders& f = *s.folders;
  const SetupStrings& t = m.text().setup;
  const std::vector<std::string> items = ticked(m);
  const AddedPage a = addedPage(static_cast<int>(items.size()));
  f.height = items.size() * kItemH - kRowGap;
  f.top = a.box[1] + kPad;
  f.bottom = a.box[1] + a.box[3] - kPad;
  f.left = a.box[0] + kPad;
  f.scrollMax = std::max(0.0, f.height - (f.bottom - f.top));
  f.scroll = std::clamp(f.scroll, 0.0, f.scrollMax);
  f.right = a.box[0] + a.box[2] - kPad - (f.scrollMax > 0 ? kBarX : 0);
  for (size_t i = 0; i < items.size(); ++i) {
    const double y = f.top + i * kItemH - f.scroll + (kItemH - kRowGap - kRemove) / 2;
    const double top = std::max(y, f.top), bottom = std::min(y + kRemove, f.bottom);
    if (bottom - top < 1) continue;
    // Its × is drawn with the row (drawAdded()), whole where the row is cut.
    Menu::Button remove = m.textButton("", Menu::Icon::None, [&m, path = items[i]] {
      SetupMenu& s = *m.setup_;
      s.places.erase(std::remove(s.places.begin(), s.places.end(), path), s.places.end());
      ticksChanged(m);
      if (s.places.empty()) backToPicker(m);
      m.dirty_ = true;
      return MenuCommand{};
    });
    remove.bare = true;
    remove.radius = kRemove / 2;
    add(m, remove, f.right - kit::kSpace - kRemove, top, kRemove, bottom - top);
  }
  Menu::Action next{t.next, "", [&m] {
                      setStep(m, SetupMenu::Step::Stash);
                      return MenuCommand{};
                    }};
  next.primary = true;
  addFooter(m, {t.addMore, "", [&m] {
                  backToPicker(m);
                  return MenuCommand{};
                }},
            Menu::Icon::Plus, {next}, true);
}

void SetupPages::drawAdded(Menu& m, cairo_t* cr) {
  SetupMenu& s = *m.setup_;
  SetupFolders& f = *s.folders;
  const SetupStrings& t = m.text().setup;
  const Palette pal = m.palette();
  const std::vector<std::string> items = ticked(m);
  const int n = static_cast<int>(items.size());
  const AddedPage a = addedPage(n);
  // A disc filled in the accent, its tick white; the title and its line.
  m.drawFinish(cr, Finish::Primary, (kW - kDone) / 2, a.done, kDone, kDone, kDone / 2);
  Menu::drawIcon(cr, Menu::Icon::Check, kW / 2, a.done + kDone / 2, 46);
  m.drawText(cr, n == 1 ? std::string(t.oneFolderAdded) : printed(t.foldersAdded, n), a.box[0], a.title, a.box[2],
             setup::kTitleH, setup::kTitleSize, true, true, pal.text.r, pal.text.g, pal.text.b);
  m.drawText(cr, t.appearAsFound, a.box[0], a.line, a.box[2], kAddedLineH, setup::kLineSize, false, true,
             pal.textDim.r, pal.textDim.g, pal.textDim.b);
  m.drawFinish(cr, Finish::Group, a.box[0], a.box[1], a.box[2], a.box[3], kit::kGroupCorner);
  double clipX0, clipY0, clipX1, clipY1;
  cairo_clip_extents(cr, &clipX0, &clipY0, &clipX1, &clipY1);
  cairo_save(cr);
  cairo_rectangle(cr, f.left - 4, f.top, f.right - f.left + 8, f.bottom - f.top);
  cairo_clip(cr);
  const double ink[4] = {pal.text.r, pal.text.g, pal.text.b, 1};
  for (int i = 0; i < n; ++i) {
    // Its place's icon in a sunk tile, the folder's name over where it is
    // (as a list's row: its text at a list's sizes).
    const double x = f.left, y = f.top + i * kItemH - f.scroll, w = f.right - f.left, h = kItemH - kRowGap;
    if (y + h < std::max(clipY0, f.top) || y > std::min(clipY1, f.bottom)) continue;
    const std::string& path = items[i];
    // Where it is: its place, then the folders it is in (in this device's,
    // from Home when it is in it).
    std::string root = placeOf(m, path), where;
    Menu::Icon icon = Menu::Icon::Folder;
    for (const Place& p : f.places)
      if (p.root == root && !root.empty()) {
        icon = placeIcon(p.kind);
        where = p.name;
        if (p.kind == Kind::Home && under(path, homeDir())) root = homeDir();
      }
    const std::string inside = root.empty() ? path.substr(0, path.rfind('/'))
                                            : path.substr(root.size(), path.rfind('/') - std::min(path.rfind('/'), root.size()));
    for (size_t at = 0; at < inside.size();) {
      const size_t end = std::min(inside.find('/', at), inside.size());
      if (end > at) where += (where.empty() ? "" : " › ") + inside.substr(at, end - at);
      at = end + 1;
    }
    const double tile = y + (h - kItemTile) / 2;
    m.drawFinish(cr, Finish::Sunk, x + kRowPictureIn, tile, kItemTile, kItemTile, kit::kPieceCorner);
    Menu::drawIcon(cr, icon, x + kRowPictureIn + kItemTile / 2, tile + kItemTile / 2, std::round(kItemTile * 0.6),
                   pal.textDim.r, pal.textDim.g, pal.textDim.b);
    const double nameX = x + kRowPictureIn + kItemTile + kRowNameGap;
    const double nameW = x + w - kit::kSpace - kRemove - kit::kSpaceTight - nameX;
    m.drawText(cr, path == homeDir() ? std::string(t.home) : lastPart(path), nameX, y + h / 2 - 34, nameW, 34,
               kit::kRowSize, false, false, ink[0], ink[1], ink[2]);
    m.drawText(cr, where, nameX, y + h / 2, nameW, 32, kItemWhereSize, false, false, pal.textDim.r,
               pal.textDim.g, pal.textDim.b);
    Menu::drawIcon(cr, Menu::Icon::Close, x + w - kit::kSpace - kRemove / 2, y + h / 2, 22, pal.textDim.r,
                   pal.textDim.g, pal.textDim.b);
  }
  cairo_restore(cr);
  if (f.scrollMax > 0) drawBar(m, cr, f.right + kBarX - kScrollTrackW, f.top, f.bottom, f.scroll, f.scrollMax);
}

// --- Scrolling -------------------------------------------------------------

bool SetupPages::scrolls(const Menu& m) {
  const SetupMenu& s = *m.setup_;
  const SetupFolders* f = s.folders.get();
  return s.step == SetupMenu::Step::Folders && f && f->scrollMax > 0 &&
         (f->page == SetupFolders::Page::Added || (!f->shown.empty() && m.network_.form.kind == Menu::SmbForm::Kind::None));
}

// By whole texture pixels, so the texture's rows move on the GPU
// (CairoCanvas::shiftUp()) and only those coming into view are drawn.
void SetupPages::scroll(Menu& m, double rows) {
  if (!scrolls(m) || rows == 0) return;
  SetupFolders& f = *m.setup_->folders;
  constexpr double perPixel = menu_geometry::kRasterScale;
  const double wanted = f.scroll + rows * (f.page == SetupFolders::Page::Added ? kItemH : kRowStep) + f.scrollRemainder;
  const double to = std::clamp(std::round(wanted * perPixel) / perPixel, 0.0, f.scrollMax);
  f.scrollRemainder = to > 0 && to < f.scrollMax ? wanted - to : 0;
  scrollTo(m, to);
}

void SetupPages::scrollTo(Menu& m, double to) {
  SetupFolders& f = *m.setup_->folders;
  if (to == f.scroll) return;
  f.scroll = to;
  m.scrolledAt_ = steadySeconds();
  if (m.setup_->row < 0) moveList(m);  // else once the page being drawn shows (update())
}

void SetupPages::moveList(Menu& m) {
  SetupFolders& f = *m.setup_->folders;
  const double up = f.scroll - f.drawnScroll;
  const double x0 = f.left - 4, x1 = f.right + 4;
  // A part of them still to draw would move with them, drawn where it was.
  const std::vector<SetupMenu::Box>& damage = m.setup_->damage;
  const bool waiting = std::any_of(damage.begin(), damage.end(), [&](const SetupMenu::Box& d) {
    return d.x1 > x0 && d.x0 < x1 && d.y1 > f.top && d.y0 < f.bottom;
  });
  if (m.dirty_ || waiting || f.drawnScroll < 0 || std::abs(up) >= f.bottom - f.top - 1) {
    damaged(m, f.top, f.bottom, x0, x1);
  } else {
    // From f.left: the rows have no pixels left of it, the box's rounded
    // corners do, and they stay where they are.
    m.setup_->canvas.shiftUp(static_cast<int>(f.left), static_cast<int>(f.top), static_cast<int>(x1),
                             static_cast<int>(f.bottom), up);
    if (up > 0)
      damaged(m, f.bottom - up, f.bottom, x0, x1);
    else
      damaged(m, f.top, f.top - up, x0, x1);
  }
  f.drawnScroll = f.scroll;
  damaged(m, f.top - 1, f.bottom + 1, f.right + 4, f.right + kBarX + 2);  // its bar
}

// A press on the list: it follows the pointer once moved kDragStart along
// it; let go unmoved, the row pressed does what it does.
bool SetupPages::press(Menu& m) {
  SetupMenu& s = *m.setup_;
  SetupFolders* f = s.folders.get();
  if (!f || s.step != SetupMenu::Step::Folders || m.pointerPanel_ != Menu::kSetup ||
      (f->page == SetupFolders::Page::Pick && (f->shown.empty() || m.network_.form.kind != Menu::SmbForm::Kind::None)) ||
      m.pointerY_ < f->top || m.pointerY_ > f->bottom || m.pointerX_ < f->left - 4 || m.pointerX_ > f->right + 20)
    return false;
  f->drag = {};
  f->drag.pressed = true;
  f->drag.pressY = f->drag.lastY = m.pointerY_;
  const int index = m.buttonAt(Menu::kSetup, m.pointerX_, m.pointerY_);
  if (index >= 0 && !m.buttons_[index].disabled) f->drag.action = m.buttons_[index].action;
  return true;
}

MenuCommand SetupPages::drag(Menu& m, bool held) {
  constexpr double kDragStart = 24;  // as a browser list's
  SetupFolders& f = *m.setup_->folders;
  SetupFolders::Drag& d = f.drag;
  if (!held) {
    const std::function<MenuCommand()> action = d.moving ? nullptr : d.action;
    d = {};
    // None while a page is being drawn, as Menu::click().
    if (!action || m.pointerPanel_ != Menu::kSetup || m.setup_->row >= 0) return {};
    return action();
  }
  if (m.pointerPanel_ != Menu::kSetup) return {};
  if (!d.moving) {
    if (std::abs(m.pointerY_ - d.pressY) <= kDragStart) return {};
    d.moving = true;  // no row's action now
    d.action = nullptr;
    d.lastY = m.pointerY_;
  }
  const double moved = m.pointerY_ - d.lastY;
  d.lastY = m.pointerY_;
  if (moved != 0 && f.scrollMax > 0) scroll(m, -moved / (f.page == SetupFolders::Page::Added ? kItemH : kRowStep));
  return {};
}
