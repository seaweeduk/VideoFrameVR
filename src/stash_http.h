#pragma once

#include <curl/curl.h>

#include <functional>
#include <string>
#include <vector>

// What stopped a request to Stash, for what the app says about it. Only
// the kinds the menu acts on differ from Other: their text is in the error.
enum class StashProblem {
  None,
  TooOld,       // below 0.31.0: nothing else is asked
  NeedsSignIn,  // a login, and no API key
  KeyRefused,   // the API key isn't Stash's (any more)
  Certificate,  // its certificate isn't trusted, and may be accepted anyway
  Other,
};

// Stash's address as typed, made whole (docs/stash.md, "Connection"):
// spaces trimmed, http:// when no scheme was typed, no trailing slash or
// /graphql. Typed with neither a scheme nor a port ("192.168.1.20"), Stash's
// own port 9999 is tried as well when nothing, or something else than
// Stash, answers on port 80. `unverified`: its certificate is accepted
// without being verified, as the user chose for its host.
struct StashAddress {
  explicit StashAddress(const std::string& typed = std::string(), bool unverified = false);
  std::string server;    // "http://host[:port][/sub-path]": the server's identity
  std::string fallback;  // "http://host:9999", or empty
  bool unverified = false;

  // Where the server answered last in this run: the fallback, or the
  // address a redirect took a request to (http → https), else `server`.
  std::string base() const;
  // An address Stash gave (an image's) as a path and query on the server,
  // for stashPerform(): Stash builds absolute addresses from what its proxy
  // passes on (or its external_host), so they may name a host that can't
  // be reached from here, such as localhost:9999. Its host is dropped, and
  // a typed sub-path ("https://host/stash") is not repeated.
  std::string pathOn(const std::string& url) const;
  // A private or Tailscale address (RFC 1918, 100.64.0.0/10, loopback,
  // link-local, .local, .ts.net, or a name without a dot): no proxy.
  bool privateHost() const;
};

// The addresses Connect tries for one typed, in order: as typed with a
// scheme, else with https:// then http://; then, with no port typed, the
// same on Stash's port 9999. Each made whole as StashAddress makes it.
std::vector<std::string> stashTries(const std::string& typed);

struct StashReply {
  CURLcode code = CURLE_FAILED_INIT;
  long status = 0;
  bool elsewhere = false;  // redirected to another host: not followed
};

// Performs a request set up on `curl` but for its URL, proxy and redirects,
// at address.base() + path (path and query, from "/"). Without a proxy for a
// private address. Redirects are followed only to the same host, so the
// ApiKey header never goes elsewhere; with `pathChange` false only to the
// same path (http → https). A POST stays a POST after 301, 302, 307 and
// 308. `restart` empties what a try wrote before the next one. `perform`
// runs the transfer (curl_easy_perform when empty).
StashReply stashPerform(CURL* curl, const StashAddress& address, const std::string& path,
                        const std::function<void()>& restart, bool pathChange = true,
                        const std::function<CURLcode(CURL*)>& perform = {});
// stashPerform()'s setup alone, for a request to address.base() that runs
// elsewhere (a curl multi's): no redirects followed, http and https only, no
// proxy for a private address, the certificate not verified if `unverified`.
void stashSetUp(CURL* curl, const StashAddress& address);

// A failed transfer in a few words (strings().stashSetup), not libcurl's:
// nothing answers, no such host, no answer in time, the certificate.
std::string stashUnreachable(CURLcode code);
// Whether a transfer failed on the server's certificate alone (self-signed,
// another issuer, expired, another name), which the user may accept.
bool stashUntrusted(CURLcode code);
// The host a typed address names, in lower case: what a certificate
// accepted unverified is kept for (StashSettings::unverifiedHost).
std::string stashHost(const std::string& typed);
