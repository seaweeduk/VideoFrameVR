#include "range_fingerprint.h"

#include "stash_http.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string_view>

namespace {

constexpr uint64_t kEnd = 64 * 1024;  // bytes read from each end

bool startsWithNoCase(std::string_view text, std::string_view start) {
  if (text.size() < start.size()) return false;
  for (size_t i = 0; i < start.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(text[i])) != std::tolower(static_cast<unsigned char>(start[i])))
      return false;
  return true;
}

// What an answer's headers say: its file's size (Content-Range's) and its
// strong ETag. Each answer's own: a redirect's are forgotten.
struct Headers {
  uint64_t length = 0;
  // Content-Range read whole ("bytes FROM-TO/TOTAL"), for a plain link's.
  uint64_t from = 0, to = 0, total = 0;
  bool range = false;   // ... it was one
  std::string etag;
  std::string anyEtag;  // weak or strong
};

// The digits `text` starts with, into `n` without overflow; `text` moves
// past them.
bool readNumber(std::string_view& text, uint64_t& n) {
  size_t i = 0;
  n = 0;
  for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
    const uint64_t digit = text[i] - '0';
    if (n > (UINT64_MAX - digit) / 10) return false;
    n = n * 10 + digit;
  }
  text.remove_prefix(i);
  return i > 0;
}

// "bytes FROM-TO/TOTAL", in any case, and nothing after it but space.
bool readRange(std::string_view text, Headers& h) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
  if (!startsWithNoCase(text, "bytes") || text.size() < 6 || text[5] != ' ') return false;
  text.remove_prefix(6);
  while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
  uint64_t from = 0, to = 0, total = 0;
  if (!readNumber(text, from) || text.empty() || text.front() != '-') return false;
  text.remove_prefix(1);
  if (!readNumber(text, to) || text.empty() || text.front() != '/') return false;
  text.remove_prefix(1);
  if (!readNumber(text, total) || !text.empty() || from > to || to >= total) return false;
  h.from = from;
  h.to = to;
  h.total = total;
  return true;
}

size_t readHeader(char* data, size_t size, size_t count, void* to) {
  auto& h = *static_cast<Headers*>(to);
  const size_t n = size * count;
  std::string_view line(data, n);
  if (startsWithNoCase(line, "HTTP/")) {
    h = {};
  } else if (startsWithNoCase(line, "content-range:")) {
    const size_t slash = line.find('/');
    if (slash != std::string_view::npos) h.length = std::strtoull(std::string(line.substr(slash + 1)).c_str(), nullptr, 10);
    h.range = readRange(line.substr(14), h);
  } else if (startsWithNoCase(line, "etag:")) {
    line.remove_prefix(5);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.remove_prefix(1);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.remove_suffix(1);
    if (line.size() <= 256) h.anyEtag = line;
    // A weak one can't be If-Range's.
    if (line.size() >= 2 && line.size() <= 256 && line.front() == '"' && line.back() == '"') h.etag = line;
  }
  return n;
}

int abortedByCancel(void* cancel, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  return static_cast<const std::atomic<bool>*>(cancel)->load() ? 1 : 0;
}

}  // namespace

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

RangeAnswer rangeFingerprint(CURL* curl, curl_slist*& headers, const RangePerform& perform, bool anyServer) {
  RangeAnswer answer;
  Headers h;
  std::string bytes;
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, readHeader);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &h);
  // A range was asked for.
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stashAppend<kEnd>);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bytes);
  curl_easy_setopt(curl, CURLOPT_FILETIME, 1L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
  auto get = [&](uint64_t from, uint64_t to) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_RANGE, (std::to_string(from) + "-" + std::to_string(to)).c_str());
    const long status = perform(curl, [&] {
      bytes.clear();
      h = {};
    });
    // Any server may answer another range than the one asked for.
    if (anyServer && status == 206 &&
        (!h.range || h.total != h.length || h.from != from || h.to != std::min(to, h.length - 1)))
      return 0L;
    return status;
  };
  auto address = [&] {
    const char* url = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &url);
    return std::string(url ? url : "");
  };
  // Its start, then its end if the file is still the same.
  answer.firstStatus = get(0, kEnd - 1);
  answer.firstEmpty = bytes.empty();
  const uint64_t size = h.length;
  const int64_t chunk = oshashChunk(size);
  curl_off_t date = -1;
  curl_easy_getinfo(curl, CURLINFO_FILETIME_T, &date);
  const std::string etag = h.etag, anyEtag = h.anyEtag, firstAddress = anyServer ? address() : std::string();
  if (anyServer && etag.empty() && date < 0) return answer;  // nothing to tell a change by
  if (anyServer && size < 2 * kEnd) return answer;            // too small to be told apart
  if (anyServer && bytes.compare(0, 7, "#EXTM3U") == 0) return answer;  // a playlist, not the video
  if (answer.firstStatus == 206 && chunk && bytes.size() >= static_cast<size_t>(chunk)) {
    std::string ends = bytes.substr(0, chunk);
    if (anyServer && !etag.empty()) headers = curl_slist_append(headers, ("If-Range: " + etag).c_str());
    else if (date >= 0) headers = curl_slist_append(headers, ("If-Range: " + httpDate(date)).c_str());
    // The second answer must not say the file is another: another ETag
    // (weak or strong), date or address.
    const auto sameFile = [&] {
      curl_off_t secondDate = -1;
      curl_easy_getinfo(curl, CURLINFO_FILETIME_T, &secondDate);
      return (h.anyEtag.empty() || h.anyEtag == anyEtag) && (secondDate < 0 || date < 0 || secondDate == date) &&
             address() == firstAddress;
    };
    if (get(size - chunk, size - 1) == 206 && h.length == size && bytes.size() == static_cast<size_t>(chunk) &&
        (!anyServer || sameFile())) {
      ends += bytes;
      answer.print.size = size;
      answer.print.mtime = std::max<int64_t>(date, 0);
      answer.print.oshash = oshashOfEnds(size, reinterpret_cast<const unsigned char*>(ends.data()));
    }
  }
  return answer;
}

bool isHttpLink(const std::string& url) {
  return startsWithNoCase(url, "http://") || startsWithNoCase(url, "https://");
}

FileFingerprint linkFingerprint(const std::string& url, const std::atomic<bool>& cancel) {
  if (!isHttpLink(url)) return {};
  CURL* curl = curl_easy_init();
  if (!curl) return {};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abortedByCancel);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<std::atomic<bool>*>(&cancel));
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_slist* headers = nullptr;
  const RangeAnswer answer = rangeFingerprint(curl, headers, [&](CURL* c, const std::function<void()>& restart) {
    if (cancel) return 0L;
    restart();
    long status = 0;
    if (curl_easy_perform(c) != CURLE_OK) return 0L;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    return status;
  }, true);
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  if (cancel || answer.print.size < 2 * kEnd) return {};
  return answer.print;
}
