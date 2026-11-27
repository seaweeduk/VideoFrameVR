#include "partial_stream.h"

#include "file_util.h"

#include <fcntl.h>
#include <mpv/client.h>
#include <mpv/stream_cb.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

namespace partial_stream {
namespace {

constexpr const char* kPrefix = "partialfile://";
constexpr std::chrono::milliseconds kLook{100};  // how often a waiting read looks again

struct Stream {
  int fd = -1;
  std::string path;
  dev_t device = 0;
  ino_t inode = 0;
  uint64_t position = 0;
  // Reads at the end stop waiting kPartialStall after it last grew,
  // however many there are.
  uint64_t sizeSeen = 0;
  std::chrono::steady_clock::time_point grewAt = std::chrono::steady_clock::now();
  std::atomic<bool> cancelled{false};
  std::mutex mutex;
  std::condition_variable cv;  // cancel_fn ends a wait with it
  ~Stream() {
    if (fd >= 0) ::close(fd);
  }
};

// Whether its download may still be writing it: once finished the browser
// renames it, and the name no longer leads to this file (nor if the download
// was cancelled and the file deleted).
bool beingWritten(const Stream& s) {
  struct stat named;
  return ::stat(s.path.c_str(), &named) == 0 && named.st_dev == s.device && named.st_ino == s.inode;
}

int64_t readStream(void* cookie, char* out, uint64_t count) {
  auto& s = *static_cast<Stream*>(cookie);
  bool finished = false;
  while (!s.cancelled) {
    const ssize_t n = ::pread(s.fd, out, count, static_cast<off_t>(s.position));
    if (n > 0) {
      s.position += static_cast<uint64_t>(n);
      return n;
    }
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    struct stat file;
    if (::fstat(s.fd, &file) != 0) return -1;
    const auto size = static_cast<uint64_t>(file.st_size);
    // Past the end, a demuxer looks for what isn't there yet (an index at
    // the end of the file): not data to wait for.
    if (s.position > size) return 0;
    if (s.position < size) continue;  // it grew since the read
    // Finished: its end, once a read after the rename finds nothing more (the
    // last block may have come between the fstat() above and the rename).
    if (!beingWritten(s)) {
      if (finished) return 0;
      finished = true;
      continue;
    }
    const auto now = std::chrono::steady_clock::now();
    if (size > s.sizeSeen) {
      s.sizeSeen = size;
      s.grewAt = now;
    } else if (now - s.grewAt >= kPartialStall) {
      return 0;
    }
    std::unique_lock lock(s.mutex);
    s.cv.wait_for(lock, kLook, [&] { return s.cancelled.load(); });
  }
  return -1;
}

int64_t seekStream(void* cookie, int64_t offset) {
  if (offset < 0) return MPV_ERROR_GENERIC;
  static_cast<Stream*>(cookie)->position = static_cast<uint64_t>(offset);
  return offset;
}

int64_t sizeOfStream(void* cookie) {
  struct stat file;
  if (::fstat(static_cast<Stream*>(cookie)->fd, &file) != 0) return MPV_ERROR_UNSUPPORTED;
  return file.st_size;
}

void cancelStream(void* cookie) {
  auto& s = *static_cast<Stream*>(cookie);
  {
    std::lock_guard lock(s.mutex);
    s.cancelled = true;
  }
  s.cv.notify_all();
}

// Opened as oshash.cpp opens a file: a pipe or a device named like a
// partial download never blocks the open, and isn't read.
int openStream(void*, char* uri, mpv_stream_cb_info* info) {
  auto s = std::make_unique<Stream>();
  s->path = std::string(uri).substr(std::char_traits<char>::length(kPrefix));
  s->fd = ::open(s->path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (s->fd < 0) return MPV_ERROR_LOADING_FAILED;
  struct stat file;
  if (::fstat(s->fd, &file) != 0 || !S_ISREG(file.st_mode)) return MPV_ERROR_LOADING_FAILED;
  s->device = file.st_dev;
  s->inode = file.st_ino;
  info->cookie = s.release();
  info->read_fn = readStream;
  info->seek_fn = seekStream;
  info->size_fn = sizeOfStream;
  info->cancel_fn = cancelStream;
  info->close_fn = [](void* cookie) { delete static_cast<Stream*>(cookie); };
  return 0;
}

}  // namespace

void addTo(mpv_handle* mpv) { mpv_stream_cb_add_ro(mpv, "partialfile", nullptr, openStream); }

std::string mpvUrl(const std::string& path) { return kPrefix + path; }

}  // namespace partial_stream
