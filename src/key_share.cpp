#include "key_share.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr auto kLifetime = 10min;
constexpr int kFirstPort = 47321, kLastPort = 47330;
constexpr size_t kMaxRequest = 8192;

// This device's IPv4 address on the route to `host`: a UDP socket
// connected to it (which sends nothing) is bound to it.
bool routeAddress(const std::string& host, int port, in_addr& address) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* found = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0) return false;
  bool ok = false;
  for (const addrinfo* a = found; a && !ok; a = a->ai_next) {  // each address, until one has a route
    const int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sockaddr_in local{};
    socklen_t size = sizeof local;
    ok = s >= 0 && connect(s, a->ai_addr, a->ai_addrlen) == 0 &&
         getsockname(s, reinterpret_cast<sockaddr*>(&local), &size) == 0 && local.sin_addr.s_addr != INADDR_ANY;
    if (s >= 0) close(s);
    if (ok) address = local.sin_addr;
  }
  freeaddrinfo(found);
  return ok;
}

// Listening on `address` and the first free port of ours, -1 if none is.
int listenOn(in_addr address, int& port) {
  for (port = kFirstPort; port <= kLastPort; ++port) {
    const int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (s < 0) return -1;
    const int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr = address;
    at.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(s, reinterpret_cast<sockaddr*>(&at), sizeof at) == 0 && listen(s, 4) == 0) return s;
    close(s);
  }
  return -1;
}

bool reply(int client, const char* status, const std::string& body, bool head = false, const char* more = "") {
  std::string out = std::string("HTTP/1.0 ") + status + "\r\nContent-Type: text/plain\r\nContent-Length: " +
                    std::to_string(body.size()) + "\r\n" + more + "Connection: close\r\n\r\n";
  if (!head) out += body;
  return send(client, out.data(), out.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(out.size());
}

// One request, read for up to half a second (curl sends it at once; one
// connection at a time, so an idle one mustn't hold up the next): GET /
// gets `body`, HEAD / its headers. True if the key went out.
bool answer(int client, const std::string& body, const std::atomic<bool>& stopped) {
  std::string request;
  const auto until = Clock::now() + 500ms;
  for (;;) {
    size_t end = request.find("\r\n\r\n");
    if (end == std::string::npos) end = request.find("\n\n");
    if (end == std::string::npos ? request.size() > kMaxRequest : end > kMaxRequest) {
      reply(client, "431 Request Header Fields Too Large", "Too large\n");
      return false;
    }
    if (end != std::string::npos) break;
    if (stopped || Clock::now() > until) return false;
    pollfd p{client, POLLIN, 0};
    if (poll(&p, 1, 100) <= 0) continue;
    char buffer[2048];
    const ssize_t got = recv(client, buffer, sizeof buffer, 0);
    if (got <= 0) return false;
    request.append(buffer, static_cast<size_t>(got));
  }
  const std::string line = request.substr(0, request.find_first_of("\r\n"));
  const size_t space = line.find(' ');
  const std::string method = line.substr(0, space);
  const std::string target =
      space == std::string::npos ? "" : line.substr(space + 1, line.find(' ', space + 1) - space - 1);
  if (target != "/") {
    reply(client, "404 Not Found", "Not found\n");
    return false;
  }
  if (method != "GET" && method != "HEAD") {
    reply(client, "405 Method Not Allowed", "Not allowed\n", false, "Allow: GET, HEAD\r\n");
    return false;
  }
  return reply(client, "200 OK", body, method == "HEAD") && method == "GET";
}

}  // namespace

struct KeyShare::Shared {
  std::atomic<bool> stopped{false};
  std::atomic<State> state{State::Starting};
  std::atomic<int> fetches{0};
  std::string address;  // set before state turns Serving
};

KeyShare::KeyShare(const std::string& host, int port, const std::string& key)
    : shared_(std::make_shared<Shared>()) {
  std::thread([s = shared_, host, port, key] {
    in_addr address{};
    int at = 0;
    const int server = routeAddress(host, port, address) ? listenOn(address, at) : -1;
    if (server < 0) {
      s->state = State::Stopped;
      return;
    }
    char ip[INET_ADDRSTRLEN] = "";
    inet_ntop(AF_INET, &address, ip, sizeof ip);
    s->address = std::string(ip) + ":" + std::to_string(at);
    s->state = State::Serving;
    // A blank line first, for an authorized_keys not ending in one (sshd
    // skips blank lines).
    const std::string body = "\n" + key + "\n";
    const auto until = Clock::now() + kLifetime;
    while (!s->stopped && Clock::now() < until) {
      pollfd p{server, POLLIN, 0};
      if (poll(&p, 1, 100) <= 0 || s->stopped) continue;
      const int client = accept4(server, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (client < 0) continue;
      if (answer(client, body, s->stopped)) {
        ++s->fetches;
        std::printf("SFTP: this device's key was fetched\n");
      }
      shutdown(client, SHUT_WR);
      close(client);
    }
    close(server);
    s->state = State::Stopped;
  }).detach();
}

KeyShare::~KeyShare() { shared_->stopped = true; }

KeyShare::State KeyShare::state() const { return shared_->state; }

std::string KeyShare::address() const { return state() == State::Serving ? shared_->address : ""; }

int KeyShare::fetches() const { return shared_->fetches; }
