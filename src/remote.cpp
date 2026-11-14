#include "remote.h"

#include "file_util.h"
#include "remote_connection.h"
#include "run_log.h"
#include "settings.h"

#include <mpv/stream_cb.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace remote {
namespace {

constexpr std::string_view kSchemes[] = {"smb", "sftp"};

// "smb" or "sftp" of a locator, else "".
std::string_view scheme(std::string_view location) {
  for (std::string_view s : kSchemes)
    if (location.size() >= s.size() + 3 && location.substr(0, s.size()) == s &&
        location.substr(s.size(), 3) == "://")
      return s;
  return {};
}

// What follows the scheme up to the path: "host", "host:port" or "[v6]:port".
std::string_view authority(std::string_view locator, std::string_view scheme) {
  const std::string_view rest = locator.substr(scheme.size() + 3);
  return rest.substr(0, rest.find('/'));
}

// SFTP's host and port of an authority: "h", "h:2222", "[v6]:2222" (or a
// bare IPv6 address); 22 when not given.
std::pair<std::string_view, int> hostAndPort(std::string_view a) {
  constexpr auto npos = std::string_view::npos;
  std::string_view h = a, p;
  if (!a.empty() && a[0] == '[') {
    const size_t end = a.find(']');
    if (end != npos) {
      h = a.substr(1, end - 1);
      if (end + 1 < a.size() && a[end + 1] == ':') p = a.substr(end + 2);
    }
  } else if (const size_t colon = a.find(':'); colon != npos && a.find(':', colon + 1) == npos) {
    h = a.substr(0, colon);
    p = a.substr(colon + 1);
  }
  int port = 0;
  for (char c : p) {
    if (c < '0' || c > '9' || port > 65535) {
      port = 0;
      break;
    }
    port = port * 10 + (c - '0');
  }
  return {h, port > 0 && port <= 65535 ? port : 22};
}

struct Saved {
  Login login;
  std::string name;
  bool remembered = false;
};

// Logins are saved per protocol (smb-logins.json, sftp-logins.json), by
// what follows the scheme in the root: the host (and port).
struct Registry {
  std::mutex mutex;
  std::map<std::string, Saved> logins;  // by root
  std::map<std::string, std::shared_ptr<Connection>> connections;

  Registry() {
    for (std::string_view s : kSchemes) load(s);
  }
  static std::filesystem::path file(std::string_view scheme) {
    return configDir() / (std::string(scheme) + "-logins.json");
  }
  void load(std::string_view scheme) {
    std::ifstream in(file(scheme));
    const auto j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object()) return;
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (!it.value().is_object()) continue;
      const auto& v = it.value();
      if (!v.contains("user") || !v["user"].is_string()) continue;
      Login login{v["user"], v.value("password", ""), v.value("key", false)};
      if (!login.key && (!v.contains("password") || !v["password"].is_string())) continue;
      const std::string name = std::string(scheme) + "://" + lowerCase(it.key());
      logins[name] = {std::move(login), v.value("name", it.key()), true};
      hideInRunLog(host(name), "<" + std::string(scheme) + "-host>");
    }
  }
  void save(std::string_view scheme) {
    const std::string prefix = std::string(scheme) + "://";
    nlohmann::json j = nlohmann::json::object();
    for (auto& [root, saved] : logins) {
      if (!saved.remembered || root.rfind(prefix, 0) != 0) continue;
      auto& v = j[root.substr(prefix.size())];
      v = {{"user", saved.login.user}, {"password", saved.login.password}, {"name", saved.name}};
      if (saved.login.key) v["key"] = true;
    }
    queueConfigWork([contents = j.dump(), path = file(scheme)] {
      const auto dir = path.parent_path();
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      if (ec || chmod(dir.c_str(), 0700) != 0) return;
      const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
      if (fd < 0) return;
      if (fchmod(fd, 0600) == 0) {
        size_t offset = 0;
        while (offset < contents.size()) {
          const auto n = ::write(fd, contents.data() + offset, contents.size() - offset);
          if (n <= 0) break;
          offset += n;
        }
      }
      ::close(fd);
    });
  }
};
// Never destroyed: detached threads (first-run setup's counts and share
// lists, a sign-in) and the connections' workers may still use it while the
// program exits, and a connection it ended then would wait for its worker's
// request (up to 5 s each, plus looking up the server's name, which has no
// limit).
Registry& registry() {
  static Registry& r = *new Registry;
  return r;
}

}  // namespace

bool isRemote(std::string_view location) { return !scheme(location).empty(); }

std::string root(const std::string& locator) {
  const std::string_view s = scheme(locator);
  if (s.empty()) return {};
  std::string a = lowerCase(std::string(authority(locator, s)));
  if (s == "sftp") {  // as one form: [v6], and no :22
    const auto [h, port] = hostAndPort(a);
    std::string shown = h.find(':') != std::string_view::npos ? "[" + std::string(h) + "]" : std::string(h);
    if (port != 22) shown += ":" + std::to_string(port);
    a = std::move(shown);
  }
  return std::string(s) + "://" + a;
}

std::string host(const std::string& locator) {
  const std::string_view s = scheme(locator);
  if (s.empty()) return {};
  const std::string_view a = authority(locator, s);
  return lowerCase(std::string(s == "sftp" ? hostAndPort(a).first : a));
}

int port(const std::string& locator) {
  const std::string_view s = scheme(locator);
  if (s.empty()) return 0;
  return s == "smb" ? 445 : hostAndPort(authority(locator, s)).second;
}

std::string parent(const std::string& locator) {
  const std::string_view s = scheme(locator);
  if (s.empty()) return {};
  const size_t end = locator.find_last_not_of('/');
  const size_t slash = locator.rfind('/', end);
  return slash < s.size() + 3 ? std::string() : locator.substr(0, slash);
}

std::shared_ptr<Connection> connection(const std::string& name) {
  auto& r = registry();
  std::lock_guard lock(r.mutex);
  auto& c = r.connections[name];
  if (!c) {
    hideInRunLog(host(name), "<" + std::string(scheme(name)) + "-host>");
    c = std::make_shared<Connection>(name, scheme(name) == "smb" ? smbBackend : sftpBackend);
  }
  return c;
}

bool knownLogin(const std::string& server, Login& login, bool& remembered) {
  auto& r = registry();
  std::lock_guard lock(r.mutex);
  const auto it = r.logins.find(root(server));
  if (it == r.logins.end()) return false;
  login = it->second.login;
  remembered = it->second.remembered;
  return true;
}

Status signIn(const std::string& server, const Login& login, bool remember) {
  const std::string name = root(server);
  if (name.empty()) return Status::Missing;
  auto& r = registry();
  std::optional<Saved> previous;
  {
    std::lock_guard lock(r.mutex);
    const auto it = r.logins.find(name);
    if (it != r.logins.end()) previous = it->second;
    r.logins[name].login = login;
  }
  auto c = connection(name);
  Login kept = login;
  const Status status = c->call([&] { return c->backend().signIn(kept); }, true);
  {
    std::lock_guard lock(r.mutex);
    if (status == Status::Ok || status == Status::KeyRefused) {
      Saved& saved = r.logins[name];
      saved.login = kept;
      saved.remembered = status == Status::Ok && remember;
      if (saved.remembered || (previous && previous->remembered)) r.save(scheme(name));  // else the file is as it was
    } else if (previous) {
      r.logins[name] = *previous;
    } else {
      r.logins.erase(name);
    }
  }
  return status;
}

void forget(const std::string& server) {
  const std::string name = root(server);
  auto& r = registry();
  std::lock_guard lock(r.mutex);
  r.logins.erase(name);
  r.save(scheme(name));
  const auto it = r.connections.find(name);
  if (it != r.connections.end()) it->second->post([c = it->second.get()] { c->destroy(); }, true);
}

std::vector<Server> servers() {
  auto& r = registry();
  std::lock_guard lock(r.mutex);
  std::vector<Server> result;
  for (auto& [name, saved] : r.logins)
    result.push_back({name, saved.name.empty() ? host(name) : saved.name});
  return result;
}

void setName(const std::string& server, const std::string& shown) {
  auto& r = registry();
  std::lock_guard lock(r.mutex);
  const std::string name = root(server);
  const auto it = r.logins.find(name);
  if (it == r.logins.end()) return;
  it->second.name = shown;
  if (it->second.remembered) r.save(scheme(name));
}

void remember(const std::string& server) {
  auto& r = registry();
  std::lock_guard lock(r.mutex);
  const std::string name = root(server);
  const auto it = r.logins.find(name);
  if (it == r.logins.end()) return;
  it->second.remembered = true;
  r.save(scheme(name));
}

Status list(const std::string& folder, std::vector<Entry>& entries) {
  entries.clear();
  if (!isRemote(folder)) return Status::Missing;
  auto c = connection(root(folder));
  return c->call([&] { return c->backend().list(folder, entries); });
}

Status stat(const std::string& locator, Entry& entry) {
  if (!isRemote(locator)) return Status::Missing;
  auto c = connection(root(locator));
  return c->call([&] { return c->backend().stat(locator, entry); });
}

Status remove(const std::string& locator, uint64_t size, int64_t modified) {
  if (!isRemote(locator)) return Status::Missing;
  auto c = connection(root(locator));
  for (int tries = 1;; ++tries) {
    const Status status = c->call([&] { return c->backend().remove(locator, size, modified); });
    if (status != Status::InUse || tries == 12) return status;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
}

std::vector<std::string> subtitlesBeside(const std::string& locator) {
  const std::string folder = parent(locator);
  const std::string base = foldFileName(std::filesystem::path(locator).stem().string());
  const std::string prefix = base + ".";
  std::vector<Entry> entries;
  std::vector<std::string> paths;
  if (list(folder, entries) != Status::Ok) return paths;
  for (const auto& entry : entries) {
    if (entry.folder || !isSubtitleFile(entry.name)) continue;
    const std::string name = foldFileName(std::filesystem::path(entry.name).stem().string());
    if (name == base || (name.size() > prefix.size() &&
                        name.compare(0, prefix.size(), prefix) == 0))
      paths.push_back(folder + "/" + entry.name);
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

struct File::Impl {
  std::shared_ptr<Connection> connection;
  std::string locator;
  Handle handle;
};

std::unique_ptr<File> File::open(const std::string& locator, Status& status) {
  status = Status::Missing;
  if (!isRemote(locator)) return nullptr;
  auto file = std::unique_ptr<File>(new File);
  file->impl_ = std::make_unique<Impl>(Impl{connection(root(locator)), locator, {}});
  auto& p = *file->impl_;
  status = p.connection->call([&] { return p.connection->backend().open(p.locator, p.handle); });
  return status == Status::Ok ? std::move(file) : nullptr;
}
File::~File() {
  auto& p = *impl_;
  p.connection->post([c = p.connection.get(), h = p.handle] {
    if (h.file && h.epoch == c->epoch) c->backend().close(h);
  });
}
uint64_t File::size() const { return impl_->handle.length; }
int64_t File::read(uint64_t offset, void* out, uint64_t count) {
  auto& p = *impl_;
  return p.connection->call([&]() -> int64_t {
    auto& c = *p.connection;
    if (p.handle.epoch != c.epoch && c.backend().open(p.locator, p.handle) != Status::Ok) return -1;
    if (offset >= p.handle.length) return 0;
    const auto n = std::min<uint64_t>({count, p.handle.length - offset, ReadAhead::kBlock});
    return c.backend().readAt(p.handle, offset, out, n);
  });
}

namespace {
// The next bytes of a stream's window; -1 once it failed or was cancelled.
int64_t readStream(Connection& c, Stream& s, void* out, uint64_t count) {
  std::unique_lock lock(s.mutex);
  while (!s.cancelled && !s.failed) {
    if (s.position >= s.handle.length) return 0;
    if (const size_t n = s.take(out, count)) {
      if (s.wantsBlocks(s.handle.length)) c.notify();  // the worker has a block to ask for
      return n;
    }
    s.cv.wait_for(lock, std::chrono::milliseconds(50));
  }
  return -1;
}

// Opens `stream` on `c` and adds it to the streams the worker reads ahead.
Status openStream(Connection& c, const std::shared_ptr<Stream>& stream, bool priority) {
  return c.call([&] {
    const Status status = c.backend().open(stream->locator, stream->handle);
    if (status == Status::Ok) c.streams.push_back(stream);
    return status;
  }, priority);
}
}  // namespace

struct Reader::Impl {
  std::shared_ptr<Connection> connection;
  std::shared_ptr<Stream> stream = std::make_shared<Stream>();
};

std::unique_ptr<Reader> Reader::open(const std::string& locator, uint64_t offset, Status& status) {
  status = Status::Missing;
  if (!isRemote(locator)) return nullptr;
  auto reader = std::unique_ptr<Reader>(new Reader);
  reader->impl_ = std::make_unique<Impl>();
  auto& p = *reader->impl_;
  p.connection = connection(root(locator));
  auto& s = *p.stream;
  s.locator = locator;
  s.background = true;
  s.position = offset;
  status = openStream(*p.connection, p.stream, false);
  return status == Status::Ok ? std::move(reader) : nullptr;
}
Reader::~Reader() { cancel(); }
uint64_t Reader::size() const {
  std::lock_guard lock(impl_->stream->mutex);
  return impl_->stream->handle.length;
}
int64_t Reader::read(void* out, uint64_t count) {
  return readStream(*impl_->connection, *impl_->stream, out, count);
}
void Reader::cancel() {
  impl_->stream->cancelled = true;
  impl_->stream->cv.notify_all();
  impl_->connection->notify();
}

namespace {
int backgroundReader;
struct MpvStream {
  std::shared_ptr<Connection> connection;
  std::shared_ptr<Stream> stream;
  std::unique_ptr<File> file;  // preview: one background positional read, no read-ahead
  uint64_t position = 0;
  std::atomic<bool> cancelled{false};
  std::atomic<bool>* failed = nullptr;
};
}  // namespace

bool openMpvStream(const std::string& locator, bool background, std::atomic<bool>* failed,
                   mpv_stream_cb_info* info) {
  if (!isRemote(locator)) return false;
  auto p = std::make_unique<MpvStream>();
  p->connection = connection(root(locator));
  if (background) {
    Status status;
    p->file = File::open(locator, status);
    if (!p->file) return false;
  } else {
    p->failed = failed;
    if (p->failed) *p->failed = false;
    p->stream = std::make_shared<Stream>();
    p->stream->locator = locator;
    if (openStream(*p->connection, p->stream, true) != Status::Ok) return false;
  }
  info->cookie = p.release();
  info->read_fn = [](void* cookie, char* out, uint64_t count) -> int64_t {
    auto& p = *static_cast<MpvStream*>(cookie);
    if (p.cancelled) return -1;
    if (p.file) {
      const int64_t n = p.file->read(p.position, out, count);
      if (n > 0) p.position += n;
      return n;
    }
    auto& s = *p.stream;
    // FFmpeg can ask for its whole grown buffer (15 MB) at a time; on a file
    // whose audio is stored apart from its video it then takes one packet and
    // jumps away, so a read is cut to mpv's own buffer size.
    const int64_t n = readStream(*p.connection, s, out, std::min<uint64_t>(count, 128 * 1024));
    if (n < 0 && s.failed && !s.cancelled && p.failed) *p.failed = true;
    return n;
  };
  info->seek_fn = [](void* cookie, int64_t offset) -> int64_t {
    auto& p = *static_cast<MpvStream*>(cookie);
    if (offset < 0 || p.cancelled) return -1;
    if (p.file) { p.position = offset; return offset; }
    auto& s = *p.stream;
    std::lock_guard lock(s.mutex);
    if (s.failed) return -1;
    if (uint64_t(offset) != s.position) {
      s.seek(offset);
      p.connection->notify();
    }
    return offset;
  };
  info->size_fn = [](void* cookie) -> int64_t {
    auto& p = *static_cast<MpvStream*>(cookie);
    if (p.file) return p.file->size();
    std::lock_guard lock(p.stream->mutex);
    return p.stream->handle.length;
  };
  info->cancel_fn = [](void* cookie) {
    auto& p = *static_cast<MpvStream*>(cookie);
    p.cancelled = true;
    if (p.stream) { p.stream->cancelled = true; p.stream->cv.notify_all(); }
    p.connection->notify();
  };
  info->close_fn = [](void* cookie) {
    auto* p = static_cast<MpvStream*>(cookie);
    if (p->stream) { p->stream->cancelled = true; p->connection->notify(); }
    delete p;  // the worker retires pending reads and closes the handle
  };
  return true;
}

namespace {
// mpv's URLs: "smbfile://h/s/a" (playback) or "smbread://h/s/a" (previews)
// for smb://h/s/a, as "sftpfile://" and "sftpread://" for sftp://.
int openMpv(void* data, char* uri, mpv_stream_cb_info* info) {
  const std::string url = uri;
  const size_t colon = url.find("://");
  if (colon == std::string::npos || colon < 4) return MPV_ERROR_LOADING_FAILED;
  const std::string locator = url.substr(0, colon - 4) + url.substr(colon);
  const bool background = data == &backgroundReader;
  return openMpvStream(locator, background, background ? nullptr : static_cast<std::atomic<bool>*>(data), info)
             ? 0 : MPV_ERROR_LOADING_FAILED;
}
}  // namespace

void addTo(mpv_handle* mpv, bool background, std::atomic<bool>* failed) {
  for (std::string_view s : kSchemes) {
    mpv_stream_cb_add_ro(mpv, (std::string(s) + "file").c_str(),
                         background ? static_cast<void*>(&backgroundReader) : failed, openMpv);
    mpv_stream_cb_add_ro(mpv, (std::string(s) + "read").c_str(), &backgroundReader, openMpv);
  }
}

std::string mpvUrl(const std::string& locator, bool background) {
  const std::string_view s = scheme(locator);
  if (s.empty()) return locator;
  return std::string(s) + (background ? "read" : "file") + locator.substr(s.size());
}

}  // namespace remote
