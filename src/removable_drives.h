#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// SteamOS drive-detection and automount policy, with the associated explanation,
// credited to FramePlayerX, Copyright 2026 remotesojourner, Apache-2.0.
// Modified for VideoFrameVR by 2026-11-03; attribution added 2026-11-10.
// See packaging/frameplayerx/ for upstream revisions, license and notices.
//
// microSD cards and USB drives. udisks mounts them under /run/media; the
// browser's navigation lists what is mounted there (places.h). On the Frame,
// SteamOS mounts only ext4 microSD cards (Steam needs symlinks) and no USB
// drives at all, so the app mounts the others itself, as Steam's own
// automounter would: exFAT, FAT and NTFS cards, and USB drives with those or
// ext4, through udisks over D-Bus (the system bus reaches into Steam's
// runtime container; no udisksctl there), which SteamOS's polkit rules allow
// without a password. Once per insertion: a drive unmounted in Steam stays so.
// A worker thread looks every few seconds, at the lowest CPU priority.
class RemovableDrives {
 public:
  enum class Kind { Other, Card, Usb };
  struct Drive {
    std::string mountPoint;
    Kind kind = Kind::Other;
    std::string label;  // its filesystem's label; empty if it has none
  };
  struct Change {
    std::vector<Drive> added;           // mounted since the last change
    std::vector<std::string> removed;   // mount points gone
    std::vector<std::string> failures;  // drives that could not be mounted: why
    std::vector<Kind> failedKinds;
  };

  RemovableDrives() = default;
  RemovableDrives(const RemovableDrives&) = delete;
  RemovableDrives& operator=(const RemovableDrives&) = delete;
  ~RemovableDrives();

  // Starts looking. `mountOthers`: mount what the system leaves alone (the Frame).
  void start(bool mountOthers);
  // Main thread: what changed since the last call. The drives mounted when
  // the app started are not reported as added.
  bool poll(Change& change);

 private:
  void work();

  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool quit_ = false;
  bool mountOthers_ = false;
  bool haveChange_ = false;
  Change change_;
};

// The drives mounted under /run/media now, by their mount points.
std::vector<RemovableDrives::Drive> mountedDrives();
