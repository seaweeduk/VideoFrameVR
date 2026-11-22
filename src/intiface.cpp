#include "intiface.h"

#include "platform.h"
#include "server_json.h"

#include <glib.h>
#include <nlohmann/json.hpp>

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

using json = nlohmann::json;

namespace {

constexpr const char kDefaultPort[] = "12345";
// A device list is a few kilobytes; a server that sends more isn't Intiface.
constexpr size_t kMaxMessage = 4 * 1024 * 1024;
// RFC 6455: what a server appends to the key before hashing it.
constexpr const char kAcceptGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

// "host", "host:port", "[v6]:port", with or without ws:// before and a path
// after. False for an address that names no host, or an encrypted one.
bool parseAddress(std::string address, std::string& host, std::string& port) {
  const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!address.empty() && blank(address.back())) address.pop_back();
  while (!address.empty() && blank(address.front())) address.erase(0, 1);
  if (const size_t scheme = address.find("://"); scheme != std::string::npos) {
    if (lower(address.substr(0, scheme)) != "ws") return false;
    address.erase(0, scheme + 3);
  }
  address = address.substr(0, address.find('/'));
  port = kDefaultPort;
  if (!address.empty() && address[0] == '[') {
    const size_t end = address.find(']');
    if (end == std::string::npos) return false;
    host = address.substr(1, end - 1);
    if (end + 1 < address.size() && address[end + 1] == ':') port = address.substr(end + 2);
  } else if (const size_t colon = address.find(':');
             colon != std::string::npos && address.find(':', colon + 1) == std::string::npos) {
    host = address.substr(0, colon);
    port = address.substr(colon + 1);
  } else {
    host = address;  // a name, an IPv4 address, or a bare IPv6 one
  }
  return !host.empty() && !port.empty() &&
         std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string base64(const unsigned char* bytes, size_t count) {
  gchar* text = g_base64_encode(bytes, count);
  std::string out = text;
  g_free(text);
  return out;
}

// What a server answers a key with (Sec-WebSocket-Accept).
std::string acceptFor(const std::string& key) {
  GChecksum* sum = g_checksum_new(G_CHECKSUM_SHA1);
  const std::string text = key + kAcceptGuid;
  g_checksum_update(sum, reinterpret_cast<const guchar*>(text.data()), static_cast<gssize>(text.size()));
  guint8 digest[20];
  gsize length = sizeof(digest);
  g_checksum_get_digest(sum, digest, &length);
  g_checksum_free(sum);
  return base64(digest, length);
}

// A header's value in a response's head, names compared without regard to case.
std::string headerValue(const std::string& head, const std::string& name) {
  const std::string wanted = lower(name) + ":";
  for (size_t line = head.find("\r\n"); line != std::string::npos;) {
    const size_t start = line + 2, end = head.find("\r\n", start);
    const std::string text = head.substr(start, end == std::string::npos ? end : end - start);
    if (lower(text.substr(0, wanted.size())) == wanted) {
      size_t value = wanted.size();
      while (value < text.size() && text[value] == ' ') ++value;
      return text.substr(value);
    }
    line = end;
  }
  return {};
}

// A device of a DeviceList or DeviceAdded, if it moves along a stroke.
bool stroker(const json& d, Intiface::Device& device) {
  if (!d.is_object()) return false;
  const auto index = d.find("DeviceIndex"), messages = d.find("DeviceMessages");
  if (index == d.end() || !index->is_number_unsigned() || messages == d.end() || !messages->is_object())
    return false;
  const auto linear = messages->find("LinearCmd");
  if (linear == messages->end() || !linear->is_array() || linear->empty()) return false;
  device.index = index->get<int>();
  device.strokes = static_cast<int>(std::min<size_t>(linear->size(), 16));
  device.name.clear();
  for (const char* key : {"DeviceDisplayName", "DeviceName"})
    if (const auto name = d.find(key);
        device.name.empty() && name != d.end() && name->is_string())
      device.name = name->get<std::string>();
  return true;
}

}  // namespace

Intiface::~Intiface() { close(); }

void Intiface::close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  gotServerInfo_ = listed_ = scanning_ = fragmentText_ = false;
  maxPingMs_ = 0;
  in_.clear();
  message_.clear();
  devices_.clear();
}

bool Intiface::connect(const std::string& address, int timeoutMs) {
  close();
  std::string host, port;
  if (!parseAddress(address, host, port)) return false;
  const int64_t until = nowMs() + timeoutMs;
  addrinfo hints{}, *found = nullptr;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0) return false;
  for (const addrinfo* a = found; a && fd_ < 0; a = a->ai_next) {
    const int fd = socket(a->ai_family, a->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, a->ai_protocol);
    if (fd < 0) continue;
    bool ok = ::connect(fd, a->ai_addr, a->ai_addrlen) == 0;
    if (!ok && errno == EINPROGRESS) {
      pollfd p{fd, POLLOUT, 0};
      int error = 0;
      socklen_t length = sizeof(error);
      const int wait = static_cast<int>(std::max<int64_t>(0, until - nowMs()));
      ok = poll(&p, 1, wait) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 &&
           error == 0;
    }
    if (ok) fd_ = fd;
    else ::close(fd);
  }
  freeaddrinfo(found);
  if (fd_ < 0) return false;
  // Each command is one small packet that must leave at once.
  const int on = 1;
  setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

  // The WebSocket's opening: an HTTP request the server answers 101 to.
  unsigned char random[16];
  std::random_device source;
  for (unsigned char& byte : random) byte = static_cast<unsigned char>(source());
  const std::string key = base64(random, sizeof(random));
  const std::string hostHeader =
      (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + port;
  const std::string request = "GET / HTTP/1.1\r\nHost: " + hostHeader +
                              "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " +
                              key + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
  size_t headEnd = std::string::npos;
  bool ok = send(fd_, request.data(), request.size(), MSG_NOSIGNAL) ==
            static_cast<ssize_t>(request.size());
  while (ok && (headEnd = in_.find("\r\n\r\n")) == std::string::npos)
    ok = in_.size() < 16384 && waitReadable(until) && readSome();
  if (ok) {
    const std::string head = in_.substr(0, headEnd + 2);
    ok = head.compare(0, 12, "HTTP/1.1 101") == 0 &&
         headerValue(head, "Sec-WebSocket-Accept") == acceptFor(key);
    in_.erase(0, headEnd + 4);
  }

  // Buttplug's: who this is and which version it speaks, answered with ServerInfo.
  ok = ok && sendMessage("RequestServerInfo", {{"ClientName", kAppTitle}, {"MessageVersion", 3}});
  while (ok && !gotServerInfo_) {
    std::string text;
    while (nextMessage(text)) handle(text);
    if (fd_ < 0) ok = false;
    else if (!gotServerInfo_) ok = waitReadable(until) && readSome();
  }
  ok = ok && sendMessage("RequestDeviceList");
  if (!ok) close();
  return ok;
}

bool Intiface::waitReadable(int64_t untilMs) {
  if (fd_ < 0) return false;
  const int64_t wait = untilMs - nowMs();
  pollfd p{fd_, POLLIN, 0};
  return wait > 0 && poll(&p, 1, static_cast<int>(wait)) == 1;
}

// What has arrived, without waiting; false at the connection's end.
bool Intiface::readSome() {
  char buffer[16384];
  for (;;) {
    const ssize_t n = recv(fd_, buffer, sizeof(buffer), 0);
    if (n > 0) {
      in_.append(buffer, static_cast<size_t>(n));
      if (in_.size() > 2 * kMaxMessage) return false;
      if (static_cast<size_t>(n) < sizeof(buffer)) return true;
    } else if (n == 0) {
      return false;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return true;
    } else if (errno != EINTR) {
      return false;
    }
  }
}

bool Intiface::receive() {
  if (fd_ < 0) return false;
  bool open = readSome();
  std::string text;
  while (fd_ >= 0 && nextMessage(text)) handle(text);
  if (fd_ < 0) return false;
  if (!open) close();
  return open;
}

bool Intiface::nextMessage(std::string& text) {
  while (fd_ >= 0 && in_.size() >= 2) {
    const auto byte = [&](size_t i) { return static_cast<unsigned char>(in_[i]); };
    const bool fin = byte(0) & 0x80, masked = byte(1) & 0x80;
    const int opcode = byte(0) & 0x0f;
    uint64_t length = byte(1) & 0x7f;
    size_t header = 2;
    if (length == 126) {
      if (in_.size() < 4) return false;
      length = static_cast<uint64_t>(byte(2)) << 8 | byte(3);
      header = 4;
    } else if (length == 127) {
      if (in_.size() < 10) return false;
      length = 0;
      for (int i = 0; i < 8; ++i) length = length << 8 | byte(2 + i);
      header = 10;
    }
    if (length > kMaxMessage) {
      close();
      return false;
    }
    const size_t mask = header;
    if (masked) header += 4;
    if (in_.size() < header + length) return false;
    std::string payload = in_.substr(header, static_cast<size_t>(length));
    if (masked)
      for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= in_[mask + i % 4];
    in_.erase(0, header + static_cast<size_t>(length));
    switch (opcode) {
      case 0x8:  // the server closes
        close();
        return false;
      case 0x9:  // ping: answered with what it carried
        if (!sendFrame(0xA, payload)) return false;
        break;
      case 0x1:  // text
      case 0x2:  // binary: none is expected; skipped
        message_ = std::move(payload);
        fragmentText_ = opcode == 0x1;
        break;
      case 0x0:  // more of a message sent in parts
        if (message_.size() + payload.size() > kMaxMessage) {
          close();
          return false;
        }
        message_ += payload;
        break;
      default:
        continue;
    }
    if ((opcode == 0x0 || opcode == 0x1 || opcode == 0x2) && fin) {
      const bool isText = fragmentText_;
      text = std::move(message_);
      message_.clear();
      if (isText) return true;
    }
  }
  return false;
}

// One or more messages, each an object under its type's name.
void Intiface::handle(const std::string& text) {
  const json messages = json::parse(text, nullptr, false);
  if (!messages.is_array()) return;
  for (const json& message : messages) {
    if (!message.is_object() || message.empty()) continue;
    const std::string type = message.begin().key();
    const json& body = message.begin().value();
    if (!body.is_object()) continue;
    if (type == "ServerInfo") {
      gotServerInfo_ = true;
      const auto ping = body.find("MaxPingTime");
      maxPingMs_ = ping != body.end() && ping->is_number_unsigned()
                       ? static_cast<int>(std::min<uint64_t>(ping->get<uint64_t>(), 3600000))
                       : 0;
    } else if (type == "DeviceList") {
      devices_.clear();
      listed_ = true;
      const auto list = body.find("Devices");
      if (list == body.end() || !list->is_array()) continue;
      for (const json& d : *list)
        if (Device device; stroker(d, device)) {
          devices_.push_back(std::move(device));
          ++added_;
        }
    } else if (type == "DeviceAdded") {
      Device device;
      if (!stroker(body, device)) continue;
      devices_.erase(std::remove_if(devices_.begin(), devices_.end(),
                                    [&](const Device& d) { return d.index == device.index; }),
                     devices_.end());
      devices_.push_back(std::move(device));
      ++added_;
    } else if (type == "DeviceRemoved") {
      const auto index = body.find("DeviceIndex");
      if (index == body.end() || !index->is_number_unsigned()) continue;
      const int gone = index->get<int>();
      devices_.erase(std::remove_if(devices_.begin(), devices_.end(),
                                    [&](const Device& d) { return d.index == gone; }),
                     devices_.end());
    } else if (type == "ScanningFinished") {
      scanning_ = false;
    } else if (type == "Error" && !errorShown_) {
      // The first of a connection's only: a device refusing commands says so each time.
      errorShown_ = true;
      const std::string reason = server_json::text(body, "ErrorMessage");
      std::fprintf(stderr, "Intiface: %.200s\n", reason.c_str());
    }
  }
}

void Intiface::move(double position, int ms) {
  position = std::clamp(position, 0.0, 1.0);
  ms = std::max(ms, 1);
  // A copy: a send that fails closes the connection, and the list with it.
  const std::vector<Device> devices = devices_;
  for (const Device& d : devices) {
    json vectors = json::array();
    for (int i = 0; i < d.strokes; ++i)
      vectors.push_back({{"Index", i}, {"Duration", ms}, {"Position", position}});
    if (!sendMessage("LinearCmd", {{"DeviceIndex", d.index}, {"Vectors", std::move(vectors)}})) return;
  }
}

void Intiface::scan(bool on) {
  if (sendMessage(on ? "StartScanning" : "StopScanning")) scanning_ = on;
}

void Intiface::ping() { sendMessage("Ping"); }

bool Intiface::sendMessage(const char* type) { return sendMessage(type, json::object()); }

bool Intiface::sendMessage(const char* type, json fields) {
  fields["Id"] = nextId_;
  nextId_ = nextId_ == 0xFFFFFFFFu ? 1 : nextId_ + 1;  // 0 is the server's own
  return sendFrame(0x1, json::array({json{{type, std::move(fields)}}}).dump());
}

// One whole frame, masked as a client's must be. A send the socket can't
// take within two seconds ends the connection.
bool Intiface::sendFrame(int opcode, std::string_view payload) {
  if (fd_ < 0) return false;
  std::string frame;
  frame.reserve(payload.size() + 14);
  frame += static_cast<char>(0x80 | opcode);
  if (payload.size() < 126) {
    frame += static_cast<char>(0x80 | payload.size());
  } else if (payload.size() < 65536) {
    frame += static_cast<char>(0x80 | 126);
    frame += static_cast<char>(payload.size() >> 8);
    frame += static_cast<char>(payload.size() & 0xff);
  } else {
    frame += static_cast<char>(0x80 | 127);
    for (int shift = 56; shift >= 0; shift -= 8)
      frame += static_cast<char>(static_cast<uint64_t>(payload.size()) >> shift & 0xff);
  }
  static thread_local std::minstd_rand random{std::random_device{}()};
  char mask[4];
  for (char& m : mask) m = static_cast<char>(random());
  frame.append(mask, 4);
  for (size_t i = 0; i < payload.size(); ++i) frame += static_cast<char>(payload[i] ^ mask[i % 4]);

  const int64_t until = nowMs() + 2000;
  for (size_t sent = 0; sent < frame.size();) {
    const ssize_t n = send(fd_, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
    if (n > 0) {
      sent += static_cast<size_t>(n);
      continue;
    }
    const bool full = n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
    pollfd p{fd_, POLLOUT, 0};
    const int64_t wait = until - nowMs();
    if (n < 0 && errno == EINTR) continue;
    if (!full || wait <= 0 || poll(&p, 1, static_cast<int>(wait)) != 1) {
      close();
      return false;
    }
  }
  return true;
}
