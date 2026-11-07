#include "bluray.h"

#include "file_util.h"
#include "remote.h"

#include <mpv/client.h>
#include <mpv/stream_cb.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace bdmv {
namespace {

// A remote image, read with background priority (parsing never delays
// playback's reads).
class RemoteSource : public Source {
 public:
  explicit RemoteSource(std::unique_ptr<remote::File> file) : file_(std::move(file)) {}
  uint64_t size() const override { return file_->size(); }
  bool read(uint64_t offset, void* out, uint64_t count) override {
    auto* p = static_cast<char*>(out);
    while (count > 0) {
      const int64_t n = file_->read(offset, p, count);
      if (n <= 0) return false;
      p += n;
      offset += n;
      count -= n;
    }
    return true;
  }

 private:
  std::unique_ptr<remote::File> file_;
};

struct Kept {
  uint64_t size = 0;
  int64_t modified = 0;  // ns (local) or s (remote)
  std::shared_ptr<const Disc> disc;
};
struct Cache {
  std::mutex mutex;
  std::map<std::string, Kept> discs;
};
Cache& cache() {
  static Cache& c = *new Cache;  // mpv's threads may still read it at exit
  return c;
}
constexpr size_t kKept = 32;

// The image's size and date; false if it isn't there (now).
bool fileStamp(const std::string& path, uint64_t& size, int64_t& modified) {
  if (remote::isRemote(path)) {
    remote::Entry entry;
    if (remote::stat(path, entry) != remote::Status::Ok || entry.folder) return false;
    size = entry.size;
    modified = entry.modified;
    return true;
  }
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
  size = st.st_size;
  modified = int64_t(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
  return true;
}

std::atomic<bool> playBothViews{false};

std::string baseViewFile(const std::string& id) { return "STREAM/" + id + ".m2ts"; }

// The file a clip of the title plays from (setPlayBothViews()).
std::string clipFile(const Disc& disc, const std::string& id, bool baseView) {
  return playsStereo(disc) && !baseView ? "SSIF/" + id + ".ssif" : baseViewFile(id);
}

// mpv's stream over a local image: positional reads.
struct LocalImage {
  int fd = -1;
  uint64_t size = 0, position = 0;
};
bool openLocalImage(const std::string& path, mpv_stream_cb_info& info) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat st {};
  if (fstat(fd, &st) != 0) {
    ::close(fd);
    return false;
  }
  info.cookie = new LocalImage{fd, uint64_t(st.st_size), 0};
  info.read_fn = [](void* cookie, char* out, uint64_t count) -> int64_t {
    auto& p = *static_cast<LocalImage*>(cookie);
    const ssize_t n = ::pread(p.fd, out, count, static_cast<off_t>(p.position));
    if (n < 0) return -1;
    p.position += n;
    return n;
  };
  info.seek_fn = [](void* cookie, int64_t offset) -> int64_t {
    static_cast<LocalImage*>(cookie)->position = offset;
    return offset;
  };
  info.size_fn = [](void* cookie) -> int64_t { return static_cast<LocalImage*>(cookie)->size; };
  info.cancel_fn = nullptr;
  info.close_fn = [](void* cookie) {
    auto* p = static_cast<LocalImage*>(cookie);
    ::close(p->fd);
    delete p;
  };
  return true;
}

// A file inside the image: its extents read from the image's stream.
struct ClipStream {
  mpv_stream_cb_info image{};
  std::vector<Extent> extents;
  std::vector<uint64_t> starts;  // each extent's first byte in the file
  uint64_t size = 0, position = 0;
  uint64_t imagePosition = UINT64_MAX;  // where the image's stream is
};
bool openClip(const std::string& path, const DiscFile& file, bool background,
              std::atomic<bool>* failed, mpv_stream_cb_info& info) {
  auto clip = std::make_unique<ClipStream>();
  const bool opened = remote::isRemote(path)
                          ? remote::openMpvStream(path, background, failed, &clip->image)
                          : openLocalImage(path, clip->image);
  if (!opened) return false;
  clip->extents = file.extents;
  clip->size = file.size;
  for (const Extent& e : file.extents) {
    clip->starts.push_back(clip->position);
    clip->position += e.length;
  }
  clip->position = 0;
  info.cookie = clip.release();
  info.read_fn = [](void* cookie, char* out, uint64_t count) -> int64_t {
    auto& p = *static_cast<ClipStream*>(cookie);
    if (p.position >= p.size) return 0;
    const size_t i = std::upper_bound(p.starts.begin(), p.starts.end(), p.position) - p.starts.begin() - 1;
    const uint64_t into = p.position - p.starts[i];
    const uint64_t offset = p.extents[i].offset + into;
    if (offset != p.imagePosition) {
      if (p.image.seek_fn(p.image.cookie, static_cast<int64_t>(offset)) != static_cast<int64_t>(offset))
        return -1;
      p.imagePosition = offset;
    }
    const int64_t n = p.image.read_fn(p.image.cookie, out, std::min(count, p.extents[i].length - into));
    if (n <= 0) {
      p.imagePosition = UINT64_MAX;
      return -1;  // the image ended inside the file
    }
    p.position += n;
    p.imagePosition += n;
    return n;
  };
  info.seek_fn = [](void* cookie, int64_t offset) -> int64_t {
    auto& p = *static_cast<ClipStream*>(cookie);
    if (offset < 0) return MPV_ERROR_GENERIC;
    p.position = offset;
    return offset;
  };
  info.size_fn = [](void* cookie) -> int64_t { return static_cast<ClipStream*>(cookie)->size; };
  info.cancel_fn = [](void* cookie) {
    auto& p = *static_cast<ClipStream*>(cookie);
    if (p.image.cancel_fn) p.image.cancel_fn(p.image.cookie);
  };
  info.close_fn = [](void* cookie) {
    auto* p = static_cast<ClipStream*>(cookie);
    p->image.close_fn(p->image.cookie);
    delete p;
  };
  return true;
}

// Text served as a stream (the title's EDL).
void openText(std::string text, mpv_stream_cb_info& info) {
  struct Text {
    std::string bytes;
    uint64_t position = 0;
  };
  info.cookie = new Text{std::move(text)};
  info.read_fn = [](void* cookie, char* out, uint64_t count) -> int64_t {
    auto& t = *static_cast<Text*>(cookie);
    const uint64_t n = std::min<uint64_t>(count, t.bytes.size() - std::min<uint64_t>(t.position, t.bytes.size()));
    std::copy_n(t.bytes.data() + t.position, n, out);
    t.position += n;
    return static_cast<int64_t>(n);
  };
  info.seek_fn = [](void* cookie, int64_t offset) -> int64_t {
    static_cast<Text*>(cookie)->position = offset;
    return offset;
  };
  info.size_fn = [](void* cookie) -> int64_t { return static_cast<Text*>(cookie)->bytes.size(); };
  info.cancel_fn = nullptr;
  info.close_fn = [](void* cookie) { delete static_cast<Text*>(cookie); };
}

// "%<bytes>%<text>": an EDL value that may hold any character.
std::string edlQuoted(const std::string& text) {
  return "%" + std::to_string(text.size()) + "%" + text;
}

// Per instance: background reads, or playback's with its read-failure flag.
int backgroundTag, playbackTag;
struct Mode {
  bool background;
  std::atomic<bool>* failed;
};
Mode modeOf(void* data) {
  if (data == &backgroundTag) return {true, nullptr};
  if (data == &playbackTag) return {false, nullptr};
  return {false, static_cast<std::atomic<bool>*>(data)};
}

const std::string kTitleScheme = "bdimage://";
const std::string kBaseViewScheme = "bdimagebase://";
const std::string kClipScheme = "bdclip://";

std::string clipUrl(const std::string& path, const std::string& file) {
  return kClipScheme + std::filesystem::path(file).filename().string() + "/" + path;
}

int openTitle(void* data, char* uri, mpv_stream_cb_info* info) {
  const std::string url = uri;
  const bool baseView = url.rfind(kBaseViewScheme, 0) == 0;
  if (!baseView && url.rfind(kTitleScheme, 0) != 0) return MPV_ERROR_LOADING_FAILED;
  const std::string path = url.substr(baseView ? kBaseViewScheme.size() : kTitleScheme.size());
  const auto disc = read(path);
  const Mode mode = modeOf(data);
  if (disc->status != Disc::Status::Ok) {
    // Once, for playback (previews fail alike).
    if (!mode.background) std::fprintf(stderr, "Blu-ray disc image: %s\n", statusName(disc->status));
    return MPV_ERROR_LOADING_FAILED;
  }
  const Title& title = disc->title;
  // One clip played whole: read directly.
  if (title.clips.size() == 1 && !title.clips[0].trimmed) {
    const auto file = disc->files.find(clipFile(*disc, title.clips[0].id, baseView));
    if (file == disc->files.end() || !openClip(path, file->second, mode.background, mode.failed, *info))
      return MPV_ERROR_LOADING_FAILED;
    return 0;
  }
  // Several clips, or one cut short: an EDL of them, each cut to its play
  // item. Its chapters come from the playlist (VideoPlayer), not one per
  // clip.
  std::string edl = "# mpv EDL v0\n!no_chapters\n";
  for (const Clip& clip : title.clips) {
    char times[96];
    std::snprintf(times, sizeof(times), ",start=%.6f,length=%.6f\n", clip.in, clip.out - clip.in);
    edl += edlQuoted(clipUrl(path, clipFile(*disc, clip.id, baseView))) + times;
  }
  openText(std::move(edl), *info);
  return 0;
}

int openClipUrl(void* data, char* uri, mpv_stream_cb_info* info) {
  const std::string url = uri;
  if (url.rfind(kClipScheme, 0) != 0) return MPV_ERROR_LOADING_FAILED;
  const size_t slash = url.find('/', kClipScheme.size());
  if (slash == std::string::npos) return MPV_ERROR_LOADING_FAILED;
  const std::string name = url.substr(kClipScheme.size(), slash - kClipScheme.size());
  const std::string path = url.substr(slash + 1);
  const auto disc = read(path);  // kept from the title's opening
  const std::string ext = std::filesystem::path(name).extension().string();
  const auto file = disc->files.find((ext == ".ssif" ? "SSIF/" : "STREAM/") + name);
  if (disc->status != Disc::Status::Ok || file == disc->files.end()) return MPV_ERROR_LOADING_FAILED;
  const Mode mode = modeOf(data);
  return openClip(path, file->second, mode.background, mode.failed, *info) ? 0 : MPV_ERROR_LOADING_FAILED;
}

}  // namespace

bool isDiscImage(const std::string& path) {
  return lowerCase(std::filesystem::path(path).extension().string()) == ".iso";
}

std::shared_ptr<const Disc> read(const std::string& path) {
  uint64_t size = 0;
  int64_t modified = 0;
  const bool there = fileStamp(path, size, modified);
  auto& c = cache();
  if (there) {
    std::lock_guard lock(c.mutex);
    const auto it = c.discs.find(path);
    if (it != c.discs.end() && it->second.size == size && it->second.modified == modified)
      return it->second.disc;
  }
  auto disc = std::make_shared<Disc>();
  std::unique_ptr<Source> source;
  if (!there) {
  } else if (remote::isRemote(path)) {
    remote::Status status;
    if (auto file = remote::File::open(path, status)) source = std::make_unique<RemoteSource>(std::move(file));
  } else {
    source = openLocalSource(path);
  }
  if (source) *disc = readDisc(*source);
  else disc->readFailed = true;
  std::lock_guard lock(c.mutex);
  if (!there || disc->readFailed) {
    // Not kept: the next try reads it again.
    c.discs.erase(path);
    return disc;
  }
  if (c.discs.size() >= kKept && !c.discs.count(path)) c.discs.erase(c.discs.begin());
  c.discs[path] = {size, modified, disc};
  return disc;
}

std::shared_ptr<const Disc> known(const std::string& path) {
  auto& c = cache();
  std::lock_guard lock(c.mutex);
  const auto it = c.discs.find(path);
  return it != c.discs.end() ? it->second.disc : nullptr;
}

bool isEncrypted(const std::string& path) {
  if (!isDiscImage(path)) return false;
  const auto disc = known(path);
  return disc && disc->status == Disc::Status::Encrypted;
}

void setPlayBothViews(bool on) { playBothViews = on; }

bool playsStereo(const Disc& disc) {
  return playBothViews && disc.status == Disc::Status::Ok && disc.title.stereo;
}

std::string mpvOptions(const std::string& path) {
  const auto disc = isDiscImage(path) ? known(path) : nullptr;
  return disc && playsStereo(*disc) && disc->title.baseViewRight ? ",vd-lavc-mvc-base-view=right" : "";
}

void addTo(mpv_handle* mpv, bool background, std::atomic<bool>* failed) {
  void* data = background ? static_cast<void*>(&backgroundTag)
               : failed   ? static_cast<void*>(failed)
                          : static_cast<void*>(&playbackTag);
  mpv_stream_cb_add_ro(mpv, "bdimage", data, openTitle);
  mpv_stream_cb_add_ro(mpv, "bdimagebase", data, openTitle);
  mpv_stream_cb_add_ro(mpv, "bdclip", data, openClipUrl);
}

std::string mpvUrl(const std::string& path, bool baseView) {
  return (baseView ? kBaseViewScheme : kTitleScheme) + path;
}

bool openFirstClip(const std::string& path, mpv_stream_cb_info& info) {
  const auto disc = read(path);
  if (disc->status != Disc::Status::Ok) return false;
  const auto file = disc->files.find(baseViewFile(disc->title.clips[0].id));
  return file != disc->files.end() && openClip(path, file->second, true, nullptr, info);
}

}  // namespace bdmv
