#include "menu_setup.h"

#include "file_util.h"
#include "menu_stash.h"  // stashPrinted
#include "menu_style.h"
#include "platform.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

// Add folders (docs/alpha/first-run.md, step 3): the places on the left
// (this device's home folder, its cards and drives, the computers on the
// network), the chosen one's folders on the right as a tree opened in
// place, each read only when opened (setup_places::readFolder()), ticks at
// any depth; a computer's sign-in in the tree's place; then what was added.
// What it ticks is pinned when setup ends (Menu::endSetup()).

namespace {

using Page = SetupFolders::Page;
using Place = SetupFolders::Place;
using Kind = SetupFolders::Place::Kind;

constexpr double kW = Menu::kSetupWidth, kH = Menu::kSetupHeight;
constexpr double kFooterY = kH - 48 - 72;
// The places, and the box the tree (or a sign-in) shows in.
constexpr double kNavX = 40, kNavW = 340, kNavH = 64, kNavStep = 68;
constexpr double kPaneX = 404, kPaneY = 196, kPaneR = kW - 40, kPaneB = kFooterY - 24;
constexpr double kPad = 12;
// The tree: a folder's row (its tick box, then its icon, name and chevron),
// each level further in; the line under an opened folder.
constexpr double kRowH = 62, kRowStep = 66, kIndent = 44, kTickW = 66, kLineH = 44, kLineStep = 46;
// A form in the box.
constexpr double kFormX = kPaneX + 48, kFieldW = 520, kTextW = kPaneR - kFormX - 48;
// Beside a choice row: its line.
constexpr double kAsideX = kFormX + kFieldW + 24, kAsideW = kPaneR - 48 - kAsideX;
// After Add: the list of what was added.
constexpr double kListW = 920, kItemH = 100;
constexpr double kFolderInk[3] = {132 / 255.0, 176 / 255.0, 240 / 255.0};
constexpr double kError[3] = {1, 0.55, 0.5};

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

// One line, cut at its end. Drawn as Menu::drawText() would.
void drawLine(cairo_t* cr, const std::string& text, double x, double y, double w, double h, int size,
              bool bold, const double ink[3], double alpha = 1) {
  PangoLayout* layout = pango_cairo_create_layout(cr);
  PangoFontDescription* font = pango_font_description_from_string(bold ? kFontBold : kFont);
  pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
  pango_layout_set_font_description(layout, font);
  pango_font_description_free(font);
  pango_layout_set_text(layout, text.c_str(), -1);
  pango_layout_set_width(layout, static_cast<int>(std::max(1.0, w) * PANGO_SCALE));
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  int textW = 0, textH = 0;
  pango_layout_get_pixel_size(layout, &textW, &textH);
  cairo_set_source_rgba(cr, ink[0], ink[1], ink[2], alpha);
  cairo_move_to(cr, x, onPixel(y + (h - textH) / 2));
  pango_cairo_show_layout(cr, layout);
  cairo_new_path(cr);
  g_object_unref(layout);
}

}  // namespace

// The places are looked for, this device's shown with its home folder
// open, and the network's computers looked for.
void SetupPages::startFolders(Menu& m) {
  SetupFolders& f = *(m.setup_->folders = std::make_unique<SetupFolders>());
  f.finding.start([] { return setup_places::findDrives(); });
  if (!m.discovery_.scanning()) m.discovery_.scan();
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
  for (auto& [path, folder] : f.folders) {
    if (!folder.reading.take(folder.listing)) continue;
    folder.read = true;
    if (s.step != SetupMenu::Step::Folders || f.page != Page::Pick || placeOf(m, path) != f.shown) continue;
    if (path == f.shown) {  // the place itself: its folders, or its sign-in
      const remote::Status status = folder.listing.status;
      if ((status == remote::Status::NeedsLogin ||
           (path.rfind("sftp://", 0) == 0 &&
            (status == remote::Status::HostKeyUnknown || status == remote::Status::HostKeyChanged))) &&
          m.smbForm_.kind == Menu::SmbForm::Kind::None && !m.smbForm_.signing)
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

Menu::Button& SetupPages::add(Menu& m, Menu::Button b, double x, double y, double w, double h) {
  b.panel = Menu::kSetup;
  b.x = x, b.y = y, b.w = w, b.h = h;
  m.buttons_.push_back(std::move(b));
  return m.buttons_.back();
}

// Back, and the page's button (if any); the dots are drawn between them.
void SetupPages::addFooter(Menu& m, std::function<MenuCommand()> back, const std::string& primary,
                           std::function<MenuCommand()> go, bool selected, bool disabled) {
  const SetupStrings& t = m.text().setup;
  Menu::Button b = m.textButton(t.back, Menu::Icon::Arrow, std::move(back));
  b.bare = true;
  b.fontSize = 26;
  add(m, b, 36, kFooterY, m.textWidth(t.back, 26, false) + 129, 72);
  if (primary.empty()) return;
  Menu::Button p = m.textButton(primary, Menu::Icon::None, std::move(go), selected);
  p.fontSize = 30;
  p.disabled = disabled;
  const double w = std::max(240.0, m.textWidth(primary, 30, false) + 90);
  add(m, p, kW - 48 - w, kFooterY, w, 72);
}

void SetupPages::layoutFolders(Menu& m, const Menu::Button& later) {
  if (!m.setup_->folders) startFolders(m);
  if (m.setup_->folders->page == Page::Added && m.setup_->places.empty()) m.setup_->folders->page = Page::Pick;
  if (m.setup_->folders->page == Page::Pick)
    layoutPick(m, later);
  else
    layoutAdded(m);
}

void SetupPages::drawFolders(Menu& m, cairo_t* cr) {
  if (m.setup_->folders->page == Page::Pick)
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
  m.smbForm_ = {};
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
void SetupPages::layoutPick(Menu& m, const Menu::Button& later) {
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
    b.fontSize = 27;
    b.radius = 14;
    Menu::Button& added = add(m, b, kNavX, y, kNavW, kNavH);
    y += kNavStep;
    return added;
  };
  for (size_t i = 0; i < f.places.size(); ++i) {
    if (i == computers) y += 14 + 46;  // "Network"
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
  if (f.places.size() == computers) y += 14 + 46;
  if (m.discovery_.scanning() && f.places.size() == computers) y += 40;  // "Looking …"
  Menu::Button& adding = row(t.addComputer, Icon::Plus, f.shown.empty(), [&m] {
    showPlace(m, "");
    return MenuCommand{};
  });
  adding.placeholder = !adding.on;

  f.top = kPaneY + kPad, f.bottom = kPaneB - kPad, f.left = kPaneX + kPad, f.right = kPaneR - 28;
  if (f.shown.empty())
    layoutAddComputer(m);
  else if (m.smbForm_.kind != Menu::SmbForm::Kind::None)
    layoutSignIn(m);
  else if (!m.smbForm_.signing || m.smbForm_.root != f.shown)  // else connecting: drawPick()'s line
    layoutTree(m);

  Menu::Button setUpLater = later;
  setUpLater.fontSize = 26;
  add(m, setUpLater, 36, kFooterY, m.textWidth(later.label, 26, false) + 64, 72);
  const int n = static_cast<int>(s.places.size());
  const std::string primary = n == 0 ? t.skip : n == 1 ? t.addOneFolder : printed(t.addFolders, n);
  Menu::Button go = m.textButton(primary, Icon::None, [&m, n] {
    SetupFolders& f = *m.setup_->folders;
    m.smbForm_ = {};
    m.editing_ = false;
    if (n == 0) {
      setStep(m, SetupMenu::Step::Stash);
    } else {
      f.scrolled[f.shown] = f.scroll;
      f.page = Page::Added;
      f.scroll = 0;
      f.drag = {};
    }
    return MenuCommand{};
  }, n > 0);
  go.fontSize = 30;
  const double w = std::max(240.0, m.textWidth(primary, 30, false) + 90);
  add(m, go, kW - 48 - w, kFooterY, w, 72);
  if (m.editing_) m.layoutKeyboard(Menu::kSetup, kW / 2, kH + 34, 90);
}

void SetupPages::drawPick(Menu& m, cairo_t* cr) {
  SetupFolders& f = *m.setup_->folders;
  const SetupStrings& t = m.text().setup;
  const Palette pal = m.palette();
  drawHead(m, cr, t.videosTitle, t.videosText);
  // "Network" over the computers (and Looking … while there are none yet).
  double y = kPaneY;
  for (const Place& p : f.places)
    if (p.kind != Kind::Computer) y += kNavStep;
  if (shows(cr, kNavX, y + 14, kNavW, 86))
    m.drawText(cr, m.text().network.nav, kNavX + 14, y + 14, kNavW, 40, 22, true, false, pal.textDim.r,
               pal.textDim.g, pal.textDim.b);
  if (shows(cr, kNavX, y + 14, kNavW, 86) && m.discovery_.scanning() && std::none_of(f.places.begin(), f.places.end(), [](const Place& p) {
        return p.kind == Kind::Computer;
      }))
    m.drawText(cr, t.looking, kNavX + 14, y + 60, kNavW, 40, 24, false, false, pal.textDim.r, pal.textDim.g,
               pal.textDim.b);
  roundedRect(cr, kPaneX, kPaneY, kPaneR - kPaneX, kPaneB - kPaneY, 22);
  cairo_set_source_rgba(cr, 0, 0, 0, 0.22);
  cairo_fill(cr);
  if (f.shown.empty())
    drawAddComputer(m, cr);
  else if (m.smbForm_.kind != Menu::SmbForm::Kind::None)
    drawSignIn(m, cr);
  else if (m.smbForm_.signing && m.smbForm_.root == f.shown)
    m.drawText(cr, t.connecting, kFormX, kPaneY + 36, kTextW, 50, 27, false, false, pal.textDim.r, pal.textDim.g,
               pal.textDim.b);
  else
    drawTree(m, cr);
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
           folders ? (listing.videos == 1 ? t.oneVideoToo : printed(t.videosToo, listing.videos))
                   : (listing.videos == 1 ? t.oneVideo : printed(t.videos, listing.videos)),
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
    again.fontSize = 26;
    add(m, again, f.left + kTickW + 4, f.top + kLineStep + 8, m.textWidth(t.tryAgain, 26, false) + 64, 64);
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
      tick.radius = 14;
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
    name.radius = 14;
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
  const double ink[3] = {pal.text.r, pal.text.g, pal.text.b};
  const double dim[3] = {pal.textDim.r, pal.textDim.g, pal.textDim.b};
  const double soft[3] = {215 / 255.0, 218 / 255.0, 228 / 255.0};
  for (const SetupFolders::Row& r : f.rows) {
    const double y = f.top + r.y - f.scroll;
    if (y + r.h < std::max(clipY0, f.top) || y > std::min(clipY1, f.bottom)) continue;
    const double x0 = f.left + r.depth * kIndent;
    if (!r.folder) {
      drawLine(cr, r.line, x0 + kIndent + kTickW + 4, y, f.right - x0 - kIndent - kTickW, r.h, 22, false,
               r.failed ? kError : dim);
      continue;
    }
    const bool on = isTicked(s.places, r.path), within = !on && inherited(s.places, r.path);
    if (on || within) {
      roundedRect(cr, x0, y, f.right - x0, r.h, 14);
      setColor(cr, pal.accent, on ? 0.22 : 0.09);
      cairo_fill(cr);
    }
    // The tick box (File system has none).
    const double bx = x0 + (kTickW - 40) / 2, by = y + (r.h - 40) / 2;
    if (r.tickable && (on || within)) {
      roundedRect(cr, bx, by, 40, 40, 10);
      setColor(cr, pal.accent, on ? 1.0 : 0.38);
      cairo_fill(cr);
      // Dimmed within a ticked folder: its group only the box's size.
      cairo_save(cr);
      cairo_rectangle(cr, bx, by, 40, 40);
      cairo_clip(cr);
      cairo_push_group(cr);
      Menu::drawIcon(cr, Menu::Icon::Check, bx + 20, by + 20, 30);
      cairo_pop_group_to_source(cr);
      cairo_paint_with_alpha(cr, on ? 1.0 : 0.6);
      cairo_restore(cr);
    } else if (r.tickable) {
      roundedRect(cr, bx + 1.5, by + 1.5, 37, 37, 8.5);
      cairo_set_source_rgba(cr, 1, 1, 1, 0.62);
      cairo_set_line_width(cr, 3);
      cairo_stroke(cr);
    }
    const double nameX = x0 + kTickW + 4;
    // The whole card or drive by its place's icon, File system as a drive.
    Menu::Icon icon = r.path == "/" ? Menu::Icon::Storage : Menu::Icon::Folder;
    for (const Place& p : f.places)
      if (r.whole && p.root == r.path) icon = placeIcon(p.kind);
    Menu::drawIcon(cr, icon, nameX + 4 + 16, y + r.h / 2, 32, kFolderInk[0], kFolderInk[1], kFolderInk[2]);
    const double textX = nameX + 4 + 32 + 14, chevronW = r.chevron ? 60 : 8;
    drawLine(cr, r.name, textX, y, f.right - chevronW - textX, r.h, 27, false, within ? soft : ink);
    if (r.chevron) {  // pointing down once opened
      cairo_save(cr);
      cairo_translate(cr, f.right - 30, y + r.h / 2);
      if (r.opened) cairo_rotate(cr, M_PI / 2);
      Menu::drawIcon(cr, Menu::Icon::Chevron, 0, 0, 28, dim[0], dim[1], dim[2]);
      cairo_restore(cr);
    }
  }
  cairo_restore(cr);
  // Where the tree is scrolled to, at its right.
  if (f.scrollMax > 0) {
    const double track = f.bottom - f.top, shown = track / (track + f.scrollMax);
    const double h = std::max(40.0, track * shown);
    const double y = f.top + (track - h) * f.scroll / f.scrollMax;
    roundedRect(cr, f.right + 10, f.top, 6, track, 3);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
    cairo_fill(cr);
    roundedRect(cr, f.right + 10, y, 6, h, 3);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.45);
    cairo_fill(cr);
  }
}

// The tree from tree pixel y down drawn again (all of it if y < 0), its
// scroll bar too.
void SetupPages::treeChanged(Menu& m, double y) {
  SetupFolders& f = *m.setup_->folders;
  const double from = y < 0 ? f.top : std::clamp(f.top + y - f.scroll, f.top, f.bottom);
  damaged(m, from, f.bottom, f.left - 4, f.right + 4);
  damaged(m, f.top - 1, f.bottom + 1, f.right + 8, f.right + 18);
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
  damaged(m, kFooterY - 4, kFooterY + 76, kW - 520, kW);
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

// As the prototype: the title, User name and Password, Sign in; the
// keyboard up on the user name (Menu::openSmbForm()), Done moving to the
// password, then signing in.
void SetupPages::layoutSignIn(Menu& m) {
  if (m.smbForm_.sftp) {
    layoutSftp(m);
    return;
  }
  Menu::SmbForm& form = m.smbForm_;
  const NetworkStrings& n = m.text().network;
  for (const int i : {1, 2}) {
    const bool active = form.field == i && m.editing_;
    const std::string& typed = form.text[i];
    Menu::Button b = m.textButton(i == 2 && !form.showPassword ? hidden(typed) : typed, Menu::Icon::None, [&m, i] {
      m.smbForm_.field = i;
      m.editing_ = true;
      return MenuCommand{};
    });
    b.field = true;
    b.on = active;
    b.caret = active ? m.caret() : -1;
    b.radius = 12;
    b.fontSize = 27;
    add(m, b, kFormX, kPaneY + (i == 1 ? 148 : 268), kFieldW - (i == 2 ? 130 : 0), 64);
    if (i == 2) {
      Menu::Button eye = m.textButton(form.showPassword ? m.text().hide : n.show, Menu::Icon::None, [&m] {
        m.smbForm_.showPassword = !m.smbForm_.showPassword;
        return MenuCommand{};
      });
      eye.fontSize = 27;
      add(m, eye, kFormX + kFieldW - 120, kPaneY + 268, 120, 64);
    }
  }
  const bool signing = form.signing != nullptr;
  Menu::Button go = m.textButton(signing ? n.signingIn : n.signIn, Menu::Icon::None, [&m] {
    m.smbForm_.field = 2;  // signs in from either field
    return m.submitSmbForm();
  }, true);
  go.fontSize = 27;
  go.disabled = signing;
  add(m, go, kFormX, kPaneY + 360 + (form.error.empty() ? 0 : 36), std::max(220.0, m.textWidth(go.label, 27, false) + 80),
      64);
}

void SetupPages::drawSignIn(Menu& m, cairo_t* cr) {
  if (m.smbForm_.sftp) {
    drawSftp(m, cr);
    return;
  }
  const Palette pal = m.palette();
  const NetworkStrings& n = m.text().network;
  const Menu::SmbForm& form = m.smbForm_;
  m.drawText(cr, printed(n.signInTitle, m.serverName(form.root)), kFormX, kPaneY + 36, kPaneR - kFormX - 48, 50,
             30, true, false, pal.text.r, pal.text.g, pal.text.b);
  for (const auto& [label, y] : {std::pair{n.fields[1], 110.0}, {n.fields[2], 230.0}})
    m.drawText(cr, label, kFormX + 4, kPaneY + y, kFieldW, 34, 23, false, false, pal.textDim.r, pal.textDim.g,
               pal.textDim.b);
  if (!form.error.empty())
    m.drawText(cr, form.error, kFormX + 4, kPaneY + 340, kPaneR - kFormX - 48, 40, 24, true, false, kError[0],
               kError[1], kError[2]);
}

// Add a computer: SMB or SFTP (what's typed may say which), its name or
// address; an SFTP one's port and user name; why it couldn't connect; Connect.
void SetupPages::layoutAddComputer(Menu& m) {
  SetupFolders& f = *m.setup_->folders;
  const SetupStrings& t = m.text().setup;
  const Menu::SmbForm& form = m.smbForm_;
  for (int i = 0; i < 2; ++i) {
    Menu::Button type = m.textButton(m.text().network.types[i], Menu::Icon::None, [&m, i] {
      m.setup_->folders->sftp = i == 1;
      m.setup_->folders->field = 0;
      m.smbForm_.error.clear();
      m.smbForm_.hint.clear();
      m.editing_ = true;
      return MenuCommand{};
    }, f.sftp == (i == 1));
    type.fontSize = 27;
    add(m, type, kFormX + i * (kFieldW + 8) / 2, kPaneY + 104, (kFieldW - 8) / 2, 56);
  }
  auto field = [&](int i, const std::string& typed, const char* placeholder, double x, double w) {
    const bool active = m.editing_ && (f.sftp ? f.field : 0) == i;
    Menu::Button b = m.textButton(typed.empty() && !active ? std::string(placeholder) : typed, Menu::Icon::None, [&m, i] {
      m.setup_->folders->field = i;
      m.editing_ = true;
      return MenuCommand{};
    });
    b.field = true;
    b.on = active;
    b.caret = active ? m.caret() : -1;
    b.placeholder = typed.empty() && !active;
    b.radius = 12;
    b.fontSize = 27;
    add(m, b, x, kPaneY + (i == 0 ? 210 : 324), w, 64);
  };
  field(0, f.computer, "", kFormX, kFieldW);
  double y = kPaneY + 290;
  if (f.sftp) {
    field(1, f.port, "", kFormX, 160);
    field(2, f.user, t.userHint, kFormX + 176, kFieldW - 176);
    y = kPaneY + 404;
  }
  if (!form.error.empty()) y += m.wrappedLines(form.error, kTextW, 24, 2, true) * 33;
  if (!form.hint.empty()) y += m.wrappedLines(form.hint, kTextW, 23, 2) * 31;
  const bool connecting = form.signing != nullptr;
  const std::string label = connecting ? t.connecting : t.connect;
  Menu::Button go = m.textButton(label, Menu::Icon::None, [&m] {
    addComputer(m);
    return MenuCommand{};
  }, true);
  go.fontSize = 27;
  auto blank = [](const std::string& s) { return s.find_first_not_of(' ') == std::string::npos; };
  go.disabled = connecting || blank(f.computer) ||
                (f.sftp && blank(f.user) && f.computer.find('@') == std::string::npos);
  add(m, go, kFormX, y + 12, std::max(220.0, m.textWidth(label, 27, false) + 80), 64);
}

void SetupPages::drawAddComputer(Menu& m, cairo_t* cr) {
  const Palette pal = m.palette();
  const SetupStrings& t = m.text().setup;
  const NetworkStrings& n = m.text().network;
  const SetupFolders& f = *m.setup_->folders;
  const Menu::SmbForm& form = m.smbForm_;
  const double dim[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, 1};
  m.drawText(cr, t.addComputerTitle, kFormX, kPaneY + 36, kTextW, 50, 30, true, false, pal.text.r,
             pal.text.g, pal.text.b);
  m.drawTextLines(cr, t.typeHint, kAsideX, kPaneY + 106, kAsideW, 23, 3, dim, false);
  m.drawText(cr, t.computerField, kFormX + 4, kPaneY + 176, kFieldW, 34, 23, false, false, dim[0], dim[1], dim[2]);
  double y = kPaneY + 290;
  if (f.sftp) {
    m.drawText(cr, n.port, kFormX + 4, y, 160, 34, 23, false, false, dim[0], dim[1], dim[2]);
    m.drawText(cr, n.fields[1], kFormX + 180, y, kFieldW - 180, 34, 23, false, false, dim[0], dim[1], dim[2]);
    y = kPaneY + 404;
  }
  if (!form.error.empty()) {
    const double red[4] = {kError[0], kError[1], kError[2], 1};
    m.drawTextLines(cr, form.error, kFormX + 4, y, kTextW, 24, 2, red, false, 0, true);
    y += m.wrappedLines(form.error, kTextW, 24, 2, true) * 33;
  }
  if (!form.hint.empty()) m.drawTextLines(cr, form.hint, kFormX + 4, y, kTextW, 23, 2, dim, false);
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
    if (typed.host.empty() || f.user.empty() || port < 1 || port > 65535 || m.smbForm_.signing) return;
    m.smbForm_ = {};
    m.smbForm_.root = Menu::sftpRoot(typed.host, port);
    m.smbForm_.then = typed.path;
    m.smbForm_.text = {f.computer, f.user, "", f.port};
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
// name), a sign-in's (Menu::smbForm_), or Stash's.
std::string* SetupPages::typed(Menu& m) {
  SetupMenu& s = *m.setup_;
  if (s.step == SetupMenu::Step::Folders && s.folders && s.folders->page == Page::Pick && s.folders->shown.empty()) {
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
  const std::string root = m.smbForm_.root, then = m.smbForm_.then;
  if (s.holdWrites && m.smbForm_.remember &&
      std::find(s.logins.begin(), s.logins.end(), root) == s.logins.end())
    s.logins.push_back(root);
  m.smbForm_ = {};
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
  const Menu::SmbForm form = m.smbForm_;
  if (form.kind == Menu::SmbForm::Kind::AddServer) {
    showPlace(m, "");
    f.sftp = true;
    f.computer = form.text[0];
    f.port = form.text[3];
    f.user = form.text[1];
    f.field = f.user.empty() ? 2 : 0;
    m.smbForm_.error = form.error;
    m.smbForm_.hint = form.hint;
    return;
  }
  if (std::find(f.hosts.begin(), f.hosts.end(), form.root) == f.hosts.end()) f.hosts.push_back(form.root);
  showRoot(f, form.root);
  m.smbForm_ = form;
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
  const auto tried = m.sftpUsers_.find(root);
  m.smbForm_ = {};
  m.smbForm_.sftp = true;
  m.smbForm_.root = root;
  m.smbForm_.text[1] = remote::knownLogin(root, login, remembered) ? login.user
                       : tried != m.sftpUsers_.end()               ? tried->second
                                                                   : "";
  if (!m.smbForm_.text[1].empty()) {
    m.connectSftp();
    return;
  }
  m.smbForm_.kind = Menu::SmbForm::Kind::AddServer;
  m.smbForm_.text[0] = remote::host(root);
  m.smbForm_.text[3] = std::to_string(remote::port(root));
  showSftp(m);
}

// --- An SFTP computer's pane ----------------------------------------------

SetupPages::SftpRows SetupPages::sftpRows(Menu& m) {
  using Kind = Menu::SmbForm::Kind;
  const Menu::SmbForm& form = m.smbForm_;
  const NetworkStrings& n = m.text().network;
  const SetupStrings& t = m.text().setup;
  const int device = kStandaloneHeadset ? 0 : 1;
  const std::string name = m.serverName(form.root), user = form.text[1];
  // The height of text wrapped across the pane, in lines of a size's height.
  auto tall = [&](const std::string& text, int size, int lines, bool bold = false) {
    return m.wrappedLines(text, kTextW, size, lines, bold) * (size * 1.36);
  };
  SftpRows r;
  double y = kPaneY + 92;
  r.line = y;
  if (form.kind == Kind::SignIn) {
    // Without the choice, where SMB's sign-in has its fields.
    if (form.keyChoice) r.choice = kPaneY + 104;
    r.user = kPaneY + (form.keyChoice ? 176 : 114);  // the labels; their fields 34 under them
    r.password = r.user + (form.keyChoice ? 108 : 120);
    y = r.password + 34 + 64 + 10;
    // The key's line, unless an error says more (the pane's height).
    if (form.byKey && form.error.empty()) {
      r.once = y;
      y += tall(stashPrinted(t.keyOnce[device], user), 23, 2) + 4;
    }
  } else if (form.kind == Kind::KeyOnly) {
    const double rgba[4] = {};
    // With the command, what it's for goes without saying (Menu::layoutSftpForm()).
    const std::string keyOnly =
        form.command.empty() ? stashPrinted(t.keyOnly[device], name, user) : stashPrinted(n.keysOnly, name);
    y += tall(keyOnly, 27, 3) + 12;
    if (form.command.empty()) {
      r.key = y + 34;  // under the label
    } else {
      // The command first; then the key, smaller and without its label.
      r.run = y;
      r.command = y + tall(stashPrinted(n.runCommand, name, user), 27, 2) + 4;
      r.commandH = m.drawMono(nullptr, form.command, 0, 0, kTextW - 32, 23, rgba) + 20;
      r.status = r.command + r.commandH + 8;
      r.key = r.status + tall(n.waitingForKey, 23, 1) + 12;
      if (!form.error.empty()) {  // in the status's place, as on the Network page
        r.error = r.status;
        r.key = r.error + tall(form.error, 24, 2, true) + 12;
      }
      r.keySize = 20;
    }
    r.keyH = m.drawMono(nullptr, form.publicKey, 0, 0, kTextW - 32, r.keySize, rgba) + 20;
    r.hint = r.key + r.keyH + 12;
    y = r.hint + tall(stashPrinted(n.addKeyLine, name, user), 23, 2) + 8;
  } else if (form.kind == Kind::Changed) {
    r.key = y + tall(n.changedText, 27, 3, true) + 10;
    y = r.key + 104;
  } else {  // KeyRefused
    r.hint = y + tall(stashPrinted(t.keyRefusedText[device], name), 27, 4) + 10;
    y = r.hint + tall(n.keyRefusedHint, 23, 2) + 8;
  }
  if (!form.error.empty() && r.run < 0) {
    r.error = y;
    y += tall(form.error, 24, 2, true);
  }
  if (!form.hint.empty() && r.run < 0) {
    r.errorHint = y;
    y += tall(form.hint, 23, 2);
  }
  r.go = y + 10;
  return r;
}

// As the prototype: an SFTP computer's sign-in, its key to add, its key
// changed, or its key refused and the password to remember.
void SetupPages::layoutSftp(Menu& m) {
  using Kind = Menu::SmbForm::Kind;
  const Menu::SmbForm& form = m.smbForm_;
  const NetworkStrings& n = m.text().network;
  const SetupStrings& t = m.text().setup;
  const int device = kStandaloneHeadset ? 0 : 1;
  const SftpRows r = sftpRows(m);
  const bool signing = form.signing != nullptr;
  auto button = [&](const std::string& label, std::function<MenuCommand()> action, bool selected, double x,
                    double y, double w = 0) -> Menu::Button& {
    Menu::Button b = m.textButton(label, Menu::Icon::None, std::move(action), selected);
    b.fontSize = 27;
    return add(m, b, x, y, w > 0 ? w : std::max(220.0, m.textWidth(label, 27, false) + 80), 64);
  };
  if (form.kind == Kind::SignIn) {
    // Both choices in one size, small enough for the longer one.
    const char* methods[2] = {t.methodKey[device], n.methods[1]};
    const double choiceW = (kFieldW - 8) / 2;
    int size = 27;
    while (size > 20 && std::max(m.textWidth(methods[0], size, false), m.textWidth(methods[1], size, false)) >
                            choiceW - 28)
      --size;
    for (int i = 0; i < 2 && r.choice >= 0; ++i) {
      Menu::Button& choice = button(methods[i], [&m, i] {
        m.smbForm_.byKey = i == 0;
        return MenuCommand{};
      }, form.byKey == (i == 0), kFormX + i * (kFieldW + 8) / 2, r.choice, choiceW);
      choice.h = 56;
      choice.fontSize = size;
    }
    for (const int i : {1, 2}) {
      const bool active = form.field == i && m.editing_;
      const std::string& typed = form.text[i];
      Menu::Button b = m.textButton(i == 2 && !form.showPassword ? hidden(typed) : typed, Menu::Icon::None, [&m, i] {
        m.smbForm_.field = i;
        m.editing_ = true;
        return MenuCommand{};
      });
      b.field = true;
      b.on = active;
      b.caret = active ? m.caret() : -1;
      b.radius = 12;
      b.fontSize = 27;
      add(m, b, kFormX, (i == 1 ? r.user : r.password) + 34, kFieldW - (i == 2 ? 130 : 0), 64);
    }
    Menu::Button& eye = button(form.showPassword ? m.text().hide : n.show, [&m] {
      m.smbForm_.showPassword = !m.smbForm_.showPassword;
      return MenuCommand{};
    }, false, kFormX + kFieldW - 120, r.password + 34, 120);
    eye.fontSize = 27;
    button(signing ? (form.byKey ? n.settingUpKey : n.signingIn) : n.signIn, [&m] {
      m.smbForm_.field = 2;  // signs in from either field
      return m.submitSmbForm();
    }, true, kFormX, r.go).disabled = signing;
  } else if (form.kind == Kind::KeyOnly) {
    button(signing ? n.signingIn : n.tryAgain, [&m] { return m.submitSmbForm(); }, true, kFormX, r.go).disabled =
        signing;
  } else if (form.kind == Kind::Changed) {
    // Don't connect: back to the address, to fix it.
    const double w = button(n.dontConnect, [&m] {
      Menu::SmbForm& f = m.smbForm_;
      f.kind = Kind::AddServer;
      f.text[0] = remote::host(f.root) + (f.then.empty() ? "" : "/" + f.then);
      f.text[3] = std::to_string(remote::port(f.root));
      showSftp(m);
      return MenuCommand{};
    }, true, kFormX, r.go).w;
    Menu::Button& trust = button(n.trustNewKey, [&m] {
      const std::string key = m.smbForm_.fingerprint;
      m.smbForm_.kind = Kind::None;
      m.connectSftp(key);
      return MenuCommand{};
    }, false, kFormX + w + 16, r.go);
    trust.danger = true;
    trust.disabled = signing;
  } else {
    // Remember password: saved now, or under --first-run when setup ends.
    const double w = button(n.notNow, [&m] {
      m.smbForm_ = {};
      m.dirty_ = true;
      return MenuCommand{};
    }, false, kFormX, r.go).w;
    button(n.remember, [&m] {
      SetupMenu& s = *m.setup_;
      const std::string root = m.smbForm_.root;
      if (!s.holdWrites)
        remote::remember(root);
      else if (std::find(s.logins.begin(), s.logins.end(), root) == s.logins.end())
        s.logins.push_back(root);
      m.smbForm_ = {};
      m.dirty_ = true;
      return MenuCommand{};
    }, true, kFormX + w + 16, r.go);
  }
}

void SetupPages::drawSftp(Menu& m, cairo_t* cr) {
  using Kind = Menu::SmbForm::Kind;
  const Menu::SmbForm& form = m.smbForm_;
  const NetworkStrings& n = m.text().network;
  const SetupStrings& t = m.text().setup;
  const int device = kStandaloneHeadset ? 0 : 1;
  const Palette pal = m.palette();
  const double ink[4] = {pal.text.r, pal.text.g, pal.text.b, 1}, dim[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, 1};
  const double red[4] = {kError[0], kError[1], kError[2], 1};
  const SftpRows r = sftpRows(m);
  const std::string name = m.serverName(form.root), user = form.text[1];
  double titleX = kFormX;
  if (form.kind == Kind::Changed) {
    Menu::drawIcon(cr, Menu::Icon::Warning, kFormX + 18, kPaneY + 61, 36, red[0], red[1], red[2]);
    titleX += 50;
  }
  const std::string title = form.kind == Kind::Changed      ? stashPrinted(n.changedTitle, name)
                            : form.kind == Kind::KeyRefused ? std::string(n.keyRefusedTitle)
                                                            : stashPrinted(n.signInTitle, name);
  m.drawText(cr, title, titleX, kPaneY + 36, kTextW, 50, 30, true, false, ink[0], ink[1], ink[2]);
  if (form.kind == Kind::SignIn) {
    if (r.choice >= 0)
      m.drawTextLines(cr, form.byKey ? t.keyChoiceHint[device] : t.passwordChoiceHint[device], kAsideX,
                      r.choice + 2, kAsideW, 23, 3, dim, false);
    for (const auto& [label, y] : {std::pair{n.fields[1], r.user}, {n.fields[2], r.password}})
      m.drawText(cr, label, kFormX + 4, y, kFieldW, 34, 23, false, false, dim[0], dim[1], dim[2]);
    if (r.once >= 0)
      m.drawTextLines(cr, stashPrinted(t.keyOnce[device], user), kFormX + 4, r.once, kTextW, 23, 2, dim, false);
  } else if (form.kind == Kind::KeyOnly) {
    m.drawTextLines(cr, r.run < 0 ? stashPrinted(t.keyOnly[device], name, user) : stashPrinted(n.keysOnly, name),
                    kFormX, r.line, kTextW, 27, 3, ink, false);
    if (r.run >= 0) {
      m.drawTextLines(cr, stashPrinted(n.runCommand, name, user), kFormX, r.run, kTextW, 27, 2, ink, false);
      roundedRect(cr, kFormX, r.command, kTextW, r.commandH, 12);
      setColor(cr, pal.field);
      cairo_fill(cr);
      m.drawMono(cr, form.command, kFormX + 16, r.command + 10, kTextW - 32, 23, ink);
      const bool fetched = m.keyFetched();
      if (r.error < 0)
        m.drawTextLines(cr, fetched ? std::string(n.keyFetched) : stashPrinted(n.waitingForKey, name), kFormX + 4,
                        r.status, kTextW, 23, 1, fetched ? ink : dim, false);
    } else {
      m.drawText(cr, t.ownKey[device], kFormX + 4, r.key - 34, kTextW, 34, 23, false, false, dim[0], dim[1], dim[2]);
    }
    roundedRect(cr, kFormX, r.key, kTextW, r.keyH, 12);
    setColor(cr, pal.field);
    cairo_fill(cr);
    m.drawMono(cr, form.publicKey, kFormX + 16, r.key + 10, kTextW - 32, r.keySize, ink);
    m.drawTextLines(cr, stashPrinted(n.addKeyLine, name, user), kFormX + 4, r.hint, kTextW, 23, 2, dim, false);
  } else if (form.kind == Kind::Changed) {
    m.drawTextLines(cr, n.changedText, kFormX, r.line, kTextW, 27, 3, red, false, 0, true);
    const double labelW = std::max(m.textWidth(n.trustedKey, 27, false), m.textWidth(n.nowKey, 27, false)) + 30;
    double y = r.key;
    for (const auto& [label, key] : {std::pair{n.trustedKey, form.trusted}, {n.nowKey, form.fingerprint}}) {
      m.drawText(cr, label, kFormX + 4, y, labelW, 44, 27, false, false, dim[0], dim[1], dim[2]);
      m.drawMono(cr, key, kFormX + 4 + labelW, y + 7, kTextW - labelW, 24, ink);
      y += 48;
    }
  } else {
    m.drawTextLines(cr, stashPrinted(t.keyRefusedText[device], name), kFormX, r.line, kTextW, 27, 4, ink, false);
    m.drawTextLines(cr, n.keyRefusedHint, kFormX + 4, r.hint, kTextW, 23, 2, dim, false);
  }
  if (r.error >= 0)
    m.drawTextLines(cr, form.error, kFormX + 4, r.error, kTextW, 24, 2, red, false, 0, true);
  if (r.errorHint >= 0)
    m.drawTextLines(cr, form.hint, kFormX + 4, r.errorHint, kTextW, 23, 2, dim, false);
}

// Refused: the password again, typed anew.
void SetupPages::signInFailed(Menu& m) {
  m.smbForm_.text[2].clear();
  m.smbForm_.field = 2;
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
  if (m.smbForm_.kind != Menu::SmbForm::Kind::None) return m.submitSmbForm();
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

namespace {

// The column: the tick mark, the heading and its line, then the list (as
// many rows as fit, the last cut partway when there are more), centred
// above the footer.
struct AddedBox {
  double markY, headY, subY, boxY, listH;
};
AddedBox addedBox(int items) {
  constexpr double kAbove = 84 + 34 + 53 + 8 + 35 + 40, kRoom = kFooterY - 32 - 40;
  const double room = kRoom - kAbove - 20;
  double listH = items * kItemH;
  if (listH > room) listH = (std::floor(room / kItemH - 0.6) + 0.6) * kItemH;
  const double top = 40 + (kRoom - kAbove - 20 - listH) / 2;
  return {top, top + 84 + 34, top + 84 + 34 + 53 + 8, top + kAbove, listH};
}

}  // namespace

// The picker again, its tree scrolled as it was.
void SetupPages::backToPicker(Menu& m) {
  SetupFolders& f = *m.setup_->folders;
  f.page = Page::Pick;
  f.scroll = f.scrolled.count(f.shown) ? f.scrolled[f.shown] : 0;
  f.drag = {};
}

// What was added, each with × to take it back out (the last one back to
// the picker); Add more folders back with the ticks, and Continue.
void SetupPages::layoutAdded(Menu& m) {
  SetupMenu& s = *m.setup_;
  SetupFolders& f = *s.folders;
  const SetupStrings& t = m.text().setup;
  const std::vector<std::string> items = ticked(m);
  const AddedBox box = addedBox(static_cast<int>(items.size()));
  const double x = (kW - kListW) / 2;
  f.top = box.boxY + 10, f.bottom = f.top + box.listH, f.left = x + 10, f.right = x + kListW - 10;
  f.height = items.size() * kItemH;
  f.scrollMax = std::max(0.0, f.height - box.listH);
  f.scroll = std::clamp(f.scroll, 0.0, f.scrollMax);
  if (f.scrollMax > 0) f.right -= 16;
  for (size_t i = 0; i < items.size(); ++i) {
    const double y = f.top + i * kItemH - f.scroll + 14;
    const double top = std::max(y, f.top), bottom = std::min(y + 72, f.bottom);
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
    remove.radius = 36;
    add(m, remove, f.right - 14 - 72, top, 72, bottom - top);
  }
  Menu::Button more = m.textButton(t.addMore, Menu::Icon::Plus, [&m] {
    backToPicker(m);
    return MenuCommand{};
  });
  more.fontSize = 27;
  add(m, more, 36, kFooterY, m.textWidth(t.addMore, 27, false) + 129, 72);
  Menu::Button go = m.textButton(t.next, Menu::Icon::None, [&m] {
    setStep(m, SetupMenu::Step::Stash);
    return MenuCommand{};
  }, true);
  go.fontSize = 30;
  const double w = std::max(240.0, m.textWidth(t.next, 30, false) + 90);
  add(m, go, kW - 48 - w, kFooterY, w, 72);
}

void SetupPages::drawAdded(Menu& m, cairo_t* cr) {
  SetupMenu& s = *m.setup_;
  SetupFolders& f = *s.folders;
  const SetupStrings& t = m.text().setup;
  const Palette pal = m.palette();
  const std::vector<std::string> items = ticked(m);
  const AddedBox box = addedBox(static_cast<int>(items.size()));
  // The mark: an accent disc with a tick, a soft ring round it.
  const double cx = kW / 2, cy = box.markY + 42;
  cairo_pattern_t* glow = cairo_pattern_create_radial(cx, cy, 40, cx, cy, 80);
  cairo_pattern_add_color_stop_rgba(glow, 0, pal.accent.r, pal.accent.g, pal.accent.b, 0.18);
  cairo_pattern_add_color_stop_rgba(glow, 1, pal.accent.r, pal.accent.g, pal.accent.b, 0);
  cairo_arc(cr, cx, cy, 80, 0, 2 * M_PI);
  cairo_set_source(cr, glow);
  cairo_fill(cr);
  cairo_pattern_destroy(glow);
  cairo_arc(cr, cx, cy, 47, 0, 2 * M_PI);
  setColor(cr, pal.accent, 0.2);
  cairo_set_line_width(cr, 10);
  cairo_stroke(cr);
  cairo_arc(cr, cx, cy, 42, 0, 2 * M_PI);
  setColor(cr, pal.accent);
  cairo_fill(cr);
  Menu::drawIcon(cr, Menu::Icon::Check, cx, cy, 46);
  const int n = static_cast<int>(items.size());
  m.drawText(cr, n == 1 ? std::string(t.oneFolderAdded) : printed(t.foldersAdded, n), 0, box.headY, kW, 53, 44,
             true, true, pal.text.r, pal.text.g, pal.text.b);
  m.drawText(cr, t.appearAsFound, 0, box.subY, kW, 35, 27, false, true, pal.textDim.r, pal.textDim.g,
             pal.textDim.b);
  const double x = (kW - kListW) / 2;
  roundedRect(cr, x, box.boxY, kListW, box.listH + 20, 26);
  cairo_set_source_rgba(cr, 20 / 255.0, 20 / 255.0, 22 / 255.0, 0.4);
  cairo_fill(cr);
  double clipX0, clipY0, clipX1, clipY1;
  cairo_clip_extents(cr, &clipX0, &clipY0, &clipX1, &clipY1);
  cairo_save(cr);
  cairo_rectangle(cr, f.left, f.top, f.right - f.left, f.bottom - f.top);
  cairo_clip(cr);
  const double ink[3] = {pal.text.r, pal.text.g, pal.text.b};
  const double dim[3] = {pal.textDim.r, pal.textDim.g, pal.textDim.b};
  for (int i = 0; i < n; ++i) {
    const double y = f.top + i * kItemH - f.scroll;
    if (y + kItemH < std::max(clipY0, f.top) || y > std::min(clipY1, f.bottom)) continue;
    const std::string& path = items[i];
    if (i > 0 && y > f.top) {
      cairo_rectangle(cr, f.left + 104, y, f.right - f.left - 104 - 18, 1.5);
      cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
      cairo_fill(cr);
    }
    // Where it is: its place, then the folders it is in (in this device's,
    // from Home when it is in it).
    std::string root = placeOf(m, path);
    Menu::Icon icon = Menu::Icon::Folder;
    std::string where;
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
    roundedRect(cr, f.left + 18, y + 18, 64, 64, 16);
    setColor(cr, pal.accent, 0.18);
    cairo_fill(cr);
    Menu::drawIcon(cr, icon, f.left + 18 + 32, y + 50, 34, kFolderInk[0], kFolderInk[1], kFolderInk[2]);
    const double textX = f.left + 18 + 64 + 22, textW = f.right - 14 - 72 - 12 - textX;
    drawLine(cr, path == homeDir() ? t.home : lastPart(path), textX, y + 14, textW, 38, 30, true, ink);
    drawLine(cr, where, textX, y + 52, textW, 32, 23, false, dim);
    Menu::drawIcon(cr, Menu::Icon::Close, f.right - 14 - 36, y + 50, 30, dim[0], dim[1], dim[2]);
  }
  cairo_restore(cr);
  if (f.scrollMax > 0) {
    const double track = f.bottom - f.top, shown = track / (track + f.scrollMax);
    const double h = std::max(40.0, track * shown);
    const double y = f.top + (track - h) * f.scroll / f.scrollMax;
    roundedRect(cr, f.right + 6, y, 6, h, 3);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.45);
    cairo_fill(cr);
  }
}

// --- Scrolling -------------------------------------------------------------

bool SetupPages::scrolls(const Menu& m) {
  const SetupMenu& s = *m.setup_;
  const SetupFolders* f = s.folders.get();
  return s.step == SetupMenu::Step::Folders && f && f->scrollMax > 0 &&
         (f->page == Page::Added || (!f->shown.empty() && m.smbForm_.kind == Menu::SmbForm::Kind::None));
}

// By whole texture pixels, so the texture's rows move on the GPU
// (CairoCanvas::shiftUp()) and only those coming into view are drawn.
void SetupPages::scroll(Menu& m, double rows) {
  if (!scrolls(m) || rows == 0) return;
  SetupFolders& f = *m.setup_->folders;
  constexpr double perPixel = menu_geometry::kRasterScale;
  const double wanted = f.scroll + rows * (f.page == Page::Added ? kItemH : kRowStep) + f.scrollRemainder;
  const double to = std::clamp(std::round(wanted * perPixel) / perPixel, 0.0, f.scrollMax);
  f.scrollRemainder = to > 0 && to < f.scrollMax ? wanted - to : 0;
  scrollTo(m, to);
}

void SetupPages::scrollTo(Menu& m, double to) {
  SetupFolders& f = *m.setup_->folders;
  if (to == f.scroll) return;
  f.scroll = to;
  const double up = to - f.drawnScroll;
  const double x0 = f.left - 4, x1 = f.right + 4;
  // A part of them still to draw would move with them, drawn where it was.
  const std::vector<SetupMenu::Box>& damage = m.setup_->damage;
  const bool waiting = std::any_of(damage.begin(), damage.end(), [&](const SetupMenu::Box& d) {
    return d.x1 > x0 && d.x0 < x1 && d.y1 > f.top && d.y0 < f.bottom;
  });
  if (m.dirty_ || waiting || m.setup_->row >= 0 || f.drawnScroll < 0 || std::abs(up) >= f.bottom - f.top - 1) {
    damaged(m, f.top, f.bottom, x0, x1);
  } else {
    m.setup_->canvas.shiftUp(static_cast<int>(x0), static_cast<int>(f.top), static_cast<int>(x1),
                             static_cast<int>(f.bottom), up);
    if (up > 0)
      damaged(m, f.bottom - up, f.bottom, x0, x1);
    else
      damaged(m, f.top, f.top - up, x0, x1);
  }
  f.drawnScroll = to;
  damaged(m, f.top - 1, f.bottom + 1, f.right + 4, f.right + 18);  // its bar
}

// A press on the list: it follows the pointer once moved kDragStart along
// it; let go unmoved, the row pressed does what it does.
bool SetupPages::press(Menu& m) {
  SetupMenu& s = *m.setup_;
  SetupFolders* f = s.folders.get();
  if (!f || s.step != SetupMenu::Step::Folders || m.pointerPanel_ != Menu::kSetup ||
      (f->page == Page::Pick && (f->shown.empty() || m.smbForm_.kind != Menu::SmbForm::Kind::None)) ||
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
    if (!action || m.pointerPanel_ != Menu::kSetup) return {};
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
  if (moved != 0 && f.scrollMax > 0) scroll(m, -moved / (f.page == Page::Added ? kItemH : kRowStep));
  return {};
}
