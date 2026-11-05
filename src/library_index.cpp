#include "library_index.h"

#include "file_util.h"
#include "settings.h"  // dataDir
#include "remote.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>

#include <dirent.h>
#include <sqlite3.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace fs = std::filesystem;
using File = LibraryIndex::File;

namespace {

// library_index.db's layout (PRAGMA user_version); a file with another is made anew.
constexpr int kVersion = 1;
constexpr const char* kSchema = R"(
CREATE TABLE roots (path TEXT PRIMARY KEY, looked_at INTEGER NOT NULL);
CREATE TABLE folders (path TEXT PRIMARY KEY);
CREATE TABLE files (path TEXT PRIMARY KEY, size INTEGER NOT NULL, mtime INTEGER NOT NULL,
                    added_at INTEGER NOT NULL, found_at INTEGER NOT NULL);
CREATE TABLE todo (path TEXT NOT NULL);
CREATE TABLE meta (key TEXT PRIMARY KEY, value INTEGER NOT NULL);
)";

// During a look, what it found is saved and shown every so often.
constexpr auto kHandOver = std::chrono::seconds(2);

int64_t now() { return static_cast<int64_t>(std::time(nullptr)); }

bool under(const std::string& path, const std::string& root) {
  return path.compare(0, root.size(), root) == 0 &&
         (path.size() == root.size() || path[root.size()] == '/' || root.back() == '/');
}

std::string parentOf(const std::string& path) {
  return remote::isRemote(path) ? remote::parent(path) : fs::path(path).parent_path().string();
}

bool exec(sqlite3* db, const char* sql) {
  return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

// Hidden folders are looked into, bins and lost+found aren't; hidden files
// stay out, as the file browser leaves them out (macOS's ._ files and
// .DS_Store with them).
bool skipped(const std::string& name, bool folder) {
  return name.empty() || name == "." || name == ".." || (folder ? binOrLost(name) : name[0] == '.');
}

}  // namespace

// Names, sizes and dates only: one stat per video, none for other files.
// Links to folders are not followed (one could lead back up).
remote::Status LibraryIndex::list(const std::string& dir, Listing& out,
                               const std::atomic<bool>* stop) {
  if (remote::isRemote(dir)) {
    std::vector<remote::Entry> entries;
    const remote::Status status = remote::list(dir, entries);
    for (const remote::Entry& e : entries) {
      if (skipped(e.name, e.folder)) continue;
      if (e.folder) {
        if (!e.link) out.folders.push_back(dir + "/" + e.name);
      } else if (isVideoFile(e.name)) {
        out.videos.push_back({dir + "/" + e.name, {e.size, e.modified}});
      }
    }
    return status;
  }
  DIR* d = opendir(dir.c_str());
  if (!d) return errno == ENOENT || errno == ENOTDIR ? remote::Status::Missing : remote::Status::Offline;
  const std::string prefix = dir.back() == '/' ? dir : dir + "/";
  errno = 0;
  bool stopped = false;
  while (const dirent* e = readdir(d)) {
    if (stop && *stop) {
      stopped = true;
      break;
    }
    const std::string path = prefix + e->d_name;
    struct stat st;
    unsigned char type = e->d_type;
    if (type == DT_UNKNOWN)
      type = ::lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) ? DT_DIR : DT_REG;
    if (type == DT_DIR) {
      if (!skipped(e->d_name, true)) out.folders.push_back(path);
    } else if ((type == DT_REG || type == DT_LNK) && !skipped(e->d_name, false) && isVideoFile(e->d_name) &&
             ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode))
      out.videos.push_back({path, {static_cast<uint64_t>(st.st_size), st.st_mtime}});
    errno = 0;
  }
  const bool failed = stopped || errno != 0;  // cut short: not a whole listing
  closedir(d);
  return failed ? remote::Status::Offline : remote::Status::Ok;
}

// The main thread's requests and what the worker found for it; the rest is
// the worker's own.
struct LibraryIndex::Shared {
  std::mutex mutex;
  std::condition_variable wake;
  std::vector<std::string> pins;
  bool pinsChanged = false, videoOpen = false, again = false, quit = false;
  std::vector<std::pair<std::string, std::vector<FolderListing::Entry>>> listings;
  std::vector<std::string> deleted;
  std::unordered_map<std::string, File> changed;
  std::unordered_set<std::string> removed;
  State state;

  struct Folder {
    std::unordered_set<std::string> folders, files;  // listed subfolders; videos
  };
  sqlite3* db = nullptr;  // none: kept in memory only (another instance has the file)
  std::map<std::string, int64_t> roots;  // the pins (none inside another): last fully looked at
  std::unordered_map<std::string, Folder> folders;  // those listed
  std::unordered_map<std::string, File> files;
  std::vector<std::string> todo;  // folders this look has still to list, the next last
  std::unordered_set<std::string> failed;  // pins this look could not list
  int64_t done = 0, total = -1;  // folders this look listed, of those known before it (-1: not)
  bool pinsKnown = false;
  std::unordered_set<std::string> toMain, dirtyFiles, dirtyFolders, dirtyRoots;
  bool dirtyTodo = false;

  void run();
  void load();
  void handOver();
  void save();
  void setPins(std::vector<std::string> pins);
  void startLook(const std::vector<std::string>& from);
  void lookNext();
  void apply(const std::string& dir, const Listing& listing);
  void feed(const std::string& dir, const std::vector<FolderListing::Entry>& entries);
  void dropFolder(const std::string& dir);
  void touch(const std::string& path) {
    toMain.insert(path);
    dirtyFiles.insert(path);
  }
  const std::pair<const std::string, int64_t>* rootOf(const std::string& path) const {
    for (const auto& root : roots)
      if (under(path, root.first)) return &root;
    return nullptr;
  }
  bool videoIsOpen() {
    std::lock_guard<std::mutex> lock(mutex);
    return videoOpen;
  }
};

void LibraryIndex::Shared::run() {
  // Behind everything else: playback, the menu, thumbnails.
  const auto tid = static_cast<id_t>(syscall(SYS_gettid));
  setpriority(PRIO_PROCESS, tid, 19);
  syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, tid, 3 << 13 /* IOPRIO_CLASS_IDLE */);
  load();
  handOver();
  auto last = std::chrono::steady_clock::now();
  for (;;) {
    std::vector<std::string> newPins;
    bool pinsNew, lookAgain;
    std::vector<std::pair<std::string, std::vector<FolderListing::Entry>>> listed;
    std::vector<std::string> gone;
    {
      std::unique_lock<std::mutex> lock(mutex);
      wake.wait(lock, [this] {
        return quit || pinsChanged || again || !listings.empty() || !deleted.empty() ||
               (!todo.empty() && !videoOpen);
      });
      if (quit) break;
      pinsNew = std::exchange(pinsChanged, false);
      lookAgain = std::exchange(again, false);
      newPins = pins;
      listed.swap(listings);
      gone.swap(deleted);
    }
    if (pinsNew) setPins(std::move(newPins));
    if (lookAgain && todo.empty()) {
      std::vector<std::string> all;
      for (const auto& root : roots) all.push_back(root.first);
      startLook(all);
    }
    for (const auto& [dir, entries] : listed) feed(dir, entries);
    for (const std::string& path : gone) {
      if (const auto folder = folders.find(parentOf(path)); folder != folders.end())
        folder->second.files.erase(path);
      if (files.erase(path)) touch(path);
    }
    const bool open = videoIsOpen();
    if (!todo.empty() && !open) lookNext();
    if (pinsNew || lookAgain || todo.empty() || videoIsOpen() ||
        std::chrono::steady_clock::now() - last >= kHandOver) {
      handOver();
      last = std::chrono::steady_clock::now();
    }
  }
  handOver();
  sqlite3_close(db);
}

void LibraryIndex::Shared::load() {
  std::error_code ec;
  fs::create_directories(dataDir(), ec);
  const std::string file = (dataDir() / "library_index.db").string();
  auto open = [&] {
    sqlite3_close(db);
    db = nullptr;
    if (sqlite3_open_v2(file.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
        SQLITE_OK)
      return false;
    sqlite3_busy_timeout(db, 200);
    return exec(db, "PRAGMA locking_mode = EXCLUSIVE") && exec(db, "BEGIN EXCLUSIVE") &&
           exec(db, "COMMIT");
  };
  auto version = [&] {
    sqlite3_stmt* st = nullptr;
    int v = -1;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, nullptr) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
      v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return v;
  };
  if (!open()) {
    std::fprintf(stderr, "Library index: library_index.db not usable; kept in memory only\n");
    sqlite3_close(db);
    db = nullptr;
    return;
  }
  if (version() != kVersion) {
    // Another layout, or not a database: made anew (it is only a cache).
    sqlite3_close(db);
    db = nullptr;
    for (const char* tail : {"", "-wal", "-shm", "-journal"}) fs::remove(file + tail, ec);
    const std::string bump = "PRAGMA user_version = " + std::to_string(kVersion);
    if (!open() || !exec(db, "PRAGMA journal_mode = WAL") || !exec(db, "BEGIN") ||
        !exec(db, kSchema) || !exec(db, bump.c_str()) || !exec(db, "COMMIT")) {
      std::fprintf(stderr, "Library index: library_index.db not usable; kept in memory only\n");
      sqlite3_close(db);
      db = nullptr;
      return;
    }
  }
  exec(db, "PRAGMA journal_mode = WAL");
  exec(db, "PRAGMA synchronous = NORMAL");
  auto rows = [this](const char* sql, auto each) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK)
      while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(st, 0);
        each(std::string(text ? reinterpret_cast<const char*>(text) : ""), st);
      }
    sqlite3_finalize(st);
  };
  rows("SELECT path, looked_at FROM roots",
       [this](std::string path, sqlite3_stmt* st) { roots[path] = sqlite3_column_int64(st, 1); });
  rows("SELECT path FROM folders", [this](std::string path, sqlite3_stmt*) { folders[path]; });
  for (auto& [path, folder] : folders)
    if (!roots.count(path))
      if (const auto parent = folders.find(parentOf(path)); parent != folders.end())
        parent->second.folders.insert(path);
  rows("SELECT path, size, mtime, added_at, found_at FROM files",
       [this](std::string path, sqlite3_stmt* st) {
         const auto folder = folders.find(parentOf(path));
         if (folder == folders.end()) return;
         folder->second.files.insert(path);
         files[path] = {static_cast<uint64_t>(sqlite3_column_int64(st, 1)),
                        sqlite3_column_int64(st, 2), sqlite3_column_int64(st, 3),
                        sqlite3_column_int64(st, 4)};
         toMain.insert(path);
       });
  rows("SELECT path FROM todo ORDER BY rowid",
       [this](std::string path, sqlite3_stmt*) { todo.push_back(std::move(path)); });
  rows("SELECT key, value FROM meta", [this](std::string key, sqlite3_stmt* st) {
    (key == "done" ? done : total) = sqlite3_column_int64(st, 1);
  });
}

// What changed goes to the main thread and into the file.
void LibraryIndex::Shared::handOver() {
  State s;
  s.looking = !todo.empty();
  bool all = !roots.empty();
  int64_t oldest = 0;
  for (const auto& [path, lookedAt] : roots) {
    all = all && lookedAt > 0;
    oldest = oldest ? std::min(oldest, lookedAt) : lookedAt;
  }
  s.firstLook = s.looking && !all;
  s.percent = s.looking && total > 0 ? static_cast<int>(std::min<int64_t>(99, done * 100 / total)) : -1;
  s.lookedAt = all ? oldest : 0;
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (const std::string& path : toMain) {
      const auto it = files.find(path);
      if (it != files.end()) {
        changed[path] = it->second;
        removed.erase(path);
      } else {
        removed.insert(path);
        changed.erase(path);
      }
    }
    state = s;
  }
  toMain.clear();
  save();
}

void LibraryIndex::Shared::save() {
  if (!db) {
    dirtyFiles.clear();
    dirtyFolders.clear();
    dirtyRoots.clear();
    return;
  }
  if (dirtyFiles.empty() && dirtyFolders.empty() && dirtyRoots.empty() && !dirtyTodo) return;
  bool ok = exec(db, "BEGIN");
  auto run = [&](const char* sql, auto bind) {
    sqlite3_stmt* st = nullptr;
    ok = ok && sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK;
    if (ok) bind(st);
    sqlite3_finalize(st);
  };
  auto step = [&](sqlite3_stmt* st) {
    ok = ok && sqlite3_step(st) == SQLITE_DONE;
    sqlite3_reset(st);
  };
  auto text = [](sqlite3_stmt* st, int i, const std::string& s) {
    sqlite3_bind_text(st, i, s.c_str(), static_cast<int>(s.size()), SQLITE_STATIC);
  };
  run("INSERT OR REPLACE INTO files VALUES (?1, ?2, ?3, ?4, ?5)", [&](sqlite3_stmt* st) {
    for (const std::string& path : dirtyFiles) {
      const auto it = files.find(path);
      if (it == files.end()) continue;
      const File& f = it->second;
      text(st, 1, path);
      sqlite3_bind_int64(st, 2, static_cast<int64_t>(f.size));
      sqlite3_bind_int64(st, 3, f.mtime);
      sqlite3_bind_int64(st, 4, f.addedAt);
      sqlite3_bind_int64(st, 5, f.foundAt);
      step(st);
    }
  });
  run("DELETE FROM files WHERE path = ?1", [&](sqlite3_stmt* st) {
    for (const std::string& path : dirtyFiles)
      if (!files.count(path)) {
        text(st, 1, path);
        step(st);
      }
  });
  run("INSERT OR IGNORE INTO folders VALUES (?1)", [&](sqlite3_stmt* st) {
    for (const std::string& path : dirtyFolders)
      if (folders.count(path)) {
        text(st, 1, path);
        step(st);
      }
  });
  run("DELETE FROM folders WHERE path = ?1", [&](sqlite3_stmt* st) {
    for (const std::string& path : dirtyFolders)
      if (!folders.count(path)) {
        text(st, 1, path);
        step(st);
      }
  });
  run("INSERT OR REPLACE INTO roots VALUES (?1, ?2)", [&](sqlite3_stmt* st) {
    for (const std::string& path : dirtyRoots)
      if (const auto it = roots.find(path); it != roots.end()) {
        text(st, 1, path);
        sqlite3_bind_int64(st, 2, it->second);
        step(st);
      }
  });
  run("DELETE FROM roots WHERE path = ?1", [&](sqlite3_stmt* st) {
    for (const std::string& path : dirtyRoots)
      if (!roots.count(path)) {
        text(st, 1, path);
        step(st);
      }
  });
  if (dirtyTodo) {
    ok = ok && exec(db, "DELETE FROM todo");
    run("INSERT INTO todo VALUES (?1)", [&](sqlite3_stmt* st) {
      for (const std::string& path : todo) {
        text(st, 1, path);
        step(st);
      }
    });
    run("INSERT OR REPLACE INTO meta VALUES ('done', ?1), ('total', ?2)", [&](sqlite3_stmt* st) {
      sqlite3_bind_int64(st, 1, done);
      sqlite3_bind_int64(st, 2, total);
      step(st);
    });
  }
  ok = ok && exec(db, "COMMIT");
  if (!ok) {
    // Only a cache: the next look puts it right.
    std::fprintf(stderr, "Library index: not saved (%s)\n", sqlite3_errmsg(db));
    exec(db, "ROLLBACK");
  }
  dirtyFiles.clear();
  dirtyFolders.clear();
  dirtyRoots.clear();
  dirtyTodo = false;
}

// The pins become the roots, those inside another left out: what is under
// no root any more is forgotten; new ones are looked through.
void LibraryIndex::Shared::setPins(std::vector<std::string> pins) {
  std::sort(pins.begin(), pins.end());
  std::vector<std::string> kept;
  for (const std::string& pin : pins)
    if (std::none_of(kept.begin(), kept.end(), [&](const std::string& k) { return under(pin, k); }))
      kept.push_back(pin);
  std::vector<std::string> gone, added;
  for (const auto& root : roots)
    if (std::find(kept.begin(), kept.end(), root.first) == kept.end()) gone.push_back(root.first);
  for (const std::string& pin : kept)
    if (!roots.count(pin)) added.push_back(pin);
  for (const std::string& root : gone) {
    roots.erase(root);
    dirtyRoots.insert(root);
  }
  for (const std::string& root : added) {
    roots[root] = 0;
    dirtyRoots.insert(root);
  }
  if (!gone.empty()) {
    auto covered = [this](const std::string& path) { return rootOf(path) != nullptr; };
    std::vector<std::string> left;
    for (const auto& folder : folders)
      if (!covered(folder.first)) left.push_back(folder.first);
    for (const std::string& dir : left) dropFolder(dir);
    const size_t before = todo.size();
    todo.erase(std::remove_if(todo.begin(), todo.end(), [&](const std::string& p) { return !covered(p); }),
               todo.end());
    dirtyTodo = dirtyTodo || todo.size() != before;
  }
  // Once per start: the look an earlier start did not finish goes on, else a new one.
  if (!pinsKnown && todo.empty()) {
    added.clear();
    for (const auto& root : roots) added.push_back(root.first);
  }
  pinsKnown = true;
  startLook(added);
}

void LibraryIndex::Shared::startLook(const std::vector<std::string>& from) {
  if (from.empty()) return;
  if (todo.empty()) {
    done = 0;
    total = static_cast<int64_t>(folders.size());
    failed.clear();
  }
  for (const std::string& root : from)
    if (roots.at(root) == 0) total = -1;  // never looked through: how many is not known
  todo.insert(todo.end(), from.begin(), from.end());
  dirtyTodo = true;
}

void LibraryIndex::Shared::lookNext() {
  const std::string dir = todo.back();
  todo.pop_back();
  dirtyTodo = true;
  ++done;
  Listing listing;
  const remote::Status status = list(dir, listing);
  const bool root = roots.count(dir) > 0;
  if (status == remote::Status::Ok) {
    apply(dir, listing);
    todo.insert(todo.end(), listing.folders.begin(), listing.folders.end());
  } else if (status == remote::Status::Missing && !root) {
    dropFolder(dir);
  } else if (root) {
    failed.insert(dir);  // a share that doesn't answer, a drive taken out: kept as it was
  }
  if (!todo.empty()) return;
  const int64_t t = now();
  for (auto& [path, lookedAt] : roots)
    if (!failed.count(path)) {
      lookedAt = t;
      dirtyRoots.insert(path);
    }
  failed.clear();
  done = 0;
  total = -1;
}

// A folder's listing: its new videos recorded, those gone and its subfolders
// gone dropped. Only its own entries: its subfolders wait for a look.
void LibraryIndex::Shared::apply(const std::string& dir, const Listing& listing) {
  const auto root = rootOf(dir);
  if (!root) return;
  const bool firstLook = root->second == 0;
  const int64_t t = now();
  const auto [at, added] = folders.try_emplace(dir);
  if (added) {
    dirtyFolders.insert(dir);
    if (!roots.count(dir))
      if (const auto parent = folders.find(parentOf(dir)); parent != folders.end())
        parent->second.folders.insert(dir);
  }
  Folder& folder = at->second;
  std::unordered_set<std::string> seen;
  for (const auto& [path, found] : listing.videos) {
    seen.insert(path);
    folder.files.insert(path);
    const auto known = files.find(path);
    if (known == files.end()) {
      File f = found;
      f.foundAt = t;
      f.addedAt = firstLook ? std::min(found.mtime, t) : t;
      files.emplace(path, f);
      touch(path);
    } else if (known->second.size != found.size || known->second.mtime != found.mtime) {
      known->second.size = found.size;
      known->second.mtime = found.mtime;
      touch(path);
    }
  }
  for (auto it = folder.files.begin(); it != folder.files.end();) {
    if (seen.count(*it)) {
      ++it;
      continue;
    }
    files.erase(*it);
    touch(*it);
    it = folder.files.erase(it);
  }
  const std::unordered_set<std::string> there(listing.folders.begin(), listing.folders.end());
  std::vector<std::string> gone;
  for (const std::string& sub : folder.folders)
    if (!there.count(sub)) gone.push_back(sub);
  for (const std::string& sub : gone) dropFolder(sub);
}

void LibraryIndex::Shared::feed(const std::string& dir,
                                const std::vector<FolderListing::Entry>& entries) {
  if (!rootOf(dir)) return;
  Listing listing;
  for (const FolderListing::Entry& e : entries) {
    if (skipped(e.name, e.directory)) continue;
    if (e.directory)
      listing.folders.push_back(e.path);
    else if (isVideoFile(e.name))
      listing.videos.push_back({e.path, {e.size, e.modified}});
  }
  apply(dir, listing);
}

void LibraryIndex::Shared::dropFolder(const std::string& dir) {
  const auto it = folders.find(dir);
  if (it == folders.end()) return;
  const Folder folder = std::move(it->second);
  folders.erase(it);
  dirtyFolders.insert(dir);
  for (const std::string& path : folder.files) {
    files.erase(path);
    touch(path);
  }
  for (const std::string& sub : folder.folders) dropFolder(sub);
  if (const auto parent = folders.find(parentOf(dir)); parent != folders.end())
    parent->second.folders.erase(dir);
}

LibraryIndex::LibraryIndex() : shared_(std::make_shared<Shared>()) {}

LibraryIndex::~LibraryIndex() {
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->quit = true;
  }
  shared_->wake.notify_one();
}

void LibraryIndex::start() {
  std::thread([shared = shared_] { shared->run(); }).detach();
}

void LibraryIndex::setPins(const std::vector<std::string>& pins) {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  shared_->pins = pins;
  shared_->pinsChanged = true;
  shared_->wake.notify_one();
}

void LibraryIndex::setVideoOpen(bool open) {
  if (open == videoOpen_) return;
  videoOpen_ = open;
  std::lock_guard<std::mutex> lock(shared_->mutex);
  shared_->videoOpen = open;
  shared_->wake.notify_one();
}

void LibraryIndex::lookAgain() {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  shared_->again = true;
  shared_->wake.notify_one();
}

void LibraryIndex::deleted(const std::string& path) {
  files_.erase(path);
  std::lock_guard<std::mutex> lock(shared_->mutex);
  shared_->deleted.push_back(path);
  shared_->wake.notify_one();
}

void LibraryIndex::listed(const std::string& folder,
                          const std::vector<FolderListing::Entry>& entries) {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  shared_->listings.emplace_back(folder, entries);
  shared_->wake.notify_one();
}

bool LibraryIndex::poll(std::vector<std::string>& changed, std::vector<std::string>& removed) {
  std::unordered_map<std::string, File> found;
  std::unordered_set<std::string> gone;
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    if (shared_->changed.empty() && shared_->removed.empty()) return false;
    found.swap(shared_->changed);
    gone.swap(shared_->removed);
  }
  changed.clear();
  removed.clear();
  for (auto& [path, file] : found) {
    files_[path] = file;
    changed.push_back(path);
  }
  for (const std::string& path : gone)
    if (files_.erase(path)) removed.push_back(path);
  return !changed.empty() || !removed.empty();
}

LibraryIndex::State LibraryIndex::state() const {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  return shared_->state;
}
