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
bool isPhotoFile(const std::string& p);
bool isSubtitleFile(const std::filesystem::path& p);
// A browser's unfinished download of a video, here or on a share: its name
// ends in .part, .crdownload or .opdownload after a video's (not a disc
// image's). Not a link, and not the app's own in an Offline videos folder.
bool isPartialDownload(const std::string& location);
// At the end of a partial download's data, how long a read waits without it
// growing: a browser can hold back what it fetched for a while, a share's
// writer longer. After it, the video ends where its data does.
constexpr std::chrono::seconds kPartialStall{30};
// A partial download the browser lists, and Previous and Next go through:
// one its reader follows as it grows, here or on an SMB share.
bool isListedPartial(const std::string& location);
// Whether a subtitle file's name goes with a video's: "movie.srt" or
// "movie.en.srt" for "movie.mp4", and for its partial download
// "movie.mp4.part".
bool subtitleBelongsTo(const std::string& video, const std::string& subtitle);
// The subtitles beside a local video, as remote::subtitlesBeside() finds
// them beside a share's: sorted paths. Lists its folder.
std::vector<std::string> localSubtitles(const std::string& video);

// ASCII case-insensitive name order.
bool fileNameLess(const std::string& a, const std::string& b);

// lost+found, trash and recycle bins (Linux, Windows, Samba's and NAS's):
// folders never looked into for videos.
bool binOrLost(const std::string& name);

// Whether a name (from a drive or a server: any bytes) is valid UTF-8.
bool validUtf8(const std::string& s);
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
// Going on by itself (at a video's end, `byItself`), never to a partial
// download (isPartialDownload()): its data may end before it does. Previous
// and Next by hand still go to one.
struct PlayOrder {
  uint64_t shuffle = 0;
  std::string start;
  bool byItself = false;
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

// Replaces `path` with `contents` by way of a new `path`.new.<pid>.<n>, synced to disk before
// it is renamed over it, so a crash leaves the old file or the new one, never part of one. A
// symlink stays one: its target is replaced. `ownerOnly`: mode 0600, else std::ofstream's
// (0666 less the umask). False if it couldn't (its temporary is removed).
bool replaceFile(const std::filesystem::path& path, const std::string& contents, bool ownerOnly = false);

// Config reads and writes, in order. Disk work runs outside the queue lock;
// shutdown waits for the last write, the frame loop never does.
void queueConfigWork(std::function<void()> work);
// Waits up to `seconds` for the config writes queued so far; whether they finished.
bool drainConfigWork(double seconds);
