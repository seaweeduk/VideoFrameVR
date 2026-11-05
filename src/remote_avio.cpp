#include "remote_avio.h"
#include "stash_stream.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

#include <cerrno>
#include <limits>

RemoteAvio::~RemoteAvio() {
  if (stash_.cookie) stash_.close_fn(stash_.cookie);
  if (io_) av_freep(&io_->buffer);
  avio_context_free(&io_);
}

bool RemoteAvio::attach(AVFormatContext* format, const std::string& path) {
  if (isStashScene(path)) {
    if (!openStashScene(path, stash_)) return false;
  } else {
    if (!remote::isRemote(path)) return true;
    remote::Status status;
    file_ = remote::File::open(path, status);
    if (!file_) return false;
  }
  constexpr int kBufferSize = 256 * 1024;
  auto* buffer = static_cast<uint8_t*>(av_malloc(kBufferSize));
  if (!buffer) return false;
  io_ = avio_alloc_context(buffer, kBufferSize, 0, this, read, nullptr, seek);
  if (!io_) {
    av_free(buffer);
    return false;
  }
  interrupt_ = format->interrupt_callback;
  format->pb = io_;
  format->flags |= AVFMT_FLAG_CUSTOM_IO;
  return true;
}

int RemoteAvio::read(void* opaque, uint8_t* out, int count) {
  auto& self = *static_cast<RemoteAvio*>(opaque);
  if (self.interrupt_.callback && self.interrupt_.callback(self.interrupt_.opaque))
    return AVERROR_EXIT;
  const int64_t n = !self.stash_.cookie ? self.file_->read(self.position_, out, count)
                    : self.stash_.seek_fn(self.stash_.cookie, self.position_) < 0
                        ? -1
                        : self.stash_.read_fn(self.stash_.cookie, reinterpret_cast<char*>(out), count);
  if (n < 0) return AVERROR(EIO);
  if (!n) return AVERROR_EOF;
  self.position_ += n;
  return static_cast<int>(n);
}

int64_t RemoteAvio::seek(void* opaque, int64_t offset, int whence) {
  auto& self = *static_cast<RemoteAvio*>(opaque);
  whence &= ~AVSEEK_FORCE;
  if (whence == AVSEEK_SIZE) return self.size();
  int64_t base = 0;
  if (whence == SEEK_CUR) base = self.position_;
  else if (whence == SEEK_END) base = self.size();
  else if (whence != SEEK_SET) return AVERROR(EINVAL);
  if (offset < -base || offset > std::numeric_limits<int64_t>::max() - base)
    return AVERROR(EINVAL);
  self.position_ = base + offset;
  return self.position_;
}

int64_t RemoteAvio::size() const {
  return stash_.cookie ? stash_.size_fn(stash_.cookie) : static_cast<int64_t>(file_->size());
}
