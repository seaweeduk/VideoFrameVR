#pragma once

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

// Whether a name (from a drive or a server: any bytes) is valid UTF-8.
bool validUtf8(const std::string& s);
// Browser names: Stash's case folding, but invalid UTF-8 keeps its bytes.
std::string foldFileName(const std::string& name);
// A collation key for an already folded name. Cache it before sorting.
std::string fileNameSortKey(const std::string& folded);

// Neighbors reuse a folder's listing for a few seconds; call this after changing
// a folder (e.g. deleting a file) so the next call lists it afresh.
void forgetFolderListing();

// The file `step` places after `current`, counting only files of the same kind.
// An empty string if there is none. Abandoning the future never waits for a slow share.
// Wrap selects the first file when there is no next file (slideshows).
std::future<std::string> mediaNeighbor(const std::string& current, int step, bool wrap = false);

// Config reads and writes, in order. Disk work runs outside the queue lock;
// shutdown waits for the last write, the frame loop never does.
void queueConfigWork(std::function<void()> work);
// Waits up to `seconds` for the config writes queued so far; whether they finished.
bool drainConfigWork(double seconds);
