#include "location.h"

#include "file_util.h"
#include "remote.h"
#include "stash_stream.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string_view>

namespace {

constexpr size_t kMaxName = 255;

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// %XX as its byte; with `plus`, + as a space too (a query's values).
std::string percentDecoded(std::string_view s, bool plus) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    const int high = s[i] == '%' && i + 2 < s.size() ? hexValue(s[i + 1]) : -1;
    const int low = high >= 0 ? hexValue(s[i + 2]) : -1;
    if (low >= 0) {
      out += static_cast<char>(high * 16 + low);
      i += 2;
    } else {
      out += plus && s[i] == '+' ? ' ' : s[i];
    }
  }
  return out;
}

bool sameNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != b[i]) return false;
  return true;
}

// A decoded name as it may be shown: what follows its last / or \, none if
// it holds a control character (C0, DEL or C1) or isn't UTF-8, and at most
// kMaxName bytes, cut between characters.
std::string checkedName(std::string name) {
  name.erase(0, name.find_last_of("/\\") + 1);
  if (!validUtf8(name)) return {};
  for (size_t i = 0; i < name.size(); ++i) {
    const auto c = static_cast<unsigned char>(name[i]);
    if (c < 0x20 || c == 0x7f) return {};
    if (c == 0xc2 && i + 1 < name.size() && static_cast<unsigned char>(name[i + 1]) < 0xa0) return {};
  }
  if (name.size() > kMaxName) {
    size_t end = kMaxName;
    while (end > 0 && (static_cast<unsigned char>(name[end]) & 0xc0) == 0x80) --end;
    name.resize(end);
  }
  return name;
}

// The file a Content-Disposition names, checked: its filename*
// (charset'language'encoded) if usable, else its filename, quoted or not.
std::string dispositionName(std::string_view d) {
  std::string plain, extended;
  for (size_t at = 0; at < d.size();) {
    while (at < d.size() && (d[at] == ' ' || d[at] == '\t' || d[at] == ';')) ++at;
    const size_t eq = d.find_first_of("=;", at);
    std::string_view key = d.substr(at, eq == std::string_view::npos ? eq : eq - at);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.remove_suffix(1);
    if (eq == std::string_view::npos || d[eq] == ';') {
      at = eq;
      continue;
    }
    std::string value;
    at = eq + 1;
    while (at < d.size() && (d[at] == ' ' || d[at] == '\t')) ++at;
    if (at < d.size() && d[at] == '"') {
      for (++at; at < d.size() && d[at] != '"'; ++at) {
        if (d[at] == '\\' && at + 1 < d.size()) ++at;
        value += d[at];
      }
      if (at < d.size()) ++at;
    } else {
      const size_t end = d.find(';', at);
      value = d.substr(at, end == std::string_view::npos ? end : end - at);
      at = end;
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
    if (sameNoCase(key, "filename*")) {
      const size_t language = value.find('\'');
      const size_t encoded = language == std::string::npos ? language : value.find('\'', language + 1);
      extended = encoded == std::string::npos ? std::string()
                                              : percentDecoded(value.substr(encoded + 1), false);
    } else if (sameNoCase(key, "filename")) {
      plain = value;
    }
  }
  if (std::string name = checkedName(extended); !name.empty()) return name;
  return checkedName(plain);
}

// A checked name has no /, so it is never a link: it is read as a file's.
bool mediaName(const std::string& name) {
  return isVideoFile(std::filesystem::path(name)) || isPhotoFile(name);
}

// The first file a query names: with `byName`, a filename or
// response-content-disposition parameter's; else any value that is a
// media file's name (as in .../file?path=clip_LR_180.mp4&sig=...).
std::string nameInQuery(std::string_view query, bool byName) {
  for (size_t at = 0; at < query.size();) {
    const size_t end = std::min(query.find('&', at), query.size());
    const std::string_view part = query.substr(at, end - at);
    at = end + 1;
    const size_t eq = part.find('=');
    if (eq == std::string_view::npos) continue;
    const std::string key = percentDecoded(part.substr(0, eq), true);
    const bool disposition = sameNoCase(key, "response-content-disposition");
    if (byName && !disposition && !sameNoCase(key, "filename")) continue;
    const std::string value = percentDecoded(part.substr(eq + 1), true);
    const std::string name = byName && disposition ? dispositionName(value) : checkedName(value);
    if (!name.empty() && (byName || mediaName(name))) return name;
  }
  return {};
}

// linkFileName() for a link.
std::string fileNameOf(const std::string& location) {
  const std::string_view link = std::string_view(location).substr(0, location.find('#'));
  const size_t question = link.find('?');
  const std::string_view query =
      question == std::string_view::npos ? std::string_view() : link.substr(question + 1);
  if (std::string named = nameInQuery(query, true); !named.empty()) return named;
  const std::string_view address = link.substr(0, question);
  std::string last;
  if (address.find_first_of('/', address.find("://") + 3) != std::string_view::npos)
    last = checkedName(percentDecoded(address.substr(address.find_last_of('/') + 1), false));
  if (mediaName(last)) return last;
  if (std::string named = nameInQuery(query, false); !named.empty()) return named;
  return last;
}

}  // namespace

bool isStream(const std::string& location) {
  return location.find("://") != std::string::npos && !remote::isRemote(location);
}

bool isLink(const std::string& location) { return isStream(location) && !isStashScene(location); }

std::string linkFileName(const std::string& location) {
  if (!isLink(location)) return {};
  // The frame loop asks about the playing link several times a frame: the
  // last answer is kept, one for each thread.
  thread_local std::string lastLink, lastName;
  if (location != lastLink) {
    lastName = fileNameOf(location);
    lastLink = location;
  }
  return lastName;
}

std::string linkTitle(const std::string& location) {
  if (!isLink(location)) return {};
  if (std::string name = linkFileName(location); !name.empty()) return name;
  const size_t start = location.find("://") + 3;
  std::string_view host = std::string_view(location).substr(start, location.find_first_of("/?#", start) - start);
  host.remove_prefix(std::min(host.size(), host.find_last_of('@') + 1));
  const size_t port = host.find(':', host.empty() || host.front() != '[' ? 0 : host.find(']'));
  return checkedName(std::string(host.substr(0, port)));
}

std::string parentFolder(const std::string& path) {
  return remote::isRemote(path) ? remote::parent(path) : std::filesystem::path(path).parent_path().string();
}
