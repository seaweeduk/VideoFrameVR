#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <string>
#include <vector>

std::string lowerCase(std::string s);
bool isVideoFile(const std::filesystem::path& p);
bool isPhotoFile(const std::filesystem::path& p);
bool isSubtitleFile(const std::filesystem::path& p);

// ASCII case-insensitive name order.
bool fileNameLess(const std::string& a, const std::string& b);

// lost+found, trash and recycle bins (Linux, Windows, Samba's and NAS's):
// folders never looked into for videos.
bool binOrLost(const std::string& name);

// Browser names: Stash's case folding, but invalid UTF-8 keeps its bytes.
std::string foldFileName(const std::string& name);
// A collation key for an already folded name. Cache it before sorting.
std::string fileNameSortKey(const std::string& folded);

// Neighbors reuse a folder's listing for a few seconds; call this after changing
// a folder (e.g. deleting a file) so the next call lists it afresh.
void forgetFolderListing();

// How Previous and Next go through a list: in its order, to its ends; or
// shuffled (shuffle != 0, the seed of the random order), round the list once
// from `start`, the entry opened by hand: Next ends before coming back to
// it, Previous at it.
struct PlayOrder {
  uint64_t shuffle = 0;
  std::string start;
};
// The entry `step` places from `current` in `list` that way. An empty string
// past its end, or if `current` isn't in it.
std::string neighborIn(const std::vector<std::string>& list, const std::string& current, int step,
                       const PlayOrder& order = {});

// The file `step` places after `current`, counting only files of the same kind.
// An empty string if there is none. Abandoning the future never waits for a slow share.
// Wrap selects the first file when there is no next file (slideshows).
// `order.start` is a file's path, as `current` is.
std::future<std::string> mediaNeighbor(const std::string& current, int step, bool wrap = false,
                                       PlayOrder order = {});

// Config reads and writes, in order. Disk work runs outside the queue lock;
// shutdown waits for the last write, the frame loop never does.
void queueConfigWork(std::function<void()> work);
// Waits up to `seconds` for the config writes queued so far; whether they finished.
bool drainConfigWork(double seconds);
