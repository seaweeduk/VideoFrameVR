#include "deo_protocol.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <charconv>
#include <utility>

namespace deo {

namespace {

using json = nlohmann::json;

void appendLength(uint32_t length, std::string& out) {
  for (int i = 0; i < 4; ++i) out += static_cast<char>((length >> (8 * i)) & 0xff);
}

bool sameNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}

// The field by its name, else by it in any case (as Json.NET reads); null
// if there is none or it is null.
const json* field(const json& j, const char* name) {
  auto it = j.find(name);
  if (it == j.end())
    for (it = j.begin(); it != j.end() && !sameNoCase(it.key(), name); ++it) {
    }
  return it == j.end() || it->is_null() ? nullptr : &*it;
}

// A number, or a string that is one whole (spaces around it and a + allowed,
// as Json.NET takes it). False if it is neither.
bool number(const json& v, double& out) {
  if (v.is_number()) {
    out = v.get<double>();
    return true;
  }
  if (!v.is_string()) return false;
  std::string_view text = v.get_ref<const std::string&>();
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
  if (!text.empty() && text.front() == '+') {
    text.remove_prefix(1);
    if (!text.empty() && text.front() == '-') return false;  // one sign only
  }
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
  return !text.empty() && error == std::errc() && end == text.data() + text.size();
}

// The length in bytes of the white space .NET's Trim() takes (Unicode
// White_Space) at `text`'s start, or at its end; 0 if there is none.
size_t spaceAt(std::string_view text, bool atEnd) {
  static constexpr std::string_view kSpaces[] = {
      "\t", "\n", "\v", "\f", "\r", " ", "\xc2\x85", "\xc2\xa0", "\xe1\x9a\x80",
      "\xe2\x80\x80", "\xe2\x80\x81", "\xe2\x80\x82", "\xe2\x80\x83", "\xe2\x80\x84",
      "\xe2\x80\x85", "\xe2\x80\x86", "\xe2\x80\x87", "\xe2\x80\x88", "\xe2\x80\x89",
      "\xe2\x80\x8a", "\xe2\x80\xa8", "\xe2\x80\xa9", "\xe2\x80\xaf", "\xe2\x81\x9f",
      "\xe3\x80\x80"};
  for (const std::string_view space : kSpaces)
    if (text.size() >= space.size() &&
        text.substr(atEnd ? text.size() - space.size() : 0, space.size()) == space)
      return space.size();
  return 0;
}

}  // namespace

void Reader::feed(const char* data, size_t size) {
  // What was read before is dropped first, so the buffer holds at most one
  // packet and what came with it.
  if (at_ > 0) {
    buffer_.erase(0, at_);
    at_ = 0;
  }
  buffer_.append(data, size);
}

Reader::Next Reader::next(std::string_view& body) {
  const size_t left = buffer_.size() - at_;
  if (left < 4) return Next::None;
  const auto byte = [&](size_t i) { return static_cast<uint32_t>(static_cast<unsigned char>(buffer_[at_ + i])); };
  const uint32_t length = byte(0) | byte(1) << 8 | byte(2) << 16 | byte(3) << 24;
  if (length & 0x80000000u) {  // negative: a ping, as DeoVR takes it
    body = {};
    at_ += 4;
    return Next::Packet;
  }
  if (length > kMaxPacket) return Next::Broken;
  if (left - 4 < length) return Next::None;
  body = std::string_view(buffer_.data() + at_ + 4, length);
  at_ += 4 + length;
  return Next::Packet;
}

bool parse(std::string_view body, Request& request) {
  request = {};
  const json j = json::parse(body.begin(), body.end(), nullptr, false);
  if (!j.is_object()) return false;
  const json* path = field(j, "path");
  const json* time = field(j, "currentTime");
  const json* state = field(j, "playerState");
  double t = 0, s = 0, unused = 0;
  if ((path && !path->is_string()) || (time && !number(*time, t)) || (state && !number(*state, s)))
    return false;
  for (const char* name : {"duration", "playbackSpeed"})
    if (const json* v = field(j, name); v && !number(*v, unused)) return false;
  if (path) {
    std::string_view text = path->get_ref<const std::string&>();
    while (const size_t n = spaceAt(text, false)) text.remove_prefix(n);
    while (const size_t n = spaceAt(text, true)) text.remove_suffix(n);
    if (!text.empty()) {
      request.hasPath = true;
      request.path = text;
    }
  }
  if (time && std::isfinite(t) && t <= kMaxTime) {
    request.hasTime = true;
    request.time = std::max(0.0, t);
  }
  if (state && !request.hasPath && (s == 0 || s == 1)) {
    request.hasState = true;
    request.paused = s == 1;
  }
  return true;
}

void merge(Request& into, Request&& later) {
  if (later.hasPath) {
    into.hasPath = true;
    into.path = std::move(later.path);
    into.hasTime = later.hasTime;
    into.time = later.time;
    into.hasState = false;
  } else if (later.hasTime) {
    into.hasTime = true;
    into.time = later.time;
  }
  if (later.hasState) {
    into.hasState = true;
    into.paused = later.paused;
  }
}

void write(const Report& report, std::string& out) {
  if (!report.open) {
    appendLength(0, out);
    return;
  }
  nlohmann::ordered_json j;
  j["path"] = report.path;
  j["duration"] = report.duration;
  j["currentTime"] = report.position;
  j["playbackSpeed"] = 1.0;
  j["playerState"] = report.ended ? 2 : report.paused ? 1 : 0;
  // A file name that isn't UTF-8 is sent with replacement characters.
  const std::string text = j.dump(-1, ' ', false, json::error_handler_t::replace);
  appendLength(static_cast<uint32_t>(text.size()), out);
  out += text;
}

}  // namespace deo
