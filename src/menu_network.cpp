#include "menu_parts.h"

#include "menu_setup.h"
#include "menu_stash.h"  // stashPrinted
#include "platform.h"

#include "remote.h"

#include <algorithm>
#include <cstring>

// The browser's Network page (SMB servers found on the network and the
// servers signed in to) and the form that signs in to one, adds one by its
// name or address, or opens a share a server doesn't list. An SFTP server
// (sftp.h) is connected to first: its key trusted, this device's key tried,
// then the sign-in it offers. The shares and folders themselves are the
// file browser's (remote.h locators).

namespace {

std::string lower(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// One "•" per character typed.
std::string hidden(const std::string& text) {
  std::string dots;
  for (char c : text)
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) dots += "•";
  return dots;
}

bool isSftp(const std::string& root) { return root.rfind("sftp://", 0) == 0; }

// The port typed (empty: 22), 0 if it isn't one.
int typedPort(const std::string& text) {
  const int port = text.empty() ? 22 : std::atoi(text.c_str());
  return port >= 1 && port <= 65535 ? port : 0;
}

}  // namespace

Menu::TypedServer Menu::splitServer(std::string text) {
  TypedServer s;
  std::replace(text.begin(), text.end(), '\\', '/');
  text.erase(0, std::min(text.size(), text.find_first_not_of(' ')));
  while (!text.empty() && text.back() == ' ') text.pop_back();
  for (const char* scheme : {"sftp://", "ssh://", "smb://"})
    if (lower(text.substr(0, std::strlen(scheme))) == scheme) text.erase(0, std::strlen(scheme));
  text.erase(0, std::min(text.size(), text.find_first_not_of('/')));
  const size_t slash = text.find('/');
  std::string host = text.substr(0, slash);
  if (slash != std::string::npos) s.path = text.substr(slash + 1);
  while (!s.path.empty() && s.path.back() == '/') s.path.pop_back();
  const size_t at = host.rfind('@');
  if (at != std::string::npos) {
    s.user = host.substr(0, at);
    host.erase(0, at + 1);
  }
  // "nas:2222", "[fe80::1]:2222"; a bare IPv6 address has no port.
  const size_t colon = host.rfind(':');
  if (colon != std::string::npos && !host.empty() &&
      (host[0] == '[' ? host.rfind(']') + 1 == colon : host.find(':') == colon)) {
    const std::string digits = host.substr(colon + 1);
    if (digits.size() <= 5 && std::all_of(digits.begin(), digits.end(), ::isdigit)) {
      s.port = digits.empty() ? 0 : std::atoi(digits.c_str());
      if (!digits.empty() && s.port == 0) s.port = -1;  // ":0", not a port
      host.erase(colon);
    }
  }
  s.host = lower(host);
  return s;
}

std::string Menu::sftpRoot(const std::string& host, int port) {
  const bool v6 = host.find(':') != std::string::npos && host.front() != '[';
  return remote::root("sftp://" + (v6 ? "[" + host + "]" : host) + ":" + std::to_string(port));
}

int Menu::serverType(const std::string& text) {
  const std::string typed = lower(text);
  if (typed.rfind("sftp://", 0) == 0 || typed.rfind("ssh://", 0) == 0) return 1;
  if (typed.rfind("smb://", 0) == 0 || typed.rfind("\\\\", 0) == 0) return 0;
  if (typed.find('@') != std::string::npos) return 1;
  const int port = splitServer(text).port;
  return port != 0 && port != 445 ? 1 : -1;
}

void Menu::showNetwork() {
  network_.form = {};
  showPage(Page::Network);
  network_.discovery.scan();
}

// The servers to list: those signed in to, then those found that aren't,
// by name.
std::vector<remote::Server> Menu::networkServers() const {
  std::vector<remote::Server> list = remote::servers();
  for (const SmbDiscovery::Server& found : network_.discovery.servers()) {
    const std::string root = remote::root("smb://" + found.address);
    if (std::none_of(list.begin(), list.end(), [&](const remote::Server& s) { return s.root == root; }))
      list.push_back({root, found.name});
  }
  std::sort(list.begin(), list.end(), [](const remote::Server& a, const remote::Server& b) {
    return lower(a.name) < lower(b.name);
  });
  return list;
}

void Menu::openServer(const remote::Server& server) {
  files_.kind = FileKind::Video;
  openDirectory(server.root);
  showPage(Page::Files);
}

// "Network", Add server and Look again; then the servers, each opening its
// shares (an SFTP server its "/"), those signed in to with Forget at their
// end: a key login asks whether to take the key off the server too.
void Menu::layoutNetworkPage() {
  const NetworkStrings& t = text().network;
  contentEdges();
  listShowsFolder_ = false;
  listColumns_ = false;
  double right = kContentRight;
  headerButton(right, t.addServer, [this] {
    openSmbForm(SmbForm::Kind::AddServer, "");
    return MenuCommand{};
  }, true);
  headerButton(right, t.lookAgain, [this] {
    network_.discovery.scan();
    dirty_ = true;
    return MenuCommand{};
  }).disabled = network_.discovery.scanning();
  contentTitle(t.nav, "", right);
  if (network_.form.kind != SmbForm::Kind::None) {
    layoutSmbForm();
    return;
  }
  std::vector<ListItem> items;
  for (const remote::Server& server : networkServers()) {
    ListItem item{server.name, false, [this, server] {
                    openServer(server);
                    return MenuCommand{};
                  }};
    item.folder = true;
    remote::Login login;
    bool remembered = false;
    if (remote::knownLogin(server.root, login, remembered)) {
      if (isSftp(server.root)) item.byline = stashPrinted(t.byline, login.user);
      item.trashTip = login.key ? t.forgetKeyTip : t.forget;
      item.trash = [this, root = server.root, key = login.key] {
        if (key)
          openDialog(Dialog::ForgetServer, root);
        else
          services_.forgetLogin(root);
        dirty_ = true;
        return MenuCommand{};
      };
    }
    items.push_back(std::move(item));
  }
  list_.top = kBrowserToolsY;
  list_.bottom = kListBottom;
  const size_t from = buttons_.size();
  addList(items, list_.left, kFileRowH, static_cast<int>((list_.bottom - list_.top) / kFileRowH));
  for (size_t i = from; i < buttons_.size(); ++i)  // a server's icon
    if (buttons_[i].listRow) buttons_[i].icon = Icon::Computer;
}

// Over the page: what an empty Network page means.
const char* Menu::networkState() const {
  if (page_ != Page::Network || network_.form.kind != SmbForm::Kind::None || list_.count > 0) return nullptr;
  return network_.discovery.scanning() ? text().network.looking : text().network.none;
}

// Opens the form; on the Files page it belongs to the server being browsed.
void Menu::openSmbForm(SmbForm::Kind kind, const std::string& root) {
  const NetworkStrings& t = text().network;
  network_.form = {};
  network_.form.kind = kind;
  network_.form.root = root;
  remote::Login login;
  bool remembered = true;
  const bool known = kind == SmbForm::Kind::SignIn && remote::knownLogin(root, login, remembered);
  network_.form.text[1] = login.user;
  network_.form.remember = remembered;
  // A login known but refused: the password has changed.
  if (known) network_.form.error = t.wrongLogin;
  network_.form.field = kind == SmbForm::Kind::AddServer ? 0
                   : kind == SmbForm::Kind::Share   ? 0
                   : login.user.empty()            ? 1
                                                   : 2;
  editing_ = true;
  dirty_ = true;
}

// The form, a group under the page's header (the kit's): Add server's type
// first, an SFTP server's sign-in how; the fields (the one typed into ringed
// in the accent), Remember password, the error if any, then Sign in (or
// Open) and Cancel; the keyboard under it.
void Menu::layoutSmbForm() {
  static_assert(kBrowserToolsY == PageLayout::kTop, "the form's group is under a content page's header");
  const NetworkStrings& t = text().network;
  const SmbForm& f = network_.form;
  const bool share = f.kind == SmbForm::Kind::Share;
  const bool signing = f.signing != nullptr;
  pageScrollMax_ = 0;  // drawGroups() mustn't cut it off as a scrolled settings page
  std::string title = share ? t.shareTitle : t.addTitle;
  if (f.kind != SmbForm::Kind::AddServer && !share) title = stashPrinted(t.signInTitle, serverName(f.root));
  auto label = [&](int i) -> std::string { return share ? t.shareField : i == 3 ? t.port : t.fields[i]; };
  PageLayout page(*this, PageLayout::UnderHeader{}, t.nav);
  GroupLayout form = page.group(0, title);
  std::vector<std::string> labels = {t.type, t.signInWith};
  for (const int i : smbFields()) labels.push_back(label(i));
  form.labelsFor(labels);
  if (share) form.note(t.sharesRefused, true, 2);
  if (f.kind == SmbForm::Kind::AddServer) {
    form.choice(t.type, {t.types[0], t.types[1]}, f.sftp, [this](int i) {
      network_.form.sftp = i == 1;
      network_.form.field = 0;
      network_.form.error.clear();
      network_.form.hint.clear();
      editing_ = true;
      return MenuCommand{};
    });
    form.note(t.typeHint);
  } else if (f.sftp) {
    layoutSftpForm(form);
  }
  for (const int i : smbFields()) {
    const bool active = f.field == i && editing_;
    Field field;
    field.text = i == 2 && !f.showPassword ? hidden(f.text[i]) : f.text[i];
    field.example = i == 0 && !share ? (f.sftp ? t.sftpServerHint : t.serverHint)
                    : i == 1         ? (f.sftp ? t.sftpUserHint : t.userHint)
                                     : "";
    field.typing = active;
    field.action = [this, i] {
      network_.form.field = i;
      editing_ = true;
      return MenuCommand{};
    };
    if (i == 2) {
      field.inner = f.showPassword ? text().hide : t.show;
      field.innerAction = [this] {
        network_.form.showPassword = !network_.form.showPassword;
        return MenuCommand{};
      };
    }
    form.field(label(i), field);
  }
  // SFTP: only a password sign-in can be remembered (a key login keeps no secret).
  const bool remember = !share && (!f.sftp || (f.kind == SmbForm::Kind::SignIn && !f.byKey));
  if (remember) {
    form.toggle(t.remember, f.remember, [this] {
      network_.form.remember = !network_.form.remember;
      return MenuCommand{};
    });
    form.note(t.rememberHint);
  }
  // A key-only form's error is under its command while it shows.
  if (!f.error.empty() && f.command.empty()) form.status(Lamp::Failed, f.error);
  if (!f.hint.empty() && f.command.empty()) form.note(f.hint, true);
  std::string go = signing ? t.signingIn : share ? t.open : t.signIn;
  bool ready = true;
  if (f.sftp && f.kind == SmbForm::Kind::AddServer) {
    go = signing ? t.connecting : t.connect;
    const TypedServer typed = splitServer(f.text[0]);
    ready = !typed.host.empty() && (!f.text[1].empty() || !typed.user.empty()) && typedPort(f.text[3]);
  } else if (f.sftp && f.kind == SmbForm::Kind::SignIn && f.byKey && signing) {
    go = t.settingUpKey;
  } else if (f.kind == SmbForm::Kind::KeyOnly && !signing) {
    go = t.tryAgain;
  }
  // An SFTP server's button signs in (connects) from any field, as setup's.
  Action submit{go, "", [this] {
                  if (network_.form.sftp && !smbFields().empty()) network_.form.field = smbFields().back();
                  return submitSmbForm();
                }};
  submit.disabled = signing || !ready;
  submit.primary = true;
  form.buttons({submit, {strings().cancel, "", [this] {
                           const bool adding = network_.form.kind == SmbForm::Kind::AddServer;
                           network_.form = {};
                           editing_ = false;
                           if (!adding) showNetwork();
                           dirty_ = true;
                           return MenuCommand{};
                         }}});
  if (editing_) layoutKeyboard();
}

// Above the fields of an SFTP server's sign-in: how to sign in, when it
// offers both. A server taking keys only: this device's key to add to the
// account, and where; while it is served (key_share.h), first the command
// that adds it from the server. The key and the command are in boxes as a
// field's (drawSmbForm()).
void Menu::layoutSftpForm(GroupLayout& form) {
  const NetworkStrings& t = text().network;
  SmbForm& f = network_.form;
  const std::string name = serverName(f.root), user = f.text[1];
  if (f.kind == SmbForm::Kind::SignIn && f.keyChoice) {
    form.choice(t.signInWith, {t.methods[0], t.methods[1]}, f.byKey ? 0 : 1, [this](int i) {
      network_.form.byKey = i == 0;
      return MenuCommand{};
    });
    form.note(f.byKey ? stashPrinted(t.keyHint, user) : std::string(t.passwordHint));
  } else if (f.kind == SmbForm::Kind::KeyOnly) {
    const double ink[4] = {};
    // Its box, the text in it the field's (drawSmbForm()).
    auto box = [&](const std::string& text, double* to) {
      const double h = drawMono(nullptr, text, 0, 0, form.rowWidth() - 2 * kit::kSpace, 24, ink) + 24;
      const GroupLayout::Box room = form.room(h);
      to[0] = room.x, to[1] = room.y, to[2] = room.w, to[3] = room.h;
    };
    // With the command, what it's for goes without saying.
    form.note(f.command.empty() ? stashPrinted(t.keyOnly, name, user) : stashPrinted(t.keysOnly, name),
              true, 3);
    if (!f.command.empty()) {
      form.note(stashPrinted(t.runCommand, name, user), true, 3);
      box(f.command, f.commandBox);
      // How it stands, or why Try again failed (its hint, about adding the
      // key by hand, left out).
      if (!f.error.empty())
        form.status(Lamp::Failed, f.error);
      else if (keyFetched())
        form.status(Lamp::Ok, t.keyFetched);
      else
        form.status(Lamp::Busy, stashPrinted(t.waitingForKey, name));
    }
    form.note(t.deviceKey, true, 1);
    box(f.publicKey, f.keyBox);
    form.note(stashPrinted(t.addKeyLine, name, user));
  }
}

// What the buttons don't draw: the key (and the command), in a box as a
// field's.
void Menu::drawSmbForm(cairo_t* cr) {
  const SmbForm& f = network_.form;
  if (f.kind != SmbForm::Kind::KeyOnly) return;
  const Palette& pal = palette();
  const double ink[4] = {pal.text.r, pal.text.g, pal.text.b, pal.text.a};
  for (const auto& [text, box] : {std::pair{&f.publicKey, f.keyBox}, {&f.command, f.commandBox}}) {
    if (text->empty()) continue;
    drawFinish(cr, Finish::Sunk, box[0], box[1], box[2], box[3], kit::kCorner);
    drawMono(cr, *text, box[0] + 16, box[1] + 12, box[2] - 32, 24, ink);
  }
}

std::vector<int> Menu::smbFields() const {
  switch (network_.form.kind) {
    case SmbForm::Kind::AddServer: return network_.form.sftp ? std::vector<int>{0, 3, 1} : std::vector<int>{0, 1, 2};
    case SmbForm::Kind::SignIn: return {1, 2};
    case SmbForm::Kind::Share: return {0};
    default: return {};
  }
}

// Typing into the form. An address shows which kind of server it is, and
// the form turns into that kind's; a port is digits.
void Menu::typedIntoSmbForm(const std::string& text) {
  SmbForm& f = network_.form;
  std::string typed = text;
  if (f.field == 3) {
    typed.erase(std::remove_if(typed.begin(), typed.end(), [](char c) { return !std::isdigit(static_cast<unsigned char>(c)); }),
                typed.end());
    typed.resize(std::min<size_t>(typed.size(), 5));
  }
  // Connect's state, or the form itself, may change with it.
  bool relayout = f.sftp && typed.empty() != f.text[f.field].empty();
  if (f.kind == SmbForm::Kind::AddServer && f.field == 0) {
    const int type = serverType(typed);
    if (type >= 0 && (type == 1) != f.sftp) {
      f.sftp = type == 1;
      relayout = true;
    }
    relayout = relayout || (f.sftp && (typed.find('@') == std::string::npos) !=
                                          (f.text[0].find('@') == std::string::npos));
  }
  f.text[f.field] = typed;
  if (relayout)
    dirty_ = true;
  else
    typedChanged();
}

// Enter: on to the next field, or from the last one, Sign in (Open,
// Connect). Leaving an SFTP server's address, "alex@nas:2222" fills in
// the user name and the port.
MenuCommand Menu::submitSmbForm() {
  SmbForm& f = network_.form;
  auto spread = [&f] {
    const TypedServer typed = splitServer(f.text[0]);
    if (!typed.user.empty()) f.text[1] = typed.user;
    if (typed.port) f.text[3] = std::to_string(std::max(typed.port, 0));
    f.text[0] = typed.host + (typed.path.empty() ? "" : "/" + typed.path);
  };
  const bool adding = f.kind == SmbForm::Kind::AddServer;
  const std::vector<int> fields = smbFields();
  const auto at = std::find(fields.begin(), fields.end(), f.field);
  if (editing_ && at != fields.end() && at + 1 != fields.end()) {
    if (f.sftp && adding && f.field == 0) spread();
    f.field = *(at + 1);
    dirty_ = true;
    return {};
  }
  if (f.signing) return {};
  if (f.kind == SmbForm::Kind::Share) {
    std::string name = f.text[0];
    name.erase(0, name.find_first_not_of("/\\ "));
    if (name.empty()) return {};
    const std::string root = f.root;
    f = {};
    editing_ = false;
    openDirectory(root + "/" + name);
    return {};
  }
  if (f.sftp) {
    if (adding) {
      spread();
      const TypedServer typed = splitServer(f.text[0]);
      const int port = typedPort(f.text[3]);
      if (typed.host.empty() || f.text[1].empty() || !port) return {};
      f.root = sftpRoot(typed.host, port);
      f.then = typed.path;
      connectSftp();
    } else if (f.kind == SmbForm::Kind::SignIn && !f.text[1].empty()) {
      signInSftp({f.text[1], f.text[2], f.byKey}, f.byKey || f.remember);
    } else if (f.kind == SmbForm::Kind::KeyOnly) {
      signInSftp({f.text[1], "", true}, true);
    }
    return {};
  }
  // "smb://nas/media", "\\nas\media" and "nas/media" all name nas, then
  // open the share (and folder) after it.
  std::string server = adding ? f.text[0] : f.root;
  std::replace(server.begin(), server.end(), '\\', '/');
  if (lower(server.substr(0, 6)) == "smb://") server.erase(0, 6);
  server.erase(0, server.find_first_not_of("/ "));
  const size_t slash = server.find('/');
  const std::string host = lower(server.substr(0, slash));
  f.then = slash == std::string::npos ? "" : server.substr(slash + 1);
  while (!f.then.empty() && f.then.back() == '/') f.then.pop_back();
  if (host.empty()) return {};
  f.root = remote::root("smb://" + host);
  auto job = std::make_shared<RemoteJob>();
  f.signing = job;
  f.error.clear();
  // --first-run's setup saves it only when it ends (SetupPages::signedIn()).
  const bool remember = f.remember && !(setup_ && setup_->holdWrites);
  services_.signIn(job, f.root, {f.text[1], f.text[2]}, remember);
  dirty_ = true;
  return {};
}

// The connect job: the server's key, trusted at once if it is new (a
// changed one stops it), then this device's key if the server takes keys;
// what's left to sign in with otherwise, and for a server taking keys only,
// this device's key to show.
void Menu::connectSftp(const std::string& trust) {
  SmbForm& f = network_.form;
  if (f.signing) return;  // never in place of a job under way
  auto job = std::make_shared<RemoteJob>();
  job->task = RemoteJob::Task::Connect;
  f.sftp = true;
  f.signing = job;
  f.error.clear();
  f.hint.clear();
  network_.sftpUsers[f.root] = f.text[1];
  // A key login keeps no secret; --first-run's setup saves it when it ends
  // (SetupPages::signedIn()).
  const bool remember = !(setup_ && setup_->holdWrites);
  services_.connectSftp(job, f.root, f.text[1], trust, remember);
  dirty_ = true;
}

// A sign-in from the SFTP form. A key never trusted (gone meanwhile) is
// trusted as on connecting; a changed one, both keys for the warning.
void Menu::signInSftp(const remote::Login& login, bool remember) {
  SmbForm& f = network_.form;
  auto job = std::make_shared<RemoteJob>();
  f.signing = job;
  f.error.clear();
  f.hint.clear();
  network_.sftpUsers[f.root] = login.user;
  remember = remember && !(setup_ && setup_->holdWrites);
  services_.signInSftp(job, f.root, login, remember);
  dirty_ = true;
}

// Signed in (network_.form's server): its root, or the folder typed after it, on
// the Files page; in setup, its tree.
void Menu::openSignedIn() {
  const std::string where = network_.form.root + (network_.form.then.empty() ? "" : "/" + network_.form.then);
  services_.setServerName(network_.form.root, serverName(network_.form.root));
  if (setup_) {
    SetupPages::signedIn(*this);
    return;
  }
  network_.form = {};
  editing_ = false;
  files_.kind = FileKind::Video;
  if (where == directory())
    refreshDirectory();
  else
    openDirectory(where);
  showPage(Page::Files);
}

// A sign-in finished, or the folder being browsed asks for one (or doesn't
// list its shares).
void Menu::updateNetwork() {
  // A search ending changes the page even with the same servers found (none
  // again): it no longer says it is looking, and Look again works again.
  // Only what shows the search or the servers: setup's Folders step, the
  // Network page's list; over a form, its title (a server's name) and Look
  // again in the header.
  const bool searching = network_.discovery.scanning();
  const bool found = network_.discovery.poll();
  if (found || (searching && !network_.discovery.scanning())) {
    if (setup_ ? setup_->step == SetupMenu::Step::Folders : networkList() || (page_ == Page::Network && found)) {
      dirty_ = true;
    } else if (page_ == Page::Network) {
      for (const Button& b : buttons_)
        if (b.panel == kBrowser && b.label == text().network.lookAgain) hoverOnlyChanged(b);  // its box
    }
  }
  updateKeyShare();
  if (!network_.form.signing || !network_.form.signing->done) return;
  const std::shared_ptr<RemoteJob> job = std::move(network_.form.signing);
  network_.form.signing.reset();
  dirty_ = true;
  if (job->task == RemoteJob::Task::RemoveKey) {  // Forget's
    if (job->status == remote::Status::Ok) {
      dialog_ = Dialog::None;
    } else {
      dialog_ = Dialog::ForgetOffline;
    }
    return;
  }
  if (network_.form.sftp) {
    sftpDone(*job);
    return;
  }
  const remote::Status status = job->status;
  const NetworkStrings& t = text().network;
  if (status == remote::Status::Ok) {
    openSignedIn();
    return;
  }
  network_.form.error = status == remote::Status::Offline ? t.offline
                   : status == remote::Status::Denied ? t.denied
                                                    : t.wrongLogin;
  if (setup_) SetupPages::signInFailed(*this);
}

// Since the key was fetched, signing in by itself (or with Try again).
bool Menu::keyFetched() const {
  const SmbForm& f = network_.form;
  return f.fetches > 0 && (f.retryUntil != std::chrono::steady_clock::time_point{} || f.signing);
}

// The key-only form's key share: the command shown while it serves (not
// once it stops, after ten minutes); after each fetch, the key's sign-in
// tried by itself (as Try again), a second later and then every three
// seconds for a minute; then the error, as for Try again.
void Menu::updateKeyShare() {
  using namespace std::chrono_literals;
  SmbForm& f = network_.form;
  if (!f.share) return;
  if (f.kind != SmbForm::Kind::KeyOnly) {
    f.share.reset();
    f.command.clear();
    return;
  }
  const std::string address = f.share->address();
  if (address.empty() != f.command.empty()) {
    f.command = address.empty() ? "" : "curl " + address + " >> ~/.ssh/authorized_keys";
    dirty_ = true;
  }
  const auto now = std::chrono::steady_clock::now();
  if (f.share->fetches() != f.fetches) {
    f.fetches = f.share->fetches();
    f.retryAt = now + 1s;
    f.retryUntil = now + 60s;
    f.error.clear();
    f.hint.clear();
    dirty_ = true;
  }
  if (f.retryUntil == std::chrono::steady_clock::time_point{} || f.signing) return;
  if (now >= f.retryUntil) {
    f.retryAt = f.retryUntil = {};
    if (f.error.empty()) {  // as sftpDone()'s
      f.error = stashPrinted(setup_ ? text().setup.keyStillRefused[kStandaloneHeadset ? 0 : 1]
                                    : text().network.keyStillRefused,
                             serverName(f.root), f.text[1]);
      f.hint = text().network.keyStillRefusedHint;
    }
    dirty_ = true;
  } else if (now >= f.retryAt) {
    f.retryAt = now + 3s;
    signInSftp({f.text[1], "", true}, true);
  }
}

// An SFTP server's connect job or sign-in ended: signed in; the sign-in it
// offers, or its key to add; its key changed; or why not. A connect with no
// form (a folder or a place that needs a login) that fails shows Add server
// with what is known filled in.
void Menu::sftpDone(const RemoteJob& job) {
  using Status = remote::Status;
  using Kind = SmbForm::Kind;
  const NetworkStrings& t = text().network;
  const SetupStrings& s = text().setup;
  const int device = kStandaloneHeadset ? 0 : 1;
  SmbForm& f = network_.form;
  const std::string root = f.root, name = serverName(root), user = f.text[1];
  switch (job.status) {
    case Status::Ok:
      openSignedIn();
      return;
    case Status::KeyRefused:
      if (setup_) {
        services_.setServerName(root, name);
        f.kind = Kind::KeyRefused;
        SetupPages::showSftp(*this);
      } else {
        openSignedIn();
        openDialog(Dialog::KeyRefused, root);
      }
      return;
    case Status::HostKeyChanged:
      // Nothing more by itself behind the warning.
      f.share.reset();
      f.command.clear();
      f.retryAt = f.retryUntil = {};
      f.trusted = job.trusted;
      f.fingerprint = job.probe.fingerprint;
      editing_ = false;
      if (setup_) {
        f.kind = Kind::Changed;
        SetupPages::showSftp(*this);
      } else {
        openDialog(Dialog::HostKeyChanged, root);
      }
      return;
    case Status::NeedsLogin:
      if (job.task == RemoteJob::Task::Connect) {
        f.password = job.probe.password;
        f.keyChoice = f.byKey = job.probe.key && job.probe.password;
        f.publicKey = job.publicKey;
        f.kind = f.password ? Kind::SignIn : Kind::KeyOnly;
        f.share = f.kind == Kind::KeyOnly && !f.publicKey.empty()
                      ? std::make_shared<KeyShare>(remote::host(root), remote::port(root), f.publicKey)
                      : nullptr;
        f.command.clear();
        f.fetches = 0;
        f.retryAt = f.retryUntil = {};
        f.text[2].clear();
        f.showPassword = false;
      } else if (f.kind == Kind::KeyOnly) {
        // Signing in by itself after the key was fetched: quiet until its
        // time is up (updateKeyShare()).
        if (f.retryUntil == std::chrono::steady_clock::time_point{}) {
          f.error = stashPrinted(setup_ ? s.keyStillRefused[device] : t.keyStillRefused, name, user);
          f.hint = t.keyStillRefusedHint;
        }
      } else {
        f.error = t.wrongLogin;
        f.hint = t.oneTimeCode;
        f.text[2].clear();
      }
      f.field = user.empty() ? 1 : 2;
      editing_ = f.kind == Kind::SignIn;
      break;
    case Status::Denied:
      f.error = stashPrinted(t.noSftp, user, name);
      f.hint = t.noSftpHint;
      break;
    default:
      f.error = stashPrinted(t.offlinePort, name, remote::port(root));
      break;
  }
  if (f.kind == Kind::None || f.kind == Kind::Changed) {
    f.kind = Kind::AddServer;
    f.text[0] = remote::host(root) + (f.then.empty() ? "" : "/" + f.then);
    f.text[3] = std::to_string(remote::port(root));
    f.field = 0;
    editing_ = true;
  }
  if (setup_) SetupPages::showSftp(*this);
}

// The folder just read: an SMB server that wants a login gets the form, one
// that doesn't list its shares the share's field. An SFTP server that wants
// one, or whose key isn't trusted as it is, is connected to again (with the
// user known, else from Add server).
void Menu::smbFolderRead() {
  if (page_ != Page::Files || !remote::isRemote(directory()) || network_.form.kind != SmbForm::Kind::None ||
      network_.form.signing)
    return;
  const std::string root = remote::root(directory());
  const remote::Status status = files_.listing.status();
  if (isSftp(root)) {
    if (status != remote::Status::NeedsLogin && status != remote::Status::HostKeyUnknown &&
        status != remote::Status::HostKeyChanged)
      return;
    remote::Login login;
    bool remembered = false;
    const auto tried = network_.sftpUsers.find(root);
    network_.form = {};
    network_.form.sftp = true;
    network_.form.root = root;
    network_.form.then = directory().size() > root.size() ? directory().substr(root.size() + 1) : "";
    network_.form.text[1] = remote::knownLogin(root, login, remembered) ? login.user
                       : tried != network_.sftpUsers.end()                ? tried->second
                                                                   : "";
    if (network_.form.text[1].empty()) {
      network_.form.kind = SmbForm::Kind::AddServer;
      network_.form.text[0] = remote::host(root) + (network_.form.then.empty() ? "" : "/" + network_.form.then);
      network_.form.text[3] = std::to_string(remote::port(root));
      network_.form.field = 1;
      editing_ = true;
      dirty_ = true;
      return;
    }
    connectSftp();
    return;
  }
  if (status == remote::Status::NeedsLogin)
    openSmbForm(SmbForm::Kind::SignIn, root);
  else if (status == remote::Status::Denied && remote::parent(directory()).empty())
    openSmbForm(SmbForm::Kind::Share, root);
}

// An SFTP server's dialogs: signed in with the password, its key changed,
// and Forget for a key login.
void Menu::layoutNetworkDialog(DialogLayout& dialog) {
  using Ink = DialogLayout::Ink;
  const NetworkStrings& t = text().network;
  const std::string root = dialogPath_, name = serverName(root);
  auto close = [this] {
    dialog_ = Dialog::None;
    return MenuCommand{};
  };
  auto forget = [this, root] {
    services_.forgetLogin(root);
    dialog_ = Dialog::None;
    return MenuCommand{};
  };
  if (dialog_ == Dialog::KeyRefused) {
    dialog.title(t.keyRefusedTitle);
    dialog.line(stashPrinted(t.keyRefusedText, name));
    dialog.line(t.keyRefusedHint, Ink::Dim, 2);
    dialog.button(textButton(t.notNow, Icon::None, close));
    dialog.button(textButton(t.remember, Icon::None, [this, root] {
      services_.rememberLogin(root);
      dialog_ = Dialog::None;
      return MenuCommand{};
    }, true));
  } else if (dialog_ == Dialog::HostKeyChanged) {
    // Don't connect is the one to take, at the right; trusting the new key
    // left of it, red only when pointed.
    dialog.title(stashPrinted(t.changedTitle, name), Icon::Warning);
    dialog.line(t.changedText, Ink::Warn);
    const double labelW =
        std::max(textWidth(t.trustedKey, DialogLayout::kLineSize, false),
                 textWidth(t.nowKey, DialogLayout::kLineSize, false)) + kit::kSpaceEdge;
    dialog.key(t.trustedKey, labelW, network_.form.trusted);
    dialog.key(t.nowKey, labelW, network_.form.fingerprint);
    Button trust = textButton(t.trustNewKey, Icon::None, [this] {
      dialog_ = Dialog::None;
      connectSftp(network_.form.fingerprint);
      return MenuCommand{};
    });
    trust.dangerHover = true;
    trust.disabled = network_.form.signing != nullptr;
    dialog.button(trust);
    dialog.button(textButton(t.dontConnect, Icon::None, [this] {
      dialog_ = Dialog::None;
      showNetwork();
      return MenuCommand{};
    }, true));
  } else {
    remote::Login login;
    bool remembered = false;
    remote::knownLogin(root, login, remembered);
    dialog.title(stashPrinted(t.forgetTitle, name));
    dialog.line(stashPrinted(t.forgetKeyText, login.user));
    if (dialog_ == Dialog::ForgetOffline) dialog.line(stashPrinted(t.forgetOffline, name), Ink::Warn, 2);
    if (dialog_ == Dialog::ForgetServer) {
      const bool removing = network_.form.signing != nullptr;
      Button cancel = textButton(strings().cancel, Icon::None, close);
      Button only = textButton(t.forgetOnly, Icon::None, forget);
      Button remove = textButton(removing ? t.removingKey : t.removeKeyForget, Icon::Trash, [this, root] {
        network_.form = {};
        auto job = std::make_shared<RemoteJob>();
        job->task = RemoteJob::Task::RemoveKey;
        network_.form.signing = job;
        services_.removeKey(job, root);
        return MenuCommand{};
      }, true);
      for (Button* b : {&cancel, &only, &remove}) {
        b->disabled = removing;
        dialog.button(*b);
      }
    } else {
      dialog.button(textButton(strings().cancel, Icon::None, close));
      dialog.button(textButton(t.forgetAnyway, Icon::None, forget, true));
    }
  }
}

// Its name as found on the network, else as signed in to (or typed).
std::string Menu::serverName(const std::string& root) const {
  for (const SmbDiscovery::Server& found : network_.discovery.servers())
    if (remote::root("smb://" + found.address) == root) return found.name;
  for (const remote::Server& s : remote::servers())
    if (s.root == root) return s.name;
  return remote::host(root);
}

// A locator as the browser shows it: the server's name, then the share and
// folders ("NAS/media/films").
std::string Menu::shownLocation(const std::string& dir) const {
  if (!remote::isRemote(dir)) return dir;
  const size_t path = std::min(dir.find('/', dir.find("://") + 3), dir.size());
  return serverName(remote::root(dir)) + dir.substr(path);
}
