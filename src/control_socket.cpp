#include "control_socket.h"

#include "command_inbox.h"
#include "menu_command.h"
#include "playback_session.h"
#include "server_json.h"
#include "video_player.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr size_t kMaxClients = 4;
constexpr size_t kMaxBuffered = 64 * 1024;  // a line read, or answers not yet sent
constexpr int kLinesPerFrame = 16;           // per client; the rest wait for the next frame
constexpr double kMaxValue = 1e9;

using json = nlohmann::json;

bool socketAddress(const std::string& path, sockaddr_un& address) {
  address = {};
  address.sun_family = AF_UNIX;
  if (path.empty() || path.size() >= sizeof(address.sun_path)) return false;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  return true;
}

// A socket file nothing listens at: what a run that was killed leaves.
bool staleSocket(const sockaddr_un& address) {
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  const bool refused = connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0 &&
                       errno == ECONNREFUSED;
  close(fd);
  return refused;
}

std::string reply(nlohmann::ordered_json& answer) {
  return answer.dump(-1, ' ', false, json::error_handler_t::replace) + '\n';
}

// The struct's fields by their names; paths as they are, for the user's own script.
nlohmann::ordered_json statusJson(const ControlSocket::Status& from) {
  const PlaybackStatus s = from.session.status();
  nlohmann::ordered_json j;
  j["file"] = s.file;
  j["asked"] = s.open ? s.asked : std::string();
  j["open"] = s.open;
  j["photo"] = s.photo;
  j["video"] = s.video;
  j["position"] = s.position;
  j["duration"] = s.duration;
  j["paused"] = s.paused;
  j["ended"] = s.ended;
  j["loading"] = s.loading;
  j["failed"] = s.failed;
  j["seeking"] = s.seeking;
  j["moving"] = s.moving;
  j["menuOpen"] = from.menuOpen;
  j["loadCount"] = from.player.loadCount();
  if (from.player.counting()) {
    const VideoPlayer::Counts& counts = from.player.counts();
    j["newFrames"] = counts.newFrames;
    j["voDrops"] = counts.voDrops;
    j["decoderDrops"] = counts.decoderDrops;
  }
  return j;
}

}  // namespace

ControlSocket::~ControlSocket() {
  for (Client& c : clients_) close(c.fd);
  if (listen_ < 0) return;
  close(listen_);
  struct stat st;
  if (lstat(path_.c_str(), &st) == 0 && st.st_dev == device_ && st.st_ino == inode_)
    unlink(path_.c_str());
}

bool ControlSocket::listen(const std::string& path) {
  sockaddr_un address;
  if (!socketAddress(path, address)) {
    std::fprintf(stderr, "Control socket: the path is empty or too long; starting without it\n");
    return false;
  }
  struct stat st;
  if (lstat(path.c_str(), &st) == 0) {
    if (!S_ISSOCK(st.st_mode) || !staleSocket(address)) {
      std::fprintf(stderr, "Control socket: something is already at the path; starting without it\n");
      return false;
    }
    unlink(path.c_str());
  }
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  // Whoever connects before chmod() is turned away by accept()'s check.
  const bool bound = fd >= 0 && bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0;
  if (!bound || chmod(path.c_str(), 0600) != 0 || ::listen(fd, 4) != 0 || lstat(path.c_str(), &st) != 0) {
    std::fprintf(stderr, "Control socket: %s; starting without it\n", std::strerror(errno));
    if (bound) unlink(path.c_str());
    if (fd >= 0) close(fd);
    return false;
  }
  listen_ = fd;
  path_ = path;
  device_ = st.st_dev;
  inode_ = st.st_ino;
  std::printf("Control socket: listening\n");
  return true;
}

void ControlSocket::update(CommandInbox& inbox, const Status& status) {
  if (listen_ < 0) return;
  polls_.resize(clients_.size() + 1);
  polls_[0] = {listen_, POLLIN, 0};
  for (size_t i = 0; i < clients_.size(); ++i) {
    const Client& c = clients_[i];
    polls_[i + 1] = {c.fd, static_cast<short>(POLLIN | (c.out.empty() ? 0 : POLLOUT)), 0};
  }
  const bool ready = poll(polls_.data(), polls_.size(), 0) > 0;
  // Lines left over from the last frame run whether or not there is more.
  for (size_t i = clients_.size(); i-- > 0;) {
    const short events = ready ? polls_[i + 1].revents : 0;
    Client& c = clients_[i];
    const bool ok = (!(events & (POLLIN | POLLHUP | POLLERR)) || read(c)) &&
                    handleLines(c, inbox, status) && flush(c);
    if (!ok) drop(i);
  }
  if (ready && (polls_[0].revents & POLLIN)) accept();
}

void ControlSocket::accept() {
  const int fd = accept4(listen_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
  if (fd < 0) return;
  ucred peer{};
  socklen_t size = sizeof peer;
  if (clients_.size() >= kMaxClients ||
      getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || peer.uid != getuid()) {
    close(fd);
    return;
  }
  clients_.push_back({fd, {}, {}});
}

bool ControlSocket::read(Client& client) {
  // Whole lines still waiting are taken first; what is behind them waits in the socket.
  if (client.in.find('\n') != std::string::npos) return true;
  char buffer[16 * 1024];
  const ssize_t n = recv(client.fd, buffer, sizeof buffer, 0);
  if (n == 0) return false;  // gone
  if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
  client.in.append(buffer, static_cast<size_t>(n));
  return true;
}

bool ControlSocket::handleLines(Client& client, CommandInbox& inbox, const Status& status) {
  size_t start = 0;
  for (int lines = 0; lines < kLinesPerFrame; ++lines) {
    const size_t end = client.in.find('\n', start);
    if (end == std::string::npos) break;
    if (end - start > kMaxBuffered) {
      std::fprintf(stderr, "Control socket: a line over 64 KiB; the client is dropped\n");
      return false;
    }
    answer(client, client.in.data() + start, client.in.data() + end, inbox, status);
    start = end + 1;
  }
  client.in.erase(0, start);
  if (client.in.size() > kMaxBuffered && client.in.find('\n') == std::string::npos) {
    std::fprintf(stderr, "Control socket: a line over 64 KiB; the client is dropped\n");
    return false;
  }
  return true;
}

void ControlSocket::answer(Client& client, const char* begin, const char* end, CommandInbox& inbox,
                           const Status& status) {
  const json line = json::parse(begin, end, nullptr, false);
  nlohmann::ordered_json answer = nlohmann::ordered_json::object();
  if (line.is_object()) {  // echoed only as a number or a string, never a nested value
    const auto id = line.find("id");
    if (id != line.end() && (id->is_number() || id->is_string())) answer["id"] = *id;
  }
  const auto fail = [&](const char* error) {
    answer["ok"] = false;
    answer["error"] = error;
    client.out += reply(answer);
  };
  if (!line.is_object()) return fail("not a JSON object");
  const std::string name = server_json::text(line, "command");
  if (name.empty()) {
    const std::string get = server_json::text(line, "get");
    if (get.empty()) return fail("no command or get");
    if (get != "status") return fail("unknown get");
    answer["ok"] = true;
    answer["status"] = statusJson(status);
    client.out += reply(answer);
    return;
  }
  MenuCommand c;
  if (!commandType(name.c_str(), c.type)) return fail("unknown command");
  c.value = server_json::number<double>(line, "value");
  const double option = server_json::number<double>(line, "option");
  if (!(std::abs(c.value) <= kMaxValue)) return fail("value out of range");
  if (!(option >= INT_MIN && option <= INT_MAX) || option != std::trunc(option))
    return fail("option is not a whole number");
  c.option = static_cast<int>(option);
  c.path = server_json::text(line, "path");
  // Deleting is the menu's dialog's, which opens before RequestDelete is sent
  // and is confirmed by a person.
  if (c.type == MenuCommand::RequestDelete || c.type == MenuCommand::DeleteFile ||
      c.type == MenuCommand::CancelDelete)
    return fail("not over the socket");
  if (!commandInRange(c)) return fail("value or option out of range");
  inbox.post(std::move(c));
  answer["ok"] = true;
  client.out += reply(answer);
}

bool ControlSocket::flush(Client& client) {
  while (!client.out.empty()) {
    const ssize_t n = send(client.fd, client.out.data(), client.out.size(), MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
      break;
    }
    client.out.erase(0, static_cast<size_t>(n));
  }
  if (client.out.size() > kMaxBuffered) {
    std::fprintf(stderr, "Control socket: a client stopped reading its answers; dropped\n");
    return false;
  }
  return true;
}

void ControlSocket::drop(size_t i) {
  close(clients_[i].fd);
  clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(i));
}
