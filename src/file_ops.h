#pragma once

#include <cstdint>
#include <string>

// Deleting the playing file (docs/alpha/delete-file.md): one regular file,
// for good, never a folder and never what a link points to; by its path
// (also on a kernel-mounted share) or its remote locator (remote.h).

// A file's size and date, as the delete dialog showed them.
struct FileStamp {
  uint64_t size = 0;
  int64_t modified = 0;  // Unix seconds
  long nanos = 0;        // ... and nanoseconds
  bool operator==(const FileStamp& o) const {
    return size == o.size && modified == o.modified && nanos == o.nanos;
  }
};

enum class DeleteStatus { Ok, Changed, Gone, NotAFile, Denied, InUse, Offline, Failed };

// Looks at the file at `location` (not following a link): Ok and its stamp
// if it is a regular file. Blocks: workers only.
DeleteStatus stampOf(const std::string& location, FileStamp& stamp);
// Deletes it for good if it is still a regular file with the stamp
// `expected`, else Changed and nothing is deleted. Failed: `error` says why.
// Blocks: workers only.
DeleteStatus removeFile(const std::string& location, const FileStamp& expected, std::string& error);
