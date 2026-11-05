#include "sftp.h"

#include "file_util.h"
#include "remote_connection.h"
#include "run_log.h"
#include "settings.h"

#include <libssh/libssh.h>
#include <libssh/sftp.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <set>
#include <fcntl.h>
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/stat.h>
#include <unistd.h>

// SFTP servers through libssh (remote.h, sftp.h): sftp://host[:port]/path.
// One SSH session per server with one SFTP channel. Playback's blocks are
// read with SFTP's own pipelining (several read requests in flight, each as
// large as the server allows), collected as they arrive between jobs.
namespace remote {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kScheme = "sftp://";

// The server's absolute path of a locator: "/" for its root.
std::string pathOf(const std::string& locator) {
  const size_t slash = locator.find('/', kScheme.size());
  return slash == std::string::npos ? "/" : locator.substr(slash);
}

std::string authorityOf(const std::string& root) { return root.substr(std::min(root.size(), kScheme.size())); }

bool writeAll(const fs::path& path, const std::string& contents) {
  const fs::path temp = path.string() + ".new";
  const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  bool ok = fchmod(fd, 0600) == 0;
  for (size_t at = 0; ok && at < contents.size();) {
    const auto n = ::write(fd, contents.data() + at, contents.size() - at);
    ok = n > 0;
    if (ok) at += n;
  }
  ok = ::fsync(fd) == 0 && ok;
  ::close(fd);
  std::error_code ec;
  if (ok) {
    fs::rename(temp, path, ec);
    ok = !ec;
  }
  if (!ok) fs::remove(temp, ec);
  return ok;
}

// Frees a file's or folder's handle here only: once the connection is gone
// it can't be closed on the server (sftp_close() would ask it).
void discard(sftp_file file) {
  free(file->name);
  ssh_string_free(file->handle);
  free(file);
}
void discard(sftp_dir dir) {
  free(dir->name);
  ssh_string_free(dir->handle);
  ssh_buffer_free(dir->buffer);
  free(dir);
}

// All of `data`: a write is cut to what the server takes in one request.
bool writeFully(sftp_file file, const std::string& data) {
  for (size_t at = 0; at < data.size();) {
    const ssize_t n = sftp_write(file, data.data() + at, data.size() - at);
    if (n <= 0) return false;
    at += n;
  }
  return true;
}

// The servers' keys trusted, and this device's own.
struct Keys {
  std::mutex mutex;
  std::map<std::string, std::string> trusted;  // by authority: "host" or "host:port"
  std::string device;                          // private key, OpenSSH's format; "" until first use

  Keys() {
    std::ifstream in(configDir() / "sftp-hosts.json");
    const auto j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object()) return;
    for (auto it = j.begin(); it != j.end(); ++it)
      if (it.value().is_string()) trusted[it.key()] = it.value();
  }
  void save() {
    nlohmann::json j = nlohmann::json::object();
    for (auto& [name, fingerprint] : trusted) j[name] = fingerprint;
    queueConfigWork([contents = j.dump()] {
      const fs::path dir = configDir();
      std::error_code ec;
      fs::create_directories(dir, ec);
      if (!ec && chmod(dir.c_str(), 0700) == 0) writeAll(dir / "sftp-hosts.json", contents);
    });
  }
  // This device's key (a new copy each time: sessions use it from several
  // threads), made and saved on first use; null if it can't be.
  ssh_key deviceKey() {
    std::lock_guard lock(mutex);
    if (device.empty()) {
      std::ifstream in(configDir() / "sftp-key");
      device.assign(std::istreambuf_iterator<char>(in), {});
    }
    ssh_key key = nullptr;
    if (!device.empty() &&
        ssh_pki_import_privkey_base64(device.c_str(), nullptr, nullptr, nullptr, &key) == SSH_OK)
      return key;
    device.clear();
    if (ssh_pki_generate_key(SSH_KEYTYPE_ED25519, nullptr, &key) != SSH_OK) return nullptr;
    char* text = nullptr;
    if (ssh_pki_export_privkey_base64(key, nullptr, nullptr, nullptr, &text) != SSH_OK) {
      ssh_key_free(key);
      return nullptr;
    }
    std::string contents = text;
    ssh_string_free_char(text);
    const fs::path dir = configDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec || chmod(dir.c_str(), 0700) != 0 || !writeAll(dir / "sftp-key", contents)) {
      ssh_key_free(key);
      return nullptr;
    }
    device = std::move(contents);
    return key;
  }
};
// Never destroyed, as remote.cpp's registry: workers may use it while the
// program exits.
Keys& keys() {
  static Keys& k = *new Keys;
  return k;
}

// "ssh-ed25519 AAAA...": the key itself, as authorized_keys lists it.
std::string publicPart(ssh_key key) {
  char* b64 = nullptr;
  if (!key || ssh_pki_export_pubkey_base64(key, &b64) != SSH_OK) return {};
  std::string line = std::string(ssh_key_type_to_char(ssh_key_type(key))) + " " + b64;
  ssh_string_free_char(b64);
  return line;
}

std::string deviceName() {
  char name[256] = {};
  if (gethostname(name, sizeof(name) - 1) != 0 || !name[0]) return "videoframevr";
  return std::string("videoframevr@") + name;
}

std::string fingerprintOf(ssh_session session) {
  ssh_key key = nullptr;
  if (ssh_get_server_publickey(session, &key) != SSH_OK) return {};
  unsigned char* hash = nullptr;
  size_t length = 0;
  std::string fingerprint;
  if (ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &length) == SSH_OK) {
    char* text = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, length);
    if (text) fingerprint = text;
    ssh_string_free_char(text);
    ssh_clean_pubkey_hash(&hash);
  }
  ssh_key_free(key);
  return fingerprint;
}

// A new session to the server `root` names, connected but not signed in;
// null if it doesn't answer.
ssh_session connectTo(const std::string& root, const std::string& user) {
  ssh_session session = ssh_new();
  if (!session) return nullptr;
  const std::string name = host(root);
  const unsigned int port = remote::port(root);
  const long timeout = 5;
  const bool no = false;
  const int yes = 1;
  ssh_options_set(session, SSH_OPTIONS_HOST, name.c_str());
  ssh_options_set(session, SSH_OPTIONS_PORT, &port);
  if (!user.empty()) ssh_options_set(session, SSH_OPTIONS_USER, user.c_str());
  ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);
  ssh_options_set(session, SSH_OPTIONS_NODELAY, &yes);
  // Nothing of the desktop's own ssh setup: its config, known hosts, agent.
  ssh_options_set(session, SSH_OPTIONS_PROCESS_CONFIG, &no);
  ssh_options_set(session, SSH_OPTIONS_KNOWNHOSTS, "/dev/null");
  ssh_options_set(session, SSH_OPTIONS_GLOBAL_KNOWNHOSTS, "/dev/null");
  // AES-GCM first: both CPUs' AES instructions decrypt it cheaply (the
  // Frame's while it decodes video); ChaCha20 for servers without them.
  constexpr const char* ciphers =
      "aes128-gcm@openssh.com,aes256-gcm@openssh.com,chacha20-poly1305@openssh.com,"
      "aes128-ctr,aes192-ctr,aes256-ctr";
  ssh_options_set(session, SSH_OPTIONS_CIPHERS_C_S, ciphers);
  ssh_options_set(session, SSH_OPTIONS_CIPHERS_S_C, ciphers);
  if (ssh_connect(session) != SSH_OK) {
    ssh_free(session);
    return nullptr;
  }
  return session;
}

// The sign-in methods the server offers `user`: SSH_AUTH_METHOD_*.
int methodsOf(ssh_session session) {
  if (ssh_userauth_none(session, nullptr) == SSH_AUTH_SUCCESS) return -1;
  return ssh_userauth_list(session, nullptr);
}

// Signs in with a password, as the server asks for it: as such, or as the
// one answer of keyboard-interactive (PAM's).
int passwordAuth(ssh_session session, int methods, const std::string& password) {
  int rc = SSH_AUTH_DENIED;
  if (methods & SSH_AUTH_METHOD_PASSWORD) {
    rc = ssh_userauth_password(session, nullptr, password.c_str());
    if (rc != SSH_AUTH_DENIED) return rc;
  }
  if (!(methods & SSH_AUTH_METHOD_INTERACTIVE)) return rc;
  bool answered = false;
  rc = ssh_userauth_kbdint(session, nullptr, nullptr);
  for (int round = 0; rc == SSH_AUTH_INFO && round < 4; ++round) {
    const int prompts = ssh_userauth_kbdint_getnprompts(session);
    if (prompts == 1 && !answered) {
      char echo = 0;
      ssh_userauth_kbdint_getprompt(session, 0, &echo);
      if (echo) return SSH_AUTH_DENIED;  // not a password: a code, a question
      ssh_userauth_kbdint_setanswer(session, 0, password.c_str());
      answered = true;
    } else if (prompts != 0) {
      return SSH_AUTH_DENIED;  // more than the password (a second factor)
    }
    rc = ssh_userauth_kbdint(session, nullptr, nullptr);
  }
  return rc == SSH_AUTH_INFO ? SSH_AUTH_DENIED : rc;
}

Entry entry(const std::string& name, sftp_attributes a) {
  return {name, a->type == SSH_FILEXFER_TYPE_DIRECTORY, a->size,
          static_cast<int64_t>(a->mtime64 ? a->mtime64 : a->mtime)};
}

// A read of a block (or of readAt()'s) in flight.
struct Request {
  Stream* stream;  // null: readAt()'s
  ReadAhead::Block* block;
  sftp_aio aio;
  sftp_file file;
  uint8_t* out;
  uint64_t offset;
  size_t length;
  ssize_t got = 0;  // readAt()'s: bytes, or < 0 on error; SSH_AGAIN until it ends
};

class Sftp : public Backend {
 public:
  using Backend::Backend;
  ~Sftp() override { disconnect(); }

  // With a key and a password: the password signs in and adds the key, then
  // the key alone must sign in (Login::key).
  Status signIn(Login& login) override {
    c.destroy();
    if (!login.key || login.password.empty()) return connect(login);
    Login password{login.user, login.password, false}, key{login.user, "", true};
    Status status = connect(password);
    if (status != Status::Ok) return status;
    bool added = false;
    status = addKey(added);
    if (status == Status::Ok) {
      c.destroy();
      if (connect(key) == Status::Ok) {
        login = key;
        return Status::Ok;
      }
      c.destroy();
      status = connect(password);
      if (status != Status::Ok) return status;
      if (added) dropKey();  // useless where the server won't take it
    } else if (!connected()) {
      return status;  // the connection broke
    }
    login = password;
    return Status::KeyRefused;
  }

  Status list(const std::string& folder, std::vector<Entry>& entries) override {
    Status status = connect();
    if (status != Status::Ok) return status;
    std::string path = pathOf(folder);
    sftp_dir dir = sftp_opendir(sftp_, path.c_str());
    if (!dir) return error();
    if (path.back() != '/') path += '/';
    std::vector<std::string> links;
    // Each request (a batch of entries) waits for its answer: playback's
    // reads are collected and asked again between them, as a long folder
    // takes many. Not between entries: more reads queued ahead of the next
    // batch would only delay it.
    while (sftp_attributes a = sftp_readdir(sftp_, dir)) {
      const std::string name = a->name ? a->name : "";
      if (name == "." || name == ".." || name.empty()) {
      } else if (a->type == SSH_FILEXFER_TYPE_SYMLINK) {
        links.push_back(name);
      } else if (a->type == SSH_FILEXFER_TYPE_DIRECTORY || a->type == SSH_FILEXFER_TYPE_REGULAR) {
        entries.push_back(entry(name, a));
      }
      sftp_attributes_free(a);
      if (dir->count == 0 && !c.pump(0)) {  // the batch read: the next is a request
        discard(dir);
        return broke();
      }
    }
    const bool complete = sftp_dir_eof(dir);
    sftp_closedir(dir);
    if (!complete) return error();
    // What links point to (one more request each); broken ones left out.
    for (const std::string& name : links) {
      sftp_attributes a = sftp_stat(sftp_, (path + name).c_str());
      if (!a) {
        if (lost()) return error();
        continue;
      }
      if (a->type == SSH_FILEXFER_TYPE_DIRECTORY || a->type == SSH_FILEXFER_TYPE_REGULAR) {
        entries.push_back(entry(name, a));
        entries.back().link = true;
      }
      sftp_attributes_free(a);
      if (!c.pump(0)) return broke();
    }
    return Status::Ok;
  }

  Status stat(const std::string& locator, Entry& out) override {
    const Status status = connect();
    if (status != Status::Ok) return status;
    sftp_attributes a = sftp_stat(sftp_, pathOf(locator).c_str());
    if (!a) return error();
    out = entry(locator.substr(locator.rfind('/') + 1), a);
    sftp_attributes_free(a);
    return Status::Ok;
  }

  // The file itself, never a folder nor what a link points to.
  Status remove(const std::string& locator, uint64_t size, int64_t modified) override {
    const Status status = connect();
    if (status != Status::Ok) return status;
    const std::string path = pathOf(locator);
    sftp_attributes a = sftp_lstat(sftp_, path.c_str());
    if (!a) return error();
    const bool same = a->type == SSH_FILEXFER_TYPE_REGULAR && a->size == size &&
                      entry({}, a).modified == modified;
    sftp_attributes_free(a);
    if (!same) return Status::Changed;
    return sftp_unlink(sftp_, path.c_str()) == SSH_OK ? Status::Ok : error();
  }

  Status open(const std::string& locator, Handle& h) override {
    const Status status = connect();
    if (status != Status::Ok) return status;
    sftp_file file = sftp_open(sftp_, pathOf(locator).c_str(), O_RDONLY, 0);
    if (!file) return error();
    sftp_attributes a = sftp_fstat(file);
    if (!a || a->type == SSH_FILEXFER_TYPE_DIRECTORY) {
      const Status failed = a ? Status::Missing : error();
      if (a) sftp_attributes_free(a);
      if (sftp_)
        sftp_close(file);
      else
        discard(file);
      return failed;
    }
    // Reads are collected without waiting (service()).
    sftp_file_set_nonblocking(file);
    files_.insert(file);
    h = {file, 0, c.epoch, a->size};
    sftp_attributes_free(a);
    return Status::Ok;
  }

  void close(const Handle& h) override {
    if (sftp_ && h.file && h.epoch == c.epoch) closeFile(static_cast<sftp_file>(h.file));
  }

  // Requests for all of it at once, collected as they arrive while the
  // connection is pumped (playback's reads go on meanwhile).
  int64_t readAt(const Handle& h, uint64_t offset, void* out, uint64_t count) override {
    auto* file = static_cast<sftp_file>(h.file);
    std::list<Request> mine;
    for (uint64_t at = 0; at < count;) {
      sftp_aio aio = nullptr;
      sftp_seek64(file, offset + at);
      const ssize_t n = sftp_aio_begin_read(file, count - at, &aio);
      if (n <= 0) break;
      mine.push_back({nullptr, nullptr, aio, file, static_cast<uint8_t*>(out) + at, offset + at,
                      static_cast<size_t>(n), SSH_AGAIN});
      at += n;
    }
    if (mine.empty()) {
      error();
      return -1;
    }
    auto until = Clock::now() + std::chrono::seconds(5);
    if (c.deadline != Clock::time_point{}) until = std::min(until, c.deadline);
    for (;;) {
      // Again while replies arrive: one waited for may have been read
      // (and kept) while waiting for a later one.
      bool waiting = false, arrived = false;
      for (Request& r : mine)
        if (r.got == SSH_AGAIN) {
          r.got = sftp_aio_wait_read(&r.aio, r.out, r.length);
          waiting |= r.got == SSH_AGAIN;
          arrived |= r.got != SSH_AGAIN;
        }
      if (!waiting) break;
      if (arrived) continue;
      quickAck();
      if (Clock::now() >= until || !c.pump()) {
        c.destroy(true);  // frees what is still in flight
        for (Request& r : mine)
          if (r.got == SSH_AGAIN) sftp_aio_free(r.aio);
        return -1;
      }
    }
    // The bytes from `offset` on, up to the first short read.
    int64_t total = 0;
    for (const Request& r : mine) {
      if (r.got < 0) {
        if (total) return total;
        error();
        return -1;
      }
      total += r.got;
      if (static_cast<size_t>(r.got) < r.length) break;
    }
    return total ? total : -1;
  }

  void read(Stream& s, ReadAhead::Block& b) override {
    b.pending = true;
    if (!request(s, b, b.offset + b.filled, b.bytes.size() - b.filled) && !inFlight(b)) {
      b.pending = false;
      s.broke(lost());
    }
  }

  void closeStream(Stream& s) override {
    if (sftp_ && s.handle.epoch == c.epoch) closeFile(static_cast<sftp_file>(s.handle.file));
    s.closed = true;
  }

  bool connected() const override { return sftp_; }

  void disconnect() override {
    for (Request& r : requests_) sftp_aio_free(r.aio);
    requests_.clear();
    for (sftp_file file : files_) discard(file);
    files_.clear();
    if (sftp_) sftp_free(sftp_);
    sftp_ = nullptr;
    if (session_) {
      ssh_disconnect(session_);
      ssh_free(session_);
    }
    session_ = nullptr;
  }

  int fd() const override { return session_ ? ssh_get_fd(session_) : -1; }
  short events() const override { return POLLIN; }

  // Each block's replies that arrived, in any order (libssh keeps those read
  // past while waiting for another). A server that stops answering, without
  // the connection closing (Wi-Fi gone), counts as lost: streams reconnect.
  bool service(short) override {
    if (!sftp_) return true;
    quickAck();
    for (auto it = requests_.begin(); it != requests_.end();) {
      Request& r = *it;
      const ssize_t n = sftp_aio_wait_read(&r.aio, r.out, r.length);
      if (n == SSH_AGAIN) {
        ++it;
        continue;
      }
      heard_ = Clock::now();
      Stream& s = *r.stream;
      ReadAhead::Block& b = *r.block;
      std::lock_guard lock(s.mutex);
      if (n > 0) {
        b.filled += n;
        if (b.generation == s.generation) s.retryUntil = {};
        // A short read: the rest asked for again.
        if (static_cast<size_t>(n) < r.length && !s.failed && !s.cancelled &&
            !request(s, b, r.offset + n, r.length - n))
          s.broke(lost());
      }
      it = requests_.erase(it);
      if (n <= 0 && !s.failed && !s.reconnecting) {
        if (lost()) return false;
        s.broke(false);  // an error, or the end of a file that got shorter
      }
      if (!inFlight(b)) {
        b.pending = false;
        b.done = b.generation != s.generation || b.filled == b.bytes.size();
        s.cv.notify_all();
      }
    }
    if (!requests_.empty() && Clock::now() - heard_ > kSilence) return false;
    return !lost();
  }

  // The account's home folder (sftp.h).
  Status home(std::string& locator) {
    const Status status = connect();
    if (status != Status::Ok) return status;
    char* path = sftp_canonicalize_path(sftp_, ".");
    if (!path) return error();
    locator = c.root + (path[0] == '/' ? "" : "/") + path;
    ssh_string_free_char(path);
    while (locator.size() > c.root.size() + 1 && locator.back() == '/') locator.pop_back();
    if (locator.size() == c.root.size() + 1) locator.pop_back();  // "/": the root itself
    return Status::Ok;
  }

  // Takes this device's key off the account (sftp.h).
  Status removeKey() {
    const Status status = connect();
    return status == Status::Ok ? dropKey() : status;
  }

 private:
  // Asks for `length` bytes at `offset` of a stream's block; false if a
  // request couldn't be sent.
  bool request(Stream& s, ReadAhead::Block& b, uint64_t offset, size_t length) {
    auto* file = static_cast<sftp_file>(s.handle.file);
    if (requests_.empty()) heard_ = Clock::now();  // waiting from now
    while (length > 0) {
      sftp_aio aio = nullptr;
      sftp_seek64(file, offset);
      const ssize_t n = sftp_aio_begin_read(file, length, &aio);
      if (n <= 0) return false;
      requests_.push_back({&s, &b, aio, file, b.bytes.data() + (offset - b.offset), offset,
                           static_cast<size_t>(n)});
      offset += n;
      length -= n;
    }
    return true;
  }

  void closeFile(sftp_file file) {
    files_.erase(file);
    sftp_close(file);
  }

  // A pump() found the connection broken: its streams reconnect.
  Status broke() {
    c.destroy(true);
    return Status::Offline;
  }

  bool inFlight(const ReadAhead::Block& b) const {
    return std::any_of(requests_.begin(), requests_.end(), [&](const Request& r) { return r.block == &b; });
  }

  // Acknowledges what arrives at once (Linux turns this off again by
  // itself): a server that holds back a reply's last segment until the
  // previous one is acknowledged (Nagle's algorithm) would otherwise wait
  // for our delayed acknowledgement, about 40 ms a read.
  void quickAck() const {
    const int one = 1;
    if (session_) setsockopt(ssh_get_fd(session_), IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
  }

  bool lost() const {
    return !session_ || !ssh_is_connected(session_) || ssh_get_error_code(session_) == SSH_FATAL;
  }

  // A request failed: what the server said, or Offline once the connection
  // broke (its streams then reconnect).
  Status error() {
    if (lost()) {
      c.destroy(true);
      return Status::Offline;
    }
    switch (sftp_get_error(sftp_)) {
      case SSH_FX_NO_SUCH_FILE:
      case SSH_FX_NO_SUCH_PATH: return Status::Missing;
      case SSH_FX_PERMISSION_DENIED:
      case SSH_FX_WRITE_PROTECT:
      case SSH_FX_FAILURE:
      case SSH_FX_OP_UNSUPPORTED: return Status::Denied;
      default: return Status::Offline;
    }
  }

  Status connect() {
    if (sftp_) return Status::Ok;
    Login login;
    bool remembered;
    if (!knownLogin(c.root, login, remembered)) return Status::NeedsLogin;
    return connect(login);
  }

  // A session to the trusted server, signed in, with its SFTP channel.
  Status connect(const Login& login) {
    if (sftp_) return Status::Ok;
    disconnect();
    if (!login.user.empty()) hideInRunLog(login.user, "<sftp-user>");
    session_ = connectTo(c.root, login.user);
    if (!session_) return Status::Offline;
    const std::string known = sftp::trusted(c.root), fingerprint = fingerprintOf(session_);
    if (fingerprint.empty() || known != fingerprint) {
      disconnect();
      return fingerprint.empty() ? Status::Offline
             : known.empty()     ? Status::HostKeyUnknown
                                 : Status::HostKeyChanged;
    }
    const int methods = methodsOf(session_);
    int rc = SSH_AUTH_SUCCESS;
    if (methods >= 0 && login.key) {
      ssh_key key = keys().deviceKey();
      rc = !(methods & SSH_AUTH_METHOD_PUBLICKEY) || !key ? SSH_AUTH_DENIED
                                                           : ssh_userauth_publickey(session_, nullptr, key);
      ssh_key_free(key);
    } else if (methods >= 0) {
      rc = passwordAuth(session_, methods, login.password);
    }
    if (rc != SSH_AUTH_SUCCESS) {
      const bool offline = rc == SSH_AUTH_ERROR || rc == SSH_AUTH_AGAIN || !ssh_is_connected(session_);
      disconnect();
      return offline ? Status::Offline : Status::NeedsLogin;
    }
    sftp_ = sftp_new(session_);
    if (!sftp_ || sftp_init(sftp_) != SSH_OK) {
      // Signed in, but the server has no SFTP for the account.
      const bool offline = lost();
      disconnect();
      return offline ? Status::Offline : Status::Denied;
    }
    return Status::Ok;
  }

  // The account's authorized_keys: its path, and what it holds ("" if it
  // doesn't exist).
  Status authorizedKeys(std::string& path, std::string& contents, uint32_t& mode) {
    char* home = sftp_canonicalize_path(sftp_, ".");
    if (!home) return error();
    const std::string dir = std::string(home) + (home[std::strlen(home) - 1] == '/' ? "" : "/") + ".ssh";
    ssh_string_free_char(home);
    path = dir + "/authorized_keys";
    contents.clear();
    mode = 0600;
    sftp_attributes a = sftp_stat(sftp_, dir.c_str());
    if (!a) {
      if (sftp_get_error(sftp_) != SSH_FX_NO_SUCH_FILE || lost()) return error();
      if (sftp_mkdir(sftp_, dir.c_str(), 0700) != SSH_OK) return error();
      return Status::Ok;
    }
    sftp_attributes_free(a);
    sftp_file file = sftp_open(sftp_, path.c_str(), O_RDONLY, 0);
    if (!file) return sftp_get_error(sftp_) == SSH_FX_NO_SUCH_FILE && !lost() ? Status::Ok : error();
    if (sftp_attributes fa = sftp_fstat(file)) {
      mode = fa->permissions & 07777;
      sftp_attributes_free(fa);
    }
    char buffer[32 * 1024];
    for (ssize_t n; (n = sftp_read(file, buffer, sizeof(buffer))) != 0;) {
      if (n < 0 || contents.size() > (1 << 20)) {
        sftp_close(file);
        return n < 0 ? error() : Status::Denied;
      }
      contents.append(buffer, n);
    }
    sftp_close(file);
    return Status::Ok;
  }

  static bool listed(const std::string& contents, const std::string& key) {
    for (size_t at = 0; at < contents.size();) {
      const size_t end = std::min(contents.find('\n', at), contents.size());
      if (std::string_view(contents).substr(at, end - at).find(key) != std::string_view::npos) return true;
      at = end + 1;
    }
    return false;
  }

  // Adds this device's key to the account's authorized_keys, unless it is
  // there already (added: by this call).
  Status addKey(bool& added) {
    added = false;
    ssh_key key = keys().deviceKey();
    const std::string mine = publicPart(key);
    ssh_key_free(key);
    if (mine.empty()) return Status::Denied;
    std::string path, contents;
    uint32_t mode;
    Status status = authorizedKeys(path, contents, mode);
    if (status != Status::Ok || listed(contents, mine)) return status;
    std::string line = mine + " " + deviceName() + "\n";
    if (!contents.empty() && contents.back() != '\n') line.insert(0, "\n");
    // Appended: written at the end too, for a server that ignores append.
    sftp_file file = sftp_open(sftp_, path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (!file) return error();
    sftp_seek64(file, contents.size());
    const bool written = writeFully(file, line);
    const bool closed = sftp_close(file) == SSH_OK;
    if (!written || !closed) return error();
    added = true;
    return Status::Ok;
  }

  // Takes this device's key off the account's authorized_keys: the file
  // written anew beside it, with its permissions, then put in its place.
  Status dropKey() {
    ssh_key key = keys().deviceKey();
    const std::string mine = publicPart(key);
    ssh_key_free(key);
    if (mine.empty()) return Status::Ok;
    std::string path, contents;
    uint32_t mode;
    Status status = authorizedKeys(path, contents, mode);
    if (status != Status::Ok || !listed(contents, mine)) return status;
    std::string kept;
    for (size_t at = 0; at < contents.size();) {
      const size_t end = std::min(contents.find('\n', at), contents.size());
      const std::string_view line = std::string_view(contents).substr(at, end - at);
      if (line.find(mine) == std::string_view::npos) {
        kept += line;
        if (end < contents.size()) kept += '\n';
      }
      at = end + 1;
    }
    const std::string temp = path + ".videoframevr";
    sftp_file file = sftp_open(sftp_, temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (!file) return error();
    const bool written = writeFully(file, kept);
    const bool closed = sftp_close(file) == SSH_OK;
    if (!written || !closed || sftp_rename(sftp_, temp.c_str(), path.c_str()) != SSH_OK) {
      status = error();
      if (sftp_) sftp_unlink(sftp_, temp.c_str());  // unless the connection broke
      return status;
    }
    return Status::Ok;
  }

  // Reads in flight with no answer for this long: the connection is lost.
  static constexpr auto kSilence = std::chrono::seconds(10);

  ssh_session session_ = nullptr;
  sftp_session sftp_ = nullptr;
  std::list<Request> requests_;  // playback's and downloads' reads in flight
  Clock::time_point heard_{};    // a reply to them last arrived (or the first was sent)
  std::set<sftp_file> files_;    // open: freed with the connection if not closed before
};

std::shared_ptr<Connection> sftpConnection(const std::string& root) {
  return remote::root(root).rfind(kScheme, 0) == 0 ? connection(remote::root(root)) : nullptr;
}

}  // namespace

std::unique_ptr<Backend> sftpBackend(Connection& connection) { return std::make_unique<Sftp>(connection); }

}  // namespace remote

namespace sftp {

using remote::Status;

Status probe(const std::string& root, const std::string& user, Probe& out) {
  out = {};
  const std::string name = remote::root(root);
  if (name.rfind(remote::kScheme, 0) != 0) return Status::Missing;
  ssh_session session = remote::connectTo(name, user);
  if (!session) return Status::Offline;
  out.fingerprint = remote::fingerprintOf(session);
  const std::string known = trusted(name);
  out.hostKey = known.empty()                  ? Probe::HostKey::New
                : known == out.fingerprint     ? Probe::HostKey::Trusted
                                               : Probe::HostKey::Changed;
  const int methods = remote::methodsOf(session);
  out.password = methods < 0 || (methods & (SSH_AUTH_METHOD_PASSWORD | SSH_AUTH_METHOD_INTERACTIVE));
  out.key = methods < 0 || (methods & SSH_AUTH_METHOD_PUBLICKEY);
  ssh_disconnect(session);
  ssh_free(session);
  return out.fingerprint.empty() ? Status::Offline : Status::Ok;
}

void trust(const std::string& root, const std::string& fingerprint) {
  if (fingerprint.empty()) return;
  auto& k = remote::keys();
  std::lock_guard lock(k.mutex);
  k.trusted[remote::authorityOf(remote::root(root))] = fingerprint;
  k.save();
}

std::string trusted(const std::string& root) {
  auto& k = remote::keys();
  std::lock_guard lock(k.mutex);
  const auto it = k.trusted.find(remote::authorityOf(remote::root(root)));
  return it == k.trusted.end() ? std::string() : it->second;
}

std::string publicKey() {
  ssh_key key = remote::keys().deviceKey();
  const std::string line = remote::publicPart(key);
  ssh_key_free(key);
  return line.empty() ? line : line + " " + remote::deviceName();
}

Status removeKey(const std::string& root) {
  auto c = remote::sftpConnection(root);
  if (!c) return Status::Missing;
  return c->call([&] { return static_cast<remote::Sftp&>(c->backend()).removeKey(); });
}

Status home(const std::string& root, std::string& locator) {
  auto c = remote::sftpConnection(root);
  if (!c) return Status::Missing;
  return c->call([&] { return static_cast<remote::Sftp&>(c->backend()).home(locator); });
}

}  // namespace sftp
