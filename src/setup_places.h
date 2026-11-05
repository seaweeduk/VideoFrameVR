#pragma once

#include "removable_drives.h"
#include "remote.h"

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// First-run setup's Add folders (docs/alpha/first-run.md), off the render
// thread: the drives mounted, and one folder's listing at a time as the
// tree opens it. Each runs on a detached thread and leaves its result for
// the main thread; one dropped (the step left, a share that doesn't answer)
// is never waited for.
namespace setup_places {

// The drives under /run/media (mountedDrives()).
std::vector<RemovableDrives::Drive> findDrives();

// One folder as the tree shows it, from a single listing: its subfolders
// (lost+found, trash and recycle bins left out, and /proc, /sys and /dev;
// hidden ones after the others), and how many videos are directly in it. Nothing deeper is read
// and no file is opened. smb://host lists the host's shares (remote.h).
struct Folder {
  remote::Status status = remote::Status::Ok;
  struct Sub {
    std::string name;
    // Whether it has subfolders, where its link count tells without
    // reading it (ext4, xfs, FAT): -1 not known.
    int folders = -1;
  };
  std::vector<Sub> folders;
  int videos = 0;
};
Folder readFolder(const std::string& path);

// A value worked out on a detached thread, taken once it is there.
template <typename T>
class Async {
 public:
  template <typename Work>
  void start(Work work) {
    auto box = std::make_shared<Box>();
    box_ = box;
    std::thread([box, work] {
      T value = work();
      std::lock_guard<std::mutex> lock(box->mutex);
      box->value = std::move(value);
      box->done = true;
    }).detach();
  }
  // Once it is done, its value (once).
  bool take(T& value) {
    if (!box_) return false;
    {
      std::lock_guard<std::mutex> lock(box_->mutex);
      if (!box_->done) return false;
      value = std::move(box_->value);
    }
    box_.reset();
    return true;
  }
  bool running() const { return bool(box_); }

 private:
  struct Box {
    std::mutex mutex;
    bool done = false;
    T value;
  };
  std::shared_ptr<Box> box_;
};

}  // namespace setup_places
