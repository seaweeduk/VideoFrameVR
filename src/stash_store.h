#pragma once

#include "media_server.h"
#include "stash_data.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct sqlite3;

// The local copy of a media server's data: a file per kind in dataDir()
// (stash.db, xbvr.db), SQLite, so that going to another kind and back
// finds the copy as it was. Everything in it can be fetched again, so it is
// no library: a file from another version of this layout is simply made
// anew, and each sync replaces all of it in one transaction. Not touched
// while its kind isn't in use. Blocks on the file: worker threads only, one
// at a time.
class StashStore {
 public:
  explicit StashStore(const MediaServer& kind);
  // The copy, if there is one; false (and error() says why) if there is
  // none or it can't be read.
  bool load(StashData& data);
  // Replaces the copy with `data`; false if it could not be written (the
  // old copy is then kept).
  bool save(const StashData& data);
  // The copy's scene of each oshash, and each scene's name (its title, else
  // its first file's name), if the copy is `server`'s; false if there is no
  // such copy. Reads only.
  bool scenes(const std::string& server, const std::string& kind,
              std::unordered_map<std::string, int64_t>& byOshash,
              std::unordered_map<int64_t, std::string>& names);
  const std::string& error() const { return error_; }

  // dataDir()/<the kind's id>.db
  static std::string path(const MediaServer& kind);

 private:
  std::string path_;
  std::string error_;
};

// Write-back's own file (docs/stash.md, "Write-back safety"): the app's
// ratings and plays waiting to be sent to Stash, and the plays sent (or
// found there already), so none is sent twice. Kept apart from stash.db,
// which is only a copy of Stash's data and is made anew whenever its layout
// changes: this file's contents can't be fetched again, so it is never
// deleted or made anew here (a file of another version is left alone and
// write-back waits). One per server, so another server never gets one's
// changes or takes its sent plays for its own. Made only once write-back
// has something to keep. Blocks on the file: one worker thread.
class StashOutbox {
 public:
  // `server` as StashClient::server() has it.
  // `server`: what its file is named by (serverIdentity()). `original`:
  // the one outbox from before there was one per server may be taken over
  // (the first kind's only: it was that kind's).
  StashOutbox(const std::string& server, bool original);
  ~StashOutbox();
  StashOutbox(const StashOutbox&) = delete;
  StashOutbox& operator=(const StashOutbox&) = delete;

  // Opens (or makes) the file; false (and error()) if it can't be used.
  bool open();

  struct Rating {
    std::string oshash;
    float stars = 0;  // 0: cleared in the app, sent as null
  };
  struct Play {
    int64_t row = 0;
    std::string oshash;
    int64_t at = 0;  // Unix seconds
  };
  // A rating to send; `replace`: over one still waiting (else only if none waits).
  bool addRating(const std::string& oshash, float stars, bool replace);
  // `count` plays at `at`.
  bool addPlays(const std::string& oshash, int64_t at, int count);
  bool waiting(std::vector<Rating>& ratings, std::vector<Play>& plays);
  // Sent: leaves the outbox, unless it was replaced meanwhile.
  bool ratingDone(const Rating& rating);
  // Delivered to `scene`, each sent or (found) already in Stash's history:
  // they leave the outbox and are kept as sent, in one transaction.
  bool playsDone(const std::vector<Play>& plays, int64_t scene, const std::vector<bool>& found);
  // Not Stash's (no scene has the oshash): they leave the outbox unsent.
  bool drop(const std::vector<Rating>& ratings, const std::vector<Play>& plays);
  // The times of the plays sent or found, by oshash.
  bool sent(std::unordered_map<std::string, std::vector<int64_t>>& times);
  const std::string& error() const { return error_; }

  // dataDir()/stash_outbox-<the server's hash>.db. The single file kept
  // before there was one per server, dataDir()/stash_outbox.db, was the one
  // server's: it becomes the file of the server in the settings when write-
  // back next looks for it.
  const std::string& path() const { return path_; }

 private:
  bool fail(const char* what);
  // Takes the single file over if it is there; false if it can't be yet.
  bool adopt();
  std::string path_;
  bool original_ = false;
  sqlite3* db_ = nullptr;
  std::string error_;
};
