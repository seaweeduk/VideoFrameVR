#include "setup_places.h"

#include "file_util.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cerrno>

namespace setup_places {

namespace {

bool hiddenName(const std::string& name) { return !name.empty() && name[0] == '.'; }

}  // namespace

std::vector<RemovableDrives::Drive> findDrives() { return mountedDrives(); }

Folder readFolder(const std::string& path) {
  Folder f;
  auto add = [&](const std::string& name, bool folder, int folders) {
    if (folder) {
      if (!binOrLost(name)) f.folders.push_back({name, folders});
    } else if (!hiddenName(name) && isVideoFile(name)) {
      ++f.videos;  // as the Library index lists them: hidden files left out
    }
  };
  if (remote::isRemote(path)) {
    std::vector<remote::Entry> entries;
    f.status = remote::list(path, entries);
    for (const remote::Entry& e : entries)
      if (!e.name.empty()) add(e.name, e.folder, -1);
  } else if (DIR* dir = opendir(path.c_str())) {
    const int fd = dirfd(dir);
    while (const dirent* e = readdir(dir)) {
      const std::string name = e->d_name;
      if (name == "." || name == ".." ||
          (path == "/" && (name == "proc" || name == "sys" || name == "dev")))
        continue;
      struct stat st;
      const bool stated = e->d_type == DT_DIR || e->d_type == DT_UNKNOWN || e->d_type == DT_LNK
                              ? fstatat(fd, e->d_name, &st, 0) == 0
                              : false;
      const bool folder = stated ? S_ISDIR(st.st_mode) : e->d_type == DT_DIR;
      // A folder's link count is 2 and one per subfolder on most file
      // systems; 1 (btrfs, many subfolders) says nothing.
      add(name, folder, folder && stated && st.st_nlink >= 2 ? st.st_nlink > 2 : -1);
    }
    closedir(dir);
  } else {
    f.status = errno == ENOENT || errno == ENOTDIR ? remote::Status::Missing : remote::Status::Denied;
  }
  std::sort(f.folders.begin(), f.folders.end(), [](const Folder::Sub& a, const Folder::Sub& b) {
    if (hiddenName(a.name) != hiddenName(b.name)) return hiddenName(b.name);
    return fileNameLess(a.name, b.name);
  });
  return f;
}

}  // namespace setup_places
