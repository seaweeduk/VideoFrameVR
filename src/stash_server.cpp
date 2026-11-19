#include "media_server.h"

#include "stash_client.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

// Stash as a media server (media_server.h): what it can do and where its
// files are. Its API is StashClient's.

namespace {

class StashServer : public MediaServer {
 public:
  ServerKind kind() const override { return ServerKind::Stash; }
  const char* id() const override { return "stash"; }
  const char* name() const override { return "Stash"; }
  ServerAbilities abilities() const override {
    ServerAbilities a;
    a.signIn = a.editTags = a.deleteFiles = a.playTimes = true;
    return a;
  }

  std::unique_ptr<MediaClient> client(const std::string& url, const std::string& apiKey,
                                      bool unverified) const override {
    return std::make_unique<StashClient>(url, apiKey, unverified);
  }

  curl_slist* authorized(curl_slist* headers, const std::string& apiKey) const override {
    return apiKey.empty() ? headers : curl_slist_append(headers, ("ApiKey: " + apiKey).c_str());
  }
  // The original file, never a transcoded one; Stash serves a scene's
  // primary file, so the file's id isn't asked for.
  std::string streamPath(int64_t scene, int64_t) const override {
    return "/scene/" + std::to_string(scene) + "/stream";
  }
  // Stash's generated MP4, not its full-video stream route.
  std::string previewPath(int64_t scene, const std::string&) const override {
    return "/scene/" + std::to_string(scene) + "/preview";
  }
  // Stash builds absolute addresses from what its proxy passes on: on the
  // server typed, whatever host it named.
  std::string imagePath(const StashAddress& address, const std::string& url) const override {
    return address.pathOn(url);
  }

  bool answers(const ServerLink& link) const override {
    ServerFetch fetch(*this, link, 5);
    std::string bytes;
    return fetch.post("/graphql", R"({"query":"{version{version}}"})", bytes) == 200;
  }

  // Its .srt and .vtt files Stash found, each served as WebVTT whatever its
  // file.
  std::vector<ServerCaption> captions(const ServerLink& link, int64_t scene) const override {
    std::vector<ServerCaption> captions;
    ServerFetch fetch(*this, link);
    const std::string id = std::to_string(scene);
    const std::string query =
        R"({findScene(id:")" + id + R"("){captions{language_code caption_type}}})";
    std::string bytes;
    nlohmann::json list;
    if (fetch.post("/graphql", nlohmann::json{{"query", query}}.dump(), bytes) == 200)
      list = nlohmann::json::parse(bytes, nullptr, false);
    for (const char* key : {"data", "findScene", "captions"})
      list = list.is_object() && list.contains(key) ? nlohmann::json(list[key]) : nlohmann::json();
    for (auto c : list.is_array() ? list : nlohmann::json::array()) {
      if (!c.is_object() || !c["language_code"].is_string() || !c["caption_type"].is_string())
        continue;
      ServerCaption caption{c["language_code"].get<std::string>(),
                            c["caption_type"].get<std::string>(), {}, "vtt"};
      const long status =
          fetch.get(link.address.pathOn("/scene/" + id + "/caption?lang=" +
                                        fetch.escaped(caption.language) + "&type=" +
                                        fetch.escaped(caption.type)),
                    bytes);
      if (status != 200 || bytes.empty()) continue;  // Stash answers nothing for one it can't read
      if (caption.language == "00") caption.language.clear();
      caption.text = std::move(bytes);
      captions.push_back(std::move(caption));
    }
    return captions;
  }

  // The file Stash found beside the scene's first file.
  std::string funscript(const ServerLink& link, int64_t scene) const override {
    ServerFetch fetch(*this, link);
    std::string bytes;
    return fetch.get("/scene/" + std::to_string(scene) + "/funscript", bytes) == 200
               ? bytes
               : std::string();
  }
};

}  // namespace

const MediaServer& stashServer() {
  static const StashServer& server = *new StashServer;  // never taken down (mediaServers())
  return server;
}
