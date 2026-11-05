#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Where Stash's videos are on this device (docs/stash.md, "Folders"), so
// that nobody has to type Stash's folder mappings: for each of Stash's
// library folders, a few of its files (their paths and sizes) are looked for
// under folders here, and the mapping that finds them (StashFolderMap) is
// the answer. Paths may be Linux, macOS or Windows ones (D:\..., \\NAS\...).
// It runs on its own thread; Stash polls it.
class StashFolderFinder {
 public:
  struct File {
    std::string path;  // as Stash has it
    uint64_t size = 0;
  };
  struct Mapping {
    std::string pathFrom;  // Stash's side
    std::string pathTo;    // this device's: a local path or a remote locator (remote.h)
    int found = 0;         // of the files looked for
  };

  // One of Stash's folders to look for (StashCatalogFolder), and what was
  // found for it: the mappings, the most files found first.
  struct Job {
    std::string folder;
    std::vector<File> files;
  };
  struct Result {
    std::string folder;
    std::vector<Mapping> found;
  };

  StashFolderFinder();
  ~StashFolderFinder();  // stops the search and waits for it

  // Looks for each job's files under `roots` (local paths or remote locators).
  // Empty: every likely place: the shares of the SMB servers signed in to,
  // the SFTP servers' "/" and home folders, mounted drives and network
  // mounts, and the home folder, after `also`.
  // A new search replaces one that is running. Per job: at most 64 roots, 50
  // subfolders per root (two levels), 128 listings and 16,384 stats; what
  // one job read is not read again for the next.
  void find(std::vector<Job> jobs, std::vector<std::string> roots = {},
            std::vector<std::string> also = {});
  bool poll();  // main thread: true once when a search has finished
  bool running() const;
  // The search under way (or the last): its jobs, and how many are done.
  int jobs() const;
  int done() const;
  // The last search's results, one per job in order.
  const std::vector<Result>& found() const { return found_; }
  // ... for one of Stash's folders; none found (or not looked for): empty.
  const std::vector<Mapping>& foundFor(const std::string& folder) const;

 private:
  struct State;
  std::shared_ptr<State> state_;
  std::vector<Result> found_;
};
