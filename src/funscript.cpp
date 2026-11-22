#include "funscript.h"

#include "location.h"
#include "remote.h"
#include "server_json.h"
#include "stash_stream.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

namespace {

// Scripts are a few hundred kilobytes at most; anything larger isn't one.
constexpr uint64_t kMaxBytes = 16 * 1024 * 1024;

}  // namespace

int Funscript::actionAt(double ms) const {
  const auto next = std::upper_bound(actions.begin(), actions.end(), ms,
                                     [](double t, const Action& a) { return t < a.at; });
  return static_cast<int>(next - actions.begin()) - 1;
}

double Funscript::positionAt(double ms) const {
  if (actions.empty()) return 0;
  const int i = actionAt(ms);
  if (i < 0) return actions.front().pos;
  if (i + 1 >= static_cast<int>(actions.size())) return actions.back().pos;
  const Action& a = actions[i];
  const Action& b = actions[i + 1];
  return a.pos + (b.pos - a.pos) * (ms - a.at) / (b.at - a.at);
}

bool parseFunscript(const std::string& text, Funscript& script) {
  script.actions.clear();
  const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
  if (!j.is_object()) return false;
  const auto actions = j.find("actions");
  if (actions == j.end() || !actions->is_array()) return false;
  const bool inverted = server_json::flag(j, "inverted");
  script.actions.reserve(actions->size());
  for (const nlohmann::json& a : *actions) {
    if (!a.is_object()) continue;
    const auto at = a.find("at"), pos = a.find("pos");
    if (at == a.end() || pos == a.end() || !at->is_number() || !pos->is_number()) continue;
    const double ms = at->get<double>(), place = pos->get<double>();
    // A day is longer than any video; the limit keeps the times in an int.
    if (!std::isfinite(ms) || !std::isfinite(place) || ms < 0 || ms > 86400000.0) continue;
    const int p = static_cast<int>(std::lround(std::clamp(place, 0.0, 100.0)));
    script.actions.push_back({static_cast<int>(std::lround(ms)), inverted ? 100 - p : p});
  }
  // In time order, the last of those written for one time.
  std::stable_sort(script.actions.begin(), script.actions.end(),
                   [](const Funscript::Action& a, const Funscript::Action& b) { return a.at < b.at; });
  std::vector<Funscript::Action> unique;
  unique.reserve(script.actions.size());
  for (const Funscript::Action& a : script.actions) {
    if (!unique.empty() && unique.back().at == a.at) unique.back() = a;
    else unique.push_back(a);
  }
  script.actions = std::move(unique);
  if (script.actions.size() < 2) script.actions.clear();
  return !script.actions.empty();
}

std::string funscriptBeside(const std::string& location) {
  if (location.empty() || isStashScene(location)) return {};
  // Links (http and the like) have no file beside them.
  if (isStream(location)) return {};
  const size_t slash = location.find_last_of('/');
  const size_t dot = location.find_last_of('.');
  const size_t name = slash == std::string::npos ? 0 : slash + 1;
  const bool extension = dot != std::string::npos && dot > name;
  return (extension ? location.substr(0, dot) : location) + ".funscript";
}

std::string readFunscript(const std::string& location) {
  if (isStashScene(location)) return stashFunscript(location);
  const std::string file = funscriptBeside(location);
  if (file.empty()) return {};
  std::string bytes;
  if (!remote::isRemote(file)) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in || in.tellg() <= 0 || static_cast<uint64_t>(in.tellg()) > kMaxBytes) return {};
    bytes.resize(static_cast<size_t>(in.tellg()));
    in.seekg(0);
    if (!in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) return {};
    return bytes;
  }
  remote::Status status;
  const auto remoteFile = remote::File::open(file, status);
  if (!remoteFile || remoteFile->size() == 0 || remoteFile->size() > kMaxBytes) return {};
  bytes.resize(remoteFile->size());
  for (uint64_t at = 0; at < bytes.size();) {
    const int64_t n = remoteFile->read(at, bytes.data() + at, bytes.size() - at);
    if (n <= 0) return {};
    at += static_cast<uint64_t>(n);
  }
  return bytes;
}
