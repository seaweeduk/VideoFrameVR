#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <string>

// Reading a media server's JSON answers without trusting their shape, for
// the kinds' clients: a member that is missing or of another type reads as
// empty.
namespace server_json {

using json = nlohmann::json;

inline std::string text(const json& object, const char* key) {
  const auto it = object.find(key);
  return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

template <typename T>
T number(const json& object, const char* key, T fallback = 0) {
  const auto it = object.find(key);
  return it != object.end() && it->is_number() ? it->get<T>() : fallback;
}

inline bool flag(const json& object, const char* key) {
  const auto it = object.find(key);
  return it != object.end() && it->is_boolean() && it->get<bool>();
}

inline const json& array(const json& object, const char* key) {
  static const json empty = json::array();
  const auto it = object.find(key);
  return it != object.end() && it->is_array() ? *it : empty;
}

// A member object, or an empty one.
inline const json& member(const json& object, const char* key) {
  static const json empty = json::object();
  const auto it = object.find(key);
  return it != object.end() && it->is_object() ? *it : empty;
}

// Days from 1970-01-01 to a date of the proleptic Gregorian calendar.
inline int64_t daysFromCivil(int64_t y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// An RFC 3339 time ("2019-10-04T22:37:47+01:00", "...Z", maybe with a
// fraction) as Unix seconds, the fraction dropped (as stat's st_mtime
// does). 0 if it can't be read.
inline int64_t unixTime(const std::string& t) {
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

}  // namespace server_json
