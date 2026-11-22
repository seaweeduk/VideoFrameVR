#pragma once

#include "media_server.h"
#include "stash_data.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

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
