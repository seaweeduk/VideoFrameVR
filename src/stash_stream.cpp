#include "stash_stream.h"

#include "read_ahead.h"
#include "stash_http.h"

#include <curl/curl.h>
#include <mpv/stream_cb.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::string_view kScheme = "stashscene://";

struct Server {
  int number = 0;
  uint64_t hash = 0;  // of address.server, for downloads' sources
  StashAddress address;
  std::string apiKey;
};
std::mutex servedMutex;
Server served;
std::map<std::string, int> serverNumbers;  // each server's in this run
std::unordered_map<int64_t, FileFingerprint> knownFiles;  // the served server's
// Playback streams being opened: seek previews start no request meanwhile
// (as SMB's background jobs wait for playback's opens).
std::atomic<int> opening{0};

struct Stream : ReadAhead {
  StashAddress address;
  std::string path;
  curl_slist* headers = nullptr;  // the ApiKey
  uint64_t length = 0;
  uint64_t chunk = ReadAhead::kBlock;  // playback's (a download's larger)
  bool background = false;
  std::mutex mutex;
  std::condition_variable cv;
  std::atomic<bool> cancelled{false};
  bool failed = false;
  bool changed = false;  // answered whole: the file changed since If-Range's date
  Clock::time_point retryAt{}, retryUntil{};
  ~Stream() { curl_slist_free_all(headers); }
};

// Every stream's requests run on one thread in one curl multi: a range
// request per block, on connections kept alive between them.
class Service {
 public:
  ~Service() {
    { std::lock_guard lock(mutex_); stop_ = true; }
    wake();
    worker_.join();
    curl_multi_cleanup(multi_);
  }
  void add(std::shared_ptr<Stream> s) {
    { std::lock_guard lock(mutex_); added_.push_back(std::move(s)); }
    wake();
  }
  void wake() { curl_multi_wakeup(multi_); }

 private:
  struct Transfer {
    CURL* curl;
    std::shared_ptr<Stream> stream;
    ReadAhead::Block* block;
  };
  CURLM* multi_ = curl_multi_init();
  std::mutex mutex_;
  std::vector<std::shared_ptr<Stream>> added_, streams_;
  bool stop_ = false;
  std::map<CURL*, Transfer> transfers_;
  std::vector<CURL*> idle_;
  std::thread worker_{[this] { run(); }};

  static size_t write(char* data, size_t size, size_t count, void* opaque) {
    auto& t = *static_cast<Transfer*>(opaque);
    auto& b = *t.block;
    const size_t n = size * count;
    long status = 0;
    curl_easy_getinfo(t.curl, CURLINFO_RESPONSE_CODE, &status);
    if (status != 206 || t.stream->cancelled || n > b.bytes.size() - b.filled) return 0;
    memcpy(b.bytes.data() + b.filled, data, n);  // readers wait for done
    b.filled += n;
    return n;
  }
  void start(const std::shared_ptr<Stream>& s, ReadAhead::Block& b) {
    CURL* curl = idle_.empty() ? curl_easy_init() : idle_.back();
    if (!idle_.empty()) idle_.pop_back();
    if (!curl) return;
    curl_easy_reset(curl);  // keeps its connection
    auto& t = transfers_[curl] = {curl, s, &b};
    stashSetUp(curl, s->address);
    curl_easy_setopt(curl, CURLOPT_URL, (s->address.base() + s->path).c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, s->headers);
    const std::string range = std::to_string(b.offset + b.filled) + "-" +
                              std::to_string(b.offset + b.bytes.size() - 1);
    curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 5L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    b.pending = true;
    curl_multi_add_handle(multi_, curl);
  }
  void finish(CURL* curl, CURLcode code) {
    const auto it = transfers_.find(curl);
    const Transfer t = it->second;
    transfers_.erase(it);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_multi_remove_handle(multi_, curl);
    idle_.push_back(curl);
    auto& s = *t.stream;
    auto& b = *t.block;
    std::lock_guard lock(s.mutex);
    b.pending = false;
    if (s.cancelled) return;
    const auto now = Clock::now();
    const bool failed = s.failed;
    if (status == 206 && b.filled == b.bytes.size()) {
      b.done = true;
      s.retryUntil = {};
    } else if (status == 206 || status == 0 || status >= 500) {
      // A dropped connection or a server error: again from where it stopped,
      // for up to 10 s.
      if (s.retryUntil == Clock::time_point{}) {
        s.retryUntil = now + std::chrono::seconds(10);
        std::fprintf(stderr, "Stash stream: %s (HTTP %ld), reconnecting\n",
                     curl_easy_strerror(code), status);
      }
      s.retryAt = now + std::chrono::milliseconds(250);
      s.failed = now >= s.retryUntil;
    } else {
      s.failed = true;
      s.changed = status == 200;
    }
    if (s.failed && !failed) std::fprintf(stderr, "Stash stream: failed (HTTP %ld)\n", status);
    s.cv.notify_all();
  }
  void run() {
    for (;;) {
      {
        std::lock_guard lock(mutex_);
        if (stop_) break;
        for (auto& s : added_) streams_.push_back(std::move(s));
        added_.clear();
      }
      auto over = [](Stream& s) {
        std::lock_guard lock(s.mutex);
        return s.cancelled || s.failed;
      };
      for (auto it = transfers_.begin(); it != transfers_.end();) {
        if (!over(*it->second.stream)) { ++it; continue; }
        curl_multi_remove_handle(multi_, it->first);
        idle_.push_back(it->first);
        it = transfers_.erase(it);
      }
      streams_.erase(std::remove_if(streams_.begin(), streams_.end(),
                                    [&](auto& s) { return over(*s); }), streams_.end());
      const auto now = Clock::now();
      long timeout = streams_.empty() ? 60000 : 1000;
      for (auto& s : streams_) {
        std::lock_guard lock(s->mutex);
        if (!s->background) {
          if (s->windows.empty()) s->seek(s->position);
          s->retire();
          s->queue(s->chunk, s->length);
        } else if (opening) {
          continue;
        }
        if (now < s->retryAt) {
          timeout = std::min<long>(timeout, std::chrono::duration_cast<std::chrono::milliseconds>(
                                                s->retryAt - now).count() + 1);
          continue;
        }
        for (auto& b : s->blocks)
          if (!b.pending && !b.done) start(s, b);
      }
      int running = 0, left = 0;
      curl_multi_perform(multi_, &running);
      bool finished = false;
      while (CURLMsg* m = curl_multi_info_read(multi_, &left)) {
        if (m->msg != CURLMSG_DONE) continue;
        finish(m->easy_handle, m->data.result);
        finished = true;
      }
      if (!finished) curl_multi_poll(multi_, nullptr, 0, timeout, nullptr);
    }
    for (auto& [curl, t] : transfers_) {
      curl_multi_remove_handle(multi_, curl);
      curl_easy_cleanup(curl);
    }
    for (CURL* curl : idle_) curl_easy_cleanup(curl);
  }
};
Service& service() {
  static Service s;
  return s;
}

size_t contentRange(char* data, size_t size, size_t count, void* length) {
  const size_t n = size * count;
  const std::string line(data, n);
  const std::string_view name = "content-range:";
  const size_t slash = line.find('/');
  if (n > name.size() && slash != std::string::npos &&
      std::equal(name.begin(), name.end(), line.begin(),
                 [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); }))
    *static_cast<uint64_t*>(length) = std::strtoull(line.c_str() + slash + 1, nullptr, 10);
  return n;
}
size_t discard(char*, size_t size, size_t count, void*) { return size * count; }

int backgroundReader, fileReader;  // seek previews', FFmpeg's (thumbnails)
struct MpvStream {
  std::shared_ptr<Stream> stream;
  std::atomic<bool>* failed;
};
int openMpv(void* data, char* uri, mpv_stream_cb_info* info) {
  int number = 0;
  long long id = 0;
  if (std::sscanf(uri, "stashscene://%d/%lld", &number, &id) != 2) return MPV_ERROR_LOADING_FAILED;
  Server server;
  {
    std::lock_guard lock(servedMutex);
    server = served;
  }
  if (!server.number || server.number != number) return MPV_ERROR_LOADING_FAILED;
  auto s = std::make_shared<Stream>();
  s->address = server.address;
  s->path = server.address.pathOn("/scene/" + std::to_string(id) + "/stream");
  if (!server.apiKey.empty())
    s->headers = curl_slist_append(nullptr, ("ApiKey: " + server.apiKey).c_str());
  s->background = data == &backgroundReader || data == &fileReader;
  // Seek previews' requests are as asked; FFmpeg's half a block at least:
  // fewer round trips, about as many bytes as SMB's (measured).
  if (s->background) s->chunk = data == &fileReader ? ReadAhead::kBlock / 2 : 0;
  // Its size, from one byte's range; stashPerform() also learns where the
  // server answers.
  CURL* curl = curl_easy_init();
  if (!curl) return MPV_ERROR_LOADING_FAILED;
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, s->headers);
  curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, contentRange);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &s->length);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
  if (!s->background) ++opening;
  const StashReply reply = stashPerform(curl, s->address, s->path, [&] { s->length = 0; }, false);
  if (!s->background) --opening;
  curl_easy_cleanup(curl);
  if (reply.code != CURLE_OK || reply.status != 206 || !s->length) {
    std::fprintf(stderr, "Stash stream: scene %lld didn't open: %s (HTTP %ld)\n", id,
                 curl_easy_strerror(reply.code), reply.status);
    return MPV_ERROR_LOADING_FAILED;
  }
  service().add(s);
  auto* p = new MpvStream{s, s->background ? nullptr : static_cast<std::atomic<bool>*>(data)};
  if (p->failed) *p->failed = false;
  info->cookie = p;
  info->read_fn = [](void* cookie, char* out, uint64_t count) -> int64_t {
    auto& p = *static_cast<MpvStream*>(cookie);
    auto& s = *p.stream;
    std::unique_lock lock(s.mutex);
    // As SMB's: FFmpeg's whole grown buffer at a time would read on past
    // where a file whose audio is stored apart jumps away.
    if (!s.background) count = std::min<uint64_t>(count, 128 * 1024);
    while (!s.cancelled && !s.failed) {
      if (s.position >= s.length) return 0;
      if (const size_t n = s.take(out, count)) {
        lock.unlock();
        if (!s.background) service().wake();
        return n;
      }
      // Seek previews and FFmpeg: one request at a time, for the block asked for.
      if (s.background && std::none_of(s.blocks.begin(), s.blocks.end(), [&](auto& b) {
            return !b.done && s.position >= b.offset && s.position < b.offset + b.bytes.size();
          })) {
        s.blocks.remove_if([](auto& b) { return !b.pending; });
        const uint64_t size =
            std::min({std::max(count, s.chunk), ReadAhead::kBlock, s.length - s.position});
        s.blocks.push_back({&s, s.generation, 0, s.position, std::vector<uint8_t>(size)});
        service().wake();
      }
      s.cv.wait_for(lock, std::chrono::milliseconds(50));
    }
    if (s.failed && !s.cancelled && p.failed) *p.failed = true;
    return -1;
  };
  info->seek_fn = [](void* cookie, int64_t offset) -> int64_t {
    auto& s = *static_cast<MpvStream*>(cookie)->stream;
    std::lock_guard lock(s.mutex);
    if (offset < 0 || s.cancelled || s.failed) return -1;
    if (uint64_t(offset) != s.position) {
      if (s.background) s.position = offset;
      else s.seek(offset);
      service().wake();
    }
    return offset;
  };
  info->size_fn = [](void* cookie) -> int64_t {
    return static_cast<MpvStream*>(cookie)->stream->length;
  };
  info->cancel_fn = [](void* cookie) {
    auto& s = *static_cast<MpvStream*>(cookie)->stream;
    s.cancelled = true;
    s.cv.notify_all();
    service().wake();
  };
  info->close_fn = [](void* cookie) {
    auto* p = static_cast<MpvStream*>(cookie);
    p->stream->cancelled = true;
    service().wake();
    delete p;  // the service drops its requests and the stream
  };
  return 0;
}
}  // namespace

void stashServe(const std::string& url, const std::string& apiKey, bool unverified) {
  std::lock_guard lock(servedMutex);
  knownFiles.clear();
  if (url.empty()) {
    served = Server();
    return;
  }
  const StashAddress address(url, unverified);
  int& number = serverNumbers[address.server];
  if (!number) number = static_cast<int>(serverNumbers.size());
  uint64_t hash = 14695981039346656037ull;  // FNV-1a, as StashOutbox's
  for (const unsigned char c : address.server) hash = (hash ^ c) * 1099511628211ull;
  served = {number, hash, address, apiKey};
}

std::string stashSceneLocator(int64_t id, std::string_view fileName) {
  std::lock_guard lock(servedMutex);
  if (!served.number) return {};
  return std::string(kScheme) + std::to_string(served.number) + "/" + std::to_string(id) + "/" +
         std::string(fileName);
}

bool isStashScene(std::string_view location) { return location.substr(0, kScheme.size()) == kScheme; }

int64_t stashSceneId(std::string_view location) {
  int number = 0;
  long long id = 0;
  if (!isStashScene(location) ||
      std::sscanf(std::string(location).c_str(), "stashscene://%d/%lld", &number, &id) != 2)
    return 0;
  std::lock_guard lock(servedMutex);
  return served.number && served.number == number ? id : 0;
}

void stashKnowFiles(std::unordered_map<int64_t, FileFingerprint> files) {
  std::lock_guard lock(servedMutex);
  knownFiles = std::move(files);
}

FileFingerprint stashSceneFile(std::string_view location) {
  const int64_t id = stashSceneId(location);
  std::lock_guard lock(servedMutex);
  const auto it = knownFiles.find(id);
  return id && it != knownFiles.end() ? it->second : FileFingerprint();
}

void addStashStreams(mpv_handle* mpv, bool background, std::atomic<bool>* failed) {
  mpv_stream_cb_add_ro(mpv, "stashscene", background ? static_cast<void*>(&backgroundReader) : failed,
                       openMpv);
}

bool openStashScene(const std::string& locator, mpv_stream_cb_info& info) {
  std::string uri = locator;
  return openMpv(&fileReader, uri.data(), &info) == 0;
}

namespace {
constexpr std::string_view kSource = "stash:";

// The server served now and the scene a source names on it; false if the
// source is another server's.
bool servedFor(std::string_view source, Server& server, long long& id) {
  unsigned long long hash = 0;
  if (std::sscanf(std::string(source).c_str(), "stash:%16llx/%lld/", &hash, &id) != 2) return false;
  std::lock_guard lock(servedMutex);
  server = served;
  return server.number && server.hash == hash;
}

curl_slist* keyHeader(const Server& server) {
  return server.apiKey.empty() ? nullptr
                               : curl_slist_append(nullptr, ("ApiKey: " + server.apiKey).c_str());
}

// An HTTP date (If-Range), in English whatever the locale.
std::string httpDate(int64_t seconds) {
  static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const time_t t = seconds;
  std::tm tm{};
  gmtime_r(&t, &tm);
  char text[40];
  std::snprintf(text, sizeof(text), "%s, %02d %s %04d %02d:%02d:%02d GMT", days[tm.tm_wday], tm.tm_mday,
                months[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
  return text;
}

size_t keep(char* data, size_t size, size_t count, void* to) {
  auto& bytes = *static_cast<std::string*>(to);
  const size_t n = size * count;
  if (bytes.size() + n > 64 * 1024) return 0;  // a range was asked for
  bytes.append(data, n);
  return n;
}
}  // namespace

std::string stashSource(std::string_view locator) {
  int number = 0;
  long long id = 0;
  if (!isStashScene(locator) ||
      std::sscanf(std::string(locator).c_str(), "stashscene://%d/%lld", &number, &id) != 2)
    return {};
  const size_t slash = locator.find('/', locator.find('/', kScheme.size()) + 1);
  const std::string_view name = slash == std::string_view::npos ? "" : locator.substr(slash + 1);
  std::lock_guard lock(servedMutex);
  if (!served.number || served.number != number) return {};
  char hash[17];
  std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(served.hash));
  return std::string(kSource) + hash + "/" + std::to_string(id) + "/" + std::string(name);
}

bool isStashSource(std::string_view source) { return source.substr(0, kSource.size()) == kSource; }

std::string stashSourceLocator(std::string_view source) {
  Server server;
  long long id = 0;
  if (!servedFor(source, server, id)) return {};
  return stashSceneLocator(id, source.substr(source.find_last_of('/') + 1));
}

FileFingerprint stashFingerprint(const std::string& source, bool& gone) {
  gone = false;
  FileFingerprint print;
  Server server;
  long long id = 0;
  if (!servedFor(source, server, id)) return print;
  CURL* curl = curl_easy_init();
  if (!curl) return print;
  curl_slist* headers = keyHeader(server);
  const std::string path = server.address.pathOn("/scene/" + std::to_string(id) + "/stream");
  std::string bytes;
  uint64_t length = 0;
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, contentRange);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &length);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, keep);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bytes);
  curl_easy_setopt(curl, CURLOPT_FILETIME, 1L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
  auto get = [&](uint64_t from, uint64_t to) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_RANGE, (std::to_string(from) + "-" + std::to_string(to)).c_str());
    const StashReply r = stashPerform(curl, server.address, path, [&] {
      bytes.clear();
      length = 0;
    }, false);
    return r.code == CURLE_OK ? r.status : 0;
  };
  // Its start, then its end if the file is still the same.
  const long status = get(0, 64 * 1024 - 1);
  gone = status == 404;
  const uint64_t size = length;
  const int64_t chunk = oshashChunk(size);
  curl_off_t date = -1;
  curl_easy_getinfo(curl, CURLINFO_FILETIME_T, &date);
  if (status == 206 && chunk && bytes.size() >= static_cast<size_t>(chunk)) {
    std::string ends = bytes.substr(0, chunk);
    if (date >= 0) headers = curl_slist_append(headers, ("If-Range: " + httpDate(date)).c_str());
    if (get(size - chunk, size - 1) == 206 && length == size && bytes.size() == static_cast<size_t>(chunk)) {
      ends += bytes;
      print.size = size;
      print.mtime = std::max<int64_t>(date, 0);
      print.oshash = oshashOfEnds(size, reinterpret_cast<const unsigned char*>(ends.data()));
    }
  }
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  return print;
}

bool stashAnswers() {
  Server server;
  {
    std::lock_guard lock(servedMutex);
    server = served;
  }
  if (!server.number) return true;
  CURL* curl = curl_easy_init();
  if (!curl) return true;
  curl_slist* headers = curl_slist_append(keyHeader(server), "Content-Type: application/json");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, R"({"query":"{version{version}}"})");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
  const StashReply r = stashPerform(curl, server.address, "/graphql", {}, false);
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  return r.code == CURLE_OK && r.status == 200;
}

std::vector<StashCaption> stashCaptions(std::string_view location) {
  std::vector<StashCaption> captions;
  Server server;
  long long id = 0;
  if (isStashSource(location)) {
    if (!servedFor(location, server, id)) return captions;
  } else {
    int number = 0;
    if (std::sscanf(std::string(location).c_str(), "stashscene://%d/%lld", &number, &id) != 2)
      return captions;
    std::lock_guard lock(servedMutex);
    server = served;
    if (!server.number || server.number != number) return captions;
  }
  CURL* curl = curl_easy_init();
  if (!curl) return captions;
  curl_slist* headers = curl_slist_append(keyHeader(server), "Content-Type: application/json");
  std::string bytes;
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* data, size_t size, size_t count, void* to) {
    auto& bytes = *static_cast<std::string*>(to);
    if (bytes.size() > 16 * 1024 * 1024) return size_t(0);
    bytes.append(data, size * count);
    return size * count;
  });
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bytes);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
  auto get = [&](const std::string& path) {
    const StashReply r = stashPerform(curl, server.address, path, [&] { bytes.clear(); }, false);
    return r.code == CURLE_OK && r.status == 200;
  };
  const std::string query = R"({findScene(id:")" + std::to_string(id) +
                            R"("){captions{language_code caption_type}}})";
  const std::string body = nlohmann::json{{"query", query}}.dump();
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  nlohmann::json list;
  if (get("/graphql")) list = nlohmann::json::parse(bytes, nullptr, false);
  for (const char* key : {"data", "findScene", "captions"})
    list = list.is_object() && list.contains(key) ? nlohmann::json(list[key]) : nlohmann::json();
  curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  for (auto c : list.is_array() ? list : nlohmann::json::array()) {
    if (!c.is_object() || !c["language_code"].is_string() || !c["caption_type"].is_string()) continue;
    StashCaption caption{c["language_code"].get<std::string>(), c["caption_type"].get<std::string>(), {}};
    char* lang = curl_easy_escape(curl, caption.language.c_str(), 0);
    char* type = curl_easy_escape(curl, caption.type.c_str(), 0);
    const bool got = get(server.address.pathOn("/scene/" + std::to_string(id) + "/caption?lang=" +
                                               lang + "&type=" + type));
    curl_free(lang);
    curl_free(type);
    if (!got || bytes.empty()) continue;  // Stash answers nothing for one it can't read
    if (caption.language == "00") caption.language.clear();
    caption.text = std::move(bytes);
    captions.push_back(std::move(caption));
  }
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  return captions;
}

struct StashReader::Impl {
  std::shared_ptr<Stream> stream;
};

std::unique_ptr<StashReader> StashReader::open(const std::string& source, uint64_t offset,
                                               const FileFingerprint& file) {
  Server server;
  long long id = 0;
  if (!servedFor(source, server, id)) return nullptr;
  auto s = std::make_shared<Stream>();
  s->address = server.address;
  s->path = server.address.pathOn("/scene/" + std::to_string(id) + "/stream");
  s->headers = keyHeader(server);
  if (file.mtime > 0) s->headers = curl_slist_append(s->headers, ("If-Range: " + httpDate(file.mtime)).c_str());
  s->length = file.size;
  s->position = offset;
  s->chunk = 4 * ReadAhead::kBlock;
  std::unique_ptr<StashReader> reader(new StashReader);
  reader->impl_ = std::make_unique<Impl>();
  reader->impl_->stream = s;
  service().add(s);
  return reader;
}

StashReader::~StashReader() { cancel(); }

int64_t StashReader::read(void* out, uint64_t count) {
  auto& s = *impl_->stream;
  std::unique_lock lock(s.mutex);
  while (!s.cancelled && !s.failed) {
    if (s.position >= s.length) return 0;
    if (const size_t n = s.take(out, count)) {
      lock.unlock();
      service().wake();
      return n;
    }
    s.cv.wait_for(lock, std::chrono::milliseconds(50));
  }
  return -1;
}

void StashReader::cancel() {
  auto& s = *impl_->stream;
  s.cancelled = true;
  s.cv.notify_all();
  service().wake();
}

bool StashReader::changed() const {
  auto& s = *impl_->stream;
  std::lock_guard lock(s.mutex);
  return s.changed;
}
