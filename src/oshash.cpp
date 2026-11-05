#include "oshash.h"
#include "remote.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr int64_t kChunk = 64 * 1024;

bool readAt(int fd, remote::File* remote, unsigned char* buf, int64_t length, int64_t offset) {
  while (length > 0) {
    const int64_t n = remote ? remote->read(offset, buf, length)
                            : ::pread(fd, buf, static_cast<size_t>(length), offset);
    if (n <= 0) return false;
    buf += n;
    length -= n;
    offset += n;
  }
  return true;
}

uint64_t sumWords(const unsigned char* buf, int64_t length) {
  uint64_t sum = 0;
  for (int64_t i = 0; i + 8 <= length; i += 8) {
    uint64_t v;
    std::memcpy(&v, buf + i, 8);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap64(v);
#endif
    sum += v;  // wraps, as in Stash
  }
  return sum;
}

}  // namespace

// As Stash: no hash for 8 bytes or less; a file under 64 KiB is read as
// its first and its last whole number of words (which overlap).
int64_t oshashChunk(uint64_t size) {
  if (size <= 8) return 0;
  return size < static_cast<uint64_t>(kChunk) ? static_cast<int64_t>(size / 8 * 8) : kChunk;
}

std::string oshashOfEnds(uint64_t size, const unsigned char* ends) {
  const uint64_t hash = size + sumWords(ends, 2 * oshashChunk(size));
  char hex[17];
  std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
  return hex;
}

FileFingerprint fingerprintFile(const std::string& path) {
  FileFingerprint print;
  int fd = -1;
  std::unique_ptr<remote::File> remote;
  if (remote::isRemote(path)) {
    remote::Entry st;
    if (remote::stat(path, st) != remote::Status::Ok) return print;
    remote::Status status;
    remote = remote::File::open(path, status);
    if (!remote) return print;
    print.size = remote->size();
    print.mtime = st.modified;
  } else {
    fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return print;
    struct stat st;
    if (::fstat(fd, &st) != 0) {
      ::close(fd);
      return print;
    }
    print.size = static_cast<uint64_t>(st.st_size);
    print.mtime = st.st_mtime;
  }
  const int64_t size = print.size;
  const int64_t chunk = oshashChunk(print.size);
  if (!chunk) {
    if (fd >= 0) ::close(fd);
    return print;
  }
  std::vector<unsigned char> buf(static_cast<size_t>(2 * chunk));
  const bool read = readAt(fd, remote.get(), buf.data(), chunk, 0) &&
                    readAt(fd, remote.get(), buf.data() + chunk, chunk, size - chunk);
  if (fd >= 0) ::close(fd);
  if (!read) return print;
  print.oshash = oshashOfEnds(print.size, buf.data());
  return print;
}
