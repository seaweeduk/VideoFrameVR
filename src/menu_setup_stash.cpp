#include "menu_setup.h"

#include "menu_stash.h"
#include "menu_style.h"
#include "platform.h"
#include "stash_client.h"

// Do you use Stash? (docs/alpha/first-run.md, step 4): what Stash adds,
// then its address (one small request says whether it answers and has a
// login), and its sign-in. Once Stash accepts, setup ends: main.cpp sets
// Stash up as Settings → Stash would, and its first sync runs in the
// background while the Library opens.

namespace {

using Step = SetupMenu::Step;
using Page = SetupStash::Page;
using Answer = StashClient::Answer;

using setup::kH;
using setup::kW;
// Do you use Stash?: what it adds, two to a row, an icon in the accent
// before each title and its line; a line under them.
constexpr double kLeft = setup::kTextLeft, kInner = kW - 2 * kLeft;
constexpr double kFormW = 940;  // the address's or the sign-in's group
constexpr double kRowsY = setup::kTop + kit::kSpaceEdge, kRowStep = 128, kEndY = kRowsY + 3 * kRowStep + 36;

// The address as typed, spaces and a trailing slash dropped (Connect tries
// it with https, http and Stash's port: StashClient::check()).
std::string trimmedAddress(const std::string& typed) {
  const size_t first = typed.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  std::string url = typed.substr(first, typed.find_last_not_of(" \t\r\n") - first + 1);
  while (url.size() > 1 && url.back() == '/') url.pop_back();
  return url;
}

// Whether the typed address's certificate is accepted without being
// verified: the user chose that for its host.
bool unverified(const SetupStash& st) {
  const std::string url = trimmedAddress(st.text[SetupStash::Address]);
  return !st.unverifiedHost.empty() && !url.empty() && stashHost(url) == st.unverifiedHost;
}

// One "•" per character typed.
std::string hidden(const std::string& text) {
  std::string dots;
  for (char c : text)
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) dots += "•";
  return dots;
}

}  // namespace

std::string* Menu::setupText() const {
  return setup_ ? SetupPages::typed(const_cast<Menu&>(*this)) : nullptr;
}

// Typing changes what Connect or Sign in would send: one under way stops.
// A computer's name: Connect waits for one.
void SetupPages::setText(Menu& m, const std::string& text) {
  if (m.setup_->step == Step::Folders) {
    // A computer's name (it may say SMB or SFTP), or an SFTP one's port
    // (digits) or user name: Connect, or the form, may change with it.
    SetupFolders& f = *m.setup_->folders;
    std::string& field = *typed(m);
    std::string value = text;
    if (&field == &f.port) {
      value.erase(std::remove_if(value.begin(), value.end(), [](char c) { return !std::isdigit(static_cast<unsigned char>(c)); }),
                  value.end());
      value.resize(std::min<size_t>(value.size(), 5));
    }
    const auto blank = [](const std::string& t) { return t.find_first_not_of(' ') == std::string::npos; };
    bool relayout = blank(value) != blank(field);
    if (&field == &f.computer) {
      const int type = Menu::serverType(value);
      if (type >= 0 && (type == 1) != f.sftp) {
        f.sftp = type == 1;
        relayout = true;
      }
      relayout = relayout || (value.find('@') == std::string::npos) != (field.find('@') == std::string::npos);
    }
    field = value;
    if (relayout) m.dirty_ = true;
    m.typedChanged();
    return;
  }
  SetupStash& st = m.setup_->stash;
  std::string& field = st.text[st.field];
  if (text.empty() != field.empty() || st.asking.running() || !st.error.empty())
    m.dirty_ = true;  // its button, or the line under it
  field = text;
  if (st.asking.running()) stopAsking(m);
  st.error.clear();
  st.untrusted.clear();
  m.typedChanged();
}

void SetupPages::openStashPage(Menu& m, Page page) {
  SetupStash& st = m.setup_->stash;
  stopAsking(m);
  st.page = page;
  st.error.clear();
  st.untrusted.clear();
  st.field = page == Page::SignIn ? SetupStash::User : SetupStash::Address;
  st.showPassword = false;
  st.text[SetupStash::Password].clear();
  m.editing_ = page != Page::Ask;  // the keyboard up at once
  m.dirty_ = true;
}

void SetupPages::stopAsking(Menu& m) {
  SetupStash& st = m.setup_->stash;
  if (st.client) st.client->cancel();
  st.client.reset();
  st.asking = {};
  m.dirty_ = true;
}

// Connect's one request, or Sign in; a server that doesn't answer holds up
// nothing, and Back drops it.
void SetupPages::askStash(Menu& m) {
  SetupStash& st = m.setup_->stash;
  const std::string url = trimmedAddress(st.text[SetupStash::Address]);
  if (url.empty() || st.asking.running()) return;
  st.error.clear();
  st.untrusted.clear();
  auto client = std::make_shared<StashClient>(url, std::string(), unverified(st));
  st.client = client;
  if (st.page == Page::SignIn) {
    st.asking.start([client, user = st.text[SetupStash::User], password = st.text[SetupStash::Password]] {
      SetupStash::Reply r;
      bool refused = false;
      const bool ok = client->signIn(user, password, r.apiKey, refused);
      r.answer = static_cast<int>(ok ? Answer::Open : Answer::Failed);
      r.error = refused ? strings().stashSetup.wrongLogin : client->error();
      r.untrusted = !ok && client->problem() == StashProblem::Certificate;
      return r;
    });
  } else {
    st.asking.start([client] {
      SetupStash::Reply r;
      r.answer = static_cast<int>(client->check());
      r.url = client->server();
      r.error = client->error();
      r.untrusted = client->problem() == StashProblem::Certificate;
      return r;
    });
  }
  m.dirty_ = true;
}

// Each frame while the step shows: Stash's answer. Found, the field shows
// the address it answered at.
void SetupPages::updateStash(Menu& m) {
  SetupStash& st = m.setup_->stash;
  SetupStash::Reply r;
  if (!st.asking.take(r)) return;
  st.client.reset();
  m.dirty_ = true;
  const SetupStrings& t = m.text().setup;
  const Answer answer = static_cast<Answer>(r.answer);
  if (st.page == Page::Address && (answer == Answer::Open || answer == Answer::NeedsSignIn))
    st.text[SetupStash::Address] = r.url;
  switch (answer) {
    case Answer::Open: m.command_ = stashAccepted(m, r.apiKey); break;
    case Answer::NeedsSignIn: openStashPage(m, Page::SignIn); break;
    case Answer::Unreached: st.error = kStandaloneHeadset ? t.unreachableHeadset : t.unreachableComputer; break;
    case Answer::Failed:
      st.error = r.error;
      // A certificate that isn't trusted: the page says what connecting
      // anyway means, and Connect offers it.
      if (r.untrusted && !unverified(st)) st.untrusted = stashHost(trimmedAddress(st.text[SetupStash::Address]));
      break;
  }
}

// Stash is set up (MenuCommand::SetupStash) and setup ends; its first sync
// runs meanwhile.
MenuCommand SetupPages::stashAccepted(Menu& m, const std::string& apiKey) {
  SetupStash& st = m.setup_->stash;
  MenuCommand c;
  c.type = MenuCommand::SetupStash;
  c.path = trimmedAddress(st.text[SetupStash::Address]) + "\n" + apiKey;
  c.option = unverified(st) ? 1 : 0;
  st.text[SetupStash::Password].clear();
  m.editing_ = false;
  return c;
}

void SetupPages::layoutStash(Menu& m) {
  using Icon = Menu::Icon;
  SetupStash& st = m.setup_->stash;
  const SetupStrings& t = m.text().setup;
  const NetworkStrings& n = m.text().network;
  const bool asking = st.asking.running();
  if (st.page == Page::Ask) {
    // Not now and Connect Stash; with Stash connected already (by setup
    // left and started again before it asked for folders first), Change
    // and Continue. Either way on to the Library.
    const bool connected = m.stash_ != nullptr;
    auto open = [&m, connected] {
      if (connected) m.setup_->stash.text[SetupStash::Address] = m.view_.stash.url;
      openStashPage(m, Page::Address);
      return MenuCommand{};
    };
    auto on = command(MenuCommand::EndSetup, 1);
    Menu::Action other{connected ? t.change : t.notNow, "", connected ? std::function<MenuCommand()>(open) : on};
    Menu::Action primary{connected ? t.next : t.connectStash, "",
                         connected ? std::function<MenuCommand()>(on) : open};
    primary.primary = true;
    // No Set up later: Not now is it. Back to the folders as they were left.
    addFooter(m, back(m, [&m] {
                setStep(m, Step::Folders);
                if (!m.setup_->places.empty()) m.setup_->folders->page = SetupFolders::Page::Added;
                return MenuCommand{};
              }),
              Icon::Arrow, {other, primary});
    return;
  }

  // The address, or the sign-in's fields, in a group (the kit's form): the
  // sign-in's titled with the address; why it failed under them.
  const bool signIn = st.page == Page::SignIn;
  Menu::PageLayout page(m, Menu::PageLayout::UnderHeader{}, t.stashTitle);
  Menu::GroupLayout g = page.groupAt(0, setup::kLeft, setup::kTop, kFormW,
                                     signIn ? trimmedAddress(st.text[SetupStash::Address]) : std::string(t.address));
  auto field = [&](int i, const std::string& label) {
    Menu::Field f;
    f.text = i == SetupStash::Password && !st.showPassword ? hidden(st.text[i]) : st.text[i];
    f.example = i == SetupStash::Address ? m.text().stashSetup.serverHint : "";
    f.typing = st.field == i && m.editing_;
    f.action = [&m, i] {
      m.setup_->stash.field = i;
      m.editing_ = true;
      return MenuCommand{};
    };
    if (i == SetupStash::Password) {
      f.inner = st.showPassword ? m.text().hide : n.show;
      f.innerAction = [&m] {
        m.setup_->stash.showPassword = !m.setup_->stash.showPassword;
        return MenuCommand{};
      };
    }
    g.field(label, f);
  };
  if (signIn) {
    g.labelsFor({n.fields[1], n.fields[2]});
    field(SetupStash::User, n.fields[1]);
    field(SetupStash::Password, n.fields[2]);
  } else {
    field(SetupStash::Address, "");
  }
  if (!st.untrusted.empty()) {
    // As an SFTP server's changed key: what connecting anyway means, and
    // its risk in red (drawStash()).
    const StashSetupStrings& ts = m.text().stashSetup;
    g.status(Lamp::Failed, stashPrinted(ts.untrustedTitle, st.untrusted));
    g.note(ts.untrusted, true, 3);
    const int lines = m.wrappedLines(ts.untrustedRisk, g.rowWidth() - 2 * kit::kSpace, kit::kNoteSize, 2, true);
    const Menu::GroupLayout::Box risk = g.room(lines * kit::kNoteLine);
    st.risk = {risk.x, risk.y, risk.w};
  } else if (!st.error.empty()) {
    g.status(Lamp::Failed, st.error);
  }
  // Back stops a request under way, else goes back a page. After a
  // certificate that isn't trusted, Connect connects without verifying it,
  // and isn't the button selected.
  const bool untrusted = !st.untrusted.empty() && !asking;
  const std::string go = asking      ? (signIn ? n.signingIn : t.connecting)
                         : untrusted ? m.text().stashSetup.connectUnverified
                         : signIn    ? n.signIn
                                     : t.connect;
  Menu::Action connect{go, "", [&m] {
                         SetupStash& st = m.setup_->stash;
                         if (!st.untrusted.empty()) st.unverifiedHost = st.untrusted;
                         askStash(m);
                         return MenuCommand{};
                       }};
  connect.primary = !untrusted;
  connect.disabled = asking || trimmedAddress(st.text[SetupStash::Address]).empty() ||
                     (signIn && st.text[SetupStash::User].empty());
  addFooter(m, back(m, [&m, signIn] {
              SetupStash& st = m.setup_->stash;
              if (st.asking.running() && !signIn)
                stopAsking(m);
              else
                openStashPage(m, signIn ? Page::Address : Page::Ask);
              return MenuCommand{};
            }),
            Icon::Arrow, {connect});
  if (m.editing_) m.layoutKeyboard(Menu::kSetup, kW / 2, kH + 34, 90);
}

void SetupPages::drawStash(Menu& m, cairo_t* cr) {
  SetupStash& st = m.setup_->stash;
  const SetupStrings& t = m.text().setup;
  const Palette pal = m.palette();
  if (st.page == Page::Ask) {
    drawHead(m, cr, t.stashTitle, t.stashText, kInner);
    // With no folders added, every scene plays from Stash: with Stash off,
    // only those downloaded.
    const bool folders = !m.setup_->places.empty();
    const StashIcon stashIcons[6] = {StashIcon::Scenes, StashIcon::None, StashIcon::Performers,
                                     StashIcon::Filters, StashIcon::None, StashIcon::None};
    const Menu::Icon menuIcons[6] = {Menu::Icon::None, Menu::Icon::Play, Menu::Icon::None,
                                     Menu::Icon::None, Menu::Icon::Refresh, Menu::Icon::Power};
    const double accent[4] = {pal.textAccent.r, pal.textAccent.g, pal.textAccent.b, 1};
    const double columnW = (kInner - 40) / 2;
    for (int k = 0; k < 6; ++k) {
      const double x = kLeft + (k % 2) * (columnW + 40), y = kRowsY + (k / 2) * kRowStep;
      const double cx = x + 32, cy = y + 36;
      if (stashIcons[k] != StashIcon::None) {
        StashPages::drawIcon(cr, stashIcons[k], cx, cy, 54, accent);
      } else if (menuIcons[k] == Menu::Icon::Play) {  // in a ring
        cairo_new_path(cr);
        cairo_arc(cr, cx, cy, 20, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, accent[0], accent[1], accent[2], 1);
        cairo_set_line_width(cr, 54 * 0.075);
        cairo_stroke(cr);
        Menu::drawIcon(cr, Menu::Icon::Play, cx + 1, cy, 18, accent[0], accent[1], accent[2]);
      } else {
        Menu::drawIcon(cr, menuIcons[k], cx, cy, 54, accent[0], accent[1], accent[2]);
      }
      const char* line = t.stashFeatures[2 * k + 1];
      if (k == 5) line = !folders ? t.forDownloads : kStandaloneHeadset ? line : t.keptOnComputer;
      m.drawText(cr, t.stashFeatures[2 * k], x + 88, y, columnW - 88, 44, 32, true, false, pal.text.r,
                 pal.text.g, pal.text.b);
      m.drawText(cr, line, x + 88, y + 48, columnW - 88, 36, 26, false, false, pal.textDim.r, pal.textDim.g,
                 pal.textDim.b);
    }
    if (m.stash_) {
      Menu::drawIcon(cr, Menu::Icon::Check, kLeft + 14, kEndY + 20, 26, accent[0], accent[1], accent[2]);
      char line[512];
      std::snprintf(line, sizeof(line), t.stashConnected, m.view_.stash.url.c_str());
      m.drawText(cr, line, kLeft + 40, kEndY, kInner - 40, 40, 24, false, false, pal.text.r,
                 pal.text.g, pal.text.b);
    } else {
      m.drawText(cr, folders ? t.stashFree : t.stashFreeNoFolders, kLeft, kEndY, kInner, 40, 24, false, false,
                 pal.textDim.r, pal.textDim.g, pal.textDim.b);
    }
  } else {
    if (st.page == Page::Address)
      drawHead(m, cr, t.connectStash, t.addressText);
    else
      drawHead(m, cr, t.stashSignInTitle, t.stashSignInText);
    if (!st.untrusted.empty()) {
      const Rgba warn = hexColour(0xff8a85);
      const double red[4] = {warn.r, warn.g, warn.b, 1};
      m.drawTextLines(cr, m.text().stashSetup.untrustedRisk, st.risk[0] + kit::kSpace, st.risk[1],
                      st.risk[2] - 2 * kit::kSpace, kit::kNoteSize, 2, red, false, 0, true);
    }
  }
  drawDots(m, cr);
  drawKeyboardPanel(m, cr);
}
