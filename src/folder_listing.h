#pragma once

#include "remote.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// One folder of the file browser: its subfolders and its files of one kind
// (videos and photos, or subtitle files), folders first, each in the chosen
// order; and which of them a name search matches.
class FolderListing {
 public:
  // Kept by number (folders.conf): new orders go at the end. Those from
  // RatingHigh to AddedOldest come from the media library, which the
  // listing does not know: it lists by name, and the browser arranges the
  // files (arrange()).
  enum class Order { NameUp, NameDown, Newest, Oldest, Largest, RatingHigh, RatingLow, PlaysMost,
                     PlaysFewest, PlayedNewest, PlayedOldest, AddedNewest, AddedOldest, Smallest,
                     kCount };
  static bool byLibrary(Order order) {
    return order >= Order::RatingHigh && order <= Order::AddedOldest;
  }
  struct Entry {
    std::string name;
    std::string path;
    bool directory;
    int64_t modified = 0;  // seconds since 1970 (0 = not known yet)
    uint64_t size = 0;     // bytes (files; 0 = not known yet)
    std::string nameKey;     // collation key, prepared by the worker
    std::string searchName;  // folded, without the extension, prepared by the worker
  };

  // Reads `dir`, falling back to its nearest existing parent if missing.
  // An unreadable folder reports unavailable(). Hidden files are left
  // out, hidden folders are listed. Dates and sizes cost a file system query
  // each (slow on network shares): the order's are read first, the files'
  // others for the browser's columns afterwards, and poll() brings them in
  // place. Sorted by date, a folder's date is that of the newest thing in it.
  // All reads run in the background: until poll() brings one,
  // entries() is empty for another folder and the old order for this one.
  void read(const std::string& dir, bool subtitles, Order order,
            const std::vector<std::string>& fallbacks = {});
  // Takes a finished background read; true if entries() changed.
  bool poll();
  bool reading() const { return reading_; }
  bool unavailable() const { return unavailable_; }
  remote::Status status() const { return status_; }
  const std::string& directory() const { return directory_; }
  const std::vector<Entry>& entries() const { return entries_; }
  // Puts the entries in this order: entries()[order[i]] becomes the i-th.
  // Until the next poll() that brings entries.
  void arrange(const std::vector<size_t>& order);

  // Whether the entry's name holds all words of `query`, ignoring case,
  // punctuation and a file's extension. An empty query matches everything.
  static bool matches(const Entry& entry, const std::string& query);
  int matchCount(const std::string& query) const;

 private:
  struct Pending;
  std::string directory_;
  std::vector<Entry> entries_;
  std::shared_ptr<Pending> pending_;  // the background read, if one is still due
  bool reading_ = false;              // ... and its entries have not arrived yet
  bool unavailable_ = false;
  remote::Status status_ = remote::Status::Ok;
};
