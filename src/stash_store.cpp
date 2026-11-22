#include "stash_store.h"

#include "app_dirs.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>

#include <sqlite3.h>

namespace fs = std::filesystem;

namespace {

// The layout this build writes (PRAGMA user_version). A file with another
// is made anew at the next sync, and meanwhile counts as no copy; but for
// kVersionBefore's, which is read as it is: the same without files.id and
// scenes.vr.
constexpr int kVersion = 2;
constexpr int kVersionBefore = 1;
bool readable(int version) { return version == kVersion || version == kVersionBefore; }

// Lists (URLs, aliases) are kept one per line.
constexpr const char* kSchema = R"(
CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID;
CREATE TABLE scenes (
  id INTEGER PRIMARY KEY, title TEXT, code TEXT, details TEXT, director TEXT, date TEXT,
  created_at INTEGER, studio_id INTEGER, urls TEXT, cover TEXT, vr INTEGER);
CREATE TABLE files (
  scene_id INTEGER NOT NULL, position INTEGER NOT NULL, path TEXT NOT NULL, size INTEGER,
  mod_time INTEGER, width INTEGER, height INTEGER, duration REAL, fps REAL, codec TEXT, oshash TEXT,
  id INTEGER,
  PRIMARY KEY (scene_id, position)) WITHOUT ROWID;
CREATE TABLE scene_performers (
  scene_id INTEGER NOT NULL, performer_id INTEGER NOT NULL,
  PRIMARY KEY (scene_id, performer_id)) WITHOUT ROWID;
CREATE TABLE scene_tags (
  scene_id INTEGER NOT NULL, tag_id INTEGER NOT NULL, PRIMARY KEY (scene_id, tag_id)) WITHOUT ROWID;
CREATE TABLE scene_groups (
  scene_id INTEGER NOT NULL, group_id INTEGER NOT NULL, scene_index INTEGER,
  PRIMARY KEY (scene_id, group_id)) WITHOUT ROWID;
CREATE TABLE performers (
  id INTEGER PRIMARY KEY, name TEXT, disambiguation TEXT, aliases TEXT, gender TEXT, birthdate TEXT,
  country TEXT, ethnicity TEXT, hair_color TEXT, eye_color TEXT, height_cm INTEGER,
  measurements TEXT, fake_tits TEXT, tattoos TEXT, piercings TEXT, career_start TEXT,
  career_end TEXT, image TEXT, favorite INTEGER);
CREATE TABLE studios (id INTEGER PRIMARY KEY, name TEXT, parent_id INTEGER, image TEXT);
CREATE TABLE tags (id INTEGER PRIMARY KEY, name TEXT);
CREATE TABLE groups (id INTEGER PRIMARY KEY, name TEXT, date TEXT, studio_id INTEGER, image TEXT);
)";

constexpr const char* kTables[] = {"meta",       "scenes",       "files",      "scene_performers",
                                   "scene_tags", "scene_groups", "performers", "studios",
                                   "tags",       "groups"};

std::string joinLines(const std::vector<std::string>& list) {
  std::string out;
  for (const std::string& s : list) {
    if (!out.empty()) out += '\n';
    out += s;
  }
  return out;
}

std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> list;
  for (size_t at = 0; at < text.size();) {
    const size_t end = std::min(text.find('\n', at), text.size());
    list.push_back(text.substr(at, end - at));
    at = end + 1;
  }
  return list;
}

// One prepared statement, bound and read by column number.
class Statement {
 public:
  Statement(sqlite3* db, const char* sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &st_, nullptr) != SQLITE_OK) st_ = nullptr;
  }
  ~Statement() { sqlite3_finalize(st_); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  bool ok() const { return st_ != nullptr; }

  Statement& bind(int i, const std::string& s) {
    if (s.empty()) sqlite3_bind_null(st_, i);
    else sqlite3_bind_text(st_, i, s.data(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
    return *this;
  }
  Statement& bind(int i, int64_t v) {
    sqlite3_bind_int64(st_, i, v);
    return *this;
  }
  Statement& bind(int i, double v) {
    sqlite3_bind_double(st_, i, v);
    return *this;
  }
  // Runs a bound INSERT; false on an error.
  bool run() {
    const bool done = sqlite3_step(st_) == SQLITE_DONE;
    sqlite3_reset(st_);
    sqlite3_clear_bindings(st_);
    return done;
  }

  bool next() { return sqlite3_step(st_) == SQLITE_ROW; }
  std::string text(int i) const {
    const unsigned char* t = sqlite3_column_text(st_, i);
    return t ? std::string(reinterpret_cast<const char*>(t), sqlite3_column_bytes(st_, i))
             : std::string();
  }
  int64_t integer(int i) const { return sqlite3_column_int64(st_, i); }
  double real(int i) const { return sqlite3_column_double(st_, i); }

 private:
  sqlite3_stmt* st_ = nullptr;
};

bool exec(sqlite3* db, const char* sql) {
  return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

int userVersion(sqlite3* db) {
  Statement st(db, "PRAGMA user_version");
  return st.ok() && st.next() ? static_cast<int>(st.integer(0)) : -1;
}

}  // namespace

std::string StashStore::path(const MediaServer& kind) {
  return (dataDir() / (std::string(kind.id()) + ".db")).string();
}

StashStore::StashStore(const MediaServer& kind) : path_(path(kind)) {}

bool StashStore::load(StashData& data) {
  error_.clear();
  std::error_code ec;
  if (!fs::exists(path_, ec)) {
    error_ = "no local copy yet";
    return false;
  }
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path_.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    error_ = "The local copy could not be opened";
    return false;
  }
  sqlite3_busy_timeout(db, 1000);
  const int version = userVersion(db);
  if (!readable(version)) {
    sqlite3_close(db);
    error_ = "The local copy is from another version";
    return false;
  }
  const bool before = version == kVersionBefore;
  data = StashData{};
  bool ok = exec(db, "BEGIN");  // one consistent read
  {
    Statement meta(db, "SELECT key, value FROM meta");
    ok = ok && meta.ok();
    while (ok && meta.next()) {
      const std::string key = meta.text(0);
      if (key == "server") data.server = meta.text(1);
      else if (key == "kind") data.kind = meta.text(1);
      else if (key == "version") data.version = meta.text(1);
      else if (key == "vr_tag") data.vrTag = meta.text(1);
      else if (key == "synced_at") data.syncedAt = std::stoll("0" + meta.text(1));
      else if (key == "library_folders") {  // one per line
        std::istringstream lines(meta.text(1));
        for (std::string line; std::getline(lines, line);)
          if (!line.empty()) data.libraryFolders.push_back(line);
      }
    }

    std::unordered_map<int64_t, size_t> at;  // scene id -> index
    Statement scenes(db, before ? "SELECT id, title, code, details, director, date, created_at, "
                                  "studio_id, urls, cover, 0 FROM scenes ORDER BY id"
                                : "SELECT id, title, code, details, director, date, created_at, "
                                  "studio_id, urls, cover, vr FROM scenes ORDER BY id");
    ok = ok && scenes.ok();
    while (ok && scenes.next()) {
      StashData::Scene s;
      s.id = scenes.integer(0);
      s.title = scenes.text(1);
      s.code = scenes.text(2);
      s.details = scenes.text(3);
      s.director = scenes.text(4);
      s.date = scenes.text(5);
      s.createdAt = scenes.integer(6);
      s.studio = scenes.integer(7);
      s.urls = splitLines(scenes.text(8));
      s.cover = scenes.text(9);
      s.vr = scenes.integer(10) != 0;
      at[s.id] = data.scenes.size();
      data.scenes.push_back(std::move(s));
    }
    auto scene = [&](int64_t id) -> StashData::Scene* {
      const auto it = at.find(id);
      return it != at.end() ? &data.scenes[it->second] : nullptr;
    };
    Statement files(db, before ? "SELECT scene_id, path, size, mod_time, width, height, duration, "
                                 "fps, codec, oshash, 0 FROM files ORDER BY scene_id, position"
                               : "SELECT scene_id, path, size, mod_time, width, height, duration, "
                                 "fps, codec, oshash, id FROM files ORDER BY scene_id, position");
    ok = ok && files.ok();
    while (ok && files.next()) {
      StashData::Scene* s = scene(files.integer(0));
      if (!s) continue;
      StashData::File f;
      f.path = files.text(1);
      f.size = static_cast<uint64_t>(files.integer(2));
      f.modified = files.integer(3);
      f.width = static_cast<int>(files.integer(4));
      f.height = static_cast<int>(files.integer(5));
      f.duration = files.real(6);
      f.fps = files.real(7);
      f.codec = files.text(8);
      f.oshash = files.text(9);
      f.id = files.integer(10);
      s->files.push_back(std::move(f));
    }
    Statement performers(db, "SELECT scene_id, performer_id FROM scene_performers");
    ok = ok && performers.ok();
    while (ok && performers.next())
      if (StashData::Scene* s = scene(performers.integer(0))) s->performers.push_back(performers.integer(1));
    Statement tags(db, "SELECT scene_id, tag_id FROM scene_tags");
    ok = ok && tags.ok();
    while (ok && tags.next())
      if (StashData::Scene* s = scene(tags.integer(0))) s->tags.push_back(tags.integer(1));
    Statement groups(db, "SELECT scene_id, group_id, scene_index FROM scene_groups");
    ok = ok && groups.ok();
    while (ok && groups.next())
      if (StashData::Scene* s = scene(groups.integer(0)))
        s->groups.emplace_back(groups.integer(1), static_cast<int>(groups.integer(2)));

    Statement p(db,
                "SELECT id, name, disambiguation, aliases, gender, birthdate, country, ethnicity, "
                "hair_color, eye_color, height_cm, measurements, fake_tits, tattoos, piercings, "
                "career_start, career_end, image, favorite FROM performers");
    ok = ok && p.ok();
    while (ok && p.next()) {
      StashData::Performer r;
      r.id = p.integer(0);
      r.name = p.text(1);
      r.disambiguation = p.text(2);
      r.aliases = splitLines(p.text(3));
      r.gender = p.text(4);
      r.birthdate = p.text(5);
      r.country = p.text(6);
      r.ethnicity = p.text(7);
      r.hairColor = p.text(8);
      r.eyeColor = p.text(9);
      r.heightCm = static_cast<int>(p.integer(10));
      r.measurements = p.text(11);
      r.fakeTits = p.text(12);
      r.tattoos = p.text(13);
      r.piercings = p.text(14);
      r.careerStart = p.text(15);
      r.careerEnd = p.text(16);
      r.image = p.text(17);
      r.favorite = p.integer(18) != 0;
      data.performers.push_back(std::move(r));
    }
    Statement studios(db, "SELECT id, name, image, parent_id FROM studios");
    ok = ok && studios.ok();
    while (ok && studios.next())
      data.studios.push_back(
          {studios.integer(0), studios.text(1), studios.text(2), studios.integer(3)});
    Statement t(db, "SELECT id, name FROM tags");
    ok = ok && t.ok();
    while (ok && t.next()) data.tags.push_back({t.integer(0), t.text(1)});
    Statement g(db, "SELECT id, name, date, image, studio_id FROM groups");
    ok = ok && g.ok();
    while (ok && g.next())
      data.groups.push_back({g.integer(0), g.text(1), g.text(2), g.text(3), g.integer(4)});
  }
  exec(db, "COMMIT");
  sqlite3_close(db);
  if (!ok) {
    error_ = "The local copy could not be read";
    data = StashData{};
  }
  return ok;
}

bool StashStore::save(const StashData& data) {
  error_.clear();
  std::error_code ec;
  fs::create_directories(dataDir(), ec);
  const std::string& file = path_;
  sqlite3* db = nullptr;
  auto open = [&] {
    sqlite3_close(db);
    db = nullptr;
    return sqlite3_open_v2(file.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                           nullptr) == SQLITE_OK;
  };
  auto fail = [&](const char* why) {
    error_ = why;
    sqlite3_close(db);
    return false;
  };
  if (!open()) return fail("The local copy could not be opened");
  sqlite3_busy_timeout(db, 1000);
  const int version = userVersion(db);
  if (version != kVersion) {
    // Another layout, or not a database at all: made anew.
    bool empty = false;
    if (version == 0) {
      Statement count(db, "SELECT count(*) FROM sqlite_master");
      empty = count.ok() && count.next() && count.integer(0) == 0;
    }
    if (!empty) {
      sqlite3_close(db);
      db = nullptr;
      for (const char* tail : {"", "-wal", "-shm", "-journal"}) fs::remove(file + tail, ec);
      if (!open()) return fail("The local copy could not be created");
    }
    const std::string bump = "PRAGMA user_version = " + std::to_string(kVersion);
    if (!exec(db, "PRAGMA journal_mode = WAL") || !exec(db, "BEGIN") || !exec(db, kSchema) ||
        !exec(db, bump.c_str()) || !exec(db, "COMMIT"))
      return fail("The local copy could not be created");
  }
  exec(db, "PRAGMA synchronous = NORMAL");

  bool ok = exec(db, "BEGIN IMMEDIATE");
  for (const char* table : kTables)
    ok = ok && exec(db, ("DELETE FROM " + std::string(table)).c_str());
  {
    Statement meta(db, "INSERT INTO meta (key, value) VALUES (?1, ?2)");
    Statement scene(db,
                    "INSERT INTO scenes (id, title, code, details, director, date, created_at, "
                    "studio_id, urls, cover, vr) "
                    "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)");
    Statement file(db,
                   "INSERT INTO files (scene_id, position, path, size, mod_time, width, height, "
                   "duration, fps, codec, oshash, id) "
                   "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)");
    Statement performer(db, "INSERT OR IGNORE INTO scene_performers VALUES (?1, ?2)");
    Statement tag(db, "INSERT OR IGNORE INTO scene_tags VALUES (?1, ?2)");
    Statement group(db, "INSERT OR IGNORE INTO scene_groups VALUES (?1, ?2, ?3)");
    Statement p(db,
                "INSERT INTO performers (id, name, disambiguation, aliases, gender, birthdate, "
                "country, ethnicity, hair_color, eye_color, height_cm, measurements, fake_tits, "
                "tattoos, piercings, career_start, career_end, image, favorite) VALUES "
                "(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, "
                "?18, ?19)");
    Statement studio(db, "INSERT INTO studios (id, name, parent_id, image) VALUES (?1, ?2, ?3, ?4)");
    Statement t(db, "INSERT INTO tags (id, name) VALUES (?1, ?2)");
    Statement g(db,
                "INSERT INTO groups (id, name, date, studio_id, image) VALUES (?1, ?2, ?3, ?4, ?5)");
    ok = ok && meta.ok() && scene.ok() && file.ok() && performer.ok() && tag.ok() && group.ok() &&
         p.ok() && studio.ok() && t.ok() && g.ok();

    // What the server has none of (a VR tag, folders) is left out: a value
    // can't be empty.
    auto keep = [&](const char* key, const std::string& value) {
      if (ok && !value.empty()) ok = meta.bind(1, std::string(key)).bind(2, value).run();
    };
    keep("server", data.server);
    keep("kind", data.kind);
    keep("version", data.version);
    keep("vr_tag", data.vrTag);
    keep("synced_at", std::to_string(data.syncedAt));
    {
      std::string folders;
      for (const std::string& f : data.libraryFolders) folders += f + "\n";
      keep("library_folders", folders);
    }
    for (const StashData::Scene& s : data.scenes) {
      if (!ok) break;
      ok = scene.bind(1, s.id).bind(2, s.title).bind(3, s.code).bind(4, s.details)
               .bind(5, s.director).bind(6, s.date).bind(7, s.createdAt).bind(8, s.studio)
               .bind(9, joinLines(s.urls)).bind(10, s.cover)
               .bind(11, static_cast<int64_t>(s.vr)).run();
      for (size_t i = 0; ok && i < s.files.size(); ++i) {
        const StashData::File& f = s.files[i];
        ok = file.bind(1, s.id).bind(2, static_cast<int64_t>(i)).bind(3, f.path)
                 .bind(4, static_cast<int64_t>(f.size)).bind(5, f.modified)
                 .bind(6, static_cast<int64_t>(f.width)).bind(7, static_cast<int64_t>(f.height))
                 .bind(8, f.duration).bind(9, f.fps).bind(10, f.codec).bind(11, f.oshash)
                 .bind(12, f.id).run();
      }
      for (const int64_t id : s.performers) ok = ok && performer.bind(1, s.id).bind(2, id).run();
      for (const int64_t id : s.tags) ok = ok && tag.bind(1, s.id).bind(2, id).run();
      for (const auto& [id, index] : s.groups)
        ok = ok && group.bind(1, s.id).bind(2, id).bind(3, static_cast<int64_t>(index)).run();
    }
    for (const StashData::Performer& r : data.performers) {
      if (!ok) break;
      ok = p.bind(1, r.id).bind(2, r.name).bind(3, r.disambiguation).bind(4, joinLines(r.aliases))
               .bind(5, r.gender).bind(6, r.birthdate).bind(7, r.country).bind(8, r.ethnicity)
               .bind(9, r.hairColor).bind(10, r.eyeColor).bind(11, static_cast<int64_t>(r.heightCm))
               .bind(12, r.measurements).bind(13, r.fakeTits).bind(14, r.tattoos)
               .bind(15, r.piercings).bind(16, r.careerStart).bind(17, r.careerEnd)
               .bind(18, r.image).bind(19, static_cast<int64_t>(r.favorite)).run();
    }
    for (const StashData::Studio& s : data.studios)
      ok = ok && studio.bind(1, s.id).bind(2, s.name).bind(3, s.parent).bind(4, s.image).run();
    for (const StashData::Tag& s : data.tags) ok = ok && t.bind(1, s.id).bind(2, s.name).run();
    for (const StashData::Group& s : data.groups)
      ok = ok && g.bind(1, s.id).bind(2, s.name).bind(3, s.date).bind(4, s.studio).bind(5, s.image).run();
  }
  ok = ok && exec(db, "COMMIT");
  if (!ok) {
    const std::string why = std::string("The local copy could not be written (") + sqlite3_errmsg(db) + ")";
    if (!sqlite3_get_autocommit(db)) exec(db, "ROLLBACK");
    error_ = why;
    sqlite3_close(db);
    return false;
  }
  sqlite3_close(db);
  return true;
}

bool StashStore::scenes(const std::string& server, const std::string& kind,
                        std::unordered_map<std::string, int64_t>& byOshash,
                        std::unordered_map<int64_t, std::string>& names) {
  error_.clear();
  byOshash.clear();
  names.clear();
  std::error_code ec;
  sqlite3* db = nullptr;
  if (!fs::exists(path_, ec) ||
      sqlite3_open_v2(path_.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    error_ = "no copy of Stash's scenes yet";
    return false;
  }
  sqlite3_busy_timeout(db, 1000);
  bool ok = readable(userVersion(db)) && exec(db, "BEGIN");
  if (ok) {
    Statement meta(db, "SELECT value FROM meta WHERE key = 'server'");
    ok = meta.ok() && meta.next() && meta.text(0) == server;
    // Another kind's at the same address has other scenes under its ids. A
    // copy from before kinds were kept is the first kind's (StashData::kind).
    Statement of(db, "SELECT value FROM meta WHERE key = 'kind'");
    ok = ok && of.ok() && (of.next() ? of.text(0) : StashData().kind) == kind;
    if (!ok) error_ = "the copy of Stash's scenes is another server's";
  } else {
    error_ = "no copy of Stash's scenes yet";
  }
  if (ok) {
    Statement files(db, "SELECT scene_id, oshash, path FROM files ORDER BY scene_id, position");
    Statement titles(db, "SELECT id, title FROM scenes");
    ok = files.ok() && titles.ok();
    while (ok && files.next()) {
      const int64_t scene = files.integer(0);
      if (const std::string oshash = files.text(1); !oshash.empty()) byOshash.emplace(oshash, scene);
      if (!names.count(scene)) names[scene] = fs::path(files.text(2)).filename().string();
    }
    while (ok && titles.next())
      if (const std::string title = titles.text(1); !title.empty()) names[titles.integer(0)] = title;
    if (!ok) error_ = "The local copy could not be read";
  }
  exec(db, "COMMIT");
  sqlite3_close(db);
  return ok;
}

namespace {

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
