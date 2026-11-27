#include "deo_remote.h"

#include "command_inbox.h"
#include "file_util.h"
#include "menu_command.h"
#include "menu_view.h"
#include "playback_session.h"
#include "remote.h"
#include "stash.h"
#include "stash_stream.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr size_t kMaxClients = 8;
constexpr size_t kMaxUnsent = 64 * 1024;  // a client that doesn't read is dropped past this (or two statuses)
constexpr auto kSendEvery = milliseconds(1000);
constexpr auto kChangeGap = milliseconds(100);      // sent at once on a change, but no sooner than this
constexpr auto kSilence = milliseconds(3000);       // a client heard nothing from this long is dropped
constexpr auto kRetry = milliseconds(3000);         // the port tried again
constexpr auto kAcceptPause = milliseconds(1000);   // no clients taken after accept() fails
constexpr auto kAddressEvery = milliseconds(10000); // this device's address looked up again
constexpr auto kOpenGap = milliseconds(1000);       // between two opens a client asked for
constexpr auto kShowEvery = milliseconds(250);      // the menu's status taken at most this often

// Network interfaces that aren't the device's own way onto the network.
bool virtualInterface(const char* name) {
  for (const char* prefix : {"docker", "veth", "br-", "virbr", "lxc", "vmnet", "vboxnet"})
    if (std::strncmp(name, prefix, std::strlen(prefix)) == 0) return true;
  return false;
}

// This device's IPv4 address on the network: a private one first; empty if
// it has none.
std::string localAddress() {
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return {};
  std::string best;
  int bestRank = 0;
  for (const ifaddrs* i = list; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) ||
        (i->ifa_flags & IFF_LOOPBACK) || virtualInterface(i->ifa_name))
      continue;
    const uint32_t a = ntohl(reinterpret_cast<const sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
    const bool lan = (a >> 24) == 10 || (a >> 20) == (172u << 4 | 1) || (a >> 16) == (192u << 8 | 168);
    const int rank = lan ? 2 : 1;
    if (rank <= bestRank) continue;
    char text[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(i->ifa_addr)->sin_addr, text, sizeof text))
      continue;
    best = text;
    bestRank = rank;
  }
  freeifaddrs(list);
  return best;
}

// A socket listening on DeoVR's port, IPv6 and IPv4 where the system has
// both; -1 and errno if it can't.
int listenOnPort() {
  int fd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  const int on = 1, off = 0;
  if (fd >= 0) {
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_any;
    address.sin6_port = htons(deo::kPort);
    if (bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0 && listen(fd, 8) == 0)
      return fd;
    const int error = errno;
    close(fd);
    if (error != EAFNOSUPPORT && error != EADDRNOTAVAIL) {
      errno = error;
      return -1;
    }
  }
  fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(deo::kPort);
  if (bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0 && listen(fd, 8) == 0)
    return fd;
  const int error = errno;
  close(fd);
  errno = error;
  return -1;
}

}  // namespace

struct DeoRemote::Shared {
  std::mutex mutex;
  int wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);  // written to wake the worker
  bool quit = false;
  deo::Report report;    // the render thread's, sent once a second
  bool changed = false;  // ... and at once after this (what plays, its state, a seek)
  deo::Request request;  // what clients asked for since the render thread last took it
  bool requested = false;
  RemoteControlStatus status;
  uint64_t generation = 0;  // counts changes of status

  ~Shared() {
    if (wake >= 0) close(wake);
  }
  void wakeWorker() {
    const uint64_t one = 1;
    if (wake >= 0 && write(wake, &one, sizeof one) < 0) {
      // Full: it is waking already.
    }
  }
};

namespace {

struct Client {
  int fd = -1;
  deo::Reader reader;
  std::string out;  // not sent yet
  Clock::time_point heard;
};

// The worker: every socket, until told to quit. Nothing in it waits longer
// than one poll(), which the wake-up ends.
class Worker {
 public:
  explicit Worker(DeoRemote::Shared& shared) : s_(shared) {}
  ~Worker() {
    for (Client& c : clients_) close(c.fd);
    if (listen_ >= 0) close(listen_);
  }
  void run();

 private:
  DeoRemote::Shared& s_;
  int listen_ = -1;
  std::vector<Client> clients_;
  std::vector<pollfd> polls_;
  std::string packet_;  // the status, as sent
  size_t maxUnsent_ = kMaxUnsent;  // a client is dropped with more than this unsent
  RemoteControlStatus status_;
  Clock::time_point retryAt_{}, sendAt_{}, sentAt_{}, addressAt_{};
  // accept() failing on the system's side (no file descriptors left): the
  // connection stays queued, so clients aren't taken until then.
  Clock::time_point acceptAt_{};
  bool refusedSaid_ = false;  // a refusal logged since the last client left
  bool acceptFailSaid_ = false;  // ... and accept() failing, since one was taken

  void setStatus(const RemoteControlStatus& status);
  void tryListening(Clock::time_point now);
  void sendStatus();
  void accept(Clock::time_point now);
  bool read(Client& c, Clock::time_point now);
  bool flush(Client& c);
  void drop(size_t i, const char* why);
};

void Worker::setStatus(const RemoteControlStatus& status) {
  if (status == status_) return;
  status_ = status;
  std::lock_guard lock(s_.mutex);
  s_.status = status;
  ++s_.generation;
}

void Worker::tryListening(Clock::time_point now) {
  listen_ = listenOnPort();
  const int error = errno;
  RemoteControlStatus status = status_;
  if (listen_ >= 0) {
    std::printf("Remote control: listening on port %d\n", deo::kPort);
    status.link = RemoteControlStatus::Link::Listening;
  } else {
    const bool inUse = error == EADDRINUSE;
    const auto link = inUse ? RemoteControlStatus::Link::PortInUse : RemoteControlStatus::Link::Failed;
    if (link != status_.link) {
      if (inUse) std::printf("Remote control: port %d is in use; trying again every few seconds\n", deo::kPort);
      else std::printf("Remote control: can't listen (%s); trying again every few seconds\n", std::strerror(error));
    }
    status.link = link;
    retryAt_ = now + kRetry;
  }
  setStatus(status);
}

void Worker::run() {
  setStatus({RemoteControlStatus::Link::Starting, {}, 0});
  for (;;) {
    bool changed;
    {
      std::lock_guard lock(s_.mutex);
      if (s_.quit) break;
      changed = s_.changed;
      s_.changed = false;
    }
    Clock::time_point now = Clock::now();
    if (changed) sendAt_ = std::min(sendAt_, std::max(now, sentAt_ + kChangeGap));
    if (listen_ < 0 && now >= retryAt_) tryListening(now);
    if (now >= addressAt_) {
      RemoteControlStatus status = status_;
      status.address = localAddress();
      setStatus(status);
      addressAt_ = now + kAddressEvery;
    }
    if (now >= sendAt_) {
      sendStatus();
      sentAt_ = now;
      sendAt_ = now + kSendEvery;
    }
    for (size_t i = clients_.size(); i-- > 0;) {
      if (now - clients_[i].heard > kSilence) drop(i, "silent for 3 seconds");
      else if (!flush(clients_[i])) drop(i, "it stopped reading");
    }

    Clock::time_point next = std::min(sendAt_, addressAt_);
    if (listen_ < 0) next = std::min(next, retryAt_);
    const bool accepting = now >= acceptAt_;
    if (!accepting) next = std::min(next, acceptAt_);
    for (const Client& c : clients_) next = std::min(next, c.heard + kSilence + milliseconds(1));
    const int timeout = static_cast<int>(
        std::clamp<int64_t>(std::chrono::duration_cast<milliseconds>(next - now).count() + 1, 0, 1000));
    polls_.clear();
    polls_.push_back({s_.wake, POLLIN, 0});
    polls_.push_back({accepting ? listen_ : -1, POLLIN, 0});  // ignored by poll() while -1
    for (const Client& c : clients_)
      polls_.push_back({c.fd, static_cast<short>(POLLIN | (c.out.empty() ? 0 : POLLOUT)), 0});
    if (poll(polls_.data(), polls_.size(), timeout) <= 0) continue;
    now = Clock::now();
    if (polls_[0].revents & POLLIN) {
      uint64_t count;
      if (::read(s_.wake, &count, sizeof count) < 0) {
        // Nothing to take: another read had it.
      }
    }
    // Clients from the back, so that dropping one leaves the others' polls in place.
    for (size_t i = clients_.size(); i-- > 0;) {
      const short events = polls_[i + 2].revents;
      if (events & (POLLIN | POLLHUP | POLLERR)) {
        if (!read(clients_[i], now)) drop(i, "");
      } else if ((events & POLLOUT) && !flush(clients_[i])) {
        drop(i, "it stopped reading");
      }
    }
    if (polls_[1].fd >= 0 && (polls_[1].revents & POLLIN)) accept(now);
  }
}

void Worker::sendStatus() {
  packet_.clear();
  {
    std::lock_guard lock(s_.mutex);
    deo::write(s_.report, packet_);
  }
  // A client that hasn't read one whole status by the next is behind.
  maxUnsent_ = std::max(kMaxUnsent, 2 * packet_.size());
  for (Client& c : clients_) c.out += packet_;
}

// A few at a time, so that connections arriving without end can't keep
// the worker from its other work or from quitting.
void Worker::accept(Clock::time_point now) {
  for (size_t taken = 0; taken < kMaxClients; ++taken) {
    const int fd = accept4(listen_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      if (errno == EINTR || errno == ECONNABORTED || errno == EPROTO) continue;  // that one is gone
      if (!acceptFailSaid_)
        std::printf("Remote control: can't take a client (%s); trying again every second\n", std::strerror(errno));
      acceptFailSaid_ = true;
      acceptAt_ = now + kAcceptPause;
      return;
    }
    acceptFailSaid_ = false;
    if (clients_.size() >= kMaxClients) {
      close(fd);
      if (!refusedSaid_)
        std::printf("Remote control: a client refused (%zu are connected); more aren't logged\n", kMaxClients);
      refusedSaid_ = true;
      continue;
    }
    const int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    Client c;
    c.fd = fd;
    c.heard = now;
    {
      std::lock_guard lock(s_.mutex);
      deo::write(s_.report, c.out);  // at once, then with the others
    }
    clients_.push_back(std::move(c));
    std::printf("Remote control: a client connected (%zu connected)\n", clients_.size());
    RemoteControlStatus status = status_;
    status.clients = static_cast<int>(clients_.size());
    setStatus(status);
  }
}

bool Worker::read(Client& c, Clock::time_point now) {
  char buffer[16 * 1024];
  const ssize_t n = recv(c.fd, buffer, sizeof buffer, 0);
  if (n == 0) return false;  // gone
  if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
  c.reader.feed(buffer, static_cast<size_t>(n));
  // Every packet this read completed, as one request, handed over under
  // one lock.
  deo::Request merged;
  bool any = false, broken = false;
  for (std::string_view body;;) {
    const deo::Reader::Next next = c.reader.next(body);
    if (next == deo::Reader::Next::None) break;
    if (next == deo::Reader::Next::Broken) {
      std::printf("Remote control: a client sent a packet over 64 KiB; dropped\n");
      broken = true;
      break;
    }
    c.heard = now;  // a whole packet, even one that isn't JSON
    deo::Request request;
    if (body.empty() || !deo::parse(body, request)) continue;  // a ping, or one DeoVR would drop
    deo::merge(merged, std::move(request));
    any = true;
  }
  if (any) {
    std::lock_guard lock(s_.mutex);
    deo::merge(s_.request, std::move(merged));
    s_.requested = true;
  }
  return !broken;
}

bool Worker::flush(Client& c) {
  while (!c.out.empty()) {
    const ssize_t n = send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
      break;
    }
    c.out.erase(0, static_cast<size_t>(n));
  }
  return c.out.size() <= maxUnsent_;
}

void Worker::drop(size_t i, const char* why) {
  close(clients_[i].fd);
  clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(i));
  refusedSaid_ = false;
  if (*why) std::printf("Remote control: a client dropped, %s (%zu connected)\n", why, clients_.size());
  else std::printf("Remote control: a client left (%zu connected)\n", clients_.size());
  RemoteControlStatus status = status_;
  status.clients = static_cast<int>(clients_.size());
  setStatus(status);
}

}  // namespace

DeoRemote::~DeoRemote() { stop(); }

void DeoRemote::start() {
  shared_ = std::make_shared<Shared>();
  // Nothing from a run before: what is open is reported again, and the
  // new worker's state is shown from its first change.
  reported_.clear();
  said_ = {};
  seeking_ = false;
  reportedFor_.clear();
  reportedCatalog_ = nullptr;
  pending_ = {};
  hasPending_ = false;
  shown_ = {RemoteControlStatus::Link::Starting, {}, 0};
  shownGeneration_ = 0;
  shownAt_ = {};
  // Nothing could wake a worker to stop it (no file descriptors left).
  if (shared_->wake < 0) {
    if (!startFailSaid_) std::printf("Remote control: can't start (%s); trying again every few seconds\n", std::strerror(errno));
    startFailSaid_ = true;
    startAgainAt_ = Clock::now() + kRetry;
    shown_ = {RemoteControlStatus::Link::Failed, {}, 0};
    return;
  }
  startFailSaid_ = false;
  worker_ = std::thread([shared = shared_] { Worker(*shared).run(); });
}

void DeoRemote::stop() {
  if (!shared_) return;
  {
    std::lock_guard lock(shared_->mutex);
    shared_->quit = true;
  }
  shared_->wakeWorker();
  if (worker_.joinable()) worker_.join();
  shared_.reset();
  shown_ = {};
  std::printf("Remote control: off\n");
}

void DeoRemote::fill(MenuView& view) const { view.remoteControlStatus = shown_; }

std::string DeoRemote::reportedPath(const Frame& frame) const {
  const PlaybackStatus& s = frame.status;
  // What was asked to open, as it was asked, as DeoVR reports it, whatever
  // plays in its place (a copy downloaded here). A scene streamed from a
  // media server by the app's own address, which means nothing to a
  // client: by the server's path of its file, which a funscript player
  // matches on; empty while the server's scenes don't have it.
  if (isStashScene(s.asked)) return frame.catalog ? stashFilePathAt(*frame.catalog, s.asked) : std::string();
  return s.asked.empty() ? s.file : s.asked;
}

void DeoRemote::update(const Frame& frame, CommandInbox& inbox) {
  if (frame.enabled && shared_ && !worker_.joinable() && Clock::now() >= startAgainAt_) shared_.reset();
  if (frame.enabled != (shared_ != nullptr)) frame.enabled ? start() : stop();
  if (!shared_) return;
  const PlaybackStatus& s = frame.status;
  const Clock::time_point now = Clock::now();
  // Worked out, and copied, only when what is open changes, or the
  // server's scenes do. A scene they don't have (now) keeps the path it was
  // told by; one they never had is told by what was asked.
  bool newPath = false;
  if (s.open && (s.asked != reportedFor_ || frame.catalog != reportedCatalog_)) {
    const bool told = s.asked == reportedFor_;
    reportedFor_ = s.asked;
    reportedCatalog_ = frame.catalog;
    std::string path = reportedPath(frame);
    if (path.empty()) path = told ? reported_ : s.asked;
    newPath = path != reported_;
    if (newPath) reported_ = std::move(path);
  }
  const auto finite = [](double v) { return std::isfinite(v) ? std::max(0.0, v) : 0.0; };
  const bool ended = s.video && s.ended;
  const double duration = s.video ? finite(s.duration) : 0;
  const double position = !s.video ? 0 : ended && duration > 0 ? duration : finite(s.position);
  const bool paused = !s.video || !s.playing;
  // Clients hear at once of another video, a pause, play, the end or a
  // seek: one that lands, a move while paused, or a jump while playing
  // (playing moves the position by a frame's time).
  const bool changed = newPath || s.open != said_.open || paused != said_.paused ||
                       ended != said_.ended || duration != said_.duration || (seeking_ && !s.seeking) ||
                       (paused && position != said_.position) || std::abs(position - said_.position) > 1;
  said_ = {s.open, {}, duration, position, paused, ended};
  seeking_ = s.seeking;
  deo::Request request;
  bool requested;
  {
    std::lock_guard lock(shared_->mutex);
    deo::Report& r = shared_->report;
    r.open = s.open;
    if (newPath) r.path = reported_;
    r.duration = duration;
    r.position = position;
    r.paused = paused;
    r.ended = ended;
    shared_->changed = shared_->changed || changed;
    requested = shared_->requested;
    if (requested) {
      request = std::move(shared_->request);
      shared_->request = {};
      shared_->requested = false;
    }
    if (shared_->generation != shownGeneration_ && now >= shownAt_ + kShowEvery) {
      shown_ = shared_->status;
      shownGeneration_ = shared_->generation;
      shownAt_ = now;
    }
  }
  if (changed) shared_->wakeWorker();
  if (requested) {
    deo::merge(pending_, std::move(request));
    hasPending_ = true;
  }
  if (hasPending_) act(frame, inbox);
}

namespace {

bool startsWithNoCase(std::string_view text, std::string_view start) {
  if (text.size() < start.size()) return false;
  for (size_t i = 0; i < start.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(text[i])) != std::tolower(static_cast<unsigned char>(start[i])))
      return false;
  return true;
}

}  // namespace

std::string DeoRemote::openable(const std::string& asked) const {
  // A line break or other control character is never part of a path, and
  // mpv takes some of them as the start of another file.
  for (const char c : asked)
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) return {};
  if (asked.empty() || asked.size() > deo::kMaxPath) return {};
  if (startsWithNoCase(asked, "http://") || startsWithNoCase(asked, "https://")) return asked;
  // Files of the kinds the file browser lists, nothing else: never a device
  // or a pipe, which could hold up the library's worker reading it.
  if (!isVideoFile(asked) && !isPhotoFile(asked)) return {};
  // A share or server signed in to before: a client never makes the app ask.
  if (startsWithNoCase(asked, "smb://") || startsWithNoCase(asked, "sftp://")) {
    const std::string locator = remote::lowerScheme(asked);
    const std::string root = remote::root(locator);
    remote::Login login;
    bool remembered = false;
    if (root.empty() || locator == root || !remote::knownLogin(root, login, remembered)) return {};
    return locator;
  }
  // A path here; nothing with a scheme of its own (edl://, av:// and the
  // like, which mpv acts on by itself).
  if (asked.front() == '/' && asked.find("://") == std::string::npos) return asked;
  return {};
}

void DeoRemote::act(const Frame& frame, CommandInbox& inbox) {
  const PlaybackStatus& s = frame.status;
  if (frame.setupRunning) {
    std::printf("Remote control: a request refused while setup shows\n");
    pending_ = {};
    hasPending_ = false;
    return;
  }
  // A path always opens, the one open too, as in DeoVR: at the packet's
  // time, else from the start.
  const bool opens = pending_.hasPath;
  if (opens) {
    const Clock::time_point now = Clock::now();
    if (now < nextOpen_) return;  // waits; newer requests replace it
    nextOpen_ = now + kOpenGap;
    std::string target = openable(pending_.path);
    if (target.empty()) {
      std::printf("Remote control: a request refused (not something the app opens here)\n");
      pending_ = {};
      hasPending_ = false;
      return;
    }
    MenuCommand c;
    c.type = MenuCommand::OpenFileAt;
    c.path = std::move(target);
    c.value = pending_.hasTime ? pending_.time : 0;
    inbox.post(std::move(c));
  }
  if (!opens && pending_.hasTime && s.video) {
    // As the seek bar lands: exactly for a short jump, on the keyframe
    // before for a long one.
    inbox.post({MenuCommand::SeekTo, pending_.time, {}, 1});
  }
  if (pending_.hasState && (opens || s.video)) inbox.post({MenuCommand::SetPaused, pending_.paused ? 1.0 : 0.0});
  pending_ = {};
  hasPending_ = false;
}
