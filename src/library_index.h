#pragma once

#include "folder_listing.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// The videos in the pinned folders and their subfolders, for the Library
// view (docs/library.md). Its paths are a cache, kept in library_index.db
// in dataDir() so the view opens at once, and made anew if the file is lost.
//
// A worker thread keeps it up to date with looks: it lists every folder
// under the pins (names, sizes and dates; no file is opened), records the
// videos it finds and drops those gone. A full look runs once per start and
// after a folder is pinned, only while no video is open: opening one stops
// it after the folder it is listing, and it goes on where it stopped once no
// video is open, or at the next start. Folders the file browser lists
// update it too. Main thread only.
class LibraryIndex {
 public:
  struct File {
    uint64_t size = 0;
    int64_t mtime = 0;
    // When it was found; on a pin's first look its own date instead (else
    // all of the pin's videos would share one).
    int64_t addedAt = 0;
    int64_t foundAt = 0;
  };
  struct State {
    bool looking = false;    // a look is under way, or waits for no video to be open
    bool firstLook = false;  // ... and a pin has not been looked through before
    int percent = -1;        // of the folders, -1 = not known (no earlier look)
    int64_t lookedAt = 0;    // the last full look of every pin ended, 0 = never
  };

  // One folder as a look lists it: its subfolders and videos, hidden ones
  // left out, with sizes and dates (one stat per video; no file opened).
  // May block on the network: workers only. Also first-run setup's count,
  // which may `stop` it between entries (cut short: Offline).
  struct Listing {
    std::vector<std::string> folders;
    std::vector<std::pair<std::string, File>> videos;
  };
  static remote::Status list(const std::string& dir, Listing& out,
                          const std::atomic<bool>* stop = nullptr);

  LibraryIndex();
  ~LibraryIndex();  // without waiting for the worker
  void start();
  // The pinned folders; a full look of those new to it follows.
  void setPins(const std::vector<std::string>& pins);
  void setVideoOpen(bool open);
  bool videoOpen() const { return videoOpen_; }
  // A full look now, or as soon as no video is open.
  void lookAgain();
  // A folder the file browser listed (its videos with their sizes and dates).
  void listed(const std::string& folder, const std::vector<FolderListing::Entry>& entries);
  // A video the app deleted: gone from files() at once.
  void deleted(const std::string& path);
  // Takes what the worker found: the paths added or changed, and those
  // gone. True if files() changed.
  bool poll(std::vector<std::string>& changed, std::vector<std::string>& removed);
  const std::unordered_map<std::string, File>& files() const { return files_; }
  State state() const;

 private:
  struct Shared;
  std::shared_ptr<Shared> shared_;
  std::unordered_map<std::string, File> files_;
  bool videoOpen_ = false;
};
