#include "library_index.h"

#include "file_util.h"
#include "app_dirs.h"
#include "remote.h"
#include "stash_catalog.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
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

// The index is a disposable path cache, separate from playback history.
constexpr int kVersion = 2;
constexpr const char* kSchema = R"(
CREATE TABLE roots (path TEXT PRIMARY KEY, looked_at INTEGER NOT NULL);
CREATE TABLE folders (path TEXT PRIMARY KEY, mtime INTEGER NOT NULL DEFAULT 0,
                     mtime_ns INTEGER NOT NULL DEFAULT 0, ctime INTEGER NOT NULL DEFAULT 0,
                     ctime_ns INTEGER NOT NULL DEFAULT 0, file_id INTEGER NOT NULL DEFAULT 0,
                     checked_at INTEGER NOT NULL DEFAULT 0);
CREATE TABLE files (path TEXT PRIMARY KEY, size INTEGER NOT NULL, mtime INTEGER NOT NULL,
                    added_at INTEGER NOT NULL, found_at INTEGER NOT NULL);
CREATE TABLE todo (path TEXT NOT NULL);
CREATE TABLE meta (key TEXT PRIMARY KEY, value INTEGER NOT NULL);
CREATE TABLE failed_roots (path TEXT PRIMARY KEY);
)";
constexpr const char* kUpgrade = R"(
ALTER TABLE folders ADD COLUMN mtime INTEGER NOT NULL DEFAULT 0;
ALTER TABLE folders ADD COLUMN mtime_ns INTEGER NOT NULL DEFAULT 0;
ALTER TABLE folders ADD COLUMN ctime INTEGER NOT NULL DEFAULT 0;
ALTER TABLE folders ADD COLUMN ctime_ns INTEGER NOT NULL DEFAULT 0;
ALTER TABLE folders ADD COLUMN file_id INTEGER NOT NULL DEFAULT 0;
ALTER TABLE folders ADD COLUMN checked_at INTEGER NOT NULL DEFAULT 0;
CREATE TABLE failed_roots (path TEXT PRIMARY KEY);
)";

// During a look, what it found is saved and shown every so often.
constexpr auto kHandOver = std::chrono::seconds(2);
constexpr size_t kOutstanding = 32;
constexpr int64_t kVerifyAfter = 24 * 60 * 60;

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

void readEntries(const std::string& dir, const std::vector<remote::Entry>& entries,
                 LibraryIndex::Listing& out) {
  for (const auto& e : entries) {
    if (skipped(e.name, e.folder)) continue;
    if (e.folder) {
      if (!e.link) out.folders.push_back({dir + "/" + e.name, e.stamp});
    } else if (isVideoFile(e.name)) {
      out.videos.push_back({dir + "/" + e.name, {e.size, e.modified}});
    }
  }
}

// Stash's generated folder, wherever its settings put it: Stash makes
// screenshots/ (scene previews), vtt/, markers/, transcodes/,
// interactive_heatmaps/ and download_stage/ in it at every start. A folder
// with screenshots/ and two more of them is taken for one.
bool stashGenerated(const std::vector<LibraryIndex::Listing::Directory>& folders) {
  static const char* const kMore[] = {"vtt", "markers", "transcodes", "interactive_heatmaps",
                                      "download_stage"};
  bool screenshots = false;
  int more = 0;
  for (const auto& folder : folders) {
    const std::string& path = folder.path;
    const size_t slash = path.rfind('/');
    const char* name = path.c_str() + (slash == std::string::npos ? 0 : slash + 1);
    if (std::strcmp(name, "screenshots") == 0)
      screenshots = true;
    else
      more += std::any_of(std::begin(kMore), std::end(kMore),
                          [name](const char* m) { return std::strcmp(name, m) == 0; });
  }
  return screenshots && more >= 2;
}

}  // namespace

// Names, sizes and dates only: one stat per video, none for other files.
// Links to folders are not followed (one could lead back up).
remote::Status LibraryIndex::list(const std::string& dir, Listing& out,
                               const std::atomic<bool>* stop) {
  if (remote::isRemote(dir)) {
    std::vector<remote::Entry> entries;
    const remote::Status status = remote::list(dir, entries);
    if (status == remote::Status::Ok) readEntries(dir, entries, out);
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
      if (!skipped(e->d_name, true)) out.folders.push_back({path, {}});
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
  std::vector<std::shared_ptr<const StashCatalog>> stashes;  // handed over, the last the newest
  std::unordered_map<std::string, File> changed;
  std::unordered_set<std::string> removed;
  State state;

  struct Folder {
    std::unordered_set<std::string> folders, files;  // listed subfolders; videos
    remote::DirectoryStamp stamp;
    int64_t checkedAt = 0;
    uint64_t revision = 0;
  };
  struct Visit {
    std::string path;
    remote::DirectoryStamp stamp;
    int64_t beganAt = 0;
    uint64_t revision = 0;
  };
  struct Pending {
    Visit visit;
    std::future<remote::Listing> result;
  };
  sqlite3* db = nullptr;  // none: kept in memory only (another instance has the file)
  std::map<std::string, int64_t> roots;  // the pins (none inside another): last fully looked at
  std::unordered_map<std::string, Folder> folders;  // those listed
  std::unordered_map<std::string, File> files;
  std::vector<Visit> todo;  // folders this look has still to list, the next last
  std::vector<Pending> pending;  // also saved as todo until their results are applied
  std::unordered_set<std::string> failed;  // pins this look could not list
  std::shared_ptr<const StashCatalog> stash;
  std::unordered_set<std::string> generated;  // Stash's generated folders this start found
  int64_t done = 0, total = -1;  // folders this look listed, of those known before it (-1: not)
  bool pinsKnown = false;
  bool fullLook = true;
  int64_t verifiedAt = 0;
  std::unordered_set<std::string> toMain, dirtyFiles, dirtyFolders, dirtyRoots;
  bool dirtyTodo = false;

  void run();
  void load();
  void handOver();
  void save();
  void setPins(std::vector<std::string> pins);
  void startLook(const std::vector<std::string>& from, bool full = false);
  void lookNext();
  void received(const Visit& visit, remote::Status status, Listing& listing);
  bool skip(const Listing::Directory& dir) const;
  bool looking() const { return !todo.empty() || !pending.empty(); }
  bool ready() const {
    return std::any_of(pending.begin(), pending.end(), [](const Pending& p) {
      return p.result.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    });
  }
  void apply(const std::string& dir, const Listing& listing);
  void feed(const std::string& dir, const std::vector<FolderListing::Entry>& entries);
  void dropFolder(const std::string& dir);
  bool stashCopy(const std::string& path, uint64_t size) const;
  void dropStashCopies();
  void sift(const std::string& dir, Listing& listing);
  bool inGenerated(const std::string& dir, const std::string& root) const;
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
    std::vector<std::shared_ptr<const StashCatalog>> catalogs;
    {
      std::unique_lock<std::mutex> lock(mutex);
      auto work = [this] {
        return quit || pinsChanged || again || !listings.empty() || !deleted.empty() ||
               !stashes.empty() ||
               (!todo.empty() && !videoOpen && pending.size() < kOutstanding) || ready();
      };
      if (pending.empty()) wake.wait(lock, work);
      else wake.wait_for(lock, std::chrono::milliseconds(5), work);
      if (quit) break;
      pinsNew = std::exchange(pinsChanged, false);
      lookAgain = std::exchange(again, false);
      newPins = pins;
      listed.swap(listings);
      gone.swap(deleted);
      catalogs.swap(stashes);
    }
    if (!catalogs.empty()) {
      stash = std::move(catalogs.back());
      catalogs.clear();  // freed here, not on the main thread
      dropStashCopies();
    }
    if (pinsNew) setPins(std::move(newPins));
    if (lookAgain && !looking()) {
      std::vector<std::string> all;
      for (const auto& root : roots) all.push_back(root.first);
      startLook(all, true);  // explicit refresh always verifies every directory
    }
    for (const auto& [dir, entries] : listed) feed(dir, entries);
    for (const std::string& path : gone) {
      if (const auto folder = folders.find(parentOf(path)); folder != folders.end())
        if (folder->second.files.erase(path)) ++folder->second.revision;
      if (files.erase(path)) touch(path);
    }
    if (looking()) lookNext();
    if (pinsNew || lookAgain || !looking() || videoIsOpen() ||
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
  if (version() == 1) {
    if (!exec(db, "BEGIN") || !exec(db, kUpgrade) ||
        !exec(db, "PRAGMA user_version = 2") || !exec(db, "COMMIT")) {
      exec(db, "ROLLBACK");
      std::fprintf(stderr, "Library index: cannot upgrade its cache; kept in memory only\n");
      sqlite3_close(db);
      db = nullptr;
      return;
    }
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
  rows("SELECT path, mtime, mtime_ns, ctime, ctime_ns, file_id, checked_at FROM folders",
       [this](std::string path, sqlite3_stmt* st) {
         Folder& folder = folders[path];
         folder.stamp = {sqlite3_column_int64(st, 1), sqlite3_column_int64(st, 2),
                         sqlite3_column_int64(st, 3), sqlite3_column_int64(st, 4),
                         static_cast<uint64_t>(sqlite3_column_int64(st, 5))};
         folder.checkedAt = sqlite3_column_int64(st, 6);
       });
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
        [this](std::string path, sqlite3_stmt*) { todo.push_back({std::move(path), {}}); });
  for (const Visit& visit : todo) {
    if (folders.try_emplace(visit.path).second) dirtyFolders.insert(visit.path);
    if (roots.count(visit.path)) continue;  // "/" is its own parent
    if (const auto parent = folders.find(parentOf(visit.path)); parent != folders.end())
      parent->second.folders.insert(visit.path);
  }
  rows("SELECT key, value FROM meta", [this](std::string key, sqlite3_stmt* st) {
    const int64_t value = sqlite3_column_int64(st, 1);
    if (key == "done") done = value;
    else if (key == "total") total = value;
    else if (key == "full") fullLook = value != 0;
    else if (key == "verified_at") verifiedAt = value;
  });
  rows("SELECT path FROM failed_roots",
       [this](std::string path, sqlite3_stmt*) { failed.insert(std::move(path)); });
}

// What changed goes to the main thread and into the file.
void LibraryIndex::Shared::handOver() {
  State s;
  s.looking = looking();
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
  run("INSERT OR REPLACE INTO folders VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)", [&](sqlite3_stmt* st) {
    for (const std::string& path : dirtyFolders)
      if (const auto it = folders.find(path); it != folders.end()) {
        text(st, 1, path);
        const Folder& f = it->second;
        sqlite3_bind_int64(st, 2, f.stamp.modified);
        sqlite3_bind_int64(st, 3, f.stamp.modifiedNs);
        sqlite3_bind_int64(st, 4, f.stamp.changed);
        sqlite3_bind_int64(st, 5, f.stamp.changedNs);
        sqlite3_bind_int64(st, 6, static_cast<int64_t>(f.stamp.id));
        sqlite3_bind_int64(st, 7, f.checkedAt);
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
      // In-flight requests are not durable results. Revisit them after a restart.
      for (const Pending& p : pending) {
        text(st, 1, p.visit.path);
        step(st);
      }
      for (const Visit& visit : todo) {
        text(st, 1, visit.path);
        step(st);
      }
    });
    run("INSERT OR REPLACE INTO meta VALUES ('done', ?1), ('total', ?2)", [&](sqlite3_stmt* st) {
      sqlite3_bind_int64(st, 1, done);
      sqlite3_bind_int64(st, 2, total);
      step(st);
    });
    run("INSERT OR REPLACE INTO meta VALUES ('full', ?1), ('verified_at', ?2)", [&](sqlite3_stmt* st) {
      sqlite3_bind_int64(st, 1, fullLook);
      sqlite3_bind_int64(st, 2, verifiedAt);
      step(st);
    });
    ok = ok && exec(db, "DELETE FROM failed_roots");
    run("INSERT INTO failed_roots VALUES (?1)", [&](sqlite3_stmt* st) {
      for (const std::string& path : failed) {
        text(st, 1, path);
        step(st);
      }
    });
  }
  ok = ok && exec(db, "COMMIT");
  if (!ok) {
    // Only a cache: the next look puts it right.
    std::fprintf(stderr, "Library index: not saved (%s)\n", sqlite3_errmsg(db));
    exec(db, "ROLLBACK");
    return;  // retry the same dirty paths at the next handover
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
    todo.erase(std::remove_if(todo.begin(), todo.end(), [&](const Visit& p) { return !covered(p.path); }),
               todo.end());
    dirtyTodo = dirtyTodo || todo.size() != before;
  }
  // Once per start: the look an earlier start did not finish goes on, else a new one.
  if (!pinsKnown && !looking()) {
    added.clear();
    for (const auto& root : roots) added.push_back(root.first);
  }
  pinsKnown = true;
  startLook(added);
}

void LibraryIndex::Shared::startLook(const std::vector<std::string>& from, bool full) {
  if (from.empty()) return;
  if (!looking()) {
    done = 0;
    total = static_cast<int64_t>(folders.size());
    failed.clear();
    const int64_t t = now();
    fullLook = full || verifiedAt <= 0 || t < verifiedAt || t - verifiedAt >= kVerifyAfter;
  }
  for (const std::string& root : from)
    if (roots.at(root) == 0) total = -1;  // never looked through: how many is not known
  for (const std::string& path : from) todo.push_back({path, {}});
  dirtyTodo = true;
}

void LibraryIndex::Shared::lookNext() {
  // Apply every completed result, not just the oldest request. A slow folder
  // must not hold up the rest of discovery or the connection's playback jobs.
  for (size_t i = 0; i < pending.size();) {
    if (pending[i].result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
      ++i;
      continue;
    }
    auto result = pending[i].result.get();
    Visit visit = std::move(pending[i].visit);
    pending.erase(pending.begin() + static_cast<ptrdiff_t>(i));
    Listing listing;
    if (result.status == remote::Status::Ok) readEntries(visit.path, result.entries, listing);
    received(visit, result.status, listing);
  }
  // Only submitting is paused. Results already in flight can finish and be
  // saved without waiting on, or submitting work from, the render loop.
  size_t local = 0;
  while (!todo.empty() && pending.size() < kOutstanding && local < kOutstanding && !videoIsOpen()) {
    Visit visit = std::move(todo.back());
    todo.pop_back();
    dirtyTodo = true;
    if (!rootOf(visit.path)) continue;
    if (!roots.count(visit.path) && !folders.count(visit.path)) continue;
    visit.beganAt = now();
    if (const auto f = folders.find(visit.path); f != folders.end())
      visit.revision = f->second.revision;
    if (visit.path.compare(0, 6, "smb://") == 0) {
      auto future = remote::listAsync(visit.path);
      pending.push_back({std::move(visit), std::move(future)});
    } else {
      Listing listing;
      const auto status = list(visit.path, listing);
      received(visit, status, listing);
      ++local;
      // SFTP's backend lists synchronously. Queueing a window of those
      // requests would put browsing behind the entire scan window.
      if (remote::isRemote(visit.path)) break;
    }
  }
  if (looking()) return;
  const int64_t t = now();
  for (auto& [path, lookedAt] : roots)
    if (!failed.count(path)) {
      lookedAt = t;
      dirtyRoots.insert(path);
    }
  if (fullLook && failed.empty()) verifiedAt = t;
  failed.clear();
  done = 0;
  total = -1;
  dirtyTodo = true;
}

bool LibraryIndex::Shared::skip(const Listing::Directory& dir) const {
  if (fullLook || !dir.stamp.usable()) return false;
  const auto it = folders.find(dir.path);
  if (it == folders.end()) return false;
  const Folder& f = it->second;
  const int64_t t = now();
  // Never skip branches: an unchanged ancestor says nothing about changes
  // deeper down. Always list videos too, since in-place edits need not change
  // their directory. Only a verified, unchanged, video-free leaf qualifies.
  return f.files.empty() && f.folders.empty() && f.stamp == dir.stamp &&
         f.checkedAt > 0 && t >= f.checkedAt && t - f.checkedAt < kVerifyAfter &&
         f.stamp.modified < f.checkedAt - 2 && f.stamp.changed < f.checkedAt - 2;
}

void LibraryIndex::Shared::received(const Visit& visit, remote::Status status,
                                     Listing& listing) {
  const std::string& dir = visit.path;
  const auto owner = rootOf(dir);
  if (!owner) return;  // the pin was removed while its request was in flight
  if (!roots.count(dir) && !folders.count(dir)) return;  // its parent removed it
  if (const auto f = folders.find(dir);
      f != folders.end() && f->second.revision != visit.revision) {
    todo.push_back({dir, {}});  // a newer browser listing/deletion won the race
    dirtyTodo = true;
    return;
  }
  ++done;
  dirtyTodo = true;
  const bool root = roots.count(dir) > 0;
  if (status == remote::Status::Ok) {
    sift(dir, listing);
    apply(dir, listing);
    const auto it = folders.find(dir);
    if (it == folders.end()) return;  // inside Stash's generated folder
    Folder& folder = it->second;
    folder.stamp = visit.stamp;
    folder.checkedAt = visit.beganAt;
    dirtyFolders.insert(dir);
    for (const auto& child : listing.folders) {
      if (skip(child)) ++done;
      else todo.push_back({child.path, child.stamp});
    }
  } else if (status == remote::Status::Missing && !root) {
    dropFolder(dir);
  } else {
    failed.insert(owner->first);  // partial/offline/denied trees are never marked up to date
  }
}

// A folder's listing: its new videos recorded, those gone and its subfolders
// gone dropped. Only its own entries: its subfolders wait for a look.
void LibraryIndex::Shared::apply(const std::string& dir, const Listing& listing) {
  const auto root = rootOf(dir);
  if (!root) return;
  if (inGenerated(dir, root->first)) {
    dropFolder(dir);
    return;
  }
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
    if (stashCopy(path, found.size)) continue;
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
  std::unordered_set<std::string> there;
  for (const auto& child : listing.folders) there.insert(child.path);
  std::vector<std::string> gone;
  for (const std::string& sub : folder.folders)
    if (!there.count(sub)) gone.push_back(sub);
  for (const std::string& sub : gone) dropFolder(sub);
  // Remember children even if their own listing later fails. Otherwise an
  // unreadable child could make its parent look like a verified empty leaf.
  for (const auto& child : listing.folders) {
    folder.folders.insert(child.path);
    if (folders.try_emplace(child.path).second) dirtyFolders.insert(child.path);
  }
}

void LibraryIndex::Shared::feed(const std::string& dir,
                                const std::vector<FolderListing::Entry>& entries) {
  if (!rootOf(dir)) return;
  Listing listing;
  for (const FolderListing::Entry& e : entries) {
    if (skipped(e.name, e.directory)) continue;
    if (e.directory)
      listing.folders.push_back({e.path, {}});
    else if (isVideoFile(e.name))
      listing.videos.push_back({e.path, {e.size, e.modified}});
  }
  sift(dir, listing);
  apply(dir, listing);
  const auto it = folders.find(dir);
  if (it == folders.end()) return;  // inside Stash's generated folder
  // Browser folder dates may be synthetic; only a scan's metadata can
  // authorize skipping its own future enumeration.
  it->second.stamp = {};
  it->second.checkedAt = 0;
  ++it->second.revision;
  dirtyFolders.insert(dir);
}

// Stash names a scene's preview (screenshots/<oshash>.mp4) and transcode
// after the oshash of its file. The scene's own file, if it is named so,
// has that oshash's size and stays.
bool LibraryIndex::Shared::stashCopy(const std::string& path, uint64_t size) const {
  if (!stash || path.size() < 4 || path.compare(path.size() - 4, 4, ".mp4") != 0) return false;
  const size_t slash = path.rfind('/');
  const size_t start = slash == std::string::npos ? 0 : slash + 1;
  const std::string stem = path.substr(start, path.size() - 4 - start);
  const int scene = stash->sceneByOshash(stem);
  if (scene < 0) return false;
  const StashScene& s = stash->scenes()[static_cast<size_t>(scene)];
  return s.oshash != stem || s.size != size;
}

// A new catalog: those found before it that are Stash's copies go.
void LibraryIndex::Shared::dropStashCopies() {
  if (!stash) return;
  std::vector<std::string> copies;
  for (const auto& [path, file] : files)
    if (stashCopy(path, file.size)) copies.push_back(path);
  for (const std::string& path : copies) {
    if (const auto folder = folders.find(parentOf(path)); folder != folders.end())
      folder->second.files.erase(path);
    files.erase(path);
    touch(path);
  }
}

// Stash's generated folder lists as empty: nothing in it is looked at, and
// what was found in it before (by an earlier look, the file browser or a
// look of an older version) goes.
void LibraryIndex::Shared::sift(const std::string& dir, Listing& listing) {
  if (!stashGenerated(listing.folders)) {
    generated.erase(dir);
    return;
  }
  listing = {};
  if (!generated.insert(dir).second) return;
  std::vector<std::string> inside;
  for (const auto& folder : folders)
    if (folder.first != dir && under(folder.first, dir)) inside.push_back(folder.first);
  for (const std::string& sub : inside) dropFolder(sub);
  const size_t before = todo.size();
  todo.erase(std::remove_if(todo.begin(), todo.end(),
                            [&](const Visit& v) { return v.path != dir && under(v.path, dir); }),
             todo.end());
  dirtyTodo = dirtyTodo || todo.size() != before;
}

// Whether a folder is inside one of Stash's generated folders.
bool LibraryIndex::Shared::inGenerated(const std::string& dir, const std::string& root) const {
  if (generated.empty()) return false;
  for (std::string path = dir; path.size() > root.size();) {
    std::string parent = parentOf(path);
    if (parent.size() >= path.size()) break;
    if (generated.count(parent)) return true;
    path = std::move(parent);
  }
  return false;
}

void LibraryIndex::Shared::dropFolder(const std::string& dir) {
  const auto it = folders.find(dir);
  if (it == folders.end()) return;
  const Folder folder = std::move(it->second);
  folders.erase(it);
  generated.erase(dir);
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

void LibraryIndex::setStash(const std::shared_ptr<const StashCatalog>& catalog) {
  if (catalog.get() == stash_) return;
  stash_ = catalog.get();
  std::lock_guard<std::mutex> lock(shared_->mutex);
  shared_->stashes.push_back(catalog);
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
