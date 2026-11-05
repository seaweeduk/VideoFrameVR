#include "menu_style.h"

#include "menu_setup.h"
#include "menu_stash.h"  // stashPrinted
#include "platform.h"

#include "remote.h"
#include "sftp.h"

#include <algorithm>
#include <cstring>
#include <thread>

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

constexpr double kRed[4] = {1.0, 0.45, 0.4, 1};

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
  smbForm_ = {};
  showPage(Page::Network);
  discovery_.scan();
}

// The servers to list: those signed in to, then those found that aren't,
// by name.
std::vector<remote::Server> Menu::networkServers() const {
  std::vector<remote::Server> list = remote::servers();
  for (const SmbDiscovery::Server& found : discovery_.servers()) {
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
  fileKind_ = FileKind::Video;
  openDirectory(server.root);
  showPage(Page::Files);
}

// "Network", Add server and Look again; then the servers, each opening its
// shares (an SFTP server its "/"), those signed in to with Forget at their
// end: a key login asks whether to take the key off the server too.
void Menu::layoutNetworkPage() {
  const NetworkStrings& t = text().network;
  const double x = kPageX + kPagePad, right = kPageX + kPageWidth - kPagePad;
  addLabel(x + 12, kBrowserRowY, 600, kBrowserRowH, t.nav, 30);
  double buttonX = right;
  for (auto [label, action] : {std::pair<const char*, std::function<MenuCommand()>>{
                                   t.addServer,
                                   [this] {
                                     openSmbForm(SmbForm::Kind::AddServer, "");
                                     return MenuCommand{};
                                   }},
                               {t.lookAgain, [this] {
                                  discovery_.scan();
                                  dirty_ = true;
                                  return MenuCommand{};
                                }}}) {
    Button b = textButton(label, Icon::None, action, label == t.addServer);
    b.panel = kBrowser;
    b.x = buttonX - b.w;
    b.y = kBrowserRowY;
    b.h = kBrowserRowH;
    b.disabled = label == t.lookAgain && discovery_.scanning();
    buttons_.push_back(b);
    buttonX = b.x - kGap;
  }
  if (smbForm_.kind != SmbForm::Kind::None) {
    layoutSmbForm(kBrowserToolsY);
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
          remote::forget(root);
        dirty_ = true;
        return MenuCommand{};
      };
    }
    items.push_back(std::move(item));
  }
  listTop_ = kBrowserToolsY;
  listBottom_ = kListBottom;
  listRight_ = right + 6;
  addList(items, x, kFileRowH, kFileRows);
}

// Over the page: what an empty Network page means.
const char* Menu::networkState() const {
  if (page_ != Page::Network || smbForm_.kind != SmbForm::Kind::None || listCount_ > 0) return nullptr;
  return discovery_.scanning() ? text().network.looking : text().network.none;
}

// Opens the form; on the Files page it belongs to the server being browsed.
void Menu::openSmbForm(SmbForm::Kind kind, const std::string& root) {
  const NetworkStrings& t = text().network;
  smbForm_ = {};
  smbForm_.kind = kind;
  smbForm_.root = root;
  remote::Login login;
  bool remembered = true;
  const bool known = kind == SmbForm::Kind::SignIn && remote::knownLogin(root, login, remembered);
  smbForm_.text[1] = login.user;
  smbForm_.remember = remembered;
  // A login known but refused: the password has changed.
  if (known) smbForm_.error = t.wrongLogin;
  smbForm_.field = kind == SmbForm::Kind::AddServer ? 0
                   : kind == SmbForm::Kind::Share   ? 0
                   : login.user.empty()            ? 1
                                                   : 2;
  editing_ = true;
  dirty_ = true;
}

// The form's fields (the one typed into outlined blue), Remember password,
// the error if any, then Cancel and Sign in (or Open); the keyboard under it.
// Add server chooses SMB or SFTP first; an SFTP server's sign-in, how.
void Menu::layoutSmbForm(double top) {
  const NetworkStrings& t = text().network;
  const SmbForm& f = smbForm_;
  const double x0 = kPageX + kPagePad, w = kPageWidth - 2 * kPagePad;
  const double x = x0 + kGroupPad, inner = w - 2 * kGroupPad;
  constexpr double labelW = 300, h = 56, step = 66;
  const bool share = f.kind == SmbForm::Kind::Share;
  const bool signing = f.signing != nullptr;
  pageScrollMax_ = 0;  // drawGroups() mustn't cut it off as a scrolled settings page
  double y = firstRow(top);
  // A hint (an error not dim) under a row, on two lines if it needs them.
  auto hint = [&](const std::string& text, bool dim) {
    const int lines = wrappedLines(text, inner - 12, kHintSize, 2);
    addLabel(x + 12, lines > 1 ? y - 15 : y, inner - 12, 0, text, kHintSize, dim, lines);
    y += hintHeight(lines);
  };
  if (share) {
    addLabel(x + 12, y, inner - 12, 0, t.sharesRefused, kHintSize, true, 1);
    y += hintHeight(1) + 8;
  }
  if (f.kind == SmbForm::Kind::AddServer) {
    addChoiceRow(x, y, inner, labelW, t.type, {t.types[0], t.types[1]}, f.sftp, [this](int i) {
      smbForm_.sftp = i == 1;
      smbForm_.field = 0;
      smbForm_.error.clear();
      smbForm_.hint.clear();
      editing_ = true;
      return MenuCommand{};
    });
    // The hint belongs to the row above: clear of its buttons, nearer them
    // than the fields under it.
    y += kSettingStep + 12;
    hint(t.typeHint, true);
    y -= 10;
  } else if (f.sftp) {
    layoutSftpForm(x, y, inner);
  }
  for (const int i : smbFields()) {
    const bool active = f.field == i && editing_;
    const std::string& typed = f.text[i];
    const char* placeholder = i == 0 && !share ? (f.sftp ? t.sftpServerHint : t.serverHint)
                              : i == 1         ? (f.sftp ? t.sftpUserHint : t.userHint)
                                               : "";
    addLabel(x + 12, y, labelW - 12, h, share ? t.shareField : i == 3 ? t.port : t.fields[i]);
    std::string shown = i == 2 && !f.showPassword ? hidden(typed) : typed;
    Button b = textButton(typed.empty() && !active ? std::string(placeholder) : shown,
                          Icon::None, [this, i] {
                            smbForm_.field = i;
                            editing_ = true;
                            return MenuCommand{};
                          });
    b.panel = kBrowser;
    b.x = x + labelW;
    b.y = y;
    b.w = i == 3 ? 200 : inner - labelW - (i == 2 ? 130 : 0);
    b.h = h;
    b.field = true;
    b.placeholder = typed.empty() && !active;
    b.on = active;
    b.caret = active ? caret() : -1;
    b.radius = 12;
    b.fontSize = kTextSize;
    buttons_.push_back(b);
    if (i == 2) {
      Button eye = textButton(f.showPassword ? text().hide : t.show, Icon::None, [this] {
        smbForm_.showPassword = !smbForm_.showPassword;
        return MenuCommand{};
      });
      eye.panel = kBrowser;
      eye.x = x + inner - 120;
      eye.y = y;
      eye.w = 120;
      eye.h = h;
      buttons_.push_back(eye);
    }
    y += step;
  }
  // SFTP: only a password sign-in can be remembered (a key login keeps no secret).
  const bool remember = !share && (!f.sftp || (f.kind == SmbForm::Kind::SignIn && !f.byKey));
  if (remember) {
    addToggleRow(x, y, inner, t.remember, f.remember, [this] {
      smbForm_.remember = !smbForm_.remember;
      return MenuCommand{};
    });
    y += kSettingStep;
    hint(t.rememberHint, true);
  }
  // A key-only form's error is under its command while it shows.
  if (!f.error.empty() && f.command.empty()) {
    if (!remember && f.kind != SmbForm::Kind::KeyOnly) y += 12;  // clear of the field above
    hint(f.error, false);
  }
  if (!f.hint.empty() && f.command.empty()) {
    hint(f.hint, true);
  }
  y += 8;
  std::string label = signing ? t.signingIn : share ? t.open : t.signIn;
  bool ready = true;
  if (f.sftp && f.kind == SmbForm::Kind::AddServer) {
    label = signing ? t.connecting : t.connect;
    const TypedServer typed = splitServer(f.text[0]);
    ready = !typed.host.empty() && (!f.text[1].empty() || !typed.user.empty()) && typedPort(f.text[3]);
  } else if (f.sftp && f.kind == SmbForm::Kind::SignIn && f.byKey && signing) {
    label = t.settingUpKey;
  } else if (f.kind == SmbForm::Kind::KeyOnly && !signing) {
    label = t.tryAgain;
  }
  // An SFTP server's button signs in (connects) from any field, as setup's.
  Button go = textButton(label, Icon::None, [this] {
    if (smbForm_.sftp && !smbFields().empty()) smbForm_.field = smbFields().back();
    return submitSmbForm();
  }, true);
  go.panel = kBrowser;
  go.x = x + inner - go.w;
  go.y = y;
  go.h = h;
  go.disabled = signing || !ready;
  buttons_.push_back(go);
  Button cancel = textButton(strings().cancel, Icon::None, [this] {
    const bool adding = smbForm_.kind == SmbForm::Kind::AddServer;
    smbForm_ = {};
    editing_ = false;
    if (!adding) showNetwork();
    dirty_ = true;
    return MenuCommand{};
  });
  cancel.panel = kBrowser;
  cancel.x = go.x - kGap - cancel.w;
  cancel.y = y;
  cancel.h = h;
  buttons_.push_back(cancel);
  y += h + 16;
  std::string title = share ? t.shareTitle : t.addTitle;
  if (f.kind != SmbForm::Kind::AddServer && !share) title = stashPrinted(t.signInTitle, serverName(f.root));
  addGroup(x0, top, w, y - top, title);
  if (editing_) layoutKeyboard();
}

// Above the fields of an SFTP server's sign-in: how to sign in, when it
// offers both. A server taking keys only: this device's key to add to the
// account, and where; while it is served (key_share.h), first the command
// that adds it from the server.
void Menu::layoutSftpForm(double x, double& y, double inner) {
  const NetworkStrings& t = text().network;
  SmbForm& f = smbForm_;
  constexpr double labelW = 300;
  const std::string name = serverName(f.root), user = f.text[1];
  auto hint = [&](const std::string& text, bool dim = true) {  // as layoutSmbForm()'s
    const int lines = wrappedLines(text, inner - 12, kHintSize, 2);
    addLabel(x + 12, lines > 1 ? y - 15 : y, inner - 12, 0, text, kHintSize, dim, lines);
    y += hintHeight(lines);
  };
  if (f.kind == SmbForm::Kind::SignIn && f.keyChoice) {
    addChoiceRow(x, y, inner, labelW, t.signInWith, {t.methods[0], t.methods[1]}, f.byKey ? 0 : 1,
                 [this](int i) {
                   smbForm_.byKey = i == 0;
                   return MenuCommand{};
                 });
    y += kSettingStep + 12;  // as Add server's type row
    hint(f.byKey ? stashPrinted(t.keyHint, user) : std::string(t.passwordHint));
    y -= 10;
  } else if (f.kind == SmbForm::Kind::KeyOnly) {
    // With the command, what it's for goes without saying.
    const std::string line = f.command.empty() ? stashPrinted(t.keyOnly, name, user) : stashPrinted(t.keysOnly, name);
    addLabel(x + 12, y, inner - 12, 0, line, kTextSize, false, 2);
    y += 18 + wrappedLines(line, inner - 12, kTextSize, 2) * 34;
    const double rgba[4] = {};
    if (!f.command.empty()) {
      const std::string run = stashPrinted(t.runCommand, name, user);
      addLabel(x + 12, y, inner - 12, 0, run, kTextSize, false, 2);
      y += 6 + wrappedLines(run, inner - 12, kTextSize, 2) * 34;
      const double h = drawMono(nullptr, f.command, 0, 0, inner - 12 - 32, 24, rgba) + 24;
      f.commandBox[0] = x + 12, f.commandBox[1] = y, f.commandBox[2] = inner - 12, f.commandBox[3] = h;
      y += h + 10 + hintHeight(1) / 2;
      // How it stands, or why Try again failed (its hint, about adding the
      // key by hand, left out).
      if (f.error.empty())
        hint(keyFetched() ? std::string(t.keyFetched) : stashPrinted(t.waitingForKey, name), !keyFetched());
      else
        hint(f.error, false);
      y += 2;
    }
    addLabel(x + 12, y, labelW - 12, 56, t.deviceKey);
    const double w = inner - labelW;
    const double h = drawMono(nullptr, f.publicKey, 0, 0, w - 32, 24, rgba) + 24;
    f.keyBox[0] = x + labelW, f.keyBox[1] = y, f.keyBox[2] = w, f.keyBox[3] = h;
    y += h + 10 + hintHeight(1) / 2;
    hint(stashPrinted(t.addKeyLine, name, user));
  }
}

// What the buttons don't draw: the key (and the command), in a box as a
// field's.
void Menu::drawSmbForm(cairo_t* cr) {
  const SmbForm& f = smbForm_;
  if (f.kind != SmbForm::Kind::KeyOnly) return;
  const Palette& pal = palette();
  const double ink[4] = {pal.text.r, pal.text.g, pal.text.b, pal.text.a};
  for (const auto& [text, box] : {std::pair{&f.publicKey, f.keyBox}, {&f.command, f.commandBox}}) {
    if (text->empty()) continue;
    roundedRect(cr, box[0], box[1], box[2], box[3], 12);
    setColor(cr, pal.field);
    cairo_fill(cr);
    drawMono(cr, *text, box[0] + 16, box[1] + 12, box[2] - 32, 24, ink);
  }
}

std::vector<int> Menu::smbFields() const {
  switch (smbForm_.kind) {
    case SmbForm::Kind::AddServer: return smbForm_.sftp ? std::vector<int>{0, 3, 1} : std::vector<int>{0, 1, 2};
    case SmbForm::Kind::SignIn: return {1, 2};
    case SmbForm::Kind::Share: return {0};
    default: return {};
  }
}

// Typing into the form. An address shows which kind of server it is, and
// the form turns into that kind's; a port is digits.
void Menu::typedIntoSmbForm(const std::string& text) {
  SmbForm& f = smbForm_;
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
  SmbForm& f = smbForm_;
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
  if (server.rfind("smb://", 0) == 0) server.erase(0, 6);
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
  std::thread([job, root = f.root, login = remote::Login{f.text[1], f.text[2]}, remember] {
    job->status = remote::signIn(root, login, remember);
    job->done = true;
  }).detach();
  dirty_ = true;
  return {};
}

// The connect job: the server's key, trusted at once if it is new (a
// changed one stops it), then this device's key if the server takes keys;
// what's left to sign in with otherwise, and for a server taking keys only,
// this device's key to show.
void Menu::connectSftp(const std::string& trust) {
  SmbForm& f = smbForm_;
  if (f.signing) return;  // never in place of a job under way
  auto job = std::make_shared<RemoteJob>();
  job->task = RemoteJob::Task::Connect;
  f.sftp = true;
  f.signing = job;
  f.error.clear();
  f.hint.clear();
  sftpUsers_[f.root] = f.text[1];
  // A key login keeps no secret; --first-run's setup saves it when it ends
  // (SetupPages::signedIn()).
  const bool remember = !(setup_ && setup_->holdWrites);
  std::thread([job, root = f.root, user = f.text[1], trust, remember] {
    using Status = remote::Status;
    using HostKey = sftp::Probe::HostKey;
    RemoteJob& j = *job;
    // Trusting writes sftp-hosts.json at once, under --first-run too: the
    // sign-ins after it check the server's key against that file.
    if (!trust.empty()) sftp::trust(root, trust);
    j.status = sftp::probe(root, user, j.probe);
    if (j.status == Status::Ok && j.probe.hostKey == HostKey::Changed) {
      j.status = Status::HostKeyChanged;
    } else if (j.status == Status::Ok) {
      if (j.probe.hostKey == HostKey::New) sftp::trust(root, j.probe.fingerprint);
      j.status = j.probe.key ? remote::signIn(root, {user, "", true}, remember) : Status::NeedsLogin;
      if (j.status == Status::NeedsLogin && !j.probe.password) j.publicKey = sftp::publicKey();
      // Changed since the probe: the warning shows the key it has now.
      if (j.status == Status::HostKeyChanged) {
        const Status probed = sftp::probe(root, user, j.probe);
        if (probed != Status::Ok) j.status = probed;
      }
    }
    if (j.status == Status::HostKeyChanged) j.trusted = sftp::trusted(root);
    j.done = true;
  }).detach();
  dirty_ = true;
}

// A sign-in from the SFTP form. A key never trusted (gone meanwhile) is
// trusted as on connecting; a changed one, both keys for the warning.
void Menu::signInSftp(const remote::Login& login, bool remember) {
  SmbForm& f = smbForm_;
  auto job = std::make_shared<RemoteJob>();
  f.signing = job;
  f.error.clear();
  f.hint.clear();
  sftpUsers_[f.root] = login.user;
  remember = remember && !(setup_ && setup_->holdWrites);
  // With a password and the key, the key is added to the account on the
  // server at once, under --first-run too: it changes the server's
  // account, not this profile.
  std::thread([job, root = f.root, login, remember] {
    using Status = remote::Status;
    RemoteJob& j = *job;
    j.status = remote::signIn(root, login, remember);
    if (j.status == Status::HostKeyUnknown || j.status == Status::HostKeyChanged) {
      const Status probed = sftp::probe(root, login.user, j.probe);
      if (probed != Status::Ok) {
        j.status = probed;
      } else if (j.probe.hostKey == sftp::Probe::HostKey::Changed) {
        j.status = Status::HostKeyChanged;
        j.trusted = sftp::trusted(root);
      } else {
        if (j.probe.hostKey == sftp::Probe::HostKey::New) sftp::trust(root, j.probe.fingerprint);
        j.status = remote::signIn(root, login, remember);
      }
    }
    j.done = true;
  }).detach();
  dirty_ = true;
}

// Signed in (smbForm_'s server): its root, or the folder typed after it, on
// the Files page; in setup, its tree.
void Menu::openSignedIn() {
  const std::string where = smbForm_.root + (smbForm_.then.empty() ? "" : "/" + smbForm_.then);
  remote::setName(smbForm_.root, serverName(smbForm_.root));
  if (setup_) {
    SetupPages::signedIn(*this);
    return;
  }
  smbForm_ = {};
  editing_ = false;
  fileKind_ = FileKind::Video;
  if (where == directory())
    refreshDirectory();
  else
    openDirectory(where);
  showPage(Page::Files);
}

// A sign-in finished, or the folder being browsed asks for one (or doesn't
// list its shares).
void Menu::updateNetwork() {
  if (discovery_.poll() && (page_ == Page::Network || setup_)) dirty_ = true;
  updateKeyShare();
  if (!smbForm_.signing || !smbForm_.signing->done) return;
  const std::shared_ptr<RemoteJob> job = std::move(smbForm_.signing);
  smbForm_.signing.reset();
  dirty_ = true;
  if (job->task == RemoteJob::Task::RemoveKey) {  // Forget's
    if (job->status == remote::Status::Ok) {
      dialog_ = Dialog::None;
    } else {
      dialog_ = Dialog::ForgetOffline;
    }
    return;
  }
  if (smbForm_.sftp) {
    sftpDone(*job);
    return;
  }
  const remote::Status status = job->status;
  const NetworkStrings& t = text().network;
  if (status == remote::Status::Ok) {
    openSignedIn();
    return;
  }
  smbForm_.error = status == remote::Status::Offline ? t.offline
                   : status == remote::Status::Denied ? t.denied
                                                    : t.wrongLogin;
  if (setup_) SetupPages::signInFailed(*this);
}

// Since the key was fetched, signing in by itself (or with Try again).
bool Menu::keyFetched() const {
  const SmbForm& f = smbForm_;
  return f.fetches > 0 && (f.retryUntil != std::chrono::steady_clock::time_point{} || f.signing);
}

// The key-only form's key share: the command shown while it serves (not
// once it stops, after ten minutes); after each fetch, the key's sign-in
// tried by itself (as Try again), a second later and then every three
// seconds for a minute; then the error, as for Try again.
void Menu::updateKeyShare() {
  using namespace std::chrono_literals;
  SmbForm& f = smbForm_;
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
  SmbForm& f = smbForm_;
  const std::string root = f.root, name = serverName(root), user = f.text[1];
  switch (job.status) {
    case Status::Ok:
      openSignedIn();
      return;
    case Status::KeyRefused:
      if (setup_) {
        remote::setName(root, name);
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
  if (page_ != Page::Files || !remote::isRemote(directory()) || smbForm_.kind != SmbForm::Kind::None ||
      smbForm_.signing)
    return;
  const std::string root = remote::root(directory());
  const remote::Status status = listing_.status();
  if (isSftp(root)) {
    if (status != remote::Status::NeedsLogin && status != remote::Status::HostKeyUnknown &&
        status != remote::Status::HostKeyChanged)
      return;
    remote::Login login;
    bool remembered = false;
    const auto tried = sftpUsers_.find(root);
    smbForm_ = {};
    smbForm_.sftp = true;
    smbForm_.root = root;
    smbForm_.then = directory().size() > root.size() ? directory().substr(root.size() + 1) : "";
    smbForm_.text[1] = remote::knownLogin(root, login, remembered) ? login.user
                       : tried != sftpUsers_.end()                ? tried->second
                                                                   : "";
    if (smbForm_.text[1].empty()) {
      smbForm_.kind = SmbForm::Kind::AddServer;
      smbForm_.text[0] = remote::host(root) + (smbForm_.then.empty() ? "" : "/" + smbForm_.then);
      smbForm_.text[3] = std::to_string(remote::port(root));
      smbForm_.field = 1;
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
void Menu::layoutNetworkDialog(std::vector<Button>& row) {
  const NetworkStrings& t = text().network;
  const std::string root = dialogPath_;
  auto close = [this] {
    dialog_ = Dialog::None;
    return MenuCommand{};
  };
  auto forget = [this, root] {
    remote::forget(root);
    dialog_ = Dialog::None;
    return MenuCommand{};
  };
  if (dialog_ == Dialog::KeyRefused) {
    row.push_back(textButton(t.notNow, Icon::None, close));
    row.push_back(textButton(t.remember, Icon::None, [this, root] {
      remote::remember(root);
      dialog_ = Dialog::None;
      return MenuCommand{};
    }, true));
  } else if (dialog_ == Dialog::HostKeyChanged) {
    row.push_back(textButton(t.dontConnect, Icon::None, [this] {
      dialog_ = Dialog::None;
      showNetwork();
      return MenuCommand{};
    }, true));
    Button trust = textButton(t.trustNewKey, Icon::None, [this] {
      dialog_ = Dialog::None;
      connectSftp(smbForm_.fingerprint);
      return MenuCommand{};
    });
    trust.danger = true;
    trust.disabled = smbForm_.signing != nullptr;
    row.push_back(trust);
  } else if (dialog_ == Dialog::ForgetServer) {
    const bool removing = smbForm_.signing != nullptr;
    Button cancel = textButton(strings().cancel, Icon::None, close);
    Button only = textButton(t.forgetOnly, Icon::None, forget);
    Button remove = textButton(removing ? t.removingKey : t.removeKeyForget, Icon::Trash, [this, root] {
      smbForm_ = {};
      auto job = std::make_shared<RemoteJob>();
      job->task = RemoteJob::Task::RemoveKey;
      smbForm_.signing = job;
      // Forgotten here, not when the menu takes the result: closing the
      // menu meanwhile drops the form, and with it the job.
      std::thread([job, root] {
        job->status = sftp::removeKey(root);
        if (job->status == remote::Status::Ok) remote::forget(root);
        job->done = true;
      }).detach();
      return MenuCommand{};
    }, true);
    for (Button* b : {&cancel, &only, &remove}) {
      b->disabled = removing;
      row.push_back(*b);
    }
  } else {
    row.push_back(textButton(strings().cancel, Icon::None, close));
    row.push_back(textButton(t.forgetAnyway, Icon::None, forget, true));
  }
}

void Menu::drawNetworkDialog(cairo_t* cr, double textX, double boxY) {
  const NetworkStrings& t = text().network;
  const double textW = kDialogW - 100, dim[4] = {0.75, 0.75, 0.8, 1};
  const std::string name = serverName(dialogPath_);
  // The text, on up to three lines; what follows it moves down for a third.
  auto body = [&](const std::string& s, int size, const double rgba[4], bool bold) {
    drawTextLines(cr, s, textX, boxY + 100, textW, size, 3, rgba, true, 0, bold);
    return wrappedLines(s, textW, size, 3, bold) > 2 ? size * 1.36 : 0.0;
  };
  if (dialog_ == Dialog::KeyRefused) {
    drawText(cr, t.keyRefusedTitle, textX, boxY + 30, textW, 60, 38, true, true, 1, 1, 1);
    const double more = body(stashPrinted(t.keyRefusedText, name), 27, dim, false);
    const double hint[4] = {0.7, 0.7, 0.75, 1};
    drawTextLines(cr, t.keyRefusedHint, textX, boxY + 200 + more, textW, 23, 2, hint, true);
  } else if (dialog_ == Dialog::HostKeyChanged) {
    const std::string title = stashPrinted(t.changedTitle, name);
    const double titleW = textWidth(title, 38, true), iconX = kPageCenterX - (titleW + 60) / 2;
    drawIcon(cr, Icon::Warning, iconX + 22, boxY + 60, 44, kRed[0], kRed[1], kRed[2]);
    drawText(cr, title, iconX + 60, boxY + 30, titleW + 4, 60, 38, true, false, 1, 1, 1);
    const double more = body(t.changedText, 26, kRed, true);
    const double labelW = std::max(textWidth(t.trustedKey, 26, false), textWidth(t.nowKey, 26, false)) + 30;
    const double x = textX + 60, ink[4] = {1, 1, 1, 1};
    double y = boxY + 200 + more;
    for (const auto& [label, key] : {std::pair{t.trustedKey, smbForm_.trusted}, {t.nowKey, smbForm_.fingerprint}}) {
      drawText(cr, label, x, y, labelW, 40, 26, false, false, dim[0], dim[1], dim[2]);
      drawMono(cr, key, x + labelW, y + 5, textX + textW - 60 - x - labelW, 24, ink);
      y += 48;
    }
  } else {
    remote::Login login;
    bool remembered = false;
    remote::knownLogin(dialogPath_, login, remembered);
    drawText(cr, stashPrinted(t.forgetTitle, name), textX, boxY + 30, textW, 60, 38, true, true, 1, 1, 1);
    const double more = body(stashPrinted(t.forgetKeyText, login.user), 27, dim, false);
    if (dialog_ == Dialog::ForgetOffline)
      drawTextLines(cr, stashPrinted(t.forgetOffline, name), textX, boxY + 200 + more, textW, 25, 2, kRed, true,
                    0, true);
  }
}

// Its name as found on the network, else as signed in to (or typed).
std::string Menu::serverName(const std::string& root) const {
  for (const SmbDiscovery::Server& found : discovery_.servers())
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
