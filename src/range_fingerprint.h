#pragma once

#include "oshash.h"

#include <curl/curl.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

// A file served over HTTP fingerprinted as Stash does (oshash.h) without
// downloading it: a range request for its first 64 KiB, which also gives
// its size, then one for its last 64 KiB, If-Range the first answer, so a
// file that changed in between gives no fingerprint. Both must answer 206
// with the same size; at most 64 KiB is read from each.

// An HTTP date (If-Range), in English whatever the locale.
std::string httpDate(int64_t seconds);

// What the two requests found: the fingerprint (no oshash if there is
// none), and the first answer's status and whether it had a byte in it.
struct RangeAnswer {
  FileFingerprint print;
  long firstStatus = 0;
  bool firstEmpty = true;
};
// Runs one request on `curl`, set up for it but for its URL where the
// caller sets that: calls `restart` before each try (a redirect followed
// by hand is one), and returns the final HTTP status, 0 if it failed.
using RangePerform = std::function<long(CURL* curl, const std::function<void()>& restart)>;
// The two requests on `curl`, with `headers` (If-Range is appended to
// them: the caller frees the list). If-Range is the first answer's date
// (Last-Modified). With `anyServer` (a plain link, not a media server's)
// it is the strong ETag when there is one, and more is asked of the
// answers: a date or an ETag to send, a file of 128 KiB or more, each
// Content-Range whole and exactly the range asked for, and no other ETag
// (weak or strong), date or final address the second time. Blocks: worker
// threads only.
RangeAnswer rangeFingerprint(CURL* curl, curl_slist*& headers, const RangePerform& perform, bool anyServer);

// An http:// or https:// link: one linkFingerprint() reads.
bool isHttpLink(const std::string& url);
// A link's file (http:// or https:// only), from any server: certificates
// checked, redirects followed. No oshash for another scheme, a server that
// doesn't answer the ranges asked for with a size and a date or an ETag, a
// file under 128 KiB or once `cancel` is set. At most 3 s to connect and 10 s for each request. Blocks: worker
// threads only.
FileFingerprint linkFingerprint(const std::string& url, const std::atomic<bool>& cancel);
