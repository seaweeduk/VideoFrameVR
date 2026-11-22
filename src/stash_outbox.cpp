#include "stash_outbox.h"

#include "app_dirs.h"
#include "sqlite_util.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>

#include <sqlite3.h>

namespace fs = std::filesystem;

namespace {

using sqlite::Statement;
using sqlite::exec;
using sqlite::userVersion;

constexpr int kOutboxVersion = 1;

constexpr const char* kOutboxSchema = R"(
CREATE TABLE ratings (oshash TEXT PRIMARY KEY, stars REAL NOT NULL, queued_at INTEGER NOT NULL)
  WITHOUT ROWID;
CREATE TABLE plays (
  id INTEGER PRIMARY KEY, oshash TEXT NOT NULL, played_at INTEGER NOT NULL,
  queued_at INTEGER NOT NULL);
CREATE TABLE sent_plays (
  oshash TEXT NOT NULL, scene_id INTEGER NOT NULL, played_at INTEGER NOT NULL,
  sent_at INTEGER NOT NULL, found INTEGER NOT NULL);
CREATE INDEX sent_plays_oshash ON sent_plays (oshash);
PRAGMA user_version = 1;
)";

int64_t unixNow() { return static_cast<int64_t>(std::time(nullptr)); }

}  // namespace

StashOutbox::StashOutbox(const std::string& server, bool original) : original_(original) {
  uint64_t hash = 14695981039346656037ull;  // FNV-1a
  for (const unsigned char c : server) hash = (hash ^ c) * 1099511628211ull;
  char name[48];
  std::snprintf(name, sizeof(name), "stash_outbox-%016llx.db", static_cast<unsigned long long>(hash));
  path_ = (dataDir() / name).string();
}

StashOutbox::~StashOutbox() { sqlite3_close(db_); }

bool StashOutbox::adopt() {
  std::error_code ec;
  const std::string single = (dataDir() / "stash_outbox.db").string();
  if (!original_ || fs::exists(path_, ec) || !fs::exists(single, ec)) return true;
  // Its write-ahead log folded in first, so it moves as one file; not while
  // another connection has it open (a stopping write-back's).
  sqlite3* db = nullptr;
  bool ok = sqlite3_open_v2(single.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK;
  if (ok) {
    Statement mode(db, "PRAGMA journal_mode = DELETE");
    ok = mode.ok() && mode.next() && mode.text(0) == "delete";
  }
  sqlite3_close(db);
  if (ok) fs::rename(single, path_, ec);
  if (!ok || ec) {
    error_ = "stash_outbox.db could not be taken over yet; nothing is sent";
    return false;
  }
  return true;
}

bool StashOutbox::fail(const char* what) {
  error_ = std::string(what) + (db_ ? std::string(" (") + sqlite3_errmsg(db_) + ")" : std::string());
  if (db_ && !sqlite3_get_autocommit(db_)) exec(db_, "ROLLBACK");
  return false;
}

bool StashOutbox::open() {
  if (db_) return true;
  error_.clear();
  if (!adopt()) return false;
  std::error_code ec;
  fs::create_directories(dataDir(), ec);
  if (sqlite3_open_v2(path_.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
      SQLITE_OK) {
    fail("stash_outbox.db could not be opened");
    sqlite3_close(db_);
    db_ = nullptr;
    return false;
  }
  sqlite3_busy_timeout(db_, 1000);
  int version = userVersion(db_);
  if (version == 0) {
    bool empty = false;
    {
      Statement count(db_, "SELECT count(*) FROM sqlite_master");
      empty = count.ok() && count.next() && count.integer(0) == 0;
    }
    if (empty) {
      if (!exec(db_, "PRAGMA journal_mode = WAL") || !exec(db_, "BEGIN") ||
          !exec(db_, kOutboxSchema) || !exec(db_, "COMMIT")) {
        fail("stash_outbox.db could not be made");
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
      }
      version = kOutboxVersion;
    }
  }
  if (version != kOutboxVersion) {
    // Not ours to throw away: what waits in it can't be fetched again.
    error_ = "stash_outbox.db is from another version of VideoFrameVR; nothing is sent";
    sqlite3_close(db_);
    db_ = nullptr;
    return false;
  }
  exec(db_, "PRAGMA journal_mode = WAL");  // also for a file taken over
  exec(db_, "PRAGMA synchronous = NORMAL");
  return true;
}

bool StashOutbox::addRating(const std::string& oshash, float stars, bool replace) {
  if (!open()) return false;
  Statement st(db_, replace ? "INSERT OR REPLACE INTO ratings VALUES (?1, ?2, ?3)"
                            : "INSERT OR IGNORE INTO ratings VALUES (?1, ?2, ?3)");
  if (!st.ok() || !st.bind(1, oshash).bind(2, static_cast<double>(stars)).bind(3, unixNow()).run())
    return fail("stash_outbox.db could not be written");
  return true;
}

bool StashOutbox::addPlays(const std::string& oshash, int64_t at, int count) {
  if (!open()) return false;
  bool ok = exec(db_, "BEGIN IMMEDIATE");
  {
    Statement st(db_, "INSERT INTO plays (oshash, played_at, queued_at) VALUES (?1, ?2, ?3)");
    ok = ok && st.ok();
    for (int i = 0; ok && i < count; ++i) ok = st.bind(1, oshash).bind(2, at).bind(3, unixNow()).run();
  }
  if (!(ok && exec(db_, "COMMIT"))) return fail("stash_outbox.db could not be written");
  return true;
}

bool StashOutbox::waiting(std::vector<Rating>& ratings, std::vector<Play>& plays) {
  ratings.clear();
  plays.clear();
  std::error_code ec;
  if (!db_ && !adopt()) return false;
  if (!db_ && !fs::exists(path_, ec)) return true;  // nothing kept yet
  if (!open()) return false;
  bool ok = exec(db_, "BEGIN");
  {
    Statement r(db_, "SELECT oshash, stars FROM ratings ORDER BY queued_at");
    ok = ok && r.ok();
    while (ok && r.next()) ratings.push_back({r.text(0), static_cast<float>(r.real(1))});
    Statement p(db_, "SELECT id, oshash, played_at FROM plays ORDER BY id");
    ok = ok && p.ok();
    while (ok && p.next()) plays.push_back({p.integer(0), p.text(1), p.integer(2)});
  }
  if (!(ok && exec(db_, "COMMIT"))) return fail("stash_outbox.db could not be read");
  return true;
}

bool StashOutbox::ratingDone(const Rating& rating) {
  if (!open()) return false;
  Statement st(db_, "DELETE FROM ratings WHERE oshash = ?1 AND stars = ?2");
  if (!st.ok() || !st.bind(1, rating.oshash).bind(2, static_cast<double>(rating.stars)).run())
    return fail("stash_outbox.db could not be written");
  return true;
}

bool StashOutbox::playsDone(const std::vector<Play>& plays, int64_t scene,
                            const std::vector<bool>& found) {
  if (!open()) return false;
  bool ok = exec(db_, "BEGIN IMMEDIATE");
  {
    Statement remove(db_, "DELETE FROM plays WHERE id = ?1");
    Statement keep(db_, "INSERT INTO sent_plays VALUES (?1, ?2, ?3, ?4, ?5)");
    ok = ok && remove.ok() && keep.ok();
    for (size_t i = 0; ok && i < plays.size(); ++i)
      ok = remove.bind(1, plays[i].row).run() &&
           keep.bind(1, plays[i].oshash).bind(2, scene).bind(3, plays[i].at).bind(4, unixNow())
               .bind(5, static_cast<int64_t>(i < found.size() && found[i])).run();
  }
  if (!(ok && exec(db_, "COMMIT"))) return fail("stash_outbox.db could not be written");
  return true;
}

bool StashOutbox::drop(const std::vector<Rating>& ratings, const std::vector<Play>& plays) {
  if (!open()) return false;
  bool ok = exec(db_, "BEGIN IMMEDIATE");
  {
    Statement r(db_, "DELETE FROM ratings WHERE oshash = ?1 AND stars = ?2");
    Statement p(db_, "DELETE FROM plays WHERE id = ?1");
    ok = ok && r.ok() && p.ok();
    for (const Rating& x : ratings)
      ok = ok && r.bind(1, x.oshash).bind(2, static_cast<double>(x.stars)).run();
    for (const Play& x : plays) ok = ok && p.bind(1, x.row).run();
  }
  if (!(ok && exec(db_, "COMMIT"))) return fail("stash_outbox.db could not be written");
  return true;
}

bool StashOutbox::sent(std::unordered_map<std::string, std::vector<int64_t>>& times) {
  times.clear();
  std::error_code ec;
  if (!db_ && !adopt()) return false;
  if (!db_ && !fs::exists(path_, ec)) return true;
  if (!open()) return false;
  Statement st(db_, "SELECT oshash, played_at FROM sent_plays");
  if (!st.ok()) return fail("stash_outbox.db could not be read");
  while (st.next()) times[st.text(0)].push_back(st.integer(1));
  return true;
}
