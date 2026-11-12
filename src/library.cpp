#include "library.h"
#include "remote.h"

#include "file_util.h"
#include "library_schema.h"  // kLibrarySchema, made from library_schema.sql at build time
#include "oshash.h"
#include "settings.h"  // dataDir

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <iterator>
#include <map>
#include <mutex>
#include <system_error>
#include <thread>

#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <sys/stat.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// The schema version this build reads and writes.
constexpr int kSchemaVersion = 1;
// For later versions: the SQL that takes a library from version n - 1 to n.
const std::map<int, const char*> kMigrations = {};

constexpr int64_t kDay = 24 * 60 * 60;

int64_t now() { return static_cast<int64_t>(std::time(nullptr)); }

bool isStream(const std::string& location) {
  return location.find("://") != std::string::npos && !remote::isRemote(location);
}

bool endsWith(const std::string& s, const char* tail) {
  const size_t n = std::char_traits<char>::length(tail);
  return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

// The form a path is kept in: absolute and without "." or ".." steps. Paths
// from the file browser already are, and are taken as they are.
const fs::path workingDirectory = [] {
  std::error_code ec;
  return fs::current_path(ec);
}();

std::string normalPath(const std::string& path) {
  if (path.empty() || remote::isRemote(path)) return path;
  if (path[0] == '/' && path.find("//") == std::string::npos &&
      path.find("/./") == std::string::npos && path.find("/../") == std::string::npos &&
      (path.size() == 1 || path.back() != '/') && !endsWith(path, "/.") && !endsWith(path, "/.."))
    return path;
  if (fs::path(path).is_relative() && workingDirectory.empty()) return path;
  const fs::path absolute = workingDirectory / path;
  std::string normal = absolute.lexically_normal().string();
  if (normal.size() > 1 && normal.back() == '/') normal.pop_back();
  return normal;
}

std::string keyOf(const std::string& location) {
  return isStream(location) ? location : normalPath(location);
}

json colourJson(const KeyColour& c) { return json::array({c.r, c.g, c.b}); }

// A colour stored by colourJson(): false (and `out` as it was) if there is none.
bool colourField(const json& object, const char* key, KeyColour& out) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_array() || it->size() != 3) return false;
  uint8_t c[3];
  for (int i = 0; i < 3; ++i) {
    const json& v = (*it)[i];
    if (!v.is_number_integer() || v.get<int>() < 0 || v.get<int>() > 255) return false;
    c[i] = static_cast<uint8_t>(v.get<int>());
  }
  out = {c[0], c[1], c[2]};
  return true;
}

// Per-video settings as JSON, with only what differs from the defaults (so
// later defaults apply to the rest). "{}" means the defaults were chosen,
// unlike no settings at all, after which videos.conf may still have some.
// The enums are stored by number: new values go at the end (video_format.h).
std::string settingsJson(const VideoSettings& s) {
  json j = json::object();
  if (s.hasFormat) {
    json format = {{"projection", static_cast<int>(s.format.projection)},
                   {"stereo", static_cast<int>(s.format.stereo)}};
    if (s.format.packing != Packing::Auto) format["packing"] = static_cast<int>(s.format.packing);
    j["format"] = format;
  }
  if (s.brightness) j["brightness"] = s.brightness;
  if (s.contrast) j["contrast"] = s.contrast;
  if (s.saturation) j["saturation"] = s.saturation;
  if (s.balance.warmth) j["warmth"] = s.balance.warmth;
  if (s.balance.tint) j["tint"] = s.balance.tint;
  if (s.balance.exposure) j["exposure"] = s.balance.exposure;
  if (s.zoom != 1.0f) j["zoom"] = std::round(s.zoom * 1000.0) / 1000.0;
  if (s.originForward != 0) j["origin_forward"] = s.originForward;
  // Kept off too once chosen by hand, so that it is not switched on by itself.
  if (s.passthroughChosen) j["passthrough"] = s.passthrough;
  if (s.greenCheckVersion > 0)
    j["green_check"] = {{"version", s.greenCheckVersion}, {"found", s.greenScreenFound}};
  if (s.fillCheckVersion > 0) {
    j["fill_check"] = {{"version", s.fillCheckVersion}, {"found", s.fillFound}};
    if (s.fillFound) j["fill_check"]["colour"] = colourJson(s.fillColour);
  }
  if (s.packedMask.has_value()) j["packed_mask"] = *s.packedMask;
  if (s.maskLayersVersion > 0 || s.maskLayersFound)
    j["mask_layers"] = {{"version", s.maskLayersVersion}, {"found", s.maskLayersFound}};
  if (s.secondLayerHidden) j["second_layer_hidden"] = true;
  if (s.opacity != 100) j["opacity"] = s.opacity;
  if (s.legacyTvGamma.has_value()) j["tv_gamma"] = *s.legacyTvGamma;
  if (s.volume >= 0) j["volume"] = s.volume;
  if (s.greenScreen != GreenScreen()) {
    const GreenScreen& g = s.greenScreen;
    const KeySliders& k = g.green;
    json key = {{"on", g.on},         {"strength", k.strength}, {"softness", k.softness},
                {"spill", k.spill},   {"shrink", k.shrink}};
    if (g.source != KeySource::Automatic) key["background"] = static_cast<int>(g.source);
    if (g.source == KeySource::Picked) key["picked"] = colourJson(g.picked);
    // The colour key's sliders where they differ from its defaults.
    const KeySliders& c = g.colour;
    const KeySliders d = GreenScreen().colour;
    json colour = json::object();
    if (c.strength != d.strength) colour["strength"] = c.strength;
    if (c.softness != d.softness) colour["softness"] = c.softness;
    if (c.spill != d.spill) colour["spill"] = c.spill;
    if (c.shrink != d.shrink) colour["shrink"] = c.shrink;
    if (!colour.empty()) key["colour_key"] = colour;
    j["green_screen"] = key;
  }
  if (s.hasFocus) {
    const FocusSettings& f = s.focus;
    j["focus"] = {{"mode", static_cast<int>(f.mode)}, {"distance_cm", f.distanceCm},
                  {"baseline_mm", f.baselineMm},      {"min_cm", f.minCm},
                  {"max_cm", f.maxCm},                {"speed", f.speed},
                  {"sensitivity", f.sensitivity},     {"accuracy", f.accuracy},
                  {"pitch_deg", f.pitchDeg},          {"yaw_deg", f.yawDeg}};
  }
  if (s.hasScreen) {
    auto mm = [](float v) { return std::round(v * 1000.0) / 1000.0; };
    j["screen"] = {{"distance_m", mm(s.screen.distance)}, {"height_m", mm(s.screen.height)},
                   {"curve", mm(s.screen.curve)}};
  }
  return j.dump();
}

template <typename T>
T field(const json& object, const char* key, T fallback) {
  const auto it = object.find(key);
  return it != object.end() && it->is_number() ? it->get<T>() : fallback;
}

bool parseSettings(const std::string& text, VideoSettings& s) {
  const json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return false;
  s = VideoSettings{};
  if (const auto format = j.find("format"); format != j.end() && format->is_object()) {
    const int projection = field(*format, "projection", 0), stereo = field(*format, "stereo", 0),
              packing = field(*format, "packing", 0);
    if (projection >= 0 && projection < static_cast<int>(Projection::kCount) && stereo >= 0 &&
        stereo < static_cast<int>(StereoLayout::kCount) && packing >= 0 &&
        packing < static_cast<int>(Packing::kCount)) {
      s.hasFormat = true;
      s.format.projection = static_cast<Projection>(projection);
      s.format.stereo = static_cast<StereoLayout>(stereo);
      s.format.packing = static_cast<Packing>(packing);
    }
  }
  s.brightness = field(j, "brightness", 0);
  s.contrast = field(j, "contrast", 0);
  s.saturation = field(j, "saturation", 0);
  s.balance.warmth = std::clamp(field(j, "warmth", 0), -100, 100);
  s.balance.tint = std::clamp(field(j, "tint", 0), -100, 100);
  s.balance.exposure = std::clamp(field(j, "exposure", 0), -100, 100);
  s.zoom = field(j, "zoom", 1.0f);
  s.originForward = std::clamp(field(j, "origin_forward", 0.0f), -VideoSettings::kMaxOriginForward,
                               VideoSettings::kMaxOriginForward);
  if (const auto p = j.find("passthrough"); p != j.end() && p->is_boolean()) {
    s.passthrough = p->get<bool>();
    s.passthroughChosen = true;  // (older versions stored it only when on)
  }
  if (const auto g = j.find("green_check"); g != j.end() && g->is_object()) {
    s.greenCheckVersion = field(*g, "version", 0);
    const auto found = g->find("found");
    s.greenScreenFound = found != g->end() && found->is_boolean() && found->get<bool>();
  }
  if (const auto fill = j.find("fill_check"); fill != j.end() && fill->is_object()) {
    s.fillCheckVersion = field(*fill, "version", 0);
    const auto found = fill->find("found");
    s.fillFound = found != fill->end() && found->is_boolean() && found->get<bool>() &&
                  colourField(*fill, "colour", s.fillColour);
  }
  if (const auto mask = j.find("packed_mask"); mask != j.end() && mask->is_boolean())
    s.packedMask = mask->get<bool>();
  if (const auto layers = j.find("mask_layers"); layers != j.end() && layers->is_object()) {
    s.maskLayersVersion = field(*layers, "version", 0);
    const auto found = layers->find("found");
    s.maskLayersFound = found != layers->end() && found->is_boolean() && found->get<bool>();
  }
  if (const auto hidden = j.find("second_layer_hidden"); hidden != j.end() && hidden->is_boolean())
    s.secondLayerHidden = hidden->get<bool>();
  s.opacity = std::clamp(field(j, "opacity", 100), 0, 100);
  if (const auto tv = j.find("tv_gamma"); tv != j.end() && tv->is_boolean())
    s.legacyTvGamma = tv->get<bool>();
  s.volume = field(j, "volume", -1);
  if (const auto key = j.find("green_screen"); key != j.end() && key->is_object()) {
    GreenScreen& g = s.greenScreen;
    const auto on = key->find("on");
    g.on = on != key->end() && on->is_boolean() && on->get<bool>();
    const int source = field(*key, "background", 0);
    if (source > 0 && source < static_cast<int>(KeySource::kCount))
      g.source = static_cast<KeySource>(source);
    if (g.source == KeySource::Picked && !colourField(*key, "picked", g.picked))
      g.source = KeySource::Automatic;
    auto sliders = [](const json& object, KeySliders& k) {
      k.strength = field(object, "strength", k.strength);
      k.softness = field(object, "softness", k.softness);
      k.spill = field(object, "spill", k.spill);
      k.shrink = field(object, "shrink", k.shrink);
    };
    sliders(*key, g.green);
    if (const auto c = key->find("colour_key"); c != key->end() && c->is_object())
      sliders(*c, g.colour);
  }
  if (const auto focus = j.find("focus"); focus != j.end() && focus->is_object()) {
    FocusSettings f;
    const int mode = field(*focus, "mode", static_cast<int>(f.mode));
    if (mode >= 0 && mode < static_cast<int>(FocusMode::kCount))
      f.mode = static_cast<FocusMode>(mode);
    f.distanceCm = field(*focus, "distance_cm", f.distanceCm);
    f.baselineMm = field(*focus, "baseline_mm", f.baselineMm);
    f.minCm = field(*focus, "min_cm", f.minCm);
    f.maxCm = field(*focus, "max_cm", f.maxCm);
    f.speed = field(*focus, "speed", f.speed);
    f.sensitivity = field(*focus, "sensitivity", f.sensitivity);
    f.accuracy = field(*focus, "accuracy", f.accuracy);
    f.pitchDeg = field(*focus, "pitch_deg", f.pitchDeg);
    f.yawDeg = field(*focus, "yaw_deg", f.yawDeg);
    // Through the videos.conf form, which keeps each value in its range.
    s.hasFocus = parseFocusToken(focusToken(f), s.focus);
  }
  if (const auto screen = j.find("screen"); screen != j.end() && screen->is_object()) {
    ScreenSize size;
    size.distance = field(*screen, "distance_m", size.distance);
    size.height = field(*screen, "height_m", size.height);
    size.curve = field(*screen, "curve", size.curve);
    s.hasScreen = true;
    s.screen = size.clamped();
  }
  return true;
}

// Runs SQL without results; false (and logged) on an error.
bool exec(sqlite3* db, const char* sql) {
  char* error = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &error) == SQLITE_OK) return true;
  std::fprintf(stderr, "Library: %s\n", error ? error : sqlite3_errmsg(db));
  sqlite3_free(error);
  return false;
}

}  // namespace

double resumePoint(double position, double duration) {
  if (!(position >= 10)) return -1;
  if (duration > 0 && (position > duration - 30 || position > duration * 0.98)) return -1;
  return position;
}

void Library::Change::merge(const Change& later) {
  if (later.rating) {
    rating = true;
    ratingValue = later.ratingValue;
  }
  plays += later.plays;
  playedAt = std::max(playedAt, later.playedAt);
  if (later.resume) {
    resume = true;
    resumeSeconds = later.resumeSeconds;
  }
  if (later.duration) {
    duration = true;
    durationSeconds = later.durationSeconds;
  }
  settingsValue.take(later.settingsValue, later.settingsParts);
  settingsParts |= later.settingsParts;
  if (later.carryOver && !carryOver) {
    carryOver = true;
    carriedValue = later.carriedValue;
    carriedRecord = later.carriedRecord;
  }
}

VideoSettings Library::Change::settingsOver(const VideoSettings* own, int64_t record) const {
  const bool carry = carryOver && (!carriedRecord || carriedRecord == record);
  VideoSettings s = own ? *own : carry ? carriedValue : VideoSettings{};
  s.take(settingsValue, settingsParts);
  return s;
}

namespace {

// What a change does to the record's own fields (not the settings).
void overlay(VideoRecord& r, const Library::Change& c) {
  if (c.rating) r.rating = c.ratingValue;
  if (c.plays) {
    r.playCount += c.plays;
    r.lastPlayedAt = std::max(r.lastPlayedAt, c.playedAt);
  }
  if (c.resume) r.resumeSeconds = c.resumeSeconds;
  if (c.duration) r.durationSeconds = c.durationSeconds;
}

}  // namespace

// The thread that owns the database connection and writes the changes,
// queued in order, a batch per transaction.
struct Library::Writer {
  struct VideoRow {
    int64_t id = 0;
    std::string oshash, streamKey;
    VideoRecord record;
    int64_t addedAt = 0;
    bool hasSettings = false;
    VideoSettings settings;  // made JSON on the writer thread
  };
  struct FileRow {
    std::string path;
    File file;
  };
  struct Op {
    enum Kind { PutVideo, PutFile, DropFile } kind;
    VideoRow video;
    FileRow file;
  };

  sqlite3* db = nullptr;
  sqlite3_stmt *putVideo = nullptr, *putFile = nullptr, *dropFile = nullptr;
  std::mutex mutex;
  std::condition_variable wake;
  std::vector<Op> queue;
  bool stop = false;
  std::thread thread;

  bool prepare() {
    return sqlite3_prepare_v2(
               db,
               "INSERT INTO videos (id, oshash, stream_key, rating, play_count, last_played_at, "
               "added_at, resume_seconds, duration_seconds, settings) "
               "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10) ON CONFLICT(id) DO UPDATE SET "
               "oshash = ?2, stream_key = ?3, rating = ?4, play_count = ?5, last_played_at = ?6, "
               "added_at = ?7, resume_seconds = ?8, duration_seconds = ?9, settings = ?10",
               -1, &putVideo, nullptr) == SQLITE_OK &&
           sqlite3_prepare_v2(
               db,
               "INSERT INTO files (path, folder, video_id, size, mtime, seen_at) "
               "VALUES (?1, ?2, ?3, ?4, ?5, ?6) ON CONFLICT(path) DO UPDATE SET "
               "folder = ?2, video_id = ?3, size = ?4, mtime = ?5, seen_at = ?6",
               -1, &putFile, nullptr) == SQLITE_OK &&
           sqlite3_prepare_v2(db, "DELETE FROM files WHERE path = ?1", -1, &dropFile, nullptr) ==
               SQLITE_OK;
  }

  void push(Op op) {
    std::lock_guard<std::mutex> lock(mutex);
    queue.push_back(std::move(op));
    wake.notify_one();
  }

  // A batch that could not be saved (the disk full, say) goes back in front
  // of what came meanwhile and is tried again, after 1, 2, 4 ... 60 s. On
  // quitting it gets two more tries.
  void run() {
    std::unique_lock<std::mutex> lock(mutex);
    int failures = 0, triesAfterStop = 0;
    for (;;) {
      wake.wait(lock, [this] { return stop || !queue.empty(); });
      if (queue.empty()) return;  // stopped, all written
      std::vector<Op> batch;
      batch.swap(queue);
      lock.unlock();
      const bool saved = writeBatch(batch);
      lock.lock();
      if (saved) {
        if (failures) std::fprintf(stderr, "Library: changes saved again\n");
        failures = 0;
        continue;
      }
      queue.insert(queue.begin(), std::make_move_iterator(batch.begin()),
                   std::make_move_iterator(batch.end()));
      ++failures;
      if (stop && ++triesAfterStop >= 2) {
        std::fprintf(stderr, "Library: %zu changes could not be saved\n", queue.size());
        return;
      }
      const int seconds = std::min(60, 1 << std::min(failures - 1, 6));
      if (!stop) {
        std::fprintf(stderr, "Library: changes not saved; trying again in %d s\n", seconds);
        wake.wait_for(lock, std::chrono::seconds(seconds), [this] { return stop; });
      }
    }
  }

  // One transaction; false (and rolled back) if it could not be saved.
  bool writeBatch(const std::vector<Op>& batch) {
    bool saved = exec(db, "BEGIN");
    for (size_t i = 0; saved && i < batch.size(); ++i) saved = write(batch[i]);
    saved = saved && exec(db, "COMMIT");
    if (!saved && !sqlite3_get_autocommit(db)) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    return saved;
  }

  // False if the change could not be saved now (it is tried again); one the
  // database refuses for good (it breaks a constraint) is logged and dropped.
  bool write(const Op& op) {
    sqlite3_stmt* st =
        op.kind == Op::PutVideo ? putVideo : op.kind == Op::PutFile ? putFile : dropFile;
    std::string settings, folder;
    auto text = [st](int i, const std::string& s) {
      if (s.empty()) sqlite3_bind_null(st, i);
      else sqlite3_bind_text(st, i, s.c_str(), static_cast<int>(s.size()), SQLITE_STATIC);
    };
    auto real = [st](int i, double v, bool null) {
      if (null) sqlite3_bind_null(st, i);
      else sqlite3_bind_double(st, i, v);
    };
    auto integer = [st](int i, int64_t v, bool null) {
      if (null) sqlite3_bind_null(st, i);
      else sqlite3_bind_int64(st, i, v);
    };
    if (op.kind == Op::PutVideo) {
      const VideoRow& v = op.video;
      const VideoRecord& r = v.record;
      if (v.hasSettings) settings = settingsJson(v.settings);
      integer(1, v.id, false);
      text(2, v.oshash);
      text(3, v.streamKey);
      real(4, r.rating, r.rating <= 0);
      integer(5, r.playCount, false);
      integer(6, r.lastPlayedAt, r.lastPlayedAt == 0);
      integer(7, v.addedAt, v.addedAt == 0);
      real(8, r.resumeSeconds, r.resumeSeconds < 0);
      real(9, r.durationSeconds, r.durationSeconds < 0);
      text(10, settings);
    } else {
      text(1, op.file.path);
      if (op.kind == Op::PutFile) {
        const File& f = op.file.file;
        folder = remote::isRemote(op.file.path) ? remote::parent(op.file.path)
                                        : fs::path(op.file.path).parent_path().string();
        text(2, folder);
        integer(3, f.videoId, false);
        integer(4, static_cast<int64_t>(f.size), false);
        integer(5, f.mtime, false);
        integer(6, f.seenAt, false);
      }
    }
    const int result = sqlite3_step(st);
    bool done = result == SQLITE_DONE;
    if (!done) {
      std::fprintf(stderr, "Library: a change could not be saved (%s)\n", sqlite3_errmsg(db));
      const int kind = result & 0xff;
      done = kind == SQLITE_CONSTRAINT || kind == SQLITE_MISMATCH || kind == SQLITE_TOOBIG ||
             kind == SQLITE_RANGE;
    }
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    return done;
  }

  ~Writer() {
    if (thread.joinable()) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        stop = true;
      }
      wake.notify_one();
      thread.join();
    }
    sqlite3_finalize(putVideo);
    sqlite3_finalize(putFile);
    sqlite3_finalize(dropFile);
    sqlite3_close(db);
  }
};

// The thread that reads files: stats and fingerprints. Detached, so that
// quitting never waits for a share that stopped answering; it only touches
// this, which it shares.
struct Library::Checker {
  struct Job {
    enum Kind { Resolve, Verify, Move, Prune } kind;
    std::string path;
    uint64_t size = 0;  // Verify: as recorded; Resolve: as listed with `oshash`
    int64_t mtime = 0;
    // Move: recorded files of the same size; Prune: the video's other files.
    std::vector<std::string> candidates;
    // Verify, Move: asked by lookup() for a file listed with this size and date.
    bool listed = false;
    uint64_t listedSize = 0;
    int64_t listedMtime = 0;
    // Resolve: the fingerprint known without reading the file, if it still
    // has the size and date it was listed with.
    std::string oshash;
  };
  struct Result {
    Job job;
    FileFingerprint print;  // no oshash: not read, or unreadable
    std::vector<std::string> gone;  // Move, Prune: candidates no longer there
    // Verify, Move: the file was fingerprinted, or found as recorded after all.
    bool decided = false;
  };

  std::mutex mutex;
  std::condition_variable wake;
  std::deque<Job> urgent, background;  // urgent: files being played or rated
  std::vector<Result> results;
  bool stop = false;

  void push(Job job, bool now) {
    std::lock_guard<std::mutex> lock(mutex);
    (now ? urgent : background).push_back(std::move(job));
    wake.notify_one();
  }

  static void run(std::shared_ptr<Checker> self) {
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(self->mutex);
        self->wake.wait(lock, [&] {
          return self->stop || !self->urgent.empty() || !self->background.empty();
        });
        if (self->stop) return;
        std::deque<Job>& from = self->urgent.empty() ? self->background : self->urgent;
        job = std::move(from.front());
        from.pop_front();
      }
      Result result;
      auto metadata = [](const std::string& path, remote::Entry& entry) {
        if (remote::isRemote(path)) return remote::stat(path, entry);
        struct stat st;
        if (::stat(path.c_str(), &st) == 0) {
          entry.size = st.st_size;
          entry.modified = st.st_mtime;
          return remote::Status::Ok;
        }
        return errno == ENOENT || errno == ENOTDIR ? remote::Status::Missing : remote::Status::Offline;
      };
      remote::Entry st;
      switch (job.kind) {
        case Job::Resolve:
          if (!job.oshash.empty() && metadata(job.path, st) == remote::Status::Ok &&
              st.size == job.size && st.modified == job.mtime)
            result.print = {job.oshash, job.size, job.mtime};
          else
            result.print = fingerprintFile(job.path);
          break;
        case Job::Verify:
          if (metadata(job.path, st) != remote::Status::Ok) break;
          if (st.size != job.size || st.modified != job.mtime)
            result.print = fingerprintFile(job.path);
          else
            result.decided = true;  // as recorded after all
          break;
        case Job::Move:
        case Job::Prune:
          for (const std::string& path : job.candidates)
            if (metadata(path, st) == remote::Status::Missing)
              result.gone.push_back(path);
          // A moved file, or a copy of a recorded one: the size alone can't
          // tell, the fingerprint does.
          if (job.kind == Job::Move) result.print = fingerprintFile(job.path);
          break;
      }
      if (!result.print.oshash.empty()) result.decided = true;
      result.job = std::move(job);
      std::lock_guard<std::mutex> lock(self->mutex);
      self->results.push_back(std::move(result));
    }
  }
};

Library::Library() = default;

Library::~Library() {
  if (checker_) {
    // Changes to files new to the library wait for their fingerprints.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!resolving_.empty() && std::chrono::steady_clock::now() < until) {
      poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    poll();
    const size_t lost = std::count_if(pending_.begin(), pending_.end(),
                                      [](const auto& p) { return !p.second.empty(); });
    if (lost)
      std::fprintf(stderr, "Library: changes to %zu new files not saved (not readable)\n", lost);
    {
      std::lock_guard<std::mutex> lock(checker_->mutex);
      checker_->stop = true;
    }
    checker_->wake.notify_one();
  }
  writer_.reset();  // writes what is queued
}

bool Library::load() {
  const auto start = std::chrono::steady_clock::now();
  const fs::path dir = dataDir();
  std::error_code ec;
  fs::create_directories(dir, ec);
  sqlite3* db = nullptr;
  auto fail = [&db](const std::string& why) {
    std::fprintf(stderr, "Library: %s; running without it\n", why.c_str());
    sqlite3_close(db);
    return false;
  };
  if (sqlite3_open_v2((dir / "library.db").c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                      nullptr) != SQLITE_OK)
    return fail("library.db could not be opened");
  // Held for the whole session: a second instance (or the importer) would
  // work from its own copy and overwrite this one's changes.
  sqlite3_busy_timeout(db, 200);
  if (!exec(db, "PRAGMA locking_mode = EXCLUSIVE") || !exec(db, "BEGIN EXCLUSIVE") ||
      !exec(db, "COMMIT"))
    return fail(sqlite3_errcode(db) == SQLITE_BUSY
                    ? "library.db is in use (by another VideoFrameVR or the importer)"
                    : "library.db is not readable");
  if (!exec(db, "PRAGMA journal_mode = WAL") || !exec(db, "PRAGMA synchronous = NORMAL") ||
      !exec(db, "PRAGMA foreign_keys = ON"))
    return fail("library.db could not be set up");

  auto scalar = [db](const char* sql, std::string& value) {
    sqlite3_stmt* st = nullptr;
    bool found = false;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
      const unsigned char* text = sqlite3_column_text(st, 0);
      value = text ? reinterpret_cast<const char*>(text) : "";
      found = true;
    }
    sqlite3_finalize(st);
    return found;
  };
  std::string value;
  if (!scalar("SELECT count(*) FROM sqlite_master", value))
    return fail("library.db is not readable");
  if (value == "0") {
    if (!exec(db, "BEGIN") || !exec(db, kLibrarySchema) || !exec(db, "COMMIT"))
      return fail("library.db could not be created");
    std::printf("Library: new\n");
  }
  if (!scalar("SELECT value FROM meta WHERE key = 'schema_version'", value))
    return fail("library.db is not a VideoFrameVR library");
  int version = std::atoi(value.c_str());
  if (version > kSchemaVersion)
    return fail("library.db is from a newer VideoFrameVR (version " + std::to_string(version) +
                ", this one knows " + std::to_string(kSchemaVersion) + ")");
  if (version < 1) return fail("library.db has no valid version");
  if (version < kSchemaVersion) {
    const fs::path backup = dir / ("library.db.bak-v" + std::to_string(version));
    FILE* file = std::fopen(backup.c_str(), "wx");
    if (!file && (errno != EEXIST || !fs::is_regular_file(backup, ec)))
      return fail("library.db backup failed; not migrating");
    if (file) {
      std::fclose(file);
      char* sql = sqlite3_mprintf("VACUUM INTO %Q", backup.c_str());
      const bool saved = sql && exec(db, sql);
      sqlite3_free(sql);
      if (!saved) {
        fs::remove(backup, ec);
        return fail("library.db backup failed; not migrating");
      }
    }
  }
  for (; version < kSchemaVersion; ++version) {
    const auto migration = kMigrations.find(version + 1);
    const std::string bump = "UPDATE meta SET value = '" + std::to_string(version + 1) +
                             "' WHERE key = 'schema_version'";
    if (migration == kMigrations.end() || !exec(db, "BEGIN") || !exec(db, migration->second) ||
        !exec(db, bump.c_str()) || !exec(db, "COMMIT"))
      return fail("library.db could not be brought up to date");
    std::printf("Library: updated to version %d\n", version + 1);
  }

  sqlite3_stmt* st = nullptr;
  int badSettings = 0;
  if (sqlite3_prepare_v2(db,
                         "SELECT id, oshash, stream_key, rating, play_count, last_played_at, "
                         "added_at, resume_seconds, duration_seconds, settings FROM videos",
                         -1, &st, nullptr) != SQLITE_OK)
    return fail("library.db is not readable");
  auto textAt = [&st](int i) {
    const unsigned char* text = sqlite3_column_text(st, i);
    return text ? std::string(reinterpret_cast<const char*>(text)) : std::string();
  };
  auto isNull = [&st](int i) { return sqlite3_column_type(st, i) == SQLITE_NULL; };
  while (sqlite3_step(st) == SQLITE_ROW) {
    Video v;
    VideoRecord& r = v.record;
    r.id = sqlite3_column_int64(st, 0);
    v.oshash = textAt(1);
    v.streamKey = textAt(2);
    r.rating = isNull(3) ? 0.0f : static_cast<float>(sqlite3_column_double(st, 3));
    r.playCount = sqlite3_column_int(st, 4);
    r.lastPlayedAt = sqlite3_column_int64(st, 5);
    v.addedAt = sqlite3_column_int64(st, 6);
    r.resumeSeconds = isNull(7) ? -1 : sqlite3_column_double(st, 7);
    r.durationSeconds = isNull(8) ? -1 : sqlite3_column_double(st, 8);
    if (!isNull(9)) {
      v.hasSettings = parseSettings(textAt(9), v.settings);
      badSettings += !v.hasSettings;
    }
    lastId_ = std::max(lastId_, r.id);
    if (!v.oshash.empty()) byHash_[v.oshash] = r.id;
    else if (!v.streamKey.empty()) byStream_[v.streamKey] = r.id;
    videos_[r.id] = std::move(v);
  }
  sqlite3_finalize(st);
  if (sqlite3_prepare_v2(db, "SELECT path, video_id, size, mtime, seen_at FROM files", -1, &st,
                         nullptr) != SQLITE_OK)
    return fail("library.db is not readable");
  while (sqlite3_step(st) == SQLITE_ROW) {
    File f;
    f.videoId = sqlite3_column_int64(st, 1);
    f.size = static_cast<uint64_t>(sqlite3_column_int64(st, 2));
    f.mtime = sqlite3_column_int64(st, 3);
    f.seenAt = sqlite3_column_int64(st, 4);
    if (videos_.count(f.videoId)) indexFile(normalPath(textAt(0)), f);
  }
  sqlite3_finalize(st);
  if (badSettings) std::fprintf(stderr, "Library: settings of %d videos unreadable\n", badSettings);

  writer_ = std::make_unique<Writer>();
  writer_->db = db;
  if (!writer_->prepare()) {
    std::fprintf(stderr, "Library: %s; running without it\n", sqlite3_errmsg(db));
    writer_.reset();  // closes the database
    videos_.clear();
    byHash_.clear();
    byStream_.clear();
    files_.clear();
    bySize_.clear();
    return false;
  }
  writer_->thread = std::thread([w = writer_.get()] { w->run(); });
  checker_ = std::make_shared<Checker>();
  std::thread(Checker::run, checker_).detach();
  available_ = true;
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  std::printf("Library: %zu videos, %zu files (%.0f ms)\n", videos_.size(), files_.size(), ms);
  return true;
}

bool Library::poll() {
  if (!checker_) return false;
  std::vector<Checker::Result> results;
  {
    std::lock_guard<std::mutex> lock(checker_->mutex);
    if (checker_->results.empty()) return false;
    results.swap(checker_->results);
  }
  const uint64_t before = generation_;
  for (const Checker::Result& r : results) {
    const Checker::Job& job = r.job;
    const FileFingerprint& p = r.print;
    if (job.listed) {
      // Listing the folder again checks it again, unless this settled it.
      checking_.erase(job.path);
      if (r.decided) checked_[job.path] = {job.listedSize, job.listedMtime};
    }
    switch (job.kind) {
      case Checker::Job::Resolve:
        handleResolved(job.path, p.oshash, p.size, p.mtime, !p.oshash.empty());
        break;
      case Checker::Job::Verify:
        if (!p.oshash.empty()) handleVerified(job.path, p.oshash, p.size, p.mtime);
        break;
      case Checker::Job::Move:
        if (!p.oshash.empty()) handleMoved(job.path, p.oshash, p.size, p.mtime, r.gone);
        break;
      case Checker::Job::Prune: dropGoneFiles(job.path, r.gone); break;
    }
  }
  return generation_ != before;
}

// Files the library did not know (opened, rated or played) get their record
// here, or in handleMoved(); its changes waiting meanwhile apply then, in
// the order they were made, before any that follow.
void Library::link(const std::string& path, const File& file) {
  unindexFile(path);
  indexFile(path, file);
  writeFile(path);
  if (const auto waiting = pending_.find(path); waiting != pending_.end()) {
    const Change c = waiting->second;
    pending_.erase(waiting);
    apply(file.videoId, c);
  }
}

// A file opened, rated or played that the library did not know: its record
// is the one with the same content (a copy, or a moved file), else a new one.
void Library::handleResolved(const std::string& path, const std::string& oshash, uint64_t size,
                             int64_t mtime, bool ok) {
  resolving_.erase(path);
  // Found by listing its folder meanwhile: its changes went with it.
  if (!pending_.count(path)) return;
  if (!ok) {
    // Its changes stay waiting: the next one tries again.
    std::fprintf(stderr, "Library: a file could not be read for its fingerprint\n");
    return;
  }
  int64_t id;
  if (const auto same = byHash_.find(oshash); same != byHash_.end()) {
    id = same->second;
    // A moved file opened before its folder was listed: the paths it had
    // before are dropped if they are gone, in the background.
    Checker::Job job{Checker::Job::Prune, path, 0, 0, {}};
    for (const auto& [other, file] : files_)
      if (file.videoId == id) job.candidates.push_back(other);
    checker_->push(std::move(job), false);
  } else {
    id = addVideo(oshash, "");
  }
  link(path, {id, size, mtime, now()});
  ++generation_;
}

// A changed file belongs to the record for its new content. The old record
// and its other paths keep their history and settings.
void Library::handleVerified(const std::string& path, const std::string& oshash, uint64_t size,
                             int64_t mtime) {
  if (pending_.count(path)) return;  // an open has its own fingerprint on the way
  const auto it = files_.find(path);
  if (it == files_.end()) return;
  File f = it->second;
  const int64_t id = f.videoId;
  f.size = size;
  f.mtime = mtime;
  f.seenAt = now();
  const auto same = byHash_.find(oshash);
  f.videoId = same != byHash_.end() ? same->second : addVideo(oshash, "");
  link(path, f);
  if (f.videoId != id) ++generation_;
}

// An unknown file the size of a recorded one: if it has the content of a
// recorded video, it is that video, moved (or a copy of it).
void Library::handleMoved(const std::string& path, const std::string& oshash, uint64_t size,
                          int64_t mtime, const std::vector<std::string>& gone) {
  if (files_.count(path)) return;
  const auto same = byHash_.find(oshash);
  if (same == byHash_.end()) return;  // not a recorded video: stays unknown
  link(path, {same->second, size, mtime, now()});
  std::printf("Library: %s\n",
              dropGoneFiles(path, gone) ? "a moved video found again" : "a copy of a video found");
  ++generation_;
}

// Forgets those of `gone` that were files of the same video as `path`.
bool Library::dropGoneFiles(const std::string& path, const std::vector<std::string>& gone) {
  const auto at = files_.find(path);
  if (at == files_.end()) return false;
  const int64_t id = at->second.videoId;
  bool dropped = false;
  for (const std::string& old : gone) {
    const auto it = files_.find(old);
    if (it == files_.end() || it->second.videoId != id) continue;
    unindexFile(old);
    writeDroppedFile(old);
    dropped = true;
  }
  return dropped;
}

std::vector<VideoRecord> Library::lookup(const std::vector<FolderListing::Entry>& entries) {
  std::vector<VideoRecord> records(entries.size());
  if (!available_) return records;
  const int64_t time = now();
  for (size_t i = 0; i < entries.size(); ++i) {
    const FolderListing::Entry& e = entries[i];
    if (e.directory) continue;
    const std::string path = normalPath(e.path);
    const bool detailsKnown = e.size != 0 && e.modified != 0;
    if (const auto waiting = pending_.find(path); waiting != pending_.end()) {
      overlay(records[i], waiting->second);
      if (files_.count(path)) continue;
    }
    const auto it = files_.find(path);
    if (it != files_.end()) {
      File& f = it->second;
      records[i] = videos_.at(f.videoId).record;
      if (!detailsKnown) continue;
      if (e.size != f.size || e.modified != f.mtime) {
        if (!checkDue(path, e)) continue;
        Checker::Job job{Checker::Job::Verify, path, f.size, f.mtime, {}};
        job.listed = true;
        job.listedSize = e.size;
        job.listedMtime = e.modified;
        checker_->push(std::move(job), false);
      } else if (time - f.seenAt > kDay) {
        f.seenAt = time;
        writeFile(path);
      }
      continue;
    }
    // Unknown: only a file the size of a recorded one can be a recorded
    // video, moved or copied here.
    if (!detailsKnown || resolving_.count(path)) continue;
    if (fingerprintSource_ && takeKnownFingerprint(path, e)) {
      if (const auto found = files_.find(path); found != files_.end())
        records[i] = videos_.at(found->second.videoId).record;
      continue;
    }
    const auto sameSize = bySize_.equal_range(e.size);
    if (sameSize.first == sameSize.second || !checkDue(path, e)) continue;
    Checker::Job job{Checker::Job::Move, path, 0, 0, {}};
    for (auto s = sameSize.first; s != sameSize.second; ++s) job.candidates.push_back(s->second);
    job.listed = true;
    job.listedSize = e.size;
    job.listedMtime = e.modified;
    checker_->push(std::move(job), false);
  }
  return records;
}

// An unknown listed file whose fingerprint the source knows: a recorded
// video's file moved or copied here becomes its file at once (its other
// files that are gone are dropped in the background, a stat each); any
// other is no recorded video, so it isn't read to find out. False if the
// source doesn't know it.
bool Library::takeKnownFingerprint(const std::string& path, const FolderListing::Entry& e) {
  const auto checked = checked_.find(path);
  if (checked != checked_.end() && checked->second == std::make_pair(e.size, e.modified)) return true;
  const std::string oshash = fingerprintSource_(path, e.size, e.modified);
  if (oshash.empty()) return false;
  checked_[path] = {e.size, e.modified};
  knownPrints_[path] = {oshash, e.size, e.modified};
  const auto same = byHash_.find(oshash);
  if (same == byHash_.end()) return true;
  Checker::Job job{Checker::Job::Prune, path, 0, 0, {}};
  for (const auto& [other, file] : files_)
    if (file.videoId == same->second) job.candidates.push_back(other);
  link(path, {same->second, e.size, e.modified, now()});
  checker_->push(std::move(job), false);
  std::printf("Library: a moved or copied video found by its known fingerprint\n");
  ++generation_;
  return true;
}

// Whether a listed file is to be checked in the background: not while it
// is, and once a session for each size and date it is listed with (unless
// the check could not tell, e.g. the file was not readable).
bool Library::checkDue(const std::string& path, const FolderListing::Entry& e) {
  if (checking_.count(path)) return false;
  const auto checked = checked_.find(path);
  if (checked != checked_.end() && checked->second == std::make_pair(e.size, e.modified))
    return false;
  checking_.insert(path);
  return true;
}

const Library::Video* Library::videoAt(const std::string& key, bool stream) const {
  if (stream) {
    if (const auto located = located_.find(key); located != located_.end()) {
      const auto it = byHash_.find(located->second);
      return it != byHash_.end() ? &videos_.at(it->second) : nullptr;
    }
    const auto it = byStream_.find(key);
    return it != byStream_.end() ? &videos_.at(it->second) : nullptr;
  }
  if (pending_.count(key)) return nullptr;
  const auto it = files_.find(key);
  return it != files_.end() ? &videos_.at(it->second.videoId) : nullptr;
}

VideoRecord Library::record(const std::string& location) const {
  VideoRecord r;
  if (!available_ || location.empty()) return r;
  const std::string key = keyOf(location);
  if (const Video* v = videoAt(key, isStream(location))) return v->record;
  if (const auto waiting = pending_.find(key); waiting != pending_.end())
    overlay(r, waiting->second);
  return r;
}

bool Library::known(const std::string& location) const {
  return available_ && !location.empty() && videoAt(keyOf(location), isStream(location)) != nullptr;
}

int64_t Library::addedAt(int64_t id) const {
  const auto it = videos_.find(id);
  return available_ && it != videos_.end() ? it->second.addedAt : 0;
}

VideoRecord Library::video(int64_t id) const {
  const auto it = videos_.find(id);
  return available_ && it != videos_.end() ? it->second.record : VideoRecord{};
}

bool Library::settings(const std::string& location, VideoSettings& settings) const {
  if (!available_ || location.empty()) return false;
  const std::string key = keyOf(location);
  if (const Video* v = videoAt(key, isStream(location))) {
    if (!v->hasSettings) return false;
    settings = v->settings;
    return true;
  }
  const auto waiting = pending_.find(key);
  if (waiting == pending_.end() ||
      (!waiting->second.settingsParts &&
       (!waiting->second.carryOver || waiting->second.carriedRecord)))
    return false;
  settings = waiting->second.settingsOver(nullptr);
  return true;
}

std::unordered_map<std::string, std::optional<VideoFormat>> Library::formatsByHash() const {
  std::unordered_map<std::string, std::optional<VideoFormat>> formats;
  if (!available_) return formats;
  for (const auto& [id, v] : videos_)
    if (!v.oshash.empty())
      formats.emplace(v.oshash, v.hasSettings && v.settings.hasFormat
                                    ? std::optional<VideoFormat>(v.settings.format)
                                    : std::nullopt);
  return formats;
}

bool Library::videoByHash(const std::string& oshash, VideoRecord& record,
                          std::optional<VideoFormat>& format) const {
  if (!available_) return false;
  const auto it = byHash_.find(oshash);
  if (it == byHash_.end()) return false;
  const Video& v = videos_.at(it->second);
  record = v.record;
  format = v.hasSettings && v.settings.hasFormat ? std::optional<VideoFormat>(v.settings.format)
                                                  : std::nullopt;
  return true;
}

std::string Library::oshashOf(const std::string& location) const {
  if (!available_ || location.empty()) return {};
  if (const auto located = located_.find(location); located != located_.end())
    return located->second;
  if (isStream(location)) return {};
  const Video* v = videoAt(normalPath(location), false);
  return v ? v->oshash : std::string();
}

void Library::addCopy(const std::string& location, const std::string& oshash, uint64_t size,
                      int64_t mtime) {
  if (!available_ || oshash.empty()) return;
  const std::string path = normalPath(location);
  const auto same = byHash_.find(oshash);
  link(path, {same != byHash_.end() ? same->second : addVideo(oshash, ""), size, mtime, now()});
  checked_[path] = {size, mtime};
  ++generation_;
}

void Library::forgetFile(const std::string& location) {
  if (!available_ || isStream(location)) return;
  const std::string path = normalPath(location);
  if (!files_.count(path)) return;
  unindexFile(path);
  writeDroppedFile(path);
  checked_.erase(path);
  ++generation_;
}

void Library::locate(const std::string& location, const std::string& oshash) {
  if (available_ && !location.empty() && !oshash.empty()) located_[location] = oshash;
}

std::vector<std::pair<std::string, VideoRecord>> Library::ratedOrPlayed() const {
  std::vector<std::pair<std::string, VideoRecord>> list;
  if (!available_) return list;
  for (const auto& [id, v] : videos_)
    if (!v.oshash.empty() && (v.record.rating > 0 || v.record.playCount > 0))
      list.emplace_back(v.oshash, v.record);
  return list;
}

void Library::open(const std::string& location) {
  // A path is only a cache: even a same-size, same-date replacement must
  // resolve before its settings, resume or new plays can use the record.
  if (available_ && !location.empty() && !isStream(location)) {
    const std::string path = normalPath(location);
    if (pending_.try_emplace(path).second) ++generation_;
    knownPrints_.erase(path);
  }
  change(location, Change{});
}

namespace {

Library::Change ratingChange(float rating) {
  Library::Change c;
  c.rating = true;
  c.ratingValue = rating <= 0 ? 0.0f : std::clamp(std::round(rating * 2.0f) / 2.0f, 0.5f, 5.0f);
  return c;
}

}  // namespace

void Library::setRating(const std::string& location, float rating) {
  change(location, ratingChange(rating));
}

void Library::setRating(int64_t id, float rating) {
  if (available_ && videos_.count(id)) apply(id, ratingChange(rating));
}

void Library::recordPlay(const std::string& location, int64_t when) {
  Change c;
  c.plays = 1;
  c.playedAt = when > 0 ? when : now();
  change(location, c);
}

void Library::setResume(const std::string& location, double seconds) {
  Change c;
  c.resume = true;
  c.resumeSeconds = seconds >= 0 ? seconds : -1;
  change(location, c);
}

void Library::setDuration(const std::string& location, double seconds) {
  Change c;
  c.duration = true;
  c.durationSeconds = seconds > 0 ? seconds : -1;
  change(location, c);
}

void Library::setSettings(const std::string& location, const VideoSettings& settings,
                          unsigned parts) {
  Change c;
  c.settingsParts = parts & VideoSettings::kAll;
  c.settingsValue = settings;
  if (c.settingsParts) change(location, c);
}

void Library::carryOverSettings(const std::string& location, const VideoSettings& settings,
                                int64_t record) {
  Change c;
  c.carryOver = true;
  c.carriedValue = settings;
  c.carriedRecord = record;
  change(location, c);
}

// Applies a change to the location's record; one for a file new to the
// library waits for its fingerprint.
void Library::change(const std::string& location, const Change& c) {
  if (!available_ || location.empty()) return;
  if (const auto located = located_.find(location); located != located_.end()) {
    const auto same = byHash_.find(located->second);
    if (same == byHash_.end()) ++generation_;  // known() from now on
    apply(same != byHash_.end() ? same->second : addVideo(located->second, ""), c);
    return;
  }
  if (isStream(location)) {
    const auto it = byStream_.find(location);
    apply(it != byStream_.end() ? it->second : addVideo("", location), c);
    return;
  }
  const std::string path = normalPath(location);
  if (const auto it = files_.find(path); it != files_.end() && !pending_.count(path)) {
    apply(it->second.videoId, c);
    return;
  }
  Change& waiting = pending_[path];
  waiting.merge(c);
  if (!c.empty()) ++generation_;  // record() shows it already
  if (resolving_.insert(path).second) {
    Checker::Job job{Checker::Job::Resolve, path, 0, 0, {}};
    if (const auto known = knownPrints_.find(path); known != knownPrints_.end()) {
      job.oshash = known->second.oshash;
      job.size = known->second.size;
      job.mtime = known->second.mtime;
    }
    checker_->push(std::move(job), true);
  }
}

void Library::apply(int64_t id, const Change& c) {
  if (c.empty()) return;
  Video& v = videos_.at(id);
  overlay(v.record, c);
  const bool carry = c.carryOver && (!c.carriedRecord || c.carriedRecord == id);
  if (c.settingsParts || carry) {
    v.settings = c.settingsOver(v.hasSettings ? &v.settings : nullptr, id);
    v.hasSettings = true;
  }
  // Settings alone don't show in the browser; a format does in Stash's
  // VR and 2D lists.
  if (c.rating || c.plays || c.resume || c.duration || (c.settingsParts & VideoSettings::kFormat) ||
      (carry && c.carriedValue.hasFormat))
    ++generation_;
  writeVideo(id);
  if (ratingPlayObserver_ && !v.oshash.empty() && (c.rating || c.plays > 0))
    ratingPlayObserver_(v.oshash, c.rating, c.ratingValue, c.plays, c.playedAt);
}

int64_t Library::addVideo(const std::string& oshash, const std::string& streamKey) {
  const int64_t id = ++lastId_;
  Video& v = videos_[id];
  v.oshash = oshash;
  v.streamKey = streamKey;
  v.record.id = id;
  v.addedAt = now();
  (oshash.empty() ? byStream_[streamKey] : byHash_[oshash]) = id;
  writeVideo(id);
  return id;
}

void Library::indexFile(const std::string& path, const File& file) {
  files_[path] = file;
  bySize_.emplace(file.size, path);
}

void Library::unindexFile(const std::string& path) {
  const auto it = files_.find(path);
  if (it == files_.end()) return;
  const auto sameSize = bySize_.equal_range(it->second.size);
  for (auto s = sameSize.first; s != sameSize.second; ++s)
    if (s->second == path) {
      bySize_.erase(s);
      break;
    }
  files_.erase(it);
}

void Library::writeVideo(int64_t id) {
  const Video& v = videos_.at(id);
  Writer::Op op{Writer::Op::PutVideo,
                {id, v.oshash, v.streamKey, v.record, v.addedAt, v.hasSettings, v.settings},
                {}};
  writer_->push(std::move(op));
}

void Library::writeFile(const std::string& path) {
  Writer::Op op{Writer::Op::PutFile, {}, {path, files_.at(path)}};
  writer_->push(std::move(op));
}

void Library::writeDroppedFile(const std::string& path) {
  Writer::Op op{Writer::Op::DropFile, {}, {path, {}}};
  writer_->push(std::move(op));
}
