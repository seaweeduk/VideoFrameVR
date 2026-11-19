#pragma once

#include "server_kind.h"
#include "stash_http.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class MediaClient;

// What the run log shows in place of the server's host (hideInRunLog()):
// the same for every kind, as an address is hidden before its kind is known.
constexpr const char* kHiddenServer = "<media server>";

// What differs between the media servers, each kind's in its own file
// (stash_server.cpp, xbvr_server.cpp) and nowhere else: the sync, the
// catalog, playback and the menu ask a MediaServer and never which kind it
// is. Everything else is shared, and keeps the names it had when Stash was
// the only one (StashData, StashCatalog, StashAddress, stashscene://, ...).
//
// Another server: a ServerKind, a MediaServer and a MediaClient for it in
// files of its own, and a line in media_server.cpp's list.

// What a server can do beyond being read and played from: the menu shows
// only what it can.
struct ServerAbilities {
  // It may have a login: an API key, or a user name and password that give
  // one. Without, nothing is asked for but its address.
  bool signIn = false;
  bool editTags = false;     // a scene's tags can be changed
  // A scene's file can be deleted through it. The deletion itself, with
  // its checks, is still Stash's own (stash_delete.cpp): a second kind that
  // deletes moves it behind MediaClient first.
  bool deleteFiles = false;
  // It keeps every play with its time. Without, only that a scene was
  // watched: a play sent marks it so, once.
  bool playTimes = false;
  // Write-back is only a rating and that a scene was watched: Settings
  // says so (StashWriteBackStrings' Basic strings).
  bool ratingsAndWatchedOnly() const { return !editTags && !deleteFiles && !playTimes; }
};

// A server as requests reach it.
struct ServerLink {
  StashAddress address;
  std::string apiKey;  // empty: none
};

// One of a scene's captions (MediaServer::captions()).
struct ServerCaption {
  std::string language;  // empty for none
  std::string type;      // its file's: srt or vtt
  std::string text;
  std::string format = "vtt";  // the text's, which may not be its file's
};

class MediaServer {
 public:
  virtual ~MediaServer() = default;
  virtual ServerKind kind() const = 0;
  // Its name in settings.conf and stash.db ("stash"): never changed.
  virtual const char* id() const = 0;
  // Its name as its makers write it ("Stash"), in every language.
  virtual const char* name() const = 0;
  virtual ServerAbilities abilities() const = 0;

  // Its API's client for the server at `url` (as typed).
  virtual std::unique_ptr<MediaClient> client(const std::string& url, const std::string& apiKey,
                                              bool unverified = false) const = 0;

  // `headers` with the one that carries `apiKey`, if the server has keys
  // and there is one. Never in an address.
  virtual curl_slist* authorized(curl_slist* headers, const std::string& apiKey) const;
  // Paths on the server (a path and query from "/", for stashPerform()).
  // A scene's original file, read with byte ranges; `file` is the server's
  // id of the scene's first file (StashData::File::id), 0 where it has none.
  virtual std::string streamPath(int64_t scene, int64_t file) const = 0;
  // A scene's short preview video; `code` is the scene's
  // (StashData::Scene::code), which a server may know it by.
  virtual std::string previewPath(int64_t scene, const std::string& code) const = 0;
  // An image by the URL the sync kept for it (a cover's, a performer's).
  virtual std::string imagePath(const StashAddress& address, const std::string& url) const = 0;

  // The requests below block: worker threads only.
  // Whether the server answers one small request, in 5 s at most.
  virtual bool answers(const ServerLink& link) const = 0;
  // A scene's captions and its funscript (empty: none).
  virtual std::vector<ServerCaption> captions(const ServerLink& link, int64_t scene) const = 0;
  virtual std::string funscript(const ServerLink& link, int64_t scene) const = 0;
};

const MediaServer& mediaServer(ServerKind kind);
// What is kept on disk per server (changes waiting to be sent, a download's
// source) is kept under this, so that two kinds at one address never share
// it: the address alone for the first kind, as it was before there were
// kinds, and with its id before it for any other.
std::string serverIdentity(ServerKind kind, const std::string& server);
// The first kind: what was kept before there were kinds is its own.
bool originalKind(ServerKind kind);
// Every kind, in the order an address is tried as each (findServer()).
const std::vector<const MediaServer*>& mediaServers();
// The kind with this id(); Stash's for any other, as settings from before
// there were kinds have none.
ServerKind serverKindOf(std::string_view id);
// Every kind's name, "Stash / XBVR": Settings' name for its servers'
// section. In a sentence they are named with the language's "or" between
// (StashSetupStrings::orKind), as no server is set up yet.
std::string mediaServerNames(const std::string& between = " / ");

// A server's small requests, one after another on one connection, for the
// kinds' own files: the key's header added, 3 s to connect and 10 s for
// each (`seconds`), an answer of up to 16 MB kept. Blocks.
class ServerFetch {
 public:
  ServerFetch(const MediaServer& server, const ServerLink& link, long seconds = 10);
  ~ServerFetch();
  ServerFetch(const ServerFetch&) = delete;
  ServerFetch& operator=(const ServerFetch&) = delete;
  // GETs `path`, or POSTs `json` to it; the HTTP status, 0 if the server
  // wasn't reached, with the answer in `bytes`.
  long get(const std::string& path, std::string& bytes);
  long post(const std::string& path, const std::string& json, std::string& bytes);
  // `text` for a query's value.
  std::string escaped(const std::string& text) const;

 private:
  long perform(const std::string& path, std::string& bytes);
  ServerLink link_;
  CURL* curl_ = nullptr;
  curl_slist* headers_ = nullptr;
  curl_slist* jsonHeaders_ = nullptr;
  long seconds_;
};
