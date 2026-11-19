#pragma once

#include "media_server.h"
#include "stash_data.h"
#include "strings.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// A media server's API as the sync and write-back use it, whatever its kind
// (media_server.h): each kind's client (StashClient, XbvrClient) fills the
// same StashData and sends the same few changes its own way. What they
// share is here: one request with its timeouts, stopping and reasons for
// failing, and Connect's tries. Blocks for as long as the server takes:
// worker threads only.
class MediaClient {
 public:
  virtual ~MediaClient() = default;
  const MediaServer& kind() const { return kind_; }
  // The server as StashData::server has it.
  const std::string& server() const { return address_.server; }

  // How long the last fetch() took, and what it brought.
  struct Timings {
    double fetchMs = 0;  // its requests
    double parseMs = 0;  // JSON to StashData
    size_t bytes = 0;    // received, uncompressed
  };
  // Everything a sync takes: the server's version, every scene (with its
  // files), performer, studio, tag and group. False (and error() says why)
  // if the server can't be reached or answers with an error.
  virtual bool fetch(StashData& data, Timings& timings) = 0;
  // Why the last request failed, in the user's language (strings()).
  const std::string& error() const { return error_; }
  StashProblem problem() const { return problem_; }
  // Whether the last request got an answer from the server (whatever it said).
  bool reached() const { return reached_; }

  // What the server has of a scene's activity: its rating (-1 = none), the
  // times it was played (Unix seconds) where it keeps them
  // (ServerAbilities::playTimes), and whether it counts as watched.
  struct Activity {
    int rating100 = -1;
    std::vector<int64_t> plays;
    bool watched = false;
  };
  // Every scene's, by the server's id; one scene's. Queries only.
  virtual bool fetchActivity(std::unordered_map<int64_t, Activity>& activity) = 0;
  // `exists` false: the server has no such scene (any more).
  virtual bool fetchActivity(int64_t scene, Activity& activity, bool& exists) = 0;

  // The write-back's changes: a scene's rating (stars × 20; 0 clears it),
  // its plays at the given times (a server without play times marks it
  // watched), and, where the server can (ServerAbilities::editTags), tags
  // added to and removed from it, its others kept. False (and error() says
  // why) if the server did not take it.
  virtual bool setRating(int64_t scene, int rating100) = 0;
  virtual bool addPlays(int64_t scene, const std::vector<int64_t>& times) = 0;
  virtual bool editTags(int64_t scene, const std::vector<int64_t>& add,
                        const std::vector<int64_t>& remove);

  // Signs in with the server's user name and password, where it has a
  // login (ServerAbilities::signIn), and reads the key requests then carry,
  // so nobody has to type it: true with `apiKey`, empty if the server has
  // no login. `refused`: it didn't take the user name and password.
  // Nothing but the key is kept.
  virtual bool signIn(const std::string& user, const std::string& password, std::string& apiKey,
                      bool& refused);

  // Whether the server answers at the address as this kind, with one small
  // request and no key, a few seconds at most, at each of stashTries() in
  // turn: first-run setup's Connect, Settings' Save. The first that answers
  // as this kind is server() from then on. Open: it answers without a
  // login. Failed: something answers that can't be used (another server,
  // too old, a certificate, …), and error() says why; problem() is
  // Certificate when that was an untrusted certificate.
  enum class Answer { Open, NeedsSignIn, Unreached, Failed };
  Answer check();
  // After check() failed: something answered at the address, only not as
  // this kind. Another kind may be asked there.
  bool foreign() const { return foreign_; }

  // Bytes received so far by the fetch() under way (compressed, as sent),
  // from any thread.
  size_t received() const { return received_; }

  // Its messages name its own kind, whichever server strings() names: for
  // one asked before it is the one in use (Settings' Save).
  void nameOwnKind() { ownName_ = true; }

  // Ends a request under way, from any thread: fetch() fails soon after
  // (within about a second).
  void cancel() { cancelled_ = true; }
  bool cancelled() const { return cancelled_; }

 protected:
  // `url` as typed (StashAddress makes it whole); `unverified`: its
  // certificate is accepted without being verified.
  MediaClient(const MediaServer& kind, const std::string& url, std::string apiKey, bool unverified);

  // One request to `path` on the server: a GET, or `body` POSTed as JSON.
  struct Call {
    std::string path;
    std::string body;
    bool post = false;
    // One that answers with every scene, which may take minutes to start.
    bool slow = false;
    // Only where the server answered (address_.base()), never on to the
    // fallback port after a failed try: a change that must not be repeated.
    bool once = false;
  };
  // The answer's HTTP status and body; false, with `error` and `problem`
  // set, if the server wasn't reached, answered from another host, or the
  // client was stopped. From several threads at once, each with its own
  // `error` and `problem`.
  bool send(const Call& call, long& status, std::string& body, std::string& error,
            StashProblem& problem);
  // What its messages are made of (strings().stashSetup, or nameOwnKind()'s).
  const StashSetupStrings& said() const;
  // An HTTP status that isn't the server's answer, in a few words.
  std::string httpProblem(long status, const std::string& body) const;
  // One of check()'s tries, at address_ with short timeouts. Open or
  // NeedsSignIn: this kind answers there. Failed: it does, in a way that
  // can't be used (error_ says why). Unreached: it doesn't; error_,
  // problem_ and reached() tell nothing there from something else.
  virtual Answer identify() = 0;

  const MediaServer& kind_;
  std::string typed_;
  StashAddress address_;
  bool quick_ = false;  // check()'s requests: short timeouts
  bool ownName_ = false;
  std::string apiKey_;
  std::string error_;
  StashProblem problem_ = StashProblem::None;
  bool foreign_ = false;
  std::atomic<bool> reached_{false};
  std::atomic<bool> cancelled_{false};
  std::atomic<size_t> received_{0};
};

// Which kind of server answers at an address, for first-run setup's Connect
// and Settings' Save: each kind is asked in turn (MediaClient::check()),
// `first` before the others (the kind its fields are for), the next only
// when something answered that isn't the kind asked. `ownNames`: why none
// was found names the kind asked (MediaClient::nameOwnKind()). Blocks, a
// few seconds for each try.
// Stops a findServer() under way, from any thread: it returns soon after
// with nothing found.
class ServerSearch {
 public:
  void cancel();
  // findServer()'s: the client it asks now, or none; false once cancelled.
  bool asking(MediaClient* client);

 private:
  std::mutex mutex_;
  MediaClient* asking_ = nullptr;
  bool cancelled_ = false;
};
struct FoundServer {
  ServerKind kind = ServerKind::Stash;  // the one that answered; else `first`
  bool found = false;      // one answered as its kind, open or wanting a sign-in
  // ... which of the two; else how the first kind asked failed.
  MediaClient::Answer answer = MediaClient::Answer::Unreached;
  std::string url;         // ... there
  bool untrusted = false;  // stopped by a certificate that isn't trusted
  std::string error;       // why none was found, as the first kind asked says it
};
FoundServer findServer(const std::string& url, bool unverified, ServerKind first,
                       bool ownNames = false, ServerSearch* search = nullptr);
