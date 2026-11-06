#include "bluray_disc.h"

#include <libbluray/bluray.h>
#include <libbluray/clpi_data.h>
#include <libbluray/filesystem.h>
#include <libbluray/log_control.h>
#include <udfread/blockinput.h>
#include <udfread/udfread.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>
#include <mutex>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace bdmv {
namespace {

constexpr uint64_t kBlock = UDF_BLOCK_SIZE;

class LocalSource : public Source {
 public:
  explicit LocalSource(int fd, uint64_t size) : fd_(fd), size_(size) {}
  ~LocalSource() override { ::close(fd_); }
  uint64_t size() const override { return size_; }
  bool read(uint64_t offset, void* out, uint64_t count) override {
    auto* p = static_cast<char*>(out);
    while (count > 0) {
      const ssize_t n = ::pread(fd_, p, count, static_cast<off_t>(offset));
      if (n <= 0) return false;
      p += n;
      offset += n;
      count -= n;
    }
    return true;
  }

 private:
  int fd_;
  uint64_t size_;
};

// libudfread's blocks from a Source. Parsing reads small structures here and
// there, often again: chunks of 32 blocks are kept, so a remote image costs
// a round trip per chunk rather than per block.
struct BlockInput {
  udfread_block_input input{};  // first: libudfread hands this back
  Source* source = nullptr;
  static constexpr uint32_t kChunk = 32;  // blocks
  static constexpr size_t kChunks = 32;
  struct Chunk {
    uint32_t first;
    std::vector<uint8_t> bytes;
  };
  std::list<Chunk> chunks;  // most recently used first
  std::mutex mutex;
  // While set, reads aren't made but noted here (first block, count): see
  // locate().
  std::vector<std::pair<uint32_t, uint32_t>>* runs = nullptr;

  const Chunk* chunk(uint32_t first) {
    for (auto it = chunks.begin(); it != chunks.end(); ++it)
      if (it->first == first) {
        chunks.splice(chunks.begin(), chunks, it);
        return &chunks.front();
      }
    const uint64_t offset = uint64_t(first) * kBlock;
    if (offset >= source->size()) return nullptr;
    Chunk c{first, std::vector<uint8_t>(std::min<uint64_t>(kChunk * kBlock, source->size() - offset))};
    if (!source->read(offset, c.bytes.data(), c.bytes.size())) return nullptr;
    chunks.push_front(std::move(c));
    if (chunks.size() > kChunks) chunks.pop_back();
    return &chunks.front();
  }

  static int read(udfread_block_input* in, uint32_t lba, void* buf, uint32_t count, int) {
    auto& self = *reinterpret_cast<BlockInput*>(in);
    std::lock_guard lock(self.mutex);
    if (self.runs) {
      self.runs->emplace_back(lba, count);
      return static_cast<int>(count);
    }
    auto* out = static_cast<uint8_t*>(buf);
    uint32_t done = 0;
    while (done < count) {
      const uint32_t block = lba + done;
      const Chunk* c = self.chunk(block / kChunk * kChunk);
      if (!c) break;
      const uint64_t at = uint64_t(block - c->first) * kBlock;
      if (at + kBlock > c->bytes.size()) break;
      const uint32_t n = std::min<uint32_t>(count - done, (c->bytes.size() - at) / kBlock);
      std::memcpy(out + uint64_t(done) * kBlock, c->bytes.data() + at, n * kBlock);
      done += n;
    }
    return done ? static_cast<int>(done) : -1;
  }
  static uint32_t size(udfread_block_input* in) {
    return static_cast<uint32_t>(reinterpret_cast<BlockInput*>(in)->source->size() / kBlock);
  }
};

// libbluray's view of the image's files, through libudfread.
BD_FILE_H* openFile(void* handle, const char* path) {
  UDFFILE* f = udfread_file_open(static_cast<udfread*>(handle), path);
  if (!f) return nullptr;
  auto* file = new BD_FILE_H{};
  file->internal = f;
  file->close = [](BD_FILE_H* file) {
    udfread_file_close(static_cast<UDFFILE*>(file->internal));
    delete file;
  };
  file->seek = [](BD_FILE_H* file, int64_t offset, int32_t origin) -> int64_t {
    return udfread_file_seek(static_cast<UDFFILE*>(file->internal), offset, origin);
  };
  file->tell = [](BD_FILE_H* file) -> int64_t {
    return udfread_file_tell(static_cast<UDFFILE*>(file->internal));
  };
  file->read = [](BD_FILE_H* file, uint8_t* out, int64_t size) -> int64_t {
    return udfread_file_read(static_cast<UDFFILE*>(file->internal), out, size);
  };
  return file;
}

BD_DIR_H* openDir(void* handle, const char* path) {
  UDFDIR* d = udfread_opendir(static_cast<udfread*>(handle), path);
  if (!d) return nullptr;
  auto* dir = new BD_DIR_H{};
  dir->internal = d;
  dir->close = [](BD_DIR_H* dir) {
    udfread_closedir(static_cast<UDFDIR*>(dir->internal));
    delete dir;
  };
  dir->read = [](BD_DIR_H* dir, BD_DIRENT* entry) -> int {
    udfread_dirent e;
    if (!udfread_readdir(static_cast<UDFDIR*>(dir->internal), &e)) return 1;
    std::snprintf(entry->d_name, sizeof(entry->d_name), "%s", e.d_name);
    return 0;
  };
  return dir;
}

// Where a file's blocks lie in the image. libudfread reads a file's blocks
// run by run, each within one of its allocation descriptors: it is asked to
// read the whole file, with the input noting the runs instead of reading.
// Blocks it fills with zeros (unallocated) fail the file, as they are none
// of the image's.
bool locate(udfread* udf, BlockInput& input, const std::string& path, DiscFile& out) {
  UDFFILE* f = udfread_file_open(udf, path.c_str());
  if (!f) return false;
  const int64_t size = udfread_file_size(f);
  bool ok = size > 0;
  out = {};
  out.size = ok ? uint64_t(size) : 0;
  const uint64_t blocks = (out.size + kBlock - 1) / kBlock;
  // Only written for zero-filled blocks: its pages are mostly never touched.
  constexpr uint32_t kStep = 4096;
  std::unique_ptr<uint8_t[]> buffer(ok ? new uint8_t[kStep * kBlock] : nullptr);
  std::vector<std::pair<uint32_t, uint32_t>> runs;
  for (uint64_t b = 0; ok && b < blocks;) {
    const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(kStep, blocks - b));
    runs.clear();
    {
      std::lock_guard lock(input.mutex);
      input.runs = &runs;
    }
    const uint32_t done = udfread_read_blocks(f, buffer.get(), static_cast<uint32_t>(b), n, 0);
    {
      std::lock_guard lock(input.mutex);
      input.runs = nullptr;
    }
    uint64_t noted = 0;
    for (const auto& [lba, count] : runs) {
      const uint64_t offset = uint64_t(lba) * kBlock;
      const uint64_t length = std::min(uint64_t(count) * kBlock, out.size - (b + noted) * kBlock);
      if (!out.extents.empty() && out.extents.back().offset + out.extents.back().length == offset)
        out.extents.back().length += length;
      else
        out.extents.push_back({offset, length});
      noted += count;
    }
    ok = done == n && noted == n;
    b += n;
  }
  udfread_file_close(f);
  return ok;
}

// A clip of an AACS disc that is still encrypted: of each 6144-byte unit,
// all but the first 16 bytes are, so its packets' sync bytes (every 192
// bytes, after a 4-byte header) don't show where they should.
bool plainTransportStream(Source& source, const DiscFile& file) {
  constexpr uint64_t kUnit = 6144;
  if (file.extents.empty() || file.size < kUnit) return false;
  for (const uint64_t at : {uint64_t(0), file.size / 2 / kUnit * kUnit}) {
    uint64_t offset = 0, skipped = 0;
    for (const Extent& e : file.extents) {
      if (at < skipped + e.length) {
        offset = e.offset + (at - skipped);
        break;
      }
      skipped += e.length;
    }
    uint8_t unit[kUnit];
    if (!source.read(offset, unit, kUnit)) return false;
    for (uint64_t p = 4; p < kUnit; p += 192)
      if (unit[p] != 0x47) return false;
  }
  return true;
}

// Whether a title's one play item plays less than its whole clip: its in
// or out time lies more than a few frames inside the clip's own
// presentation times (CLPI, 45 kHz). False when they can't be read.
bool cutsClip(BLURAY* bd, uint32_t title, const BLURAY_CLIP_INFO& item) {
  if (!bd_select_title(bd, title)) return false;
  CLPI_CL* cl = bd_get_clpi(bd, 0);
  if (!cl) return false;
  constexpr uint64_t kSlack = 90000 / 10;  // 0.1 s at 90 kHz
  bool cut = false;
  for (unsigned a = 0; a < cl->sequence.num_atc_seq; ++a) {
    const CLPI_ATC_SEQ& atc = cl->sequence.atc_seq[a];
    for (unsigned i = 0; i < atc.num_stc_seq; ++i) {
      const uint64_t start = uint64_t(atc.stc_seq[i].presentation_start_time) * 2;
      const uint64_t end = uint64_t(atc.stc_seq[i].presentation_end_time) * 2;
      if (item.in_time + kSlack < start || item.in_time >= end) continue;  // another sequence's
      cut = item.in_time > start + kSlack || item.out_time + kSlack < end;
    }
  }
  bd_free_clpi(cl);
  return cut;
}

void collect(std::vector<Track>& tracks, Track::Kind kind, const BLURAY_STREAM_INFO* streams,
             int count) {
  for (int i = 0; i < count; ++i) {
    Track t;
    t.kind = kind;
    t.pid = streams[i].pid;
    t.coding = streams[i].coding_type;
    const auto* lang = reinterpret_cast<const char*>(streams[i].lang);
    t.lang.assign(lang, strnlen(lang, 3));
    if (std::any_of(t.lang.begin(), t.lang.end(), [](char c) { return c < 'a' || c > 'z'; }))
      t.lang.clear();
    tracks.push_back(t);
  }
}

// libbluray's own errors. It always tries to open an image of its own
// before taking the files given to it, and looks for decryption libraries
// on discs that have had AACS or BD+: no errors here, which never decrypts
// (an image still encrypted is told by its clips, plainTransportStream()).
void logMessage(const char* message) {
  if (std::strstr(message, "failed opening UDF image") ||
      std::strstr(message, "No usable AACS libraries found") ||
      std::strstr(message, "No usable BD+ libraries found"))
    return;
  std::fprintf(stderr, "[bluray] %s", message);
}

}  // namespace

std::unique_ptr<Source> openLocalSource(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return nullptr;
  struct stat st {};
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return nullptr;
  }
  return std::make_unique<LocalSource>(fd, st.st_size);
}

const char* statusName(Disc::Status status) {
  switch (status) {
    case Disc::Status::Ok: return "ok";
    case Disc::Status::Unreadable: return "unreadable";
    case Disc::Status::NotBluray: return "not a Blu-ray disc image";
    case Disc::Status::NoTitle: return "no playable title";
    case Disc::Status::Encrypted: return "encrypted (AACS)";
  }
  return "?";
}

namespace {

// A source that notes its failed reads.
class Watched : public Source {
 public:
  explicit Watched(Source& source) : source_(source) {}
  uint64_t size() const override { return source_.size(); }
  bool read(uint64_t offset, void* out, uint64_t count) override {
    const bool ok = source_.read(offset, out, count);
    failed_ |= !ok;
    return ok;
  }
  bool failed() const { return failed_; }

 private:
  Source& source_;
  bool failed_ = false;
};

Disc readImage(Source& source) {
  Disc disc;
  disc.imageSize = source.size();
  BlockInput input;
  input.input.read = BlockInput::read;
  input.input.size = BlockInput::size;
  input.source = &source;
  udfread* udf = udfread_init();
  if (!udf) return disc;
  struct Closer {
    udfread* udf;
    BLURAY* bd = nullptr;
    ~Closer() {
      if (bd) bd_close(bd);
      udfread_close(udf);
    }
  } closer{udf};
  if (udfread_open_input(udf, &input.input) < 0) {
    disc.status = Disc::Status::NotBluray;
    return disc;
  }
  BLURAY* bd = closer.bd = bd_init();
  if (!bd) return disc;
  // Parsing only: never look for a Java VM (BD-J menus) or keep BD-J's
  // storage. AACS keys aren't looked for either: the clips are read apart.
  bd_set_player_setting_str(bd, BLURAY_PLAYER_JAVA_HOME, "/nonexistent");
  bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_PERSISTENT_STORAGE, 0);
  if (!bd_open_files(bd, udf, openDir, openFile)) {
    disc.status = Disc::Status::NotBluray;
    return disc;
  }
  const BLURAY_DISC_INFO* info = bd_get_disc_info(bd);
  if (!info || !info->bluray_detected) {
    disc.status = Disc::Status::NotBluray;
    return disc;
  }

  // libbluray's main title among the relevant ones (duplicates and
  // playlists that repeat their clips dropped); of those as long as it, one
  // whose clips all have both views (a 3D disc's 2D and 3D playlists).
  const uint32_t count = bd_get_titles(bd, TITLES_RELEVANT, 0);
  const int main = bd_get_main_title(bd);
  if (!count || main < 0) {
    disc.status = Disc::Status::NoTitle;
    return disc;
  }
  auto hasBothViews = [&](const BLURAY_TITLE_INFO& t) {
    if (!t.clip_count) return false;
    for (uint32_t i = 0; i < t.clip_count; ++i) {
      const std::string ssif = std::string("BDMV/STREAM/SSIF/") + t.clips[i].clip_id + ".ssif";
      UDFFILE* f = udfread_file_open(udf, ssif.c_str());
      if (!f) return false;
      udfread_file_close(f);
    }
    return true;
  };
  BLURAY_TITLE_INFO* chosen = bd_get_title_info(bd, static_cast<uint32_t>(main), 0);
  if (!chosen || !chosen->clip_count) {
    if (chosen) bd_free_title_info(chosen);
    disc.status = Disc::Status::NoTitle;
    return disc;
  }
  bool stereo = hasBothViews(*chosen);
  uint32_t chosenIndex = static_cast<uint32_t>(main);
  for (uint32_t i = 0; i < count && !stereo; ++i) {
    if (i == static_cast<uint32_t>(main)) continue;
    BLURAY_TITLE_INFO* t = bd_get_title_info(bd, i, 0);
    if (!t) continue;
    const double apart = std::fabs(double(t->duration) - double(chosen->duration)) / 90000;
    if (apart < 1 && hasBothViews(*t)) {
      std::swap(t, chosen);
      chosenIndex = i;
      stereo = true;
    }
    bd_free_title_info(t);
  }

  Title& title = disc.title;
  title.playlist = chosen->playlist;
  title.seconds = chosen->duration / 90000.0;
  title.stereo = stereo;
  title.baseViewRight = chosen->mvc_base_view_r_flag != 0;
  for (uint32_t i = 0; i < chosen->clip_count; ++i) {
    const BLURAY_CLIP_INFO& c = chosen->clips[i];
    Clip clip;
    clip.id.assign(c.clip_id, strnlen(c.clip_id, sizeof(c.clip_id)));
    clip.in = c.in_time / 90000.0;
    clip.out = c.out_time / 90000.0;
    clip.start = c.start_time / 90000.0;
    title.clips.push_back(clip);
  }
  if (title.clips.size() == 1) title.clips[0].trimmed = cutsClip(bd, chosenIndex, chosen->clips[0]);
  for (uint32_t i = 0; i < chosen->chapter_count; ++i)
    title.chapters.push_back(chosen->chapters[i].start / 90000.0);
  const BLURAY_CLIP_INFO& first = chosen->clips[0];
  collect(title.tracks, Track::Video, first.video_streams, first.video_stream_count);
  collect(title.tracks, Track::Audio, first.audio_streams, first.audio_stream_count);
  collect(title.tracks, Track::Subtitle, first.pg_streams, first.pg_stream_count);
  bd_free_title_info(chosen);

  for (const Clip& clip : title.clips) {
    const std::string m2ts = "STREAM/" + clip.id + ".m2ts";
    if (disc.files.count(m2ts)) continue;
    if (!locate(udf, input, "BDMV/" + m2ts, disc.files[m2ts])) {
      disc.files.clear();
      disc.status = Disc::Status::Unreadable;
      return disc;
    }
    if (title.stereo && !locate(udf, input, "BDMV/STREAM/SSIF/" + clip.id + ".ssif",
                                disc.files["SSIF/" + clip.id + ".ssif"]))
      title.stereo = false;
  }
  if (!title.stereo)
    for (auto it = disc.files.begin(); it != disc.files.end();)
      it = it->first.rfind("SSIF/", 0) == 0 ? disc.files.erase(it) : std::next(it);
  if (!plainTransportStream(source, disc.files["STREAM/" + title.clips[0].id + ".m2ts"])) {
    disc.status = info->aacs_detected ? Disc::Status::Encrypted : Disc::Status::Unreadable;
    return disc;
  }
  disc.status = Disc::Status::Ok;
  return disc;
}

}  // namespace

Disc readDisc(Source& source) {
  static std::once_flag logging;
  std::call_once(logging, [] { bd_set_debug_handler(logMessage); });
  Watched watched(source);
  Disc disc = readImage(watched);
  if (watched.failed()) {
    // Nothing of it stands: the reads that failed might have shown more.
    disc = Disc{};
    disc.imageSize = source.size();
    disc.readFailed = true;
  }
  return disc;
}

}  // namespace bdmv
