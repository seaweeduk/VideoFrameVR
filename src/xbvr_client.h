#pragma once

#include "media_client.h"

#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <string>

// Talks to an XBVR server's REST API (github.com/xbapps/xbvr), which has no
// login of its own: its scenes that have a file (/api/scene/list, a page at
// a time, with their files, cast and tags in them), its sites and its
// folders. It sends two changes and nothing else: a scene's rating
// (/api/scene/rate) and that a scene was watched (/api/scene/toggle, only
// from not watched to watched). Blocks for as long as the server takes:
// worker threads only.
class XbvrClient : public MediaClient {
 public:
  XbvrClient(const std::string& url, bool unverified = false);

  bool fetch(StashData& data, Timings& timings) override;
  // XBVR keeps no play times this app could add to: `watched` says whether
  // a scene counts as played.
  bool fetchActivity(std::unordered_map<int64_t, Activity>& activity) override;
  bool fetchActivity(int64_t scene, Activity& activity, bool& exists) override;
  bool setRating(int64_t scene, int rating100) override;
  // Marks the scene watched if it isn't; the times are not kept.
  bool addPlays(int64_t scene, const std::vector<int64_t>& times) override;

 private:
  // GETs `path`, or POSTs `body` to it; the answer's JSON, or false with
  // `error` and `problem` set. `change`: one of the two changes; anything
  // else POSTed but the scene list is refused.
  bool ask(const Call& call, nlohmann::json& answer, std::string& error, StashProblem& problem);
  bool get(const std::string& path, nlohmann::json& answer);
  bool listed(std::vector<nlohmann::json>& rows, bool& whole, size_t& bytes, double& ms);
  // Every scene with a file; false with error_ set. `bytes` and `ms` count
  // what the requests brought and took.
  bool scenes(const std::function<void(const nlohmann::json& scene)>& each, size_t& bytes,
              double& ms);
  // One scene by its id; `exists` false if XBVR has none.
  bool scene(int64_t id, nlohmann::json& answer, bool& exists);
  Answer identify() override;
};
