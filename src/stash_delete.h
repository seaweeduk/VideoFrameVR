#pragma once

#include "file_ops.h"

#include <cstdint>
#include <string>
#include <vector>

// Deleting the playing file through Stash (docs/alpha/delete-file.md), only
// with Send to Stash on. Blocks: workers only.

// A scene's file as a fresh query finds it: the one with both Stash's path
// the catalog has for it (stashFilePathAt()) and its oshash.
struct StashFileLook {
  bool answered = false;  // Stash answered; else `error` says why
  bool scene = false;     // ... and has the scene
  int64_t file = 0;       // its one file with the path and oshash; 0: none, or several
  std::vector<int64_t> files;  // the scene's, its primary first
  std::string trash;           // Stash's trash folder; empty: it deletes for good
  std::string error;
};
StashFileLook lookUpStashFile(const std::string& url, const std::string& apiKey, bool unverified,
                              int64_t scene, const std::string& oshash,
                              const std::string& stashPath);

// How a deletion went.
struct DeleteOutcome {
  DeleteStatus status = DeleteStatus::Failed;
  std::string error;  // Failed: why
  bool listed = false;  // the file is gone, but Stash lists it until its Clean runs
  bool stashDeleted = false;  // Stash deleted its file (whatever `status` says of the one here)
};

// Deletes the scene's file with that path and oshash, looked up again
// first: Stash deletes it, with the scene if it is the scene's only file,
// else alone (another file made the primary first). `here`: the file on
// this device or a server (remote.h), checked against `stamp` before anything is
// sent; empty: only Stash's stream has it. If Stash can't and the file is
// here, the app deletes it (removeFile()), then removes Stash's entry as
// Clean would; not if `trashPromised` (the dialog said Stash moves it to
// its trash folder). `sendToStash` false (Send to Stash off): the client
// sends no change.
DeleteOutcome deleteThroughStash(const std::string& url, const std::string& apiKey,
                                 bool unverified, bool sendToStash, int64_t scene, const std::string& oshash,
                                 const std::string& stashPath, const std::string& here,
                                 const FileStamp& stamp, bool trashPromised);
