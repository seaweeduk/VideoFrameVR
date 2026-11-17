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
#include <cstdint>
#include <ctime>
#include <memory>
#include <thread>

namespace partial_stream {
namespace {

constexpr const char* kPrefix = "partialfile://";
// A file not written to for this long has no download behind it, for now: the
// video stops where its data does. mpv's other requests can wait for a read
// (a seek outside its cache does), so this is also the longest they wait.
constexpr time_t kStallSeconds = 30;

struct Stream {
  int fd = -1;
  std::string path;
  uint64_t position = 0;
  std::atomic<bool> cancelled{false};
  ~Stream() {
    if (fd >= 0) close(fd);
  }
};

// Whether more of the file may still come. A download renames its .part file
// once it is complete; the name then no longer leads to the file being read.
bool beingWritten(const Stream& s, const struct stat& file) {
  struct stat named;
  if (stat(s.path.c_str(), &named) != 0 || named.st_ino != file.st_ino || named.st_dev != file.st_dev)
    return false;
  const time_t age = std::time(nullptr) - file.st_mtime;
  return age >= 0 && age < kStallSeconds;
}

int64_t readStream(void* cookie, char* out, uint64_t count) {
  auto& s = *static_cast<Stream*>(cookie);
  while (!s.cancelled) {
    const ssize_t n = pread(s.fd, out, count, static_cast<off_t>(s.position));
    if (n > 0) {
      s.position += static_cast<uint64_t>(n);
      return n;
    }
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    struct stat file;
    if (fstat(s.fd, &file) != 0) return -1;
    // Past the end, a demuxer looks for something that is not there yet (an
    // index at the end of the file); it isn't data to wait for.
    if (s.position != static_cast<uint64_t>(file.st_size)) {
      if (s.position > static_cast<uint64_t>(file.st_size)) return 0;
      continue;  // it has grown since the read
    }
    if (!beingWritten(s, file)) return 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
  if (fstat(static_cast<Stream*>(cookie)->fd, &file) != 0) return MPV_ERROR_UNSUPPORTED;
  return file.st_size;
}

int openStream(void*, char* uri, mpv_stream_cb_info* info) {
  auto s = std::make_unique<Stream>();
  s->path = std::string(uri).substr(std::char_traits<char>::length(kPrefix));
  s->fd = open(s->path.c_str(), O_RDONLY | O_CLOEXEC);
  if (s->fd < 0) return errno == ENOENT ? MPV_ERROR_LOADING_FAILED : MPV_ERROR_GENERIC;
  info->cookie = s.release();
  info->read_fn = readStream;
  info->seek_fn = seekStream;
  info->size_fn = sizeOfStream;
  info->cancel_fn = [](void* cookie) { static_cast<Stream*>(cookie)->cancelled = true; };
  info->close_fn = [](void* cookie) { delete static_cast<Stream*>(cookie); };
  return 0;
}

}  // namespace

void addTo(mpv_handle* mpv) { mpv_stream_cb_add_ro(mpv, "partialfile", nullptr, openStream); }

std::string mpvUrl(const std::string& path) {
  return isPartialDownload(path) && path.find("://") == std::string::npos ? kPrefix + path : path;
}

}  // namespace partial_stream
