#include "xbvr_client.h"

#include "server_json.h"
#include "strings.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <unordered_map>
#include <unordered_set>

using json = nlohmann::json;
using server_json::array;
using server_json::flag;
using server_json::member;
using server_json::number;
using server_json::text;
using server_json::unixTime;

namespace {

// Its API's description, which names it and its version: asked first, and
// by Connect. Nothing in XBVR changes for it, and it asks nobody else (its
// version check asks GitHub).
constexpr const char* kApi = "/api.json";
constexpr const char* kApiTitle = "XBVR API";
// Scenes with their files, cast, tags and history in them. A read, though
// POSTed.
constexpr const char* kList = "/api/scene/list";
constexpr const char* kSites = "/api/options/sites";
constexpr const char* kStorage = "/api/options/storage";
// The only two changes the client sends (ask() refuses any other POST).
constexpr const char* kRate = "/api/scene/rate/";
constexpr const char* kToggle = "/api/scene/toggle";

// XBVR sends 100 scenes unless asked for more, and has no limit. Each is a
// few kilobytes with its cast in it: a page of this many is a few megabytes
// and well under what its database takes in one query.
constexpr int kPage = 500;

double msSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// XBVR has sites by name, not by number: an id from the name, the same at
// every sync (FNV-1a, positive, never 0).
int64_t nameId(const std::string& name) {
  uint64_t hash = 14695981039346656037ull;
  for (const unsigned char c : name) hash = (hash ^ c) * 1099511628211ull;
  return static_cast<int64_t>(hash >> 2) | 1;
}

// XBVR writes an oshash without its leading zeros.
std::string oshash(std::string hash) {
  if (!hash.empty() && hash.size() < 16) hash.insert(0, 16 - hash.size(), '0');
  return hash;
}

// The day of one of XBVR's times; empty for none (Go's zero time, year 1).
std::string day(const std::string& time) {
  return time.size() >= 10 && time.compare(0, 4, "0001") != 0 ? time.substr(0, 10) : std::string();
}

// An image's address as a scraper kept it, if XBVR's image proxy can fetch
// it: a whole one. Some sites' are paths on the site ("/assets/x.gif").
std::string image(const std::string& url) {
  if (url.compare(0, 2, "//") == 0) return "https:" + url;
  return url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0 ? url : std::string();
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// A list XBVR keeps as text: a JSON array of strings (or of objects with a
// name), else separated by commas.
std::vector<std::string> listed(const std::string& value) {
  std::vector<std::string> list;
  if (value.empty()) return list;
  auto add = [&](std::string item) {
    const size_t start = item.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return;
    item = item.substr(start, item.find_last_not_of(" \t\r\n") - start + 1);
    if (std::find(list.begin(), list.end(), item) == list.end()) list.push_back(std::move(item));
  };
  if (value[0] == '[') {
    const json parsed = json::parse(value, nullptr, false);
    if (parsed.is_array()) {
      for (const json& item : parsed) {
        if (item.is_string()) add(item.get<std::string>());
        else if (item.is_object()) {
          for (const char* key : {"name", "description", "type", "location"})
            if (const std::string v = text(item, key); !v.empty()) {
              add(v);
              break;
            }
        }
      }
      return list;
    }
  }
  for (size_t at = 0; at <= value.size();) {
    const size_t end = std::min(value.find(',', at), value.size());
    add(value.substr(at, end - at));
    at = end + 1;
  }
  return list;
}

std::string joined(const std::vector<std::string>& list) {
  std::string out;
  for (const std::string& item : list) out += (out.empty() ? "" : ", ") + item;
  return out;
}

// XBVR's gender, as the catalog reads Stash's.
std::string gender(const std::string& value) {
  std::string v;
  for (const char c : lower(value))
    if (std::isalpha(static_cast<unsigned char>(c))) v += c;
  if (v == "female" || v == "woman" || v == "f") return "FEMALE";
  if (v == "male" || v == "man" || v == "m") return "MALE";
  if (v == "transgenderfemale" || v == "transfemale" || v == "transwoman") return "TRANSGENDER_FEMALE";
  if (v == "transgendermale" || v == "transmale" || v == "transman") return "TRANSGENDER_MALE";
  if (v == "intersex") return "INTERSEX";
  if (v == "nonbinary") return "NON_BINARY";
  return {};
}

// XBVR keeps sizes in centimetres, each on its own: as Stash's
// measurements have them, in inches ("34C-24-35"), with what there is.
std::string measurements(const json& actor) {
  auto inches = [&](const char* key) {
    const int cm = number<int>(actor, key);
    return cm > 0 ? static_cast<int>(std::lround(cm / 2.54)) : 0;
  };
  const int band = inches("band_size"), waist = inches("waist_size"), hip = inches("hip_size");
  const std::string cup = text(actor, "cup_size");
  if (!band && cup.empty()) return {};
  std::string out = (band ? std::to_string(band) : std::string()) + cup;
  if (band && waist && hip) out += "-" + std::to_string(waist) + "-" + std::to_string(hip);
  return out;
}

StashData::Performer performerOf(const json& actor) {
  StashData::Performer p;
  p.id = number<int64_t>(actor, "id");
  p.name = text(actor, "name");
  p.aliases = listed(text(actor, "aliases"));
  p.gender = gender(text(actor, "gender"));
  p.birthdate = day(text(actor, "birth_date"));
  p.country = text(actor, "nationality");
  p.ethnicity = text(actor, "ethnicity");
  p.hairColor = text(actor, "hair_color");
  p.eyeColor = text(actor, "eye_color");
  p.heightCm = number<int>(actor, "height");
  p.measurements = measurements(actor);
  p.fakeTits = text(actor, "breast_type");
  p.tattoos = joined(listed(text(actor, "tattoos")));
  p.piercings = joined(listed(text(actor, "piercings")));
  if (const int year = number<int>(actor, "start_year"); year > 0) p.careerStart = std::to_string(year);
  if (const int year = number<int>(actor, "end_year"); year > 0) p.careerEnd = std::to_string(year);
  p.image = image(text(actor, "image_url"));
  p.favorite = flag(actor, "favourite");
  return p;
}

// A file's path as XBVR has it in two parts, joined as its folder is
// written (a Windows server's with \).
std::string pathOf(const json& file) {
  const std::string folder = text(file, "path"), name = text(file, "filename");
  if (folder.empty()) return name;
  const char last = folder.back();
  if (last == '/' || last == '\\') return folder + name;
  return folder + (folder.find('\\') != std::string::npos && folder.find('/') == std::string::npos
                       ? "\\"
                       : "/") + name;
}

MediaClient::Activity activityOf(const json& scene) {
  MediaClient::Activity a;
  const double stars = number<double>(scene, "star_rating");
  if (stars > 0) a.rating100 = static_cast<int>(std::lround(stars * 20));
  a.watched = flag(scene, "is_watched");
  return a;
}

}  // namespace

const MediaServer& xbvrServer();

XbvrClient::XbvrClient(const std::string& url, bool unverified)
    : MediaClient(xbvrServer(), url, std::string(), unverified) {}

bool XbvrClient::ask(const Call& call, json& answer, std::string& error, StashProblem& problem) {
  problem = StashProblem::Other;
  if (call.post && call.path != kList && call.path != kToggle &&
      call.path.compare(0, std::strlen(kRate), kRate) != 0) {
    error = "XBVR: refused to send a change other than a rating or a scene watched";
    return false;
  }
  long status = 0;
  std::string body;
  if (!send(call, status, body, error, problem)) return false;
  if (status != 200) {
    error = httpProblem(status, body);
    return false;
  }
  // Its changes may answer with nothing.
  answer = body.empty() ? json() : json::parse(body, nullptr, false);
  if (answer.is_discarded()) {
    error = said().notStash;
    return false;
  }
  problem = StashProblem::None;
  return true;
}

bool XbvrClient::get(const std::string& path, json& answer) {
  Call call;
  call.path = path;
  return ask(call, answer, error_, problem_);
}

MediaClient::Answer XbvrClient::identify() {
  json api;
  if (!get(kApi, api)) return Answer::Unreached;
  if (text(member(api, "info"), "title") == kApiTitle) return Answer::Open;
  error_ = said().notStash;
  return Answer::Unreached;
}

// One reading of every scene with a file, a page at a time. `whole`: the
// pages fit together (XBVR counted the same scenes for each, none came
// twice and none is missing); they don't when its scenes changed between
// two of them, which moves the rest along.
bool XbvrClient::listed(std::vector<json>& rows, bool& whole, size_t& bytes, double& ms) {
  rows.clear();
  whole = true;
  std::unordered_set<int64_t> seen;
  int64_t count = -1;
  for (int offset = 0;;) {
    // By its own id, so that pages neither overlap nor skip; without hidden
    // scenes, which XBVR leaves out unless asked.
    Call call;
    call.path = kList;
    call.body = json{{"isAvailable", true}, {"limit", kPage}, {"offset", offset}, {"sort", "scene_id_desc"}}.dump();
    call.post = true;
    call.slow = true;
    long status = 0;
    std::string body;
    const auto start = std::chrono::steady_clock::now();
    const bool sent = send(call, status, body, error_, problem_);
    ms += msSince(start);
    if (!sent) return false;
    if (status != 200) {
      error_ = httpProblem(status, body);
      return false;
    }
    bytes += body.size();
    json page = json::parse(body, nullptr, false);
    if (!page.is_object() || !page.contains("scenes")) {
      error_ = said().notStash;
      return false;
    }
    const int64_t results = number<int64_t>(page, "results");
    if (count < 0) count = results;
    whole = whole && results == count;
    json& list = page["scenes"];
    const size_t size = list.is_array() ? list.size() : 0;
    for (size_t i = 0; i < size; ++i) {
      if (!list[i].is_object()) continue;
      if (seen.insert(number<int64_t>(list[i], "id")).second) rows.push_back(std::move(list[i]));
      else whole = false;
    }
    offset += static_cast<int>(size);
    if (!size || offset >= results) break;
  }
  whole = whole && static_cast<int64_t>(seen.size()) == count;
  return true;
}

// Every scene with a file. Read again if its pages didn't fit together;
// false with error_ set if they never did, or a request failed. `bytes`
// and `ms` count what the requests brought and took.
bool XbvrClient::scenes(const std::function<void(const json& scene)>& each, size_t& bytes,
                        double& ms) {
  std::vector<json> rows;
  for (int attempt = 0; attempt < 3; ++attempt) {
    bool whole = false;
    if (!listed(rows, whole, bytes, ms)) return false;
    if (!whole) continue;
    for (const json& scene : rows) each(scene);
    return true;
  }
  // Nothing is made of a list that may lack scenes: what waits to be sent
  // for one of them would be taken for a video the server doesn't have.
  error_ = "XBVR's scenes changed while they were read";
  problem_ = StashProblem::Other;
  return false;
}

bool XbvrClient::fetch(StashData& data, Timings& timings) {
  const auto start = std::chrono::steady_clock::now();
  error_.clear();
  problem_ = StashProblem::None;
  received_ = 0;
  timings = Timings{};
  auto failed = [&] {
    if (problem_ == StashProblem::None) problem_ = StashProblem::Other;
    return false;
  };
  json api;
  if (!get(kApi, api)) return failed();
  if (text(member(api, "info"), "title") != kApiTitle) {
    error_ = said().notStash;
    return failed();
  }
  data = StashData{};
  data.server = server();
  data.syncedAt = static_cast<int64_t>(std::time(nullptr));
  data.version = text(member(api, "info"), "version");

  // The folders its files are in. A file somewhere else (XBVR's Put.io
  // storage) isn't its own to serve: asked for, XBVR sends the player there.
  json storage;
  const auto storageStart = std::chrono::steady_clock::now();
  if (!get(kStorage, storage)) return failed();
  timings.fetchMs += msSince(storageStart);
  std::unordered_set<int64_t> elsewhere;
  for (const json& volume : array(storage, "volumes")) {
    if (text(volume, "type") != "local") elsewhere.insert(number<int64_t>(volume, "id"));
    else if (const std::string path = text(volume, "path"); !path.empty())
      data.libraryFolders.push_back(path);
  }

  std::unordered_set<int64_t> performers, tags;
  std::unordered_map<std::string, size_t> studios;  // by site name: its place in data.studios
  std::unordered_map<std::string, std::string> scrapers;  // each site's scraper
  const bool listed = scenes(
      [&](const json& j) {
        StashData::Scene s;
        s.id = number<int64_t>(j, "id");
        if (!s.id) return;
        s.title = text(j, "title");
        // XBVR's name for the scene ("site-123"): its previews are kept by it.
        s.code = text(j, "scene_id");
        s.details = text(j, "synopsis");
        // The day as XBVR writes it, not its time (the server's midnight).
        s.date = day(text(j, "release_date_text"));
        if (s.date.empty()) s.date = day(text(j, "release_date"));
        // When its file was added; a scene from before XBVR kept that, when
        // the scene was.
        s.createdAt = unixTime(text(j, "added_date"));
        if (s.createdAt <= 0) s.createdAt = unixTime(text(j, "created_at"));
        if (const std::string site = text(j, "site"); !site.empty()) {
          s.studio = nameId(site);
          if (studios.emplace(site, data.studios.size()).second) {
            data.studios.push_back({s.studio, site, std::string(), 0});
            scrapers[site] = text(j, "scraper_id");
          }
        }
        for (const json& actor : array(j, "cast")) {
          const int64_t id = number<int64_t>(actor, "id");
          if (!id) continue;
          s.performers.push_back(id);
          if (performers.insert(id).second) data.performers.push_back(performerOf(actor));
        }
        for (const json& tag : array(j, "tags")) {
          const int64_t id = number<int64_t>(tag, "id");
          if (!id) continue;
          s.tags.push_back(id);
          if (tags.insert(id).second) data.tags.push_back({id, text(tag, "name")});
        }
        if (const std::string url = text(j, "scene_url"); !url.empty()) s.urls.push_back(url);
        s.cover = image(text(j, "cover_url"));
        s.vr = text(j, "scene_type") != "2D";
        for (const json& f : array(j, "file")) {
          if (text(f, "type") != "video" || elsewhere.count(number<int64_t>(f, "volume_id"))) continue;
          StashData::File file;
          file.id = number<int64_t>(f, "id");
          file.path = pathOf(f);
          file.size = number<uint64_t>(f, "size");
          file.modified = unixTime(text(f, "updated_time"));
          file.width = number<int>(f, "video_width");
          file.height = number<int>(f, "video_height");
          file.duration = number<double>(f, "duration");
          file.fps = number<double>(f, "video_avgfps_val");
          file.codec = text(f, "video_codec_name");
          file.oshash = oshash(text(f, "oshash"));
          if (file.id && !file.path.empty()) s.files.push_back(std::move(file));
        }
        if (!s.files.empty()) data.scenes.push_back(std::move(s));
      },
      timings.bytes, timings.fetchMs);
  if (!listed) return failed();

  // Each site's logo, by its name or its scraper's.
  json sites;
  const auto sitesStart = std::chrono::steady_clock::now();
  if (!get(kSites, sites)) return failed();
  timings.fetchMs += msSince(sitesStart);
  for (const json& site : sites.is_array() ? sites : json::array()) {
    const std::string avatar = image(text(site, "avatar_url"));
    if (avatar.empty()) continue;
    for (auto& [name, at] : studios)
      if (data.studios[at].image.empty() &&
          (name == text(site, "name") || scrapers[name] == text(site, "id")))
        data.studios[at].image = avatar;
  }
  timings.parseMs = std::max(0.0, msSince(start) - timings.fetchMs);
  return true;
}

bool XbvrClient::scene(int64_t id, json& answer, bool& exists) {
  exists = false;
  if (!get("/api/scene/" + std::to_string(id), answer)) return false;
  // One it doesn't have comes as an empty scene.
  exists = answer.is_object() && number<int64_t>(answer, "id") == id;
  return true;
}

bool XbvrClient::fetchActivity(std::unordered_map<int64_t, Activity>& activity) {
  error_.clear();
  activity.clear();
  size_t bytes = 0;
  double ms = 0;
  return scenes([&](const json& j) { activity[number<int64_t>(j, "id")] = activityOf(j); }, bytes, ms);
}

bool XbvrClient::fetchActivity(int64_t id, Activity& activity, bool& exists) {
  error_.clear();
  json j;
  if (!scene(id, j, exists)) return false;
  activity = exists ? activityOf(j) : Activity{};
  return true;
}

bool XbvrClient::setRating(int64_t id, int rating100) {
  error_.clear();
  Call call;
  call.path = kRate + std::to_string(id);
  call.body = json{{"rating", std::max(0, rating100) / 20.0}}.dump();
  call.post = true;
  json answer;
  if (!ask(call, answer, error_, problem_)) return false;
  // It answers with the scene as it is now; one it doesn't have, empty.
  if (!answer.is_object() || number<int64_t>(answer, "id") != id ||
      std::lround(number<double>(answer, "star_rating") * 20) != std::max(0, rating100)) {
    error_ = "XBVR did not take the rating";
    return false;
  }
  return true;
}

bool XbvrClient::addPlays(int64_t id, const std::vector<int64_t>& times) {
  error_.clear();
  if (times.empty()) return true;
  // XBVR can only be told to turn "watched" over, by the scene's name: so
  // only for a scene that isn't, and looked at again after.
  json j;
  bool exists = false;
  if (!scene(id, j, exists)) return false;
  if (exists && flag(j, "is_watched")) return true;
  const std::string name = exists ? text(j, "scene_id") : std::string();
  if (name.empty()) {
    error_ = "XBVR has no such scene";
    return false;
  }
  Call call;
  call.path = kToggle;
  call.body = json{{"scene_id", name}, {"list", "watched"}}.dump();
  call.post = call.once = true;
  json answer;
  if (!ask(call, answer, error_, problem_) || !scene(id, j, exists)) return false;
  if (!exists || !flag(j, "is_watched")) {
    error_ = "XBVR did not mark the scene watched";
    return false;
  }
  return true;
}
