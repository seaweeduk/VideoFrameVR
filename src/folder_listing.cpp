#include "folder_listing.h"

#include "file_util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <thread>

#include <sys/stat.h>

namespace fs = std::filesystem;

namespace {

// Folded text, with ASCII punctuation as spaces, so "leal"
// finds "Veronica_Leal" and "try not" finds "Try-Not-To".
std::string searchable(const std::string& text) {
  std::string s = text;
  for (char& c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x80 && !std::isalnum(u)) c = ' ';
  }
  return s;
}

// What read() finds, in order.
std::vector<FolderListing::Entry> listFolder(const std::string& dir, bool subtitles,
                                            FolderListing::Order order, remote::Status& status) {
  using Entry = FolderListing::Entry;
  using Order = FolderListing::Order;
  std::vector<Entry> entries;
  std::error_code ec;
  const bool byDate = order == Order::Newest || order == Order::Oldest;
  auto modified = [](const fs::directory_entry& item) -> int64_t {
    struct stat st;
    return ::stat(item.path().c_str(), &st) == 0 ? static_cast<int64_t>(st.st_mtime) : 0;
  };
  if (remote::isRemote(dir)) {
    std::vector<remote::Entry> remote;
    status = remote::list(dir, remote);
    if (status != remote::Status::Ok) return {};
    for (const auto& item : remote) {
      if (item.name.empty() || (item.name[0] == '.' && !item.folder)) continue;
      if (item.folder || (subtitles ? isSubtitleFile(item.name)
                                    : isVideoFile(item.name) || isPhotoFile(item.name)))
        entries.push_back({item.name, dir + "/" + item.name, item.folder, item.modified, item.size});
    }
  } else for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const auto& item = *it;
    const std::string name = item.path().filename().string();
    if (name.empty()) continue;
    std::error_code typeError;
    const bool directory = item.is_directory(typeError);
    // Hidden folders are listed so dot-prefixed libraries stay reachable;
    // hidden files stay out of the list.
    if (name[0] == '.' && !directory) continue;
    const bool wanted = subtitles ? isSubtitleFile(item.path())
                                  : isVideoFile(item.path()) || isPhotoFile(item.path());
    if (directory) {
      Entry entry{name, item.path().string(), true};
      // A folder is as new as the newest thing in it (its own date if empty),
      // so one that just got a video comes first.
      if (byDate) {
        std::error_code listError;
        for (fs::directory_iterator inside(item.path(), listError), end;
             !listError && inside != end; inside.increment(listError))
          entry.modified = std::max(entry.modified, modified(*inside));
        if (entry.modified == 0) entry.modified = modified(item);
      }
      entries.push_back(std::move(entry));
    } else if (wanted) {
      Entry entry{name, item.path().string(), false};
      if (byDate) {
        entry.modified = modified(item);
      } else if (order == Order::Largest || order == Order::Smallest) {
        std::error_code sizeError;
        const auto size = item.file_size(sizeError);
        if (!sizeError) entry.size = size;
      }
      entries.push_back(std::move(entry));
    }
  }
  if (ec) {
    status = ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory
        ? remote::Status::Missing : remote::Status::Offline;
    return {};
  }
  status = remote::Status::Ok;
  for (Entry& entry : entries) {
    const std::string folded = foldFileName(entry.name);
    entry.nameKey = fileNameSortKey(folded);
    entry.searchName = searchable(entry.directory ? folded : fs::path(folded).stem().string());
  }
  // Folders first, then the files, each in the chosen order (folders by
  // name when sorting by size).
  std::sort(entries.begin(), entries.end(), [order](const Entry& a, const Entry& b) {
    if (a.directory != b.directory) return a.directory;
    switch (order) {
      case Order::NameDown: return b.nameKey < a.nameKey;
      case Order::Newest:
        if (a.modified != b.modified) return a.modified > b.modified;
        break;
      case Order::Oldest:
        if (a.modified != b.modified) return a.modified < b.modified;
        break;
      case Order::Largest:
        if (!a.directory && a.size != b.size) return a.size > b.size;
        break;
      case Order::Smallest:
        if (!a.directory && a.size != b.size) return a.size < b.size;
        break;
      default: break;
    }
    return a.nameKey < b.nameKey;
  });
  return entries;
}

// The files' dates and sizes not read for the order: one stat each.
void addDetails(std::vector<FolderListing::Entry>& entries) {
  for (FolderListing::Entry& e : entries) {
    if (e.directory || remote::isRemote(e.path) || (e.modified && e.size)) continue;
    struct stat st;
    if (::stat(e.path.c_str(), &st) != 0) continue;
    e.modified = st.st_mtime;
    e.size = static_cast<uint64_t>(st.st_size);
  }
}

}  // namespace

struct FolderListing::Pending {
  std::mutex mutex;
  const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
  bool done = false, taken = false;  // the entries are there; poll() has them
  bool unavailable = false;
  remote::Status status = remote::Status::Ok;
  std::string directory;
  std::vector<Entry> entries;
  bool detailsDone = false;  // ... then the same with the files' dates and sizes
  std::vector<Entry> details;
};

void FolderListing::read(const std::string& dir, bool subtitles, Order order,
                         const std::vector<std::string>& fallbacks) {
  if (dir != directory_) entries_.clear();
  directory_ = dir;
  // On its own thread, which keeps running if the browser moves on (a newer
  // read replaces this one; the old result is dropped).
  auto job = std::make_shared<Pending>();
  pending_ = job;
  reading_ = true;
  unavailable_ = false;
  status_ = remote::Status::Ok;
  std::thread([job, dir, subtitles, order, fallbacks] {
    std::string folder = dir;
    remote::Status status = remote::Status::Ok;
    size_t fallback = 0;
    std::vector<Entry> entries;
    while (!folder.empty()) {
      entries = listFolder(folder, subtitles, order, status);
      if (status != remote::Status::Missing) break;
      if (fallback < fallbacks.size()) {
        folder = fallbacks[fallback++];
        continue;
      }
      const std::string parent = remote::isRemote(folder) ? remote::parent(folder)
                                                   : fs::path(folder).parent_path().string();
      if (parent == folder || parent.empty()) break;
      folder = parent;
    }
    std::vector<Entry> details = entries;
    {
      std::lock_guard<std::mutex> lock(job->mutex);
      job->directory = folder;
      job->entries = std::move(entries);
      job->unavailable = status != remote::Status::Ok;
      job->status = remote::isRemote(folder) ? status : remote::Status::Ok;
      job->done = true;
    }
    addDetails(details);
    std::lock_guard<std::mutex> lock(job->mutex);
    job->details = std::move(details);
    job->detailsDone = true;
  }).detach();
}

bool FolderListing::poll() {
  if (!pending_) return false;
  const std::shared_ptr<Pending> job = pending_;  // alive until unlocked
  {
    std::lock_guard<std::mutex> lock(job->mutex);
    if (!job->done) {
      if (!reading_ || std::chrono::steady_clock::now() - job->started < std::chrono::seconds(10))
        return false;
      entries_.clear();
      reading_ = false;
      unavailable_ = true;
      if (remote::isRemote(directory_)) status_ = remote::Status::Offline;
      // A late successful read can still recover this folder, unless we move on.
      return true;
    }
    if (!job->taken) {
      directory_ = job->directory;
      entries_ = std::move(job->entries);
      job->taken = true;
      reading_ = false;
      unavailable_ = job->unavailable;
      status_ = job->status;
      return true;
    }
    if (!job->detailsDone) return false;
    entries_ = std::move(job->details);  // the same entries in the same order
  }
  pending_.reset();
  return true;
}

void FolderListing::arrange(const std::vector<size_t>& order) {
  if (order.size() != entries_.size()) return;
  std::vector<Entry> arranged;
  arranged.reserve(order.size());
  for (const size_t i : order) arranged.push_back(std::move(entries_[i]));
  entries_ = std::move(arranged);
}

bool FolderListing::matches(const Entry& entry, const std::string& query) {
  if (query.empty()) return true;
  // Files without their extension: "mp4" is not part of what you look for.
  const std::string haystack = entry.searchName.empty()
      ? searchable(foldFileName(entry.directory ? entry.name : fs::path(entry.name).stem().string()))
      : entry.searchName;
  const std::string words = searchable(foldFileName(query));
  size_t start = 0;
  while ((start = words.find_first_not_of(' ', start)) != std::string::npos) {
    const size_t end = std::min(words.find(' ', start), words.size());
    if (haystack.find(words.substr(start, end - start)) == std::string::npos) return false;
    start = end;
  }
  return true;
}

int FolderListing::matchCount(const std::string& query) const {
  int n = 0;
  for (const Entry& entry : entries_) n += matches(entry, query);
  return n;
}
