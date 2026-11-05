#include "stash_folders.h"

#include "remote.h"
#include "sftp.h"
#include "stash_settings.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include <dirent.h>
#include <mntent.h>
#include <sys/stat.h>

namespace {

constexpr size_t kRoots = 64;
constexpr size_t kSubfolders = 50;
constexpr size_t kEntries = 4096;
constexpr size_t kStats = 16384;  // Includes directory checks, across the whole search.
constexpr size_t kListings = 128;

std::string join(const std::string& folder, const std::string& name) {
  return folder + (folder.back() == '/' ? "" : "/") + name;
}

// Where a Stash path's folder names start: after the "/" of a Linux or
// macOS path, a drive's "D:" and separator, or a UNC path's server and
// separator; npos if it is none of those.
size_t firstPart(const std::string& path) {
  if (!path.empty() && path[0] == '/') return 1;
  if (!stashWindowsPath(path)) return std::string::npos;
  if (path[1] == ':') return path.size() > 3 ? 3 : std::string::npos;
  const size_t separator = path.find_first_of("\\/", 2);
  return separator == std::string::npos ? separator : separator + 1;
}

bool under(const std::string& path, const std::string& folder) {
  return path.compare(0, folder.size(), folder) == 0 &&
         (folder == "/" || path.size() == folder.size() || path[folder.size()] == '/');
}

struct Search {
  const std::atomic<uint64_t>& generation;
  uint64_t id;
  size_t stats = 0, listings = 0;
  struct Info {
    bool file = false, folder = false;
    uint64_t size = 0;
  };
  std::unordered_map<std::string, Info> checked;
  struct Listing {
    std::unordered_set<std::string> names;
    std::vector<std::string> folders;
  };
  std::unordered_map<std::string, Listing> listed;
  std::set<std::string> candidates;
  std::set<std::string> complete;
  std::set<std::pair<std::string, std::string>> mappings;
  std::vector<StashFolderFinder::Mapping> found;

  Search(const std::atomic<uint64_t>& generation, uint64_t id) : generation(generation), id(id) {}

  bool stopped() const { return generation != id || stats >= kStats; }

  bool covered(const std::string& folder) const {
    return std::any_of(complete.begin(), complete.end(),
                       [&](const auto& root) { return under(folder, root); });
  }

  Info stat(const std::string& path) {
    const auto it = checked.find(path);
    if (it != checked.end()) return it->second;
    if (stopped()) return {};
    ++stats;
    Info info;
    if (remote::isRemote(path)) {
      remote::Entry entry;
      if (remote::stat(path, entry) == remote::Status::Ok)
        info = {!entry.folder, entry.folder, entry.size};
    } else {
      struct stat st;
      if (::stat(path.c_str(), &st) == 0)
        info = {S_ISREG(st.st_mode), S_ISDIR(st.st_mode), static_cast<uint64_t>(st.st_size)};
    }
    checked.emplace(path, info);
    return info;
  }

  const Listing& list(const std::string& folder) {
    static const Listing none;
    auto [it, added] = listed.try_emplace(folder);
    Listing& out = it->second;
    if (!added) return out;
    if (stopped() || listings >= kListings) {  // not read: a later job may
      listed.erase(it);
      return none;
    }
    ++listings;
    if (remote::isRemote(folder)) {
      std::vector<remote::Entry> entries;
      if (remote::list(folder, entries) != remote::Status::Ok) return out;
      const size_t limit = remote::parent(folder).empty() ? kRoots : kSubfolders;
      for (const auto& entry : entries) {
        if (stopped()) break;
        if (entry.name == "." || entry.name == "..") continue;
        out.names.insert(entry.name);
        if (entry.folder && out.folders.size() < limit)
          out.folders.push_back(join(folder, entry.name));
      }
    } else if (DIR* dir = ::opendir(folder.c_str())) {
      size_t entries = 0;
      while (!stopped() && entries++ < kEntries) {
        const dirent* entry = ::readdir(dir);
        if (!entry) break;
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        out.names.insert(name);
        const std::string path = join(folder, name);
        if (out.folders.size() < kSubfolders &&
            (entry->d_type == DT_DIR ||
             ((entry->d_type == DT_UNKNOWN || entry->d_type == DT_LNK) && stat(path).folder)))
          out.folders.push_back(path);
      }
      ::closedir(dir);
    }
    std::sort(out.folders.begin(), out.folders.end());
    return out;
  }

  std::vector<std::string> roots() {
    std::vector<std::string> roots;
    for (const auto& server : remote::servers()) {
      if (stopped() || roots.size() >= kRoots) break;
      if (server.root.rfind("sftp://", 0) == 0) {  // its "/" (Stash's own computer) and the account's home
        roots.push_back(server.root);
        std::string home;
        if (sftp::home(server.root, home) == remote::Status::Ok && home != server.root) roots.push_back(home);
        continue;
      }
      for (const auto& share : list(server.root).folders) {
        if (roots.size() >= kRoots) break;
        if (share.back() != '$') roots.push_back(share);
      }
    }
    FILE* mounts = ::setmntent("/proc/mounts", "r");
    if (mounts) {
      mntent entry;
      char buffer[8192];
      while (!stopped() && roots.size() < kRoots &&
             ::getmntent_r(mounts, &entry, buffer, sizeof(buffer))) {
        const std::string type = entry.mnt_type, path = entry.mnt_dir;
        const bool network = type == "cifs" || type == "smb3" || type == "nfs" || type == "nfs4" ||
                             (type.compare(0, 5, "fuse.") == 0 && type != "fuse.portal");
        const bool disk = type == "ext2" || type == "ext3" || type == "ext4" || type == "btrfs" ||
                          type == "xfs" || type == "exfat" || type == "vfat" || type == "ntfs" ||
                          type == "ntfs3" || type == "fuseblk" || type == "zfs" || type == "f2fs";
        bool media = false;
        for (const char* folder : {"/mnt", "/media", "/run/media", "/var/mnt", "/srv", "/home"})
          media |= under(path, folder);
        if (network || (disk && media)) roots.push_back(path);
      }
      ::endmntent(mounts);
    }
    if (const char* home = std::getenv("HOME"))
      if (*home && roots.size() < kRoots) roots.push_back(home);
    return roots;
  }

  std::vector<std::string> children(const std::string& folder, size_t limit) {
    if (stopped() || covered(folder)) return {};
    const auto& folders = list(folder).folders;
    return {folders.begin(), folders.begin() + std::min(limit, folders.size())};
  }

  // Whether Stash's files are under `folder` here: for each part of a
  // file's path whose name is in the folder, the mapping that puts the file
  // there, kept if the file is there at its size, with how many of the
  // others it finds too.
  void look(const std::string& folder, const std::vector<StashFolderFinder::File>& files) {
    if (stopped() || covered(folder) || !candidates.insert(folder).second) return;
    const auto& names = list(folder).names;
    auto at = [&](const std::string& rest) { return rest.empty() ? folder : join(folder, rest.substr(1)); };
    for (const auto& file : files) {
      const std::string& path = file.path;
      const bool windows = stashWindowsPath(path);
      size_t part = firstPart(path);
      if (part == std::string::npos || part >= path.size() || path.back() == '/' ||
          (windows && path.back() == '\\'))
        continue;
      while (part < path.size()) {
        if (stopped()) return;
        const size_t next = path.find_first_of(windows ? "\\/" : "/", part);
        const std::string from = part == 1 && !windows ? "/" : path.substr(0, part - 1);
        std::string rest;
        if (names.count(path.substr(part, next - part)) && !mappings.count({from, folder}) &&
            stashUnder(path, from, &rest)) {
          const Info info = stat(at(rest));
          if (info.file && info.size == file.size) {
            mappings.emplace(from, folder);
            StashFolderFinder::Mapping mapping{from, folder, 0};
            for (const auto& other : files) {
              std::string otherRest;
              if (!stashUnder(other.path, from, &otherRest) || otherRest.empty()) continue;
              const Info check = stat(at(otherRest));
              mapping.found += check.file && check.size == other.size;
            }
            if (!stopped()) {
              const bool all = mapping.found == static_cast<int>(files.size());
              found.push_back(std::move(mapping));
              if (all) {
                complete.insert(folder);
                return;
              }
            }
          }
        }
        if (next == std::string::npos) break;
        part = next + 1;
      }
    }
  }

  // One job: its budget anew, what was read before kept.
  std::vector<StashFolderFinder::Mapping> run(const std::vector<StashFolderFinder::File>& files,
                                             std::vector<std::string> folders) {
    stats = listings = 0;
    candidates.clear();
    complete.clear();
    mappings.clear();
    found.clear();
    if (files.empty() || generation != id) return {};
    if (folders.empty()) folders = roots();
    std::vector<std::string> roots;
    for (std::string& folder : folders) {
      while (folder.size() > 1 && folder.back() == '/') folder.pop_back();
      if (folder.empty() || roots.size() >= kRoots ||
          std::find(roots.begin(), roots.end(), folder) != roots.end()) continue;
      roots.push_back(std::move(folder));
    }
    // Check every root before spending the budget on its children.
    for (const auto& root : roots) look(root, files);
    for (const auto& root : roots) {
      if (stopped()) break;
      std::vector<std::string> folders = children(root, kSubfolders);
      const size_t firstLevel = folders.size();
      for (size_t i = 0; i < folders.size() && !stopped(); ++i) {
        look(folders[i], files);
        if (i < firstLevel && folders.size() < kSubfolders) {
          const auto next = children(folders[i], kSubfolders - folders.size());
          folders.insert(folders.end(), next.begin(), next.end());
        }
      }
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
      if (a.found != b.found) return a.found > b.found;
      if (a.pathFrom.size() != b.pathFrom.size()) return a.pathFrom.size() < b.pathFrom.size();
      return std::tie(a.pathFrom, a.pathTo) < std::tie(b.pathFrom, b.pathTo);
    });
    return std::move(found);
  }
};

}  // namespace

struct StashFolderFinder::State {
  std::mutex mutex;
  std::condition_variable wake;
  std::atomic<uint64_t> generation{0};
  std::atomic<bool> busy{false};
  std::atomic<int> jobCount{0}, jobsDone{0};
  bool stop = false, pending = false, ready = false;
  std::vector<Job> jobs;
  std::vector<std::string> folders, also;
  std::vector<Result> result;
  std::thread worker;

  State() : worker([this] { run(); }) {}
  ~State() {
    {
      std::lock_guard lock(mutex);
      stop = true;
      ++generation;
    }
    wake.notify_one();
    worker.join();
  }

  void run() {
    std::unique_lock lock(mutex);
    for (;;) {
      wake.wait(lock, [this] { return stop || pending; });
      if (stop) return;
      const uint64_t id = generation;
      auto input = std::move(jobs);
      auto roots = std::move(folders);
      auto first = std::move(also);
      pending = false;
      lock.unlock();
      Search search{generation, id};
      std::vector<Result> results;
      // The likely places are found once, for every job.
      if (roots.empty() && !input.empty()) {
        roots = search.roots();
        roots.insert(roots.begin(), first.begin(), first.end());
      }
      for (const Job& job : input) {
        results.push_back({job.folder, search.run(job.files, roots)});
        if (generation == id) ++jobsDone;
      }
      lock.lock();
      if (generation == id) {
        result = std::move(results);
        ready = true;
        busy = false;
      }
    }
  }
};

StashFolderFinder::StashFolderFinder() : state_(std::make_shared<State>()) {}
StashFolderFinder::~StashFolderFinder() = default;

void StashFolderFinder::find(std::vector<Job> jobs, std::vector<std::string> folders,
                             std::vector<std::string> also) {
  {
    std::lock_guard lock(state_->mutex);
    ++state_->generation;
    state_->jobCount = static_cast<int>(jobs.size());
    state_->jobsDone = 0;
    state_->jobs = std::move(jobs);
    state_->folders = std::move(folders);
    state_->also = std::move(also);
    state_->pending = true;
    state_->ready = false;
    state_->busy = true;
  }
  found_.clear();
  state_->wake.notify_one();
}

bool StashFolderFinder::poll() {
  std::unique_lock lock(state_->mutex, std::try_to_lock);
  if (!lock || !state_->ready) return false;
  found_.swap(state_->result);
  state_->ready = false;
  return true;
}

bool StashFolderFinder::running() const { return state_->busy; }
int StashFolderFinder::jobs() const { return state_->jobCount; }
int StashFolderFinder::done() const { return state_->jobsDone; }

const std::vector<StashFolderFinder::Mapping>& StashFolderFinder::foundFor(
    const std::string& folder) const {
  static const std::vector<Mapping> none;
  for (const Result& r : found_)
    if (r.folder == folder) return r.found;
  return none;
}
