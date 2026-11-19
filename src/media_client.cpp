#include "media_client.h"

#include "run_log.h"
#include "strings.h"

#include <curl/curl.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>

namespace {

// Connecting may take this long; a transfer that stalls (under 1 KB/s) for
// kStallSeconds is given up, as is one that takes longer than kMaxSeconds.
// Every scene at once (the sync's requests, write-back's activity) may take
// a big library on a slow computer minutes before the server sends
// anything: those wait longer, and can be stopped meanwhile.
constexpr long kConnectSeconds = 10;
constexpr long kStallSeconds = 20;
constexpr long kMaxSeconds = 180;
constexpr long kSlowStallSeconds = 600;
constexpr long kSlowMaxSeconds = 3600;
// Each of Connect's tries (check()).
constexpr long kQuickConnectSeconds = 3, kQuickMaxSeconds = 8;

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

}  // namespace

MediaClient::MediaClient(const MediaServer& kind, const std::string& url, std::string apiKey,
                         bool unverified)
    : kind_(kind), typed_(url), address_(url, unverified), apiKey_(std::move(apiKey)) {
  // curl's messages name the host: hidden before any request, also for an
  // address only tried in setup or Settings. A scheme typed alone has none.
  if (address_.server.find("://") != std::string::npos)
    hideInRunLog(urlHost(address_.server), kHiddenServer);
}

bool MediaClient::send(const Call& call, long& status, std::string& body, std::string& error,
                       StashProblem& problem) {
  reached_ = false;
  status = 0;
  problem = StashProblem::Other;
  CURL* curl = curl_easy_init();
  if (!curl) {
    error = std::string(kind_.name()) + ": no HTTP client";
    return false;
  }
  curl_slist* headers = curl_slist_append(nullptr, "Accept: application/json");
  if (call.post) headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = kind_.authorized(headers, apiKey_);
  char message[CURL_ERROR_SIZE] = "";
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  if (call.post) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, call.body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(call.body.size()));
  }
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendToString);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // compressed if the server can
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, quick_ ? kQuickConnectSeconds : kConnectSeconds);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, call.slow ? kSlowStallSeconds : kStallSeconds);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                   quick_ ? kQuickMaxSeconds : call.slow ? kSlowMaxSeconds : kMaxSeconds);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);  // a server gone while it works is noticed
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  Transfer transfer{&cancelled_, &received_};
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, message);
  StashAddress at = address_;
  if (call.once) at.fallback.clear();
  const StashReply reply = stashPerform(curl, at, call.path, [&] {
    body.clear();
    message[0] = '\0';
  });
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  reached_ = reply.code == CURLE_OK;
  if (reply.code != CURLE_OK) {
    if (reply.code == CURLE_ABORTED_BY_CALLBACK) {
      error = std::string(kind_.name()) + ": stopped";
      return false;
    }
    error = stashUnreachable(reply.code, said());
    if (stashUntrusted(reply.code)) problem = StashProblem::Certificate;
    std::fprintf(stderr, "%s: %s\n", kind_.name(), *message ? message : curl_easy_strerror(reply.code));
    return false;
  }
  if (reply.elsewhere) {
    error = said().elsewhere;
    return false;
  }
  status = reply.status;
  return true;
}

const StashSetupStrings& MediaClient::said() const {
  return (ownName_ ? stringsNamed(kind_.name()) : strings()).stashSetup;
}

std::string MediaClient::httpProblem(long status, const std::string& body) const {
  const StashSetupStrings& t = said();
  // A server that speaks only https, asked over http (Go's and nginx's 400).
  if (status == 400 && address_.base().compare(0, 7, "http://") == 0 && mentions(body, "https"))
    return t.tryHttps;
  if (status == 403) return t.forbidden;
  if (status == 404 || status == 405) return t.notStash;
  return printed(t.httpError, static_cast<int>(status));
}

bool MediaClient::editTags(int64_t, const std::vector<int64_t>&, const std::vector<int64_t>&) {
  error_ = std::string(kind_.name()) + ": tags can't be changed";
  return false;
}

bool MediaClient::signIn(const std::string&, const std::string&, std::string& apiKey, bool& refused) {
  apiKey.clear();
  refused = false;
  error_ = std::string(kind_.name()) + ": no sign-in";
  return false;
}

MediaClient::Answer MediaClient::check() {
  const StashSetupStrings& t = said();
  quick_ = true;  // short timeouts, until it returns
  struct Reset {
    bool& quick;
    ~Reset() { quick = false; }
  } reset{quick_};
  foreign_ = false;
  int best = -1;  // how far the try whose error() is kept got
  std::string error;
  StashProblem problem = StashProblem::Other;
  const bool unverified = address_.unverified;
  for (const std::string& url : stashTries(typed_)) {
    address_ = StashAddress(url, unverified);
    error_.clear();
    const Answer answer = identify();
    if (answer != Answer::Unreached) return answer;
    if (cancelled_) break;
    // Nothing there, as against a server that answers in a way that can't be
    // used (its certificate, https on plain http, an address that isn't one),
    // or answers but not as this kind: the first that got furthest says why.
    // A certificate that isn't trusted comes first, as it can be accepted.
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
  foreign_ = best == 2;
  return best > 0 ? Answer::Failed : Answer::Unreached;
}

void ServerSearch::cancel() {
  std::lock_guard<std::mutex> lock(mutex_);
  cancelled_ = true;
  if (asking_) asking_->cancel();
}

bool ServerSearch::asking(MediaClient* client) {
  std::lock_guard<std::mutex> lock(mutex_);
  asking_ = cancelled_ ? nullptr : client;
  return !cancelled_;
}

FoundServer findServer(const std::string& url, bool unverified, ServerKind first, bool ownNames,
                       ServerSearch* search) {
  FoundServer found;
  found.kind = first;
  std::vector<const MediaServer*> kinds{&mediaServer(first)};
  for (const MediaServer* kind : mediaServers())
    if (kind->kind() != first) kinds.push_back(kind);
  for (const MediaServer* kind : kinds) {
    const std::unique_ptr<MediaClient> client = kind->client(url, std::string(), unverified);
    if (ownNames) client->nameOwnKind();
    if (search && !search->asking(client.get())) break;
    const MediaClient::Answer answer = client->check();
    if (search && !search->asking(nullptr)) break;
    if (answer == MediaClient::Answer::Open || answer == MediaClient::Answer::NeedsSignIn) {
      found.kind = kind->kind();
      found.found = true;
      found.answer = answer;
      found.url = client->server();
      found.untrusted = false;
      found.error.clear();
      return found;
    }
    // Why not, as the first kind says it, unless a later one is there and
    // can't be used (too old).
    const bool own = answer == MediaClient::Answer::Failed && !client->foreign();
    if (kind == kinds.front() || own) {
      found.answer = answer;
      found.untrusted = answer == MediaClient::Answer::Failed &&
                        client->problem() == StashProblem::Certificate;
      found.error = client->error();
    }
    // Its own way of failing, a certificate, or nothing there at all: no
    // other kind is asked.
    if (!client->foreign()) break;
  }
  return found;
}
