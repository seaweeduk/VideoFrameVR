#include "media_server.h"

#include <curl/curl.h>

// Each kind's, in its own file.
const MediaServer& stashServer();
const MediaServer& xbvrServer();

// Never taken down, like the servers themselves: a request still under way
// on its own thread as the app quits may ask them.
const std::vector<const MediaServer*>& mediaServers() {
  static const auto& all = *new std::vector<const MediaServer*>{&stashServer(), &xbvrServer()};
  return all;
}

std::string serverIdentity(ServerKind kind, const std::string& server) {
  return originalKind(kind) ? server : std::string(mediaServer(kind).id()) + " " + server;
}

bool originalKind(ServerKind kind) { return mediaServers().front()->kind() == kind; }

const MediaServer& mediaServer(ServerKind kind) {
  for (const MediaServer* server : mediaServers())
    if (server->kind() == kind) return *server;
  return *mediaServers().front();
}

ServerKind serverKindOf(std::string_view id) {
  for (const MediaServer* server : mediaServers())
    if (id == server->id()) return server->kind();
  return mediaServers().front()->kind();
}

std::string mediaServerNames(const std::string& between) {
  std::string names;
  for (const MediaServer* server : mediaServers()) {
    if (!names.empty()) names += between;
    names += server->name();
  }
  return names;
}

curl_slist* MediaServer::authorized(curl_slist* headers, const std::string&) const { return headers; }

namespace {

size_t keep(char* data, size_t size, size_t count, void* to) {
  auto& bytes = *static_cast<std::string*>(to);
  if (bytes.size() > 16 * 1024 * 1024) return 0;
  bytes.append(data, size * count);
  return size * count;
}

}  // namespace

ServerFetch::ServerFetch(const MediaServer& server, const ServerLink& link, long seconds)
    : link_(link), curl_(curl_easy_init()), seconds_(seconds) {
  headers_ = server.authorized(nullptr, link.apiKey);
  jsonHeaders_ = curl_slist_append(server.authorized(nullptr, link.apiKey),
                                   "Content-Type: application/json");
}

ServerFetch::~ServerFetch() {
  if (curl_) curl_easy_cleanup(curl_);
  curl_slist_free_all(headers_);
  curl_slist_free_all(jsonHeaders_);
}

long ServerFetch::perform(const std::string& path, std::string& bytes) {
  bytes.clear();
  curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, keep);
  curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &bytes);
  curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
  curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, seconds_ * 1000L);
  const StashReply r = stashPerform(curl_, link_.address, path, [&] { bytes.clear(); }, false);
  return r.code == CURLE_OK ? r.status : 0;
}

long ServerFetch::get(const std::string& path, std::string& bytes) {
  if (!curl_) return 0;
  curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers_);
  return perform(path, bytes);
}

long ServerFetch::post(const std::string& path, const std::string& json, std::string& bytes) {
  if (!curl_) return 0;
  curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, jsonHeaders_);
  curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, static_cast<long>(json.size()));
  curl_easy_setopt(curl_, CURLOPT_COPYPOSTFIELDS, json.c_str());
  return perform(path, bytes);
}

std::string ServerFetch::escaped(const std::string& text) const {
  char* e = curl_easy_escape(curl_, text.c_str(), static_cast<int>(text.size()));
  const std::string out = e ? e : "";
  curl_free(e);
  return out;
}
