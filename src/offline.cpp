#include "offline.h"

#include "file_util.h"
#include "oshash.h"
#include "removable_drives.h"
#include "settings.h"
#include "remote.h"
#include "stash_stream.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint64_t kChunk = 1024 * 1024;
constexpr uint64_t kRewrite = 8 * kChunk;   // a continued .part's tail, written again
constexpr uint64_t kSyncEvery = 64 * kChunk;  // flushed and dropped from the page cache
constexpr uint64_t kSpare = 64 * kChunk;      // left free on a drive

int64_t unixNow() { return static_cast<int64_t>(std::time(nullptr)); }

std::string homeDir() {
  const char* home = std::getenv("HOME");
  return home ? home : "/";
}

std::string fileName(const std::string& location) {
  return location.substr(location.find_last_of('/') + 1);
}

std::string stem(const std::string& name) { return fs::path(name).stem().string(); }

// The subtitles beside a local video, as remote::subtitlesBeside() finds them.
std::vector<std::string> localSubtitles(const std::string& video) {
  const fs::path folder = fs::path(video).parent_path();
  const std::string base = foldFileName(stem(fileName(video))), prefix = base + ".";
  std::vector<std::string> paths;
  std::error_code ec;
  for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (!isSubtitleFile(name)) continue;
    const std::string folded = foldFileName(stem(name));
    if (folded == base || (folded.size() > prefix.size() && folded.compare(0, prefix.size(), prefix) == 0))
      paths.push_back(it->path().string());
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

// A small file's bytes; false if it can't be read.
bool readWhole(const std::string& location, std::string& bytes) {
  if (!remote::isRemote(location)) {
    std::ifstream in(location, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in), {});
    return static_cast<bool>(in) || in.eof();
  }
  remote::Status status;
  auto file = remote::File::open(location, status);
  if (!file || file->size() > 64 * kChunk) return false;
  bytes.resize(file->size());
  for (uint64_t at = 0; at < bytes.size();) {
    const int64_t n = file->read(at, bytes.data() + at, bytes.size() - at);
    if (n <= 0) return false;
    at += n;
  }
  return true;
}

bool exists(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}

// Whether a server (its root locator) answers on its port within two seconds.
bool answers(const std::string& server) {
  addrinfo hints{}, *found = nullptr;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(remote::host(server).c_str(), std::to_string(remote::port(server)).c_str(), &hints,
                  &found) != 0)
    return false;
  bool ok = false;
  for (addrinfo* a = found; a && !ok; a = a->ai_next) {
    const int fd = socket(a->ai_family, a->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, a->ai_protocol);
    if (fd < 0) continue;
    if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) {
      ok = true;
    } else if (errno == EINPROGRESS) {
      pollfd p{fd, POLLOUT, 0};
      int error = 0;
      socklen_t length = sizeof(error);
      ok = poll(&p, 1, 2000) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 &&
           error == 0;
    }
    ::close(fd);
  }
  freeaddrinfo(found);
  return ok;
}

}  // namespace

struct Offline::Shared {
  std::mutex mutex;
  std::condition_variable wake, watchWake;
  bool quit = false, playing = false, pausedAll = false, rescan = false;
  bool watchStash = false, stashDown = false;
  std::vector<Download> items;
  int64_t nextId = 1;
  std::vector<Drive> drives;
  std::set<std::string> hosts, down;  // the servers to ask, and those that don't answer (roots)
  std::vector<Download> finished;
  std::vector<std::string> deleting;  // files of downloads cancelled or removed
  uint64_t version = 1;
  bool dirty = false;      // offline.json is to be written
  int64_t current = 0;     // the download being copied
  bool stopCurrent = false;  // ... is cancelled
  remote::Reader* reader = nullptr;
  StashReader* stashReader = nullptr;
  std::thread worker, watcher;

  Download* item(int64_t id) {
    for (Download& d : items)
      if (d.id == id) return &d;
    return nullptr;
  }
  void changed() {
    ++version;
    dirty = true;
  }
  void cancelRead() {
    if (reader) reader->cancel();
    if (stashReader) stashReader->cancel();
  }

  void work();
  void watch();
  void scan();
  void save();
  void copy(Download d);
};

// offline.json: what a download needs to continue, nothing about its progress
// (its .part file's length is that).
void Offline::Shared::save() {
  json list = json::array();
  bool paused;
  {
    std::lock_guard lock(mutex);
    if (!dirty) return;
    dirty = false;
    paused = pausedAll;
    for (const Download& d : items)
      list.push_back({{"source", d.source}, {"root", d.root}, {"path", d.path},
                      {"oshash", d.oshash}, {"size", d.size}, {"mtime", d.mtime},
                      {"done", d.doneAt}, {"subtitles", d.subtitles}});
  }
  const fs::path dir = dataDir(), file = dir / "offline.json", temp = dir / "offline.json.new";
  std::error_code ec;
  fs::create_directories(dir, ec);
  {
    std::ofstream out(temp);
    out << json{{"paused", paused}, {"downloads", list}}.dump(1) << '\n';
    if (!out) return;
  }
  fs::rename(temp, file, ec);
}

// The drives, their space, and which copies are there.
void Offline::Shared::scan() {
  std::vector<Drive> found;
  Drive home;
  home.root = homeDir();
  home.internal = true;
  found.push_back(home);
  for (const RemovableDrives::Drive& r : mountedDrives()) {
    Drive d;
    d.root = r.mountPoint;
    d.label = r.label;
    d.card = r.kind == RemovableDrives::Kind::Card;
    found.push_back(d);
  }
  for (Drive& d : found) {
    struct statvfs st;
    if (statvfs(d.root.c_str(), &st) != 0) continue;
    d.total = static_cast<uint64_t>(st.f_blocks) * st.f_frsize;
    d.free = static_cast<uint64_t>(st.f_bavail) * st.f_frsize;
  }
  std::vector<std::pair<int64_t, std::string>> done;
  {
    std::lock_guard lock(mutex);
    for (const Download& d : items)
      if (d.state == State::Done) done.emplace_back(d.id, d.path);
  }
  std::map<int64_t, bool> there;
  for (const auto& [id, path] : done) there[id] = exists(path);
  std::lock_guard lock(mutex);
  bool changes = found.size() != drives.size();
  for (size_t i = 0; i < found.size() && !changes; ++i)
    changes = found[i].root != drives[i].root || found[i].free / kChunk != drives[i].free / kChunk;
  for (Download& d : items) {
    const auto it = there.find(d.id);
    if (it == there.end() || it->second == d.present) continue;
    d.present = it->second;
    changes = true;
  }
  for (Drive& drive : found)
    for (const Download& d : items)
      if (d.root == drive.root && d.present) drive.used += d.size;
  drives = std::move(found);
  if (changes) ++version;
}

void Offline::Shared::work() {
  setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 19);
  std::map<int64_t, Clock::time_point> retryAt;
  auto scanned = Clock::time_point{};
  for (;;) {
    std::vector<std::string> doomed;
    {
      std::lock_guard lock(mutex);
      if (quit) break;
      doomed.swap(deleting);
    }
    std::error_code ec;
    for (const std::string& path : doomed) fs::remove(path, ec);
    bool rescanNow;
    {
      std::lock_guard lock(mutex);
      rescanNow = std::exchange(rescan, false) || !doomed.empty();
    }
    if (rescanNow || Clock::now() - scanned > std::chrono::seconds(5)) {
      scan();
      scanned = Clock::now();
    }
    save();
    Download next;
    {
      std::unique_lock lock(mutex);
      const Download* pick = nullptr;
      if (!pausedAll && !playing)
        for (const Download& d : items) {
          if (d.state == State::Done || d.state == State::Failed) continue;
          const auto at = retryAt.find(d.id);
          if (d.state == State::Waiting && at != retryAt.end() && at->second > Clock::now()) continue;
          const bool unreachable = remote::isRemote(d.source) ? down.count(remote::root(d.source)) > 0
                                                        : isStashSource(d.source) && stashDown;
          if (unreachable) {
            if (d.why != Why::Server) {
              Download& w = *item(d.id);
              w.state = State::Waiting;
              w.why = Why::Server;
              ++version;
            }
            continue;
          }
          pick = &d;
          break;
        }
      if (!pick) {
        wake.wait_for(lock, std::chrono::seconds(5));
        continue;
      }
      next = *pick;
      current = next.id;
      stopCurrent = false;
    }
    copy(next);
    std::lock_guard lock(mutex);
    current = 0;
    if (const Download* d = item(next.id); d && d->state == State::Waiting)
      retryAt[next.id] = Clock::now() + std::chrono::seconds(10);
  }
}

// Copies one download as far as it gets: done, paused (a video plays, Pause
// all), cancelled, or waiting for its server or drive.
void Offline::Shared::copy(Download d) {
  auto update = [&](auto change) {
    std::lock_guard lock(mutex);
    Download* item = this->item(d.id);
    if (!item) return false;
    change(*item);
    ++version;
    return true;
  };
  auto wait = [&](Why why) {
    update([&](Download& item) {
      item.state = State::Waiting;
      item.why = why;
    });
  };
  auto fail = [&](Why why) {
    update([&](Download& item) {
      item.state = State::Failed;
      item.why = why;
    });
  };

  // The source as it is now; a source changed since its copy started is
  // copied from the start.
  const bool stash = isStashSource(d.source);
  bool gone = false;
  const FileFingerprint source = stash ? stashFingerprint(d.source, gone) : fingerprintFile(d.source);
  if (source.oshash.empty()) {
    if (remote::isRemote(d.source)) {
      remote::Entry entry;
      gone = remote::stat(d.source, entry) == remote::Status::Missing;
    } else if (!stash) {
      gone = !exists(d.source);
    }
    gone ? fail(Why::Gone) : wait(Why::Server);
    return;
  }
  {
    std::lock_guard lock(mutex);
    for (const Download& other : items)
      if (other.id != d.id && other.oshash == source.oshash && other.state == State::Done) {
        items.erase(std::remove_if(items.begin(), items.end(),
                                   [&](const Download& x) { return x.id == d.id; }), items.end());
        if (!d.path.empty()) deleting.push_back(d.path + ".part");
        changed();
        return;
      }
  }
  struct statvfs room;
  if (statvfs(d.root.c_str(), &room) != 0) {
    wait(Why::Drive);
    return;
  }
  const fs::path folder = fs::path(d.root) / kFolder;
  std::error_code ec;
  fs::create_directories(folder, ec);
  const bool same = d.size == source.size && d.mtime == source.mtime && !d.path.empty();
  if (d.path.empty() || exists(d.path)) {
    // Its name: the source's, or "Name (2).ext" when that is taken.
    const std::string name = fileName(d.source), base = stem(name),
                      ext = fs::path(name).extension().string();
    std::lock_guard lock(mutex);
    for (int n = 1;; ++n) {
      const std::string path =
          (folder / (n == 1 ? name : base + " (" + std::to_string(n) + ")" + ext)).string();
      const bool taken = exists(path) || exists(path + ".part") ||
                         std::any_of(items.begin(), items.end(),
                                     [&](const Download& x) { return x.id != d.id && x.path == path; });
      if (!taken) {
        d.path = path;
        break;
      }
    }
  }
  const std::string part = d.path + ".part";
  uint64_t offset = 0;
  if (same) {
    struct stat st;
    if (::stat(part.c_str(), &st) == 0)
      offset = static_cast<uint64_t>(st.st_size) > kRewrite ? st.st_size - kRewrite : 0;
  }
  offset = std::min<uint64_t>(offset, source.size);
  const uint64_t free = static_cast<uint64_t>(room.f_bavail) * room.f_frsize;
  if (free < source.size - offset + kSpare) {
    fail(Why::NoRoom);
    return;
  }
  const int out = ::open(part.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
  if (out < 0 || ::ftruncate(out, static_cast<off_t>(offset)) != 0) {
    if (out >= 0) ::close(out);
    wait(Why::Drive);
    return;
  }
  if (!update([&](Download& item) {
        const bool persist = item.path != d.path || item.size != source.size ||
                             item.mtime != source.mtime || item.oshash != source.oshash;
        item.path = d.path;
        item.size = source.size;
        item.mtime = source.mtime;
        item.oshash = source.oshash;
        item.copied = offset;
        item.state = State::Copying;
        item.why = Why::None;
        if (persist) dirty = true;
      })) {
    ::close(out);
    return;
  }
  save();
  std::printf("Offline: %s a download\n", offset ? "continuing" : "starting");

  std::unique_ptr<remote::Reader> remote;
  std::unique_ptr<StashReader> streamed;
  int in = -1;
  if (remote::isRemote(d.source)) {
    remote::Status status;
    remote = remote::Reader::open(d.source, offset, status);
    std::lock_guard lock(mutex);
    reader = remote.get();
    if (quit || stopCurrent || pausedAll || playing) reader = nullptr, remote.reset();
  } else if (stash) {
    streamed = StashReader::open(d.source, offset, source);
    std::lock_guard lock(mutex);
    stashReader = streamed.get();
    if (quit || stopCurrent || pausedAll || playing) stashReader = nullptr, streamed.reset();
  } else {
    in = ::open(d.source.c_str(), O_RDONLY | O_CLOEXEC);
  }
  std::vector<char> buffer(kChunk);
  bool failed = !remote && !streamed && in < 0;
  uint64_t synced = offset;
  auto windowStart = Clock::now();
  uint64_t windowBytes = 0;
  double speed = 0;
  auto lastShown = Clock::now();
  enum { Copying, Stopped, Cancelled } outcome = Copying;
  while (!failed && offset < source.size) {
    {
      std::lock_guard lock(mutex);
      if (stopCurrent || quit) outcome = Cancelled;
      else if (pausedAll || playing) outcome = Stopped;
    }
    if (outcome != Copying) break;
    const uint64_t want = std::min<uint64_t>(kChunk, source.size - offset);
    const int64_t n = remote     ? remote->read(buffer.data(), want)
                      : streamed ? streamed->read(buffer.data(), want)
                                 : ::pread(in, buffer.data(), want, static_cast<off_t>(offset));
    if (n <= 0) {
      failed = true;
      break;
    }
    for (int64_t at = 0; at < n;) {
      const ssize_t w = ::pwrite(out, buffer.data() + at, n - at, static_cast<off_t>(offset + at));
      if (w <= 0) {
        failed = true;
        break;
      }
      at += w;
    }
    if (failed) break;
    offset += n;
    windowBytes += n;
    if (offset - synced >= kSyncEvery) {
      ::fdatasync(out);
      ::posix_fadvise(out, 0, 0, POSIX_FADV_DONTNEED);
      synced = offset;
    }
    const auto now = Clock::now();
    const double seconds = std::chrono::duration<double>(now - windowStart).count();
    if (seconds >= 2) {
      speed = windowBytes / seconds;
      windowStart = now;
      windowBytes = 0;
    }
    if (now - lastShown >= std::chrono::milliseconds(500)) {
      lastShown = now;
      update([&](Download& item) {
        item.copied = offset;
        item.speed = speed;
      });
    }
  }
  {
    std::lock_guard lock(mutex);
    reader = nullptr;
    stashReader = nullptr;
    // A read cancelled to stop it.
    if (failed && (stopCurrent || quit)) outcome = Cancelled;
    else if (failed && (pausedAll || playing)) outcome = Stopped;
  }
  const bool sourceChanged = streamed && streamed->changed();
  remote.reset();
  streamed.reset();
  if (in >= 0) ::close(in);
  if (outcome == Cancelled || failed || outcome == Stopped || offset < source.size) {
    ::fdatasync(out);
    ::close(out);
    if (outcome == Cancelled) {
      std::lock_guard lock(mutex);
      if (stopCurrent) deleting.push_back(part);
      return;
    }
    if (outcome == Stopped) {
      update([&](Download& item) {
        item.copied = offset;
        item.state = State::Paused;
      });
      return;
    }
    if (sourceChanged) {  // copied again from the start, at once
      std::printf("Offline: a download's source changed; starting it again\n");
      update([&](Download& item) { item.state = State::Queued; });
      return;
    }
    const bool driveThere = exists(d.root);
    wait(driveThere ? Why::Server : Why::Drive);
    return;
  }
  ::fdatasync(out);
  ::close(out);

  // Checked as Stash would know it before it is the copy.
  const FileFingerprint copied = fingerprintFile(part);
  if (copied.size != source.size || copied.oshash != source.oshash) {
    std::fprintf(stderr, "Offline: a copy didn't match its source; starting it again\n");
    fs::remove(part, ec);
    update([&](Download& item) {
      item.state = State::Waiting;
      item.why = Why::Broken;
      item.mtime = 0;  // from the start next time
      item.copied = 0;
    });
    return;
  }
  fs::rename(part, d.path, ec);
  if (ec) {
    wait(Why::Drive);
    return;
  }
  // Subtitles beside it with its name, under the copy's; Stash's captions as
  // WebVTT, as it serves them.
  std::vector<std::string> subtitles, beside;
  if (remote::isRemote(d.source)) beside = remote::subtitlesBeside(d.source);
  else if (!stash) beside = localSubtitles(d.source);
  const std::string sourceStem = stem(fileName(d.source)), copyStem = stem(fileName(d.path));
  auto keep = [&](const std::string& name, const std::string& bytes) {
    const std::string target = (folder / name).string();
    if (exists(target)) return;
    std::ofstream file(target, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (file) subtitles.push_back(target);
  };
  for (const std::string& sub : beside) {
    const std::string name = fileName(sub);
    std::string bytes;
    if (readWhole(sub, bytes)) keep(copyStem + name.substr(std::min(sourceStem.size(), name.size())), bytes);
  }
  if (stash)
    for (const StashCaption& c : stashCaptions(d.source))
      keep(copyStem + (c.language.empty() ? "" : "." + c.language) + ".vtt", c.text);
  struct stat st {};
  ::stat(d.path.c_str(), &st);
  std::lock_guard lock(mutex);
  Download* item = this->item(d.id);
  if (!item) {  // cancelled meanwhile
    deleting.push_back(d.path);
    for (const std::string& sub : subtitles) deleting.push_back(sub);
    return;
  }
  item->state = State::Done;
  item->doneAt = unixNow();
  item->copied = item->size;
  item->subtitles = subtitles;
  item->present = true;
  Download f = *item;
  f.mtime = st.st_mtime;  // the copy's, for the library
  finished.push_back(f);
  // Done ones after those to go.
  std::stable_partition(items.begin(), items.end(),
                        [](const Download& x) { return x.state != State::Done; });
  changed();
  std::printf("Offline: a download finished\n");
}

void Offline::Shared::watch() {
  std::map<std::string, Clock::time_point> askAt;
  auto stashAt = Clock::time_point{};
  std::unique_lock lock(mutex);
  while (!quit) {
    const std::set<std::string> hosts = this->hosts;
    std::set<std::string> nowDown = down;
    const bool askStash = watchStash || std::any_of(items.begin(), items.end(), [](const Download& d) {
                            return d.state != State::Done && isStashSource(d.source);
                          });
    bool stashNowDown = stashDown;
    lock.unlock();
    bool back = false;
    for (const std::string& server : hosts) {
      auto& at = askAt[server];
      if (Clock::now() < at) continue;
      const bool up = answers(server);
      at = Clock::now() + std::chrono::seconds(up ? 20 : 4);
      if (up && nowDown.erase(server)) back = true;
      if (!up) nowDown.insert(server);
    }
    // Stash by an HTTP request (its proxy, redirects and certificate as
    // for anything else asked of it), less often.
    if (!askStash) {
      stashNowDown = false;
      stashAt = {};
    } else if (Clock::now() >= stashAt) {
      const bool up = stashAnswers();
      stashAt = Clock::now() + std::chrono::seconds(up ? 30 : 5);
      if (up && stashNowDown) back = true;
      stashNowDown = !up;
    }
    lock.lock();
    for (auto it = nowDown.begin(); it != nowDown.end();)
      it = this->hosts.count(*it) ? std::next(it) : nowDown.erase(it);
    if (nowDown != down || stashNowDown != stashDown) {
      down = nowDown;
      stashDown = stashNowDown;
      ++version;
      if (back) wake.notify_all();
    }
    watchWake.wait_for(lock, std::chrono::seconds(1));
  }
}

Offline::Offline() : shared_(std::make_shared<Shared>()) {}

Offline::~Offline() {
  {
    std::lock_guard lock(shared_->mutex);
    shared_->quit = true;
    shared_->cancelRead();
  }
  shared_->wake.notify_all();
  shared_->watchWake.notify_all();
  if (shared_->worker.joinable()) shared_->worker.join();
  // An answer can take seconds to come; it finds nothing to tell.
  if (shared_->watcher.joinable()) shared_->watcher.detach();
  shared_->save();
}

void Offline::start() {
  Shared& sh = *shared_;
  std::ifstream in(dataDir() / "offline.json");
  const json j = json::parse(in, nullptr, false);
  if (j.is_object() && j.contains("downloads") && j["downloads"].is_array()) {
    sh.pausedAll = pausedAll_ = j.value("paused", false);
    for (const json& e : j["downloads"]) {
      if (!e.is_object() || !e.contains("source") || !e["source"].is_string()) continue;
      Download d;
      d.id = sh.nextId++;
      d.source = e["source"];
      d.root = e.value("root", homeDir());
      d.path = e.value("path", "");
      d.oshash = e.value("oshash", "");
      d.size = e.value("size", uint64_t(0));
      d.mtime = e.value("mtime", int64_t(0));
      d.doneAt = e.value("done", int64_t(0));
      if (e.contains("subtitles") && e["subtitles"].is_array())
        for (const json& s : e["subtitles"])
          if (s.is_string()) d.subtitles.push_back(s);
      if (d.doneAt) {
        d.state = State::Done;
        d.copied = d.size;
      }
      sh.items.push_back(std::move(d));
    }
    std::printf("Offline: %zu downloads\n", sh.items.size());
  }
  sh.worker = std::thread([&sh] { sh.work(); });
  sh.watcher = std::thread([s = shared_] { s->watch(); });
}

bool Offline::poll(std::vector<Download>& finished) {
  if (Clock::now() >= serversAt_) {
    serversAt_ = Clock::now() + std::chrono::seconds(2);
    std::vector<std::string> hosts;
    for (const remote::Server& s : remote::servers()) hosts.push_back(s.root);
    setServers(hosts);
  }
  Shared& sh = *shared_;
  std::lock_guard lock(sh.mutex);
  finished.swap(sh.finished);
  if (sh.version == version_) return !finished.empty();
  version_ = sh.version;
  downloads_ = sh.items;
  drives_ = sh.drives;
  down_ = sh.down;
  stashDown_ = sh.stashDown;
  servers_ = sh.hosts;
  ++generation_;
  bySource_.clear();
  byPath_.clear();
  byHash_.clear();
  for (size_t i = 0; i < downloads_.size(); ++i) {
    const Download& d = downloads_[i];
    bySource_.emplace(d.source, i);
    if (!d.path.empty()) byPath_.emplace(d.path, i);
    // A copy there now first.
    if (!d.oshash.empty() && (!byHash_.count(d.oshash) || d.present)) byHash_[d.oshash] = i;
  }
  return true;
}

const Offline::Drive* Offline::drive(const std::string& root) const {
  for (const Drive& d : drives_)
    if (d.root == root) return &d;
  return nullptr;
}

int Offline::toGo() const {
  int n = 0;
  for (const Download& d : downloads_) n += d.state != State::Done;
  return n;
}

uint64_t Offline::pending(const std::string& root) const {
  uint64_t bytes = 0;
  for (const Download& d : downloads_)
    if (d.root == root && d.state != State::Done && d.size > d.copied) bytes += d.size - d.copied;
  return bytes;
}

const Offline::Download* Offline::find(const std::string& path, const std::string& oshash) const {
  for (const auto* map : {&bySource_, &byPath_}) {
    const auto it = map->find(path);
    if (it != map->end()) return &downloads_[it->second];
  }
  if (oshash.empty()) return nullptr;
  const auto it = byHash_.find(oshash);
  return it != byHash_.end() ? &downloads_[it->second] : nullptr;
}

std::string Offline::copyOf(const std::string& path, const std::string& oshash) const {
  const Download* d = find(path, oshash);
  if ((!d || !d->present) && !oshash.empty()) {
    const auto it = byHash_.find(oshash);
    d = it != byHash_.end() ? &downloads_[it->second] : nullptr;
  }
  return d && d->present ? d->path : std::string();
}

void Offline::add(const std::string& location, uint64_t size, const std::string& oshash,
                  const std::string& root) {
  const std::string source = isStashScene(location) ? stashSource(location) : location;
  if (source.empty() || find(source, oshash)) return;
  Shared& sh = *shared_;
  {
    std::lock_guard lock(sh.mutex);
    if (std::any_of(sh.items.begin(), sh.items.end(),
                    [&](const Download& d) { return d.source == source; }))
      return;
    Download d;
    d.id = sh.nextId++;
    d.source = source;
    d.root = root;
    d.size = size;
    d.oshash = oshash;
    sh.items.insert(std::find_if(sh.items.begin(), sh.items.end(),
                                 [](const Download& x) { return x.state == State::Done; }),
                    d);
    sh.changed();
    if (remote::isRemote(source)) sh.hosts.insert(remote::root(source));
  }
  sh.wake.notify_all();
  sh.watchWake.notify_all();
}

void Offline::cancel(int64_t id) {
  Shared& sh = *shared_;
  {
    std::lock_guard lock(sh.mutex);
    const Download* d = sh.item(id);
    if (!d || d->state == State::Done) return;
    if (sh.current == id) {
      sh.stopCurrent = true;
      sh.cancelRead();
    } else if (!d->path.empty()) {
      sh.deleting.push_back(d->path + ".part");
    }
    sh.items.erase(std::remove_if(sh.items.begin(), sh.items.end(),
                                  [&](const Download& x) { return x.id == id; }), sh.items.end());
    sh.changed();
  }
  sh.wake.notify_all();
}

void Offline::remove(int64_t id) {
  Shared& sh = *shared_;
  {
    std::lock_guard lock(sh.mutex);
    const Download* d = sh.item(id);
    if (!d || d->state != State::Done) return;
    sh.deleting.push_back(d->path);
    for (const std::string& sub : d->subtitles) sh.deleting.push_back(sub);
    sh.items.erase(std::remove_if(sh.items.begin(), sh.items.end(),
                                  [&](const Download& x) { return x.id == id; }), sh.items.end());
    sh.changed();
  }
  sh.wake.notify_all();
}

void Offline::retry(int64_t id) {
  Shared& sh = *shared_;
  {
    std::lock_guard lock(sh.mutex);
    Download* d = sh.item(id);
    if (!d || d->state != State::Failed) return;
    d->state = State::Queued;
    d->why = Why::None;
    sh.changed();
  }
  sh.wake.notify_all();
}

void Offline::pauseAll(bool paused) {
  pausedAll_ = paused;
  Shared& sh = *shared_;
  {
    std::lock_guard lock(sh.mutex);
    sh.pausedAll = paused;
    if (paused) sh.cancelRead();
    if (!paused)
      for (Download& d : sh.items)
        if (d.state == State::Paused) d.state = State::Queued;
    sh.changed();
  }
  sh.wake.notify_all();
}

void Offline::setPlaying(bool playing) {
  if (playing == playing_) return;
  playing_ = playing;
  Shared& sh = *shared_;
  {
    std::lock_guard lock(sh.mutex);
    sh.playing = playing;
    if (playing) sh.cancelRead();
    if (!playing)
      for (Download& d : sh.items)
        if (d.state == State::Paused) d.state = State::Queued;
    ++sh.version;
  }
  sh.wake.notify_all();
}

void Offline::drivesChanged() {
  Shared& sh = *shared_;
  {
    std::lock_guard lock(sh.mutex);
    sh.rescan = true;
    for (Download& d : sh.items)
      if (d.state == State::Waiting && d.why == Why::Drive) d.state = State::Queued;
  }
  sh.wake.notify_all();
}

void Offline::setServers(const std::vector<std::string>& hosts) {
  Shared& sh = *shared_;
  std::set<std::string> wanted(hosts.begin(), hosts.end());
  {
    std::lock_guard lock(sh.mutex);
    for (const Download& d : sh.items)
      if (d.state != State::Done && remote::isRemote(d.source)) wanted.insert(remote::root(d.source));
    if (wanted == sh.hosts) return;
    sh.hosts = std::move(wanted);
    ++sh.version;
  }
  sh.watchWake.notify_all();
}

void Offline::watchStash(bool watch) {
  if (watch == watchStash_) return;
  watchStash_ = watch;
  {
    std::lock_guard lock(shared_->mutex);
    shared_->watchStash = watch;
  }
  shared_->watchWake.notify_all();
}

bool Offline::serverDown(const std::string& location) const {
  if (isStashScene(location) || isStashSource(location)) return stashDown_;
  return !down_.empty() && remote::isRemote(location) && down_.count(remote::root(location));
}

bool Offline::allServersDown() const {
  if (servers_.empty()) return false;
  return std::all_of(servers_.begin(), servers_.end(),
                     [&](const std::string& h) { return down_.count(h) > 0; });
}
