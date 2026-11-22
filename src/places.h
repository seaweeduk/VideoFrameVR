#pragma once

#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

// The places the browser's navigation lists under its file explorer, as a
// Linux file manager would: pinned folders, home, videos, the media library, the file
// system and mounted drives (in /mnt and /media/$USER, and microSD cards and
// USB drives as udisks mounts them, removable_drives.h).
// Looked for on a worker thread: a mount point of a dead network share can
// block whoever looks at it.
// The user's order, the places hidden and their names are kept with the pins
// in pinned_folders.conf; a drive taken out keeps its place there.
class Places {
 public:
  // What a place is, for its icon.
  enum class Kind { Folder, Home, Videos, Card, Usb, Drive };
  struct Place {
    std::string label;  // its name, else its own
    std::string path;
    bool pinned = false;
    bool available = true;
    bool hidden = false;   // not in the navigation
    std::string ownName;   // the folder's or the drive's
    Kind kind = Kind::Folder;
  };
  // What home, videos and the file system are called, and drives without a label.
  struct Names {
    std::string home, videos, fileSystem, card, usbDrive, drive;
  };
  // Starts looking (a newer look replaces an unfinished one). `library`:
  // the default media folder.
  void refresh(const std::string& library, const Names& names);
  // Takes a finished look; true if places() changed.
  bool poll();
  // In the user's order, hidden ones too.
  const std::vector<Place>& places() const { return places_; }
  bool ready() const { return loaded_; }
  bool pinned(const std::string& path) const;
  void togglePin(const std::string& path);
  // Edit places: moved by one among those shown in the editor (-1 up, 1 down),
  // hidden or shown, named ("" for its own name); back to the order found,
  // all shown, names kept.
  void move(const std::string& path, int by);
  void setHidden(const std::string& path, bool hidden);
  void rename(const std::string& path, const std::string& name);
  void resetOrder();

  // As kept in the file, with the places not there now.
  struct Entry {
    std::string path;
    bool pinned = false;
    bool hidden = false;
    std::string name;
  };

 private:
  struct Pending;
  // places_ from entries_ and the last look; places found but not in
  // entries_ join it after the one found before them.
  void arrange();
  void save();
  std::vector<Entry> entries_;
  std::vector<Place> found_;               // the last look's places other than pins, in its order
  std::map<std::string, bool> available_;  // the last look's pins
  std::vector<Place> places_;
  std::shared_ptr<Pending> pending_;
  std::shared_future<void> saved_;
  bool loaded_ = false;
};
