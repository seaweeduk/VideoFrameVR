#include "stash_http.h"

#include "run_log.h"  // urlHost
#include "strings.h"

#include <arpa/inet.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>

namespace {

// Where each server answered instead of its own address, in this run.
std::mutex answeredMutex;
std::map<std::string, std::string> answeredAt;

void remember(const std::string& server, const std::string& base) {
  std::lock_guard<std::mutex> lock(answeredMutex);
  answeredAt[server] = base;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trimmed(const std::string& s) {
  const size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return std::string();
  return s.substr(start, s.find_last_not_of(" \t\r\n") - start + 1);
}

bool endsWith(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// Where a URL's path starts: after "scheme://authority" (the URL's size if
// it has no path).
size_t pathStart(const std::string& url) {
  const size_t scheme = url.find("://");
  const size_t start = scheme == std::string::npos ? 0 : scheme + 3;
  return std::min(url.find_first_of("/?#", start), url.size());
}

// The path alone, without its query.
std::string pathOf(const std::string& url) {
  const size_t start = pathStart(url);
  return url.substr(start, url.find_first_of("?#", start) - start);
}

bool sameHost(const std::string& a, const std::string& b) {
  const std::string host = lower(urlHost(a));
  return !host.empty() && host == lower(urlHost(b));
}

// What Stash (or a proxy in front of it) answers with; anything else is
// something else answering at that address.
bool answeredAsStash(const StashReply& r) {
  return r.code == CURLE_OK &&
         (r.status == 200 || r.status == 206 || r.status == 401 || r.status == 403 || r.status == 422);
}

// Whether "user@host:port" (a URL's authority) has a port.
bool hasPort(const std::string& authority) {
  const size_t at = authority.rfind('@');
  const std::string hostPort = at == std::string::npos ? authority : authority.substr(at + 1);
  const size_t bracket = hostPort.rfind(']');
  const size_t colon = hostPort.rfind(':');
  return colon != std::string::npos && (bracket == std::string::npos || colon > bracket);
}

bool redirect(long status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

}  // namespace

StashAddress::StashAddress(const std::string& typed, bool unverified) : unverified(unverified) {
  const std::string t = trimmed(typed);
  if (t.empty()) return;
  const bool scheme = t.find("://") != std::string::npos;
  server = scheme ? t : "http://" + t;
  while (!server.empty() && server.back() == '/') server.pop_back();
  if (endsWith(server, "/graphql")) server.resize(server.size() - 8);
  while (!server.empty() && server.back() == '/') server.pop_back();
  if (scheme) return;
  // Neither scheme nor path: the port Stash has by default, if none typed.
  const size_t path = pathStart(server);
  if (path != server.size()) return;
  if (server.size() > 7 && !hasPort(server.substr(7))) fallback = server + ":9999";
}

std::vector<std::string> stashTries(const std::string& typed) {
  const std::string t = trimmed(typed);
  if (t.empty()) return {};
  std::vector<std::string> tries;
  if (t.find("://") != std::string::npos)
    tries.push_back(StashAddress(t).server);
  else
    tries = {StashAddress("https://" + t).server, StashAddress("http://" + t).server};
  for (size_t i = 0, n = tries.size(); i < n; ++i) {
    const std::string& url = tries[i];
    const size_t start = url.find("://") + 3, path = pathStart(url);
    if (path > start && !hasPort(url.substr(start, path - start)))
      tries.push_back(url.substr(0, path) + ":9999" + url.substr(path));
  }
  return tries;
}

std::string StashAddress::base() const {
  std::lock_guard<std::mutex> lock(answeredMutex);
  const auto it = answeredAt.find(server);
  return it == answeredAt.end() ? server : it->second;
}

std::string StashAddress::pathOn(const std::string& url) const {
  std::string path = url.find("://") != std::string::npos ? url.substr(pathStart(url)) : url;
  if (path.empty() || path[0] != '/') path.insert(0, "/");
  const std::string sub = pathOf(server);
  if (!sub.empty() && path.compare(0, sub.size(), sub) == 0 &&
      (path.size() == sub.size() || path[sub.size()] == '/' || path[sub.size()] == '?'))
    path.erase(0, sub.size());
  if (path.empty() || path[0] != '/') path.insert(0, "/");
  return path;
}

bool StashAddress::privateHost() const {
  const std::string host = lower(urlHost(server));
  if (host.empty()) return false;
  in_addr v4{};
  if (inet_pton(AF_INET, host.c_str(), &v4) == 1) {
    const uint32_t a = ntohl(v4.s_addr);
    auto in = [a](uint32_t net, int bits) { return (a >> (32 - bits)) == (net >> (32 - bits)); };
    return in(0x0A000000, 8) || in(0xAC100000, 12) || in(0xC0A80000, 16) ||  // RFC 1918
           in(0x64400000, 10) ||                                             // Tailscale, CGNAT
           in(0x7F000000, 8) || in(0xA9FE0000, 16);                          // loopback, link-local
  }
  in6_addr v6{};
  if (inet_pton(AF_INET6, host.c_str(), &v6) == 1) {
    const uint8_t* b = v6.s6_addr;
    static const uint8_t loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    return std::memcmp(b, loopback, 16) == 0 || (b[0] & 0xFE) == 0xFC ||  // unique local
           (b[0] == 0xFE && (b[1] & 0xC0) == 0x80);                       // link-local
  }
  return host == "localhost" || host.find('.') == std::string::npos || endsWith(host, ".local") ||
         endsWith(host, ".ts.net") || endsWith(host, ".localhost");
}

void stashSetUp(CURL* curl, const StashAddress& address) {
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
  if (address.privateHost()) curl_easy_setopt(curl, CURLOPT_PROXY, "");
  if (address.unverified) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  }
}

StashReply stashPerform(CURL* curl, const StashAddress& address, const std::string& path,
                        const std::function<void()>& restart, bool pathChange,
                        const std::function<CURLcode(CURL*)>& perform) {
  stashSetUp(curl, address);
  auto run = [&](std::string base) {
    StashReply r;
    std::string url = base + path;
    for (int hop = 0;; ++hop) {
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      if (restart) restart();
      r.code = perform ? perform(curl) : curl_easy_perform(curl);
      r.status = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);
      if (r.code != CURLE_OK || !redirect(r.status) || hop == 5) return r;
      const char* location = nullptr;
      curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &location);
      if (!location) return r;
      const std::string target = location;
      if (!sameHost(target, url)) {
        r.elsewhere = true;
        return r;
      }
      if (!pathChange && pathOf(target) != pathOf(url)) return r;
      if (r.status == 303) curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
      // The same request at another address (http → https): later ones
      // go there at once.
      const std::string rest = url.substr(std::min(base.size(), url.size()));
      if (!rest.empty() && target.size() > rest.size() && endsWith(target, rest)) {
        base = target.substr(0, target.size() - rest.size());
        remember(address.server, base);
      }
      url = target;
    }
  };
  const std::string base = address.base();
  const StashReply first = run(base);
  if (address.fallback.empty() || base != address.server || answeredAsStash(first)) return first;
  const StashReply other = run(address.fallback);
  if (answeredAsStash(other)) remember(address.server, address.fallback);
  return other;
}

std::string stashUnreachable(CURLcode code) {
  const StashSetupStrings& t = strings().stashSetup;
  switch (code) {
    case CURLE_COULDNT_CONNECT: return t.refused;
    case CURLE_COULDNT_RESOLVE_HOST: return t.noHost;
    case CURLE_OPERATION_TIMEDOUT: return t.timeout;
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_ISSUER_ERROR:
      return t.certificate;
    case CURLE_SSL_CONNECT_ERROR: return t.noSecure;
    case CURLE_URL_MALFORMAT:
    case CURLE_UNSUPPORTED_PROTOCOL:
      return t.badAddress;
    default: return t.unreachable;
  }
}

bool stashUntrusted(CURLcode code) {
  return code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_ISSUER_ERROR;
}

std::string stashHost(const std::string& typed) {
  return lower(urlHost(StashAddress(typed).server));
}
