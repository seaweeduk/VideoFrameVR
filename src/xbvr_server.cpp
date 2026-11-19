#include "media_server.h"

#include "server_json.h"
#include "xbvr_client.h"

#include <algorithm>
#include <cctype>

// XBVR as a media server (media_server.h): what it can do and where its
// files are. Its API is XbvrClient's.

namespace {

using json = nlohmann::json;
using server_json::array;
using server_json::flag;
using server_json::number;
using server_json::text;

// One of XBVR's files, by its own id. "dnt" (do not track) always: without
// it XBVR takes a request for a scene's file, a script or subtitles too, as
// the start of a viewing of its own, and marks the scene watched. What was
// played is the app's to say (write-back).
std::string filePath(int64_t file) {
  return "/api/dms/file/" + std::to_string(file) + "?dnt=true";
}

// A scene's files of one type ("script", "subtitles"), as XBVR lists them.
std::vector<json> filesOf(ServerFetch& fetch, int64_t scene, const char* type) {
  std::vector<json> files;
  std::string bytes;
  if (fetch.get("/api/scene/" + std::to_string(scene), bytes) != 200) return files;
  const json j = json::parse(bytes, nullptr, false);
  if (!j.is_object() || number<int64_t>(j, "id") != scene) return files;
  for (const json& f : array(j, "file"))
    if (text(f, "type") == type && number<int64_t>(f, "id") > 0) files.push_back(f);
  return files;
}

// `text` as a browser writes it in an address: what can't stand in one,
// escaped; as one part of a path (`address` false), what divides one too.
std::string escaped(const std::string& text, bool address) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (const unsigned char c : text) {
    const bool never = c <= ' ' || c >= 0x7f || c == '"' || c == '<' || c == '>' || c == '\\' ||
                       c == '^' || c == '`' || c == '{' || c == '|' || c == '}';
    const bool divides = c == '/' || c == '?' || c == '%' || c == '&' || c == '+';
    // "#" would end the address the proxy is asked for.
    if (never || c == '#' || (!address && divides)) {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

class XbvrServer : public MediaServer {
 public:
  ServerKind kind() const override { return ServerKind::Xbvr; }
  const char* id() const override { return "xbvr"; }
  const char* name() const override { return "XBVR"; }
  // No login of its own, no tags to change, no file deleted through it; it
  // keeps no times of plays that could be added to.
  ServerAbilities abilities() const override { return {}; }

  std::unique_ptr<MediaClient> client(const std::string& url, const std::string&,
                                      bool unverified) const override {
    return std::make_unique<XbvrClient>(url, unverified);
  }

  // The file as it is, with byte ranges. Never its DLNA server's address.
  std::string streamPath(int64_t, int64_t file) const override { return filePath(file); }
  // The preview XBVR made, if it was asked to: kept under the scene's name
  // (its "scene_id", the sync's code), not its number.
  std::string previewPath(int64_t, const std::string& code) const override {
    return "/api/dms/preview/" + escaped(code, false);
  }
  // XBVR keeps the addresses its scrapers found, on the sites' own servers:
  // fetched through its image proxy, which keeps a copy, as its own pages
  // do. Without a size, the image as it is.
  std::string imagePath(const StashAddress&, const std::string& url) const override {
    return "/img/" + escaped(url, true);
  }

  bool answers(const ServerLink& link) const override {
    ServerFetch fetch(*this, link, 5);
    std::string bytes;
    return fetch.get("/api.json", bytes) == 200;
  }

  // The subtitle files XBVR found beside the scene's videos (.srt, .ssa,
  // .ass), each as it is; its language, if its name ends in one
  // ("name.en.srt").
  std::vector<ServerCaption> captions(const ServerLink& link, int64_t scene) const override {
    std::vector<ServerCaption> captions;
    ServerFetch fetch(*this, link);
    for (const json& f : filesOf(fetch, scene, "subtitles")) {
      const std::string name = text(f, "filename");
      const size_t dot = name.find_last_of('.');
      if (dot == std::string::npos) continue;
      ServerCaption caption;
      caption.type = caption.format = lower(name.substr(dot + 1));
      // What XBVR lists as subtitles; the format becomes a file's extension.
      if (caption.format != "srt" && caption.format != "ass" && caption.format != "ssa") continue;
      const size_t before = name.find_last_of('.', dot ? dot - 1 : 0);
      if (before != std::string::npos && dot - before - 1 >= 2 && dot - before - 1 <= 3) {
        const std::string language = name.substr(before + 1, dot - before - 1);
        if (std::all_of(language.begin(), language.end(),
                        [](unsigned char c) { return std::isalpha(c); }))
          caption.language = lower(language);
      }
      if (fetch.get(filePath(number<int64_t>(f, "id")), caption.text) != 200 || caption.text.empty())
        continue;
      captions.push_back(std::move(caption));
    }
    return captions;
  }

  // The script chosen for the scene in XBVR, else its first.
  std::string funscript(const ServerLink& link, int64_t scene) const override {
    ServerFetch fetch(*this, link);
    const std::vector<json> scripts = filesOf(fetch, scene, "script");
    if (scripts.empty()) return {};
    const auto chosen = std::find_if(scripts.begin(), scripts.end(),
                                     [](const json& f) { return flag(f, "is_selected_script"); });
    std::string bytes;
    const json& script = chosen != scripts.end() ? *chosen : scripts.front();
    return fetch.get(filePath(number<int64_t>(script, "id")), bytes) == 200 ? bytes : std::string();
  }
};

}  // namespace

const MediaServer& xbvrServer() {
  static const XbvrServer& server = *new XbvrServer;  // never taken down (mediaServers())
  return server;
}
