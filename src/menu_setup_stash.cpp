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

constexpr double kW = Menu::kSetupWidth, kH = Menu::kSetupHeight;
constexpr double kFooterY = kH - 48 - 72;
constexpr double kLeft = 56, kInner = kW - 2 * kLeft;
constexpr double kFieldW = 900, kFieldH = 64;
constexpr double kBlue[4] = {140 / 255.0, 190 / 255.0, 1.0, 1.0};
constexpr double kRowsY = 220, kRowStep = 128, kEndY = 640;  // what Stash adds, two to a row

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
    // No Set up later: Not now is it. Back to the folders as they were left.
    addFooter(m, [&m] {
      setStep(m, Step::Folders);
      if (!m.setup_->places.empty()) m.setup_->folders->page = SetupFolders::Page::Added;
      return MenuCommand{};
    });
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
    const std::string first = connected ? t.change : t.notNow;
    const std::string second = connected ? t.next : t.connectStash;
    Menu::Button primary = m.textButton(second, Icon::None, connected ? std::function<MenuCommand()>(on) : open, true);
    primary.fontSize = 30;
    const double primaryW = std::max(240.0, m.textWidth(second, 30, false) + 90);
    add(m, primary, kW - 48 - primaryW, kFooterY, primaryW, 72);
    Menu::Button other = m.textButton(first, Icon::None, connected ? std::function<MenuCommand()>(open) : on);
    other.fontSize = 30;
    const double otherW = std::max(200.0, m.textWidth(first, 30, false) + 90);
    add(m, other, kW - 48 - primaryW - 16 - otherW, kFooterY, otherW, 72);
    return;
  }

  // A field: its label is drawn above it, the address's hint under it
  // (drawStash()).
  auto field = [&](int i, double y, double w) {
    const bool active = st.field == i && m.editing_;
    const std::string& typed = st.text[i];
    const std::string shown = i == SetupStash::Password && !st.showPassword ? hidden(typed) : typed;
    Menu::Button b = m.textButton(shown, Icon::None, [&m, i] {
      m.setup_->stash.field = i;
      m.editing_ = true;
      return MenuCommand{};
    });
    b.field = true;
    b.on = active;
    b.caret = active ? m.caret() : -1;
    b.radius = 12;
    b.fontSize = 28;
    add(m, b, kLeft, y, w, kFieldH);
  };
  const bool signIn = st.page == Page::SignIn;
  if (signIn) {
    field(SetupStash::User, 270, kFieldW);
    field(SetupStash::Password, 400, kFieldW);
    Menu::Button eye = m.textButton(st.showPassword ? m.text().hide : n.show, Icon::None, [&m] {
      m.setup_->stash.showPassword = !m.setup_->stash.showPassword;
      return MenuCommand{};
    });
    eye.bare = true;
    eye.fontSize = 26;
    add(m, eye, kLeft + kFieldW + 16, 400, 120, kFieldH);
  } else {
    field(SetupStash::Address, 240, kFieldW);
  }
  // Back stops a request under way, else goes back a page. After a
  // certificate that isn't trusted, Connect connects without verifying it,
  // and isn't the button selected.
  const bool untrusted = !st.untrusted.empty() && !asking;
  const std::string go = asking      ? (signIn ? n.signingIn : t.connecting)
                         : untrusted ? m.text().stashSetup.connectUnverified
                         : signIn    ? n.signIn
                                     : t.connect;
  addFooter(m, [&m, signIn] {
    SetupStash& st = m.setup_->stash;
    if (st.asking.running() && !signIn)
      stopAsking(m);
    else
      openStashPage(m, signIn ? Page::Address : Page::Ask);
    return MenuCommand{};
  }, go, [&m] {
    SetupStash& st = m.setup_->stash;
    if (!st.untrusted.empty()) st.unverifiedHost = st.untrusted;
    askStash(m);
    return MenuCommand{};
  }, !untrusted, asking || trimmedAddress(st.text[SetupStash::Address]).empty() ||
                     (signIn && st.text[SetupStash::User].empty()));
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
    const double columnW = (kInner - 40) / 2;
    for (int k = 0; k < 6; ++k) {
      const double x = kLeft + (k % 2) * (columnW + 40), y = kRowsY + (k / 2) * kRowStep;
      const double cx = x + 32, cy = y + 36;
      if (stashIcons[k] != StashIcon::None) {
        StashPages::drawIcon(cr, stashIcons[k], cx, cy, 54, kBlue);
      } else if (menuIcons[k] == Menu::Icon::Play) {  // in a ring
        cairo_new_path(cr);
        cairo_arc(cr, cx, cy, 20, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, kBlue[0], kBlue[1], kBlue[2], 1);
        cairo_set_line_width(cr, 54 * 0.075);
        cairo_stroke(cr);
        Menu::drawIcon(cr, Menu::Icon::Play, cx + 1, cy, 18, kBlue[0], kBlue[1], kBlue[2]);
      } else {
        Menu::drawIcon(cr, menuIcons[k], cx, cy, 54, kBlue[0], kBlue[1], kBlue[2]);
      }
      const char* line = t.stashFeatures[2 * k + 1];
      if (k == 5) line = !folders ? t.forDownloads : kStandaloneHeadset ? line : t.keptOnComputer;
      m.drawText(cr, t.stashFeatures[2 * k], x + 88, y, columnW - 88, 44, 32, true, false, pal.text.r,
                 pal.text.g, pal.text.b);
      m.drawText(cr, line, x + 88, y + 48, columnW - 88, 36, 26, false, false, pal.textDim.r, pal.textDim.g,
                 pal.textDim.b);
    }
    if (m.stash_) {
      Menu::drawIcon(cr, Menu::Icon::Check, kLeft + 14, kEndY + 20, 26, kBlue[0], kBlue[1], kBlue[2]);
      char line[512];
      std::snprintf(line, sizeof(line), t.stashConnected, m.view_.stash.url.c_str());
      m.drawText(cr, line, kLeft + 40, kEndY, kInner - 40, 40, 24, false, false, pal.text.r,
                 pal.text.g, pal.text.b);
    } else {
      m.drawText(cr, folders ? t.stashFree : t.stashFreeNoFolders, kLeft, kEndY, kInner, 40, 24, false, false,
                 pal.textDim.r, pal.textDim.g, pal.textDim.b);
    }
  } else if (st.page == Page::Address) {
    drawHead(m, cr, t.connectStash, t.addressText);
    m.drawText(cr, t.address, kLeft, 200, 600, 34, 24, false, false, pal.textDim.r, pal.textDim.g,
               pal.textDim.b);
    m.drawText(cr, m.text().stashSetup.serverHint, kLeft, 314, kFieldW, 34, 24, false, false,
               pal.textDim.r, pal.textDim.g, pal.textDim.b);
  } else {
    drawHead(m, cr, t.stashSignInTitle, t.stashSignInText);
    const NetworkStrings& n = m.text().network;
    for (const auto& [label, y] : {std::pair{n.fields[1], 230.0}, {n.fields[2], 360.0}})
      m.drawText(cr, label, kLeft, y, 600, 34, 24, false, false, pal.textDim.r, pal.textDim.g,
                 pal.textDim.b);
  }
  const double red[4] = {1, 0.55, 0.5, 1};
  if (!st.untrusted.empty()) {
    // As an SFTP server's changed key: a warning, what it means, and the
    // risk in red, in a box of its own.
    const StashSetupStrings& ts = m.text().stashSetup;
    const double y = st.page == Page::SignIn ? 490 : 370, pad = 28, textX = kLeft + pad + 52;
    const double textW = kInner - 2 * pad - 52;
    const int bodyLines = m.wrappedLines(ts.untrusted, textW, 24, 3);
    const int riskLines = m.wrappedLines(ts.untrustedRisk, textW, 24, 2, true);
    const double bodyY = y + pad + 50, riskY = bodyY + bodyLines * 24 * 1.36 + 12;
    const double h = riskY + riskLines * 24 * 1.36 + pad - y;
    roundedRect(cr, kLeft, y, kInner, h, 18);
    cairo_set_source_rgba(cr, red[0], red[1], red[2], 0.1);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, red[0], red[1], red[2], 0.45);
    cairo_set_line_width(cr, 2);
    cairo_stroke(cr);
    Menu::drawIcon(cr, Menu::Icon::Warning, kLeft + pad + 20, y + pad + 20, 36, red[0], red[1], red[2]);
    m.drawText(cr, stashPrinted(ts.untrustedTitle, st.untrusted), textX, y + pad, textW, 42, 30, true, false,
               pal.text.r, pal.text.g, pal.text.b);
    const double dim[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, 1};
    m.drawTextLines(cr, ts.untrusted, textX, bodyY, textW, 24, 3, dim, false);
    m.drawTextLines(cr, ts.untrustedRisk, textX, riskY, textW, 24, 2, red, false, 0, true);
  } else if (!st.error.empty()) {
    m.drawTextLines(cr, st.error, kLeft, st.page == Page::SignIn ? 482 : 362, kInner, 24, 3, red, false);
  }
  drawDots(m, cr);
  drawKeyboardPanel(m, cr);
}
