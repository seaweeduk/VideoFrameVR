#pragma once

#include "stash_data.h"
#include "stash_http.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Talks to a Stash server's GraphQL API (docs/stash.md): queries over
// libcurl, with the API key in an ApiKey header when there is one, and the
// write-back's three changes and deleting the playing file's, nothing else
// ("Write-back safety"), besides making an API key when signing in finds
// none. Blocks for as long as the server takes: worker threads only.
class StashClient {
 public:
  // `url` as typed (StashAddress makes it whole); `unverified`: its
  // certificate is accepted without being verified.
  StashClient(const std::string& url, std::string apiKey, bool unverified = false);
  // The server, without /graphql, as StashData::server has it.
  const std::string& server() const { return address_.server; }

  // How long the last fetch() took, and what it brought.
  struct Timings {
    double fetchMs = 0;  // both requests, which run at the same time
    double parseMs = 0;  // JSON to StashData
    size_t bytes = 0;    // received, uncompressed
  };
  // Everything a sync takes: the server's version and VR tag, every scene
  // (with its files), performer, studio, tag and group. False (and error()
  // says why) if the server can't be reached or answers with an error. A
  // Stash older than 0.31.0 is asked only its version.
  bool fetch(StashData& data, Timings& timings);
  // Why the last request failed, in the user's language (strings()).
  const std::string& error() const { return error_; }
  StashProblem problem() const { return problem_; }
  // Whether the last request got an answer from the server (whatever it said).
  bool reached() const { return reached_; }

  // What Stash has of a scene's activity: its rating (-1 = none) and the
  // times it was played (Unix seconds).
  struct Activity {
    int rating100 = -1;
    std::vector<int64_t> plays;
  };
  // Every scene's, by Stash id; one scene's. Queries only.
  bool fetchActivity(std::unordered_map<int64_t, Activity>& activity);
  // `exists` false: Stash has no such scene (any more).
  bool fetchActivity(int64_t scene, Activity& activity, bool& exists);

  // The write-back's only three changes, each a fixed GraphQL document:
  // sceneUpdate with the scene's id and rating100 alone (stars × 20; 0 sends
  // null, which clears it), sceneAddPlay with the scene's id and the plays'
  // times, and bulkSceneUpdate on one scene adding (ADD) and removing
  // (REMOVE) the given tag ids, its other tags kept. False (and error() says
  // why) if Stash did not take it.
  bool setRating(int64_t scene, int rating100);
  bool addPlays(int64_t scene, const std::vector<int64_t>& times);
  bool editTags(int64_t scene, const std::vector<int64_t>& add, const std::vector<int64_t>& remove);

  // Deleting the playing file (docs/alpha/delete-file.md). A scene's files
  // (its primary first) with their paths and oshashes, and Stash's trash
  // folder (empty: it deletes for good); `exists` false: no such scene. A
  // query.
  struct SceneFile {
    int64_t id = 0;
    std::string path, oshash;
  };
  bool sceneFiles(int64_t scene, std::vector<SceneFile>& files, std::string& trash, bool& exists);
  // Its changes, each a fixed document on one scene or one file, refused
  // unless setWriteBack(true) (Send to Stash on): the scene with its files
  // and generated files, or (`fromDisk` false) only their entries, as
  // Stash's Clean does; another of its files made its primary (Stash
  // deletes no primary file alone); one file, or only its entry. False
  // (and error() says why) if Stash did not do it.
  void setWriteBack(bool on) { writeBack_ = on; }
  bool destroyScene(int64_t scene, bool fromDisk);
  bool setPrimaryFile(int64_t scene, int64_t file);
  bool destroyFile(int64_t file, bool fromDisk);

  // Signs in with Stash's user name and password (its login form) and reads
  // its API key with that session, so nobody has to type the key: true with
  // `apiKey`, empty if Stash has no login (and so needs no key). With a login
  // but no key yet, one is made (generateAPIKey); a key there is never
  // replaced, as other apps use it. `refused`: Stash didn't take the user
  // name and password. Nothing but the key is kept. Blocks, each request
  // for at most 30 s.
  bool signIn(const std::string& user, const std::string& password, std::string& apiKey,
              bool& refused);

  // Whether Stash answers at the address, with one small request (its
  // version) and no API key, a few seconds at most, at each of stashTries()
  // in turn: first-run setup's Connect, Settings → Stash's Save. The first
  // that answers as Stash is server() from then on. Open: it answers
  // without a login. Failed: something answers that can't be used (not
  // Stash, too old, a certificate, …), and error() says why; problem() is
  // Certificate when that was an untrusted certificate.
  enum class Answer { Open, NeedsSignIn, Unreached, Failed };
  Answer check();

  // Bytes received so far by the fetch() under way (compressed, as sent),
  // from any thread.
  size_t received() const { return received_; }

  // Ends a request under way, from any thread: fetch() fails soon after
  // (within about a second).
  void cancel() { cancelled_ = true; }
  bool cancelled() const { return cancelled_; }

 private:
  // POSTs one query (with its variables, a JSON object or empty); the
  // response's body, or false with `error` and `problem` set. Refuses any
  // mutation but the write-back's three and, with setWriteBack(true),
  // deleting's. `slow`: one that answers with every scene, which may take
  // minutes to start.
  bool post(const std::string& query, std::string& body, std::string& error,
            StashProblem& problem, const std::string& variables = std::string(),
            bool slow = false);
  // post() and parse; the response's data as JSON text in `data`.
  bool request(const char* query, const std::string& variables, std::string& data,
               bool slow = false);

  std::string typed_;
  StashAddress address_;
  bool quick_ = false;  // check()'s requests: short timeouts
  bool writeBack_ = false;  // deleting's changes allowed
  std::string apiKey_;
  std::string error_;
  StashProblem problem_ = StashProblem::None;
  std::atomic<bool> reached_{false};
  std::atomic<bool> cancelled_{false};
  std::atomic<size_t> received_{0};
};
