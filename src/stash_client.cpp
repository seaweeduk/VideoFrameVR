#include "stash_client.h"

#include "strings.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <future>
#include <unordered_set>

using json = nlohmann::json;

namespace {

// Asked first: below 0.31.0 Stash lacks fields the sync asks for
// (career_start, career_end), and one unknown field fails a whole request.
constexpr const char* kQueryVersion = "{ version { version } }";
constexpr long kMinimumRelease = 31000;  // 0.31.0, as releaseNumber() has it

// Everything but the scenes, in one request. Scenes without a cover of
// their own still have a screenshot URL (Stash serves a placeholder there),
// so they are asked for separately.
constexpr const char* kQueryRest = R"({
  version { version }
  configuration { ui general { stashes { path excludeVideo } } }
  findPerformers(filter: {per_page: -1}) { performers {
    id name disambiguation alias_list gender birthdate country ethnicity hair_color eye_color
    height_cm measurements fake_tits tattoos piercings career_start career_end image_path favorite } }
  findStudios(filter: {per_page: -1}) { studios { id name image_path parent_studio { id } } }
  findTags(filter: {per_page: -1}) { tags { id name } }
  findGroups(filter: {per_page: -1}) { groups { id name date front_image_path studio { id } } }
  noCover: findScenes(scene_filter: {is_missing: "cover"}, filter: {per_page: -1}) { scenes { id } }
})";

constexpr const char* kQueryScenes = R"({
  findScenes(filter: {per_page: -1}) { scenes {
    id title code details director date created_at urls
    paths { screenshot }
    studio { id }
    performers { id }
    tags { id }
    groups { group { id } scene_index }
    files { path size mod_time width height duration frame_rate video_codec
            oshash: fingerprint(type: "oshash") } } }
})";

// Write-back (docs/stash.md, "Write-back safety"): the only three changes
// the client sends, fixed here. post() refuses any other document that has
// a mutation in it.
constexpr const char* kSetRating =
    "mutation SetRating($id: ID!, $rating100: Int) {"
    " sceneUpdate(input: {id: $id, rating100: $rating100}) { id } }";
constexpr const char* kAddPlays =
    "mutation AddPlays($id: ID!, $times: [Timestamp!]) {"
    " sceneAddPlay(id: $id, times: $times) { count } }";
// Adds and removes the given tags on a scene, keeping its others (never
// SET). Mutation fields run in order; one without tags is left out.
constexpr const char* kEditTags =
    "mutation EditTags($ids: [ID!], $add: [ID!], $remove: [ID!], $adding: Boolean!,"
    " $removing: Boolean!) {"
    " added: bulkSceneUpdate(input: {ids: $ids, tag_ids: {ids: $add, mode: ADD}})"
    " @include(if: $adding) { id }"
    " removed: bulkSceneUpdate(input: {ids: $ids, tag_ids: {ids: $remove, mode: REMOVE}})"
    " @include(if: $removing) { id } }";

// Deleting the playing file (docs/alpha/delete-file.md), only with Send to
// Stash on: a scene's files (its primary first) to pick the one playing by
// its path and oshash, and Stash's trash folder; then each change on one scene or
// one file: the scene with its files and generated files, or only its
// entries (as Clean does); another file made its primary; one file, or
// only its entry. Never a list of several.
constexpr const char* kQuerySceneFiles =
    "query SceneFiles($id: ID!) { findScene(id: $id) { id files { id path"
    " oshash: fingerprint(type: \"oshash\") } } configuration { general { deleteTrashPath } } }";
constexpr const char* kDeleteScene =
    "mutation DeleteScene($id: ID!) {"
    " sceneDestroy(input: {id: $id, delete_file: true, delete_generated: true}) }";
constexpr const char* kForgetScene =
    "mutation ForgetScene($id: ID!) { sceneDestroy(input: {id: $id, delete_file: false,"
    " delete_generated: true, destroy_file_entry: true}) }";
constexpr const char* kSetPrimaryFile =
    "mutation SetPrimaryFile($id: ID!, $file: ID!) {"
    " sceneUpdate(input: {id: $id, primary_file_id: $file}) { id } }";
constexpr const char* kDeleteFile = "mutation DeleteFile($id: ID!) { deleteFiles(ids: [$id]) }";
constexpr const char* kForgetFile = "mutation ForgetFile($id: ID!) { destroyFiles(ids: [$id]) }";

// Signing in (StashClient::signIn()): the key Stash has, read with the
// session the sign-in gave, and one made when it has none.
constexpr const char* kQueryKey = "{ configuration { general { apiKey username } } }";
constexpr const char* kMakeKey = "mutation { generateAPIKey(input: {clear: false}) }";

constexpr const char* kQueryActivity =
    "{ findScenes(filter: {per_page: -1}) { scenes { id rating100 play_history } } }";
constexpr const char* kQuerySceneActivity =
    "query Activity($id: ID!) { findScene(id: $id) { id rating100 play_history } }";

// Connecting may take this long; a transfer that stalls (under 1 KB/s) for
// kStallSeconds is given up, as is one that takes longer than kMaxSeconds.
// Every scene at once (the sync's two queries, write-back's activity) may
// take a big library on a slow computer minutes before Stash sends
// anything: those wait longer, and can be stopped meanwhile.
constexpr long kConnectSeconds = 10;
constexpr long kStallSeconds = 20;
constexpr long kMaxSeconds = 180;
constexpr long kSlowStallSeconds = 600;
constexpr long kSlowMaxSeconds = 3600;
// Each of Connect's tries (check()).
constexpr long kQuickConnectSeconds = 3, kQuickMaxSeconds = 8;

// A release's version ("v0.31.1") as major × 10⁶ + minor × 10³ + patch; -1
// for anything else, such as a development build ("v0.31.1-12-gabcdef0").
long releaseNumber(const std::string& version) {
  const char* v = version.c_str();
  if (*v == 'v') ++v;
  int major = 0, minor = 0, patch = 0, end = 0;
  if (std::sscanf(v, "%d.%d.%d%n", &major, &minor, &patch, &end) != 3 || v[end] != '\0') return -1;
  return major * 1000000L + minor * 1000L + patch;
}

std::string printed(const char* format, ...) {
  char buf[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  return buf;
}

bool mentions(const std::string& body, const char* word) {
  std::string lower = body.substr(0, 4096);
  for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return lower.find(word) != std::string::npos;
}

// An HTTP status that isn't Stash's answer, in a few words.
std::string httpProblem(long status, const std::string& body, const std::string& base) {
  const StashSetupStrings& t = strings().stashSetup;
  // A server that speaks only https, asked over http (Go's and nginx's 400).
  if (status == 400 && base.compare(0, 7, "http://") == 0 && mentions(body, "https"))
    return t.tryHttps;
  if (status == 403) return t.forbidden;
  if (status == 404 || status == 405) return t.notStash;
  return printed(t.httpError, static_cast<int>(status));
}

// Stash's 403 for an address outside its network while it has no login
// (its "external access" check, and its tripwire after one).
bool publicRefusal(long status, const std::string& body) {
  return status == 403 && mentions(body, "internet");
}

size_t appendToString(char* data, size_t size, size_t count, void* target) {
  static_cast<std::string*>(target)->append(data, size * count);
  return size * count;
}

// One request's progress: what it adds to the client's bytes received, and
// whether the client was cancelled.
struct Transfer {
  const std::atomic<bool>* cancelled;
  std::atomic<size_t>* received;
  curl_off_t counted = 0;
};

int progress(void* transfer, curl_off_t, curl_off_t now, curl_off_t, curl_off_t) {
  Transfer& t = *static_cast<Transfer*>(transfer);
  if (now > t.counted) {
    *t.received += static_cast<size_t>(now - t.counted);
    t.counted = now;
  }
  return t.cancelled->load() ? 1 : 0;
}

std::string text(const json& object, const char* key) {
  const auto it = object.find(key);
  return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

template <typename T>
T number(const json& object, const char* key, T fallback = 0) {
  const auto it = object.find(key);
  return it != object.end() && it->is_number() ? it->get<T>() : fallback;
}

// An id, which GraphQL sends as a string; 0 if there is none.
int64_t id(const json& object) {
  const auto it = object.find("id");
  if (it == object.end() || !it->is_string()) return 0;
  return std::strtoll(it->get_ref<const std::string&>().c_str(), nullptr, 10);
}

// The id of a linked object ({"studio": {"id": "3"}}), 0 if none.
int64_t linkedId(const json& object, const char* key) {
  const auto it = object.find(key);
  return it != object.end() && it->is_object() ? id(*it) : 0;
}

const json& array(const json& object, const char* key) {
  static const json empty = json::array();
  const auto it = object.find(key);
  return it != object.end() && it->is_array() ? *it : empty;
}

// A member object, or an empty one.
const json& member(const json& object, const char* key) {
  static const json empty = json::object();
  const auto it = object.find(key);
  return it != object.end() && it->is_object() ? *it : empty;
}

// An image URL; empty for Stash's placeholder ("&default=true").
std::string image(const json& object, const char* key) {
  std::string url = text(object, key);
  return url.find("default=true") != std::string::npos ? std::string() : url;
}

// Days from 1970-01-01 to a date of the proleptic Gregorian calendar.
int64_t daysFromCivil(int64_t y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// Stash's times are RFC 3339 ("2019-10-04T22:37:47+01:00", "...Z", maybe
// with a fraction): Unix seconds, the fraction dropped (as stat's st_mtime
// does). 0 if it can't be read.
int64_t unixTime(const std::string& t) {
  int y, mo, d, h, mi, s;
  if (std::sscanf(t.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
  size_t at = 19;
  if (at < t.size() && t[at] == '.')
    while (++at < t.size() && t[at] >= '0' && t[at] <= '9') {
    }
  int64_t offset = 0;
  if (at < t.size() && (t[at] == '+' || t[at] == '-')) {
    int oh = 0, om = 0;
    if (std::sscanf(t.c_str() + at + 1, "%2d:%2d", &oh, &om) != 2) return 0;
    offset = (t[at] == '-' ? -1 : 1) * (oh * 3600 + om * 60);
  }
  return daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s - offset;
}

// Stash's way of saying "no date" for groups.
std::string date(const json& object, const char* key) {
  std::string d = text(object, key);
  return d == "0001-01-01" ? std::string() : d;
}

void readScenes(const json& scenes, const std::unordered_set<int64_t>& noCover, StashData& data) {
  data.scenes.reserve(scenes.size());
  for (const json& j : scenes) {
    StashData::Scene s;
    s.id = id(j);
    s.title = text(j, "title");
    s.code = text(j, "code");
    s.details = text(j, "details");
    s.director = text(j, "director");
    s.date = text(j, "date");
    s.createdAt = unixTime(text(j, "created_at"));
    s.studio = linkedId(j, "studio");
    for (const json& p : array(j, "performers")) s.performers.push_back(id(p));
    for (const json& t : array(j, "tags")) s.tags.push_back(id(t));
    for (const json& g : array(j, "groups")) s.groups.emplace_back(linkedId(g, "group"), number<int>(g, "scene_index"));
    for (const json& u : array(j, "urls"))
      if (u.is_string()) s.urls.push_back(u.get<std::string>());
    if (const auto paths = j.find("paths"); paths != j.end() && paths->is_object() && !noCover.count(s.id))
      s.cover = image(*paths, "screenshot");
    for (const json& f : array(j, "files")) {
      StashData::File file;
      file.path = text(f, "path");
      file.size = number<uint64_t>(f, "size");
      file.modified = unixTime(text(f, "mod_time"));
      file.width = number<int>(f, "width");
      file.height = number<int>(f, "height");
      file.duration = number<double>(f, "duration");
      file.fps = number<double>(f, "frame_rate");
      file.codec = text(f, "video_codec");
      file.oshash = text(f, "oshash");
      s.files.push_back(std::move(file));
    }
    data.scenes.push_back(std::move(s));
  }
}

void readRest(const json& d, StashData& data) {
  if (const auto v = d.find("version"); v != d.end() && v->is_object()) data.version = text(*v, "version");
  data.vrTag = "Virtual Reality";  // Stash's default
  if (const auto c = d.find("configuration"); c != d.end() && c->is_object())
    if (const auto ui = c->find("ui"); ui != c->end() && ui->is_object())
      if (const std::string tag = text(*ui, "vrTag"); !tag.empty()) data.vrTag = tag;
  for (const json& f : array(member(member(d, "configuration"), "general"), "stashes")) {
    const auto exclude = f.find("excludeVideo");
    const bool videos = exclude == f.end() || !exclude->is_boolean() || !exclude->get<bool>();
    if (const std::string path = text(f, "path"); videos && !path.empty())
      data.libraryFolders.push_back(path);
  }

  for (const json& j : array(member(d, "findPerformers"), "performers")) {
    StashData::Performer p;
    p.id = id(j);
    p.name = text(j, "name");
    p.disambiguation = text(j, "disambiguation");
    for (const json& a : array(j, "alias_list"))
      if (a.is_string()) p.aliases.push_back(a.get<std::string>());
    p.gender = text(j, "gender");
    p.birthdate = text(j, "birthdate");
    p.country = text(j, "country");
    p.ethnicity = text(j, "ethnicity");
    p.hairColor = text(j, "hair_color");
    p.eyeColor = text(j, "eye_color");
    p.heightCm = number<int>(j, "height_cm");
    p.measurements = text(j, "measurements");
    p.fakeTits = text(j, "fake_tits");
    p.tattoos = text(j, "tattoos");
    p.piercings = text(j, "piercings");
    p.careerStart = text(j, "career_start");
    p.careerEnd = text(j, "career_end");
    p.image = image(j, "image_path");
    const auto favorite = j.find("favorite");
    p.favorite = favorite != j.end() && favorite->is_boolean() && favorite->get<bool>();
    data.performers.push_back(std::move(p));
  }
  for (const json& j : array(member(d, "findStudios"), "studios"))
    data.studios.push_back({id(j), text(j, "name"), image(j, "image_path"), linkedId(j, "parent_studio")});
  for (const json& j : array(member(d, "findTags"), "tags"))
    data.tags.push_back({id(j), text(j, "name")});
  for (const json& j : array(member(d, "findGroups"), "groups"))
    data.groups.push_back(
        {id(j), text(j, "name"), date(j, "date"), image(j, "front_image_path"), linkedId(j, "studio")});
}

// The response's data, or false with the server's first error.
bool parseResponse(const std::string& body, json& data, std::string& error) {
  json root = json::parse(body, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    error = strings().stashSetup.notStash;
    return false;
  }
  const json& errors = array(root, "errors");
  if (!errors.empty()) {
    const std::string message = errors[0].is_object() ? text(errors[0], "message") : std::string();
    error = printed(strings().stashSetup.stashSaid, message.empty() ? "error" : message.c_str());
    return false;
  }
  const auto d = root.find("data");
  if (d == root.end() || !d->is_object()) {
    error = strings().stashSetup.notStash;
    return false;
  }
  data = std::move(*d);
  return true;
}

double msSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

StashClient::Activity activityOf(const json& scene) {
  StashClient::Activity a;
  a.rating100 = number<int>(scene, "rating100", -1);
  for (const json& t : array(scene, "play_history"))
    if (t.is_string())
      if (const int64_t when = unixTime(t.get<std::string>())) a.plays.push_back(when);
  return a;
}

// "2026-10-24T18:03:12Z".
std::string rfc3339(int64_t seconds) {
  const time_t t = static_cast<time_t>(seconds);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

}  // namespace

StashClient::StashClient(const std::string& url, std::string apiKey, bool unverified)
    : typed_(url), address_(url, unverified), apiKey_(std::move(apiKey)) {}

bool StashClient::post(const std::string& query, std::string& body, std::string& error,
                       StashProblem& problem, const std::string& variables, bool slow) {
  reached_ = false;
  problem = StashProblem::Other;
  const bool deleting = query == kDeleteScene || query == kForgetScene ||
                        query == kSetPrimaryFile || query == kDeleteFile || query == kForgetFile;
  if (query.find("mutation") != std::string::npos && query != kSetRating && query != kAddPlays &&
      query != kEditTags && !(deleting && writeBack_)) {
    error = "Stash: refused to send a change other than a rating, a play, tags or a deletion "
            "with Send to Stash on";
    return false;
  }
  json payload{{"query", query}};
  if (!variables.empty()) {
    payload["variables"] = json::parse(variables, nullptr, false);
    if (!payload["variables"].is_object()) {
      error = "Stash: bad variables";
      return false;
    }
  }
  CURL* curl = curl_easy_init();
  if (!curl) {
    error = "Stash: no HTTP client";
    return false;
  }
  const std::string request = payload.dump();
  curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");
  if (!apiKey_.empty()) headers = curl_slist_append(headers, ("ApiKey: " + apiKey_).c_str());
  char message[CURL_ERROR_SIZE] = "";
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendToString);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // compressed if the server can
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, quick_ ? kQuickConnectSeconds : kConnectSeconds);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, slow ? kSlowStallSeconds : kStallSeconds);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, quick_ ? kQuickMaxSeconds : slow ? kSlowMaxSeconds : kMaxSeconds);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);  // a server gone while it works is noticed
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  Transfer transfer{&cancelled_, &received_};
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, message);
  // Deleting's go only where Stash answered (address_.base()), never on to
  // the fallback port after a failed try.
  StashAddress at = address_;
  if (deleting) at.fallback.clear();
  const StashReply reply = stashPerform(curl, at, "/graphql", [&] {
    body.clear();
    message[0] = '\0';
  });
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  reached_ = reply.code == CURLE_OK;
  if (reply.code != CURLE_OK) {
    if (reply.code == CURLE_ABORTED_BY_CALLBACK) {
      error = "Stash: stopped";
      return false;
    }
    error = stashUnreachable(reply.code);
    if (stashUntrusted(reply.code)) problem = StashProblem::Certificate;
    std::fprintf(stderr, "Stash: %s\n", *message ? message : curl_easy_strerror(reply.code));
    return false;
  }
  const StashSetupStrings& t = strings().stashSetup;
  if (reply.elsewhere) {
    error = t.elsewhere;
    return false;
  }
  // GraphQL errors come with 200, or with 422 and a body that says why.
  if (publicRefusal(reply.status, body)) {
    error = t.publicRefused;
    return false;
  }
  if (reply.status == 401) {
    problem = apiKey_.empty() ? StashProblem::NeedsSignIn : StashProblem::KeyRefused;
    error = apiKey_.empty() ? t.needsSignIn : t.keyRefused;
    return false;
  }
  if (reply.status != 200 && reply.status != 422) {
    error = httpProblem(reply.status, body, address_.base());
    return false;
  }
  problem = StashProblem::None;
  return true;
}

bool StashClient::fetch(StashData& data, Timings& timings) {
  const auto start = std::chrono::steady_clock::now();
  error_.clear();
  problem_ = StashProblem::None;
  received_ = 0;
  // The version first: an older Stash gets nothing else asked.
  std::string versionBody;
  json versionData;
  if (!post(kQueryVersion, versionBody, error_, problem_) ||
      !parseResponse(versionBody, versionData, error_)) {
    if (problem_ == StashProblem::None) problem_ = StashProblem::Other;
    return false;
  }
  const long release = releaseNumber(text(member(versionData, "version"), "version"));
  if (release >= 0 && release < kMinimumRelease) {
    error_ = strings().stashSetup.tooOld;
    problem_ = StashProblem::TooOld;
    return false;
  }

  // The scenes take most of the time; the rest comes meanwhile.
  std::string restBody, restError;
  StashProblem restProblem = StashProblem::None;
  auto rest = std::async(std::launch::async, [&] {
    return post(kQueryRest, restBody, restError, restProblem, std::string(), true);
  });
  std::string scenesBody, scenesError;
  StashProblem scenesProblem = StashProblem::None;
  const bool scenesOk = post(kQueryScenes, scenesBody, scenesError, scenesProblem, std::string(), true);
  const bool restOk = rest.get();
  timings.fetchMs = msSince(start);
  timings.bytes = restBody.size() + scenesBody.size();
  if (!scenesOk || !restOk) {
    error_ = scenesOk ? restError : scenesError;
    problem_ = scenesOk ? restProblem : scenesProblem;
    return false;
  }

  const auto parseStart = std::chrono::steady_clock::now();
  json restData, scenesData;
  if (!parseResponse(restBody, restData, error_) || !parseResponse(scenesBody, scenesData, error_)) {
    problem_ = StashProblem::Other;
    // A development build without the fields a release from 0.31.0 has.
    if (release < 0 && error_.find("Cannot query field") != std::string::npos) {
      error_ = strings().stashSetup.tooOld;
      problem_ = StashProblem::TooOld;
    }
    return false;
  }
  data = StashData{};
  data.server = server();
  data.syncedAt = static_cast<int64_t>(std::time(nullptr));
  readRest(restData, data);
  std::unordered_set<int64_t> noCover;
  for (const json& s : array(member(restData, "noCover"), "scenes")) noCover.insert(id(s));
  readScenes(array(member(scenesData, "findScenes"), "scenes"), noCover, data);
  timings.parseMs = msSince(parseStart);
  return true;
}

StashClient::Answer StashClient::check() {
  const StashSetupStrings& t = strings().stashSetup;
  quick_ = true;  // short timeouts, until it returns
  struct Reset {
    bool& quick;
    ~Reset() { quick = false; }
  } reset{quick_};
  int best = -1;  // how far the try whose error() is kept got
  std::string error;
  StashProblem problem = StashProblem::Other;
  const bool unverified = address_.unverified;
  for (const std::string& url : stashTries(typed_)) {
    address_ = StashAddress(url, unverified);
    error_.clear();
    std::string body;
    json data;
    if (post(kQueryVersion, body, error_, problem_)) {
      if (parseResponse(body, data, error_)) {
        const long release = releaseNumber(text(member(data, "version"), "version"));
        if (release < 0 || release >= kMinimumRelease) return Answer::Open;
        error_ = t.tooOld;
        return Answer::Failed;
      }
      if (error_ != t.notStash) return Answer::Failed;  // Stash's own error
    } else if (problem_ == StashProblem::NeedsSignIn) {
      return Answer::NeedsSignIn;
    } else if (error_ == t.publicRefused) {
      return Answer::Failed;
    }
    if (cancelled_) break;
    // Nothing there, as against a server that answers in a way that can't be
    // used (its certificate, https on plain http, an address that isn't one),
    // or answers but not as Stash: the first that got furthest says why. A
    // certificate that isn't trusted comes first, as it can be accepted.
    const bool there = error_ == t.certificate || error_ == t.noSecure || error_ == t.badAddress;
    const int got = problem_ == StashProblem::Certificate ? 3 : reached_ ? 2 : there ? 1 : 0;
    if (got > best) {
      best = got;
      error = error_;
      problem = problem_;
    }
    if (error_ == t.noHost) break;  // the same name in each
  }
  address_ = StashAddress(typed_, unverified);
  error_ = error;
  problem_ = problem;
  return best > 0 ? Answer::Failed : Answer::Unreached;
}

bool StashClient::request(const char* query, const std::string& variables, std::string& data,
                          bool slow) {
  error_.clear();
  std::string body;
  json d;
  if (!post(query, body, error_, problem_, variables, slow)) return false;
  if (!parseResponse(body, d, error_)) {
    problem_ = StashProblem::Other;
    return false;
  }
  data = d.dump();
  return true;
}

bool StashClient::fetchActivity(std::unordered_map<int64_t, Activity>& activity) {
  std::string text;
  if (!request(kQueryActivity, std::string(), text, true)) return false;
  activity.clear();
  const json d = json::parse(text);
  for (const json& s : array(member(d, "findScenes"), "scenes"))
    if (const int64_t scene = id(s)) activity[scene] = activityOf(s);
  return true;
}

bool StashClient::fetchActivity(int64_t scene, Activity& activity, bool& exists) {
  std::string text;
  if (!request(kQuerySceneActivity, json{{"id", std::to_string(scene)}}.dump(), text)) return false;
  const json d = json::parse(text);
  const json& s = member(d, "findScene");
  exists = id(s) == scene;
  activity = exists ? activityOf(s) : Activity{};
  return true;
}

bool StashClient::sceneFiles(int64_t scene, std::vector<SceneFile>& files, std::string& trash,
                             bool& exists) {
  std::string data;
  if (!request(kQuerySceneFiles, json{{"id", std::to_string(scene)}}.dump(), data)) return false;
  const json d = json::parse(data);
  const json& s = member(d, "findScene");
  exists = id(s) == scene;
  files.clear();
  for (const json& f : array(s, "files")) files.push_back({id(f), text(f, "path"), text(f, "oshash")});
  trash = text(member(member(d, "configuration"), "general"), "deleteTrashPath");
  return true;
}

namespace {

// A deletion's change answered with true (or the scene's id).
bool took(const json& d, const char* key, int64_t scene = 0) {
  const auto it = d.find(key);
  return it != d.end() && (scene ? id(*it) == scene : it->is_boolean() && it->get<bool>());
}

}  // namespace

bool StashClient::destroyScene(int64_t scene, bool fromDisk) {
  std::string text;
  if (!request(fromDisk ? kDeleteScene : kForgetScene, json{{"id", std::to_string(scene)}}.dump(),
               text))
    return false;
  if (took(json::parse(text), "sceneDestroy")) return true;
  error_ = "Stash did not delete the scene";
  return false;
}

bool StashClient::setPrimaryFile(int64_t scene, int64_t file) {
  std::string text;
  const json variables{{"id", std::to_string(scene)}, {"file", std::to_string(file)}};
  if (!request(kSetPrimaryFile, variables.dump(), text)) return false;
  if (took(json::parse(text), "sceneUpdate", scene)) return true;
  error_ = "Stash did not change the scene's file";
  return false;
}

bool StashClient::destroyFile(int64_t file, bool fromDisk) {
  std::string text;
  if (!request(fromDisk ? kDeleteFile : kForgetFile, json{{"id", std::to_string(file)}}.dump(), text))
    return false;
  if (took(json::parse(text), fromDisk ? "deleteFiles" : "destroyFiles")) return true;
  error_ = "Stash did not delete the file";
  return false;
}

bool StashClient::signIn(const std::string& user, const std::string& password, std::string& apiKey,
                         bool& refused) {
  error_.clear();
  apiKey.clear();
  refused = false;
  problem_ = StashProblem::Other;
  CURL* curl = curl_easy_init();
  if (!curl) {
    error_ = "Stash: no HTTP client";
    return false;
  }
  const StashSetupStrings& t = strings().stashSetup;
  std::string body;
  char message[CURL_ERROR_SIZE] = "";
  curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");  // the session cookie, for the requests after
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendToString);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectSeconds);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, message);
  // One POST on this handle: its HTTP status, 0 if Stash wasn't reached. The
  // login form follows a redirect only to the same path (http → https): an
  // older Stash answers a sign-in with one to its home page.
  auto send = [&](const char* path, const std::string& fields, curl_slist* headers) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fields.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(fields.size()));
    const StashReply reply = stashPerform(
        curl, address_, path,
        [&] {
          body.clear();
          message[0] = '\0';
        },
        std::string(path) != "/login");
    reached_ = reply.code == CURLE_OK;
    if (reply.code != CURLE_OK) {
      error_ = stashUnreachable(reply.code);
      if (stashUntrusted(reply.code)) problem_ = StashProblem::Certificate;
      std::fprintf(stderr, "Stash: %s\n", *message ? message : curl_easy_strerror(reply.code));
      return 0L;
    }
    if (reply.elsewhere) {
      error_ = t.elsewhere;
      return 0L;
    }
    return reply.status;
  };
  auto escaped = [&](const std::string& s) {
    char* e = curl_easy_escape(curl, s.c_str(), static_cast<int>(s.size()));
    const std::string out = e ? e : "";
    curl_free(e);
    return out;
  };
  curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");
  auto query = [&](const char* document, json& data) {
    const long status = send("/graphql", json{{"query", document}}.dump(), headers);
    if (!status) return false;
    if (publicRefusal(status, body)) {
      error_ = t.publicRefused;
      return false;
    }
    if (status == 401 || status == 403) {
      refused = true;
      error_ = t.wrongLogin;
      return false;
    }
    if (status != 200 && status != 422) {
      error_ = httpProblem(status, body, address_.base());
      return false;
    }
    return parseResponse(body, data, error_);
  };

  // Stash's login form answers 200 (older ones a redirect) and sets the
  // session cookie, or 401. A Stash without a login takes the query
  // without it, so the query decides.
  bool ok = false;
  json data;
  if (send("/login", "username=" + escaped(user) + "&password=" + escaped(password), nullptr) &&
      query(kQueryKey, data)) {
    const json& general = member(member(data, "configuration"), "general");
    apiKey = text(general, "apiKey");
    ok = true;
    // A login but no key yet: one is made. A key there is never replaced,
    // as other apps may use it.
    if (apiKey.empty() && !text(general, "username").empty()) {
      ok = query(kMakeKey, data);
      if (ok) apiKey = text(data, "generateAPIKey");
      if (ok && apiKey.empty()) {
        error_ = "Stash made no API key";
        ok = false;
      }
    }
  }
  if (ok) problem_ = StashProblem::None;
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return ok;
}

bool StashClient::setRating(int64_t scene, int rating100) {
  json variables{{"id", std::to_string(scene)}, {"rating100", nullptr}};
  if (rating100 > 0) variables["rating100"] = rating100;
  std::string text;
  if (!request(kSetRating, variables.dump(), text)) return false;
  const json d = json::parse(text);
  if (id(member(d, "sceneUpdate")) != scene) {
    error_ = "Stash did not take the rating";
    return false;
  }
  return true;
}

bool StashClient::addPlays(int64_t scene, const std::vector<int64_t>& times) {
  if (times.empty()) return true;  // Stash would count one play now
  json list = json::array();
  for (const int64_t t : times) list.push_back(rfc3339(t));
  std::string text;
  if (!request(kAddPlays, json{{"id", std::to_string(scene)}, {"times", list}}.dump(), text))
    return false;
  const json d = json::parse(text);
  if (!member(d, "sceneAddPlay").contains("count")) {
    error_ = "Stash did not take the plays";
    return false;
  }
  return true;
}

bool StashClient::editTags(int64_t scene, const std::vector<int64_t>& add,
                           const std::vector<int64_t>& remove) {
  if (add.empty() && remove.empty()) return true;
  auto ids = [](const std::vector<int64_t>& list) {
    json a = json::array();
    for (const int64_t id : list) a.push_back(std::to_string(id));
    return a;
  };
  const json variables{{"ids", ids({scene})}, {"add", ids(add)}, {"remove", ids(remove)},
                       {"adding", !add.empty()}, {"removing", !remove.empty()}};
  std::string text;
  if (!request(kEditTags, variables.dump(), text)) return false;
  const json d = json::parse(text);
  // Each part sent answers with the scene.
  auto took = [&](const char* key) {
    for (const json& s : array(d, key))
      if (id(s) == scene) return true;
    return false;
  };
  if ((!add.empty() && !took("added")) || (!remove.empty() && !took("removed"))) {
    error_ = "Stash did not take the tags";
    return false;
  }
  return true;
}
