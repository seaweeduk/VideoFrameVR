#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Blu-ray disc images (.iso), read for playing their main title: the UDF
// file system through libudfread, the playlists through libbluray (titles,
// clips, chapters, languages). Neither decrypts, plays menus or runs BD-J.
// Everything here blocks on reads: workers and mpv's threads only.
namespace bdmv {

// The bytes of a disc image, read at any position.
class Source {
 public:
  virtual ~Source() = default;
  virtual uint64_t size() const = 0;
  // Exactly `count` bytes at `offset`; false on an error or past the end.
  virtual bool read(uint64_t offset, void* out, uint64_t count) = 0;
};
// A local file's; null if it can't be opened.
std::unique_ptr<Source> openLocalSource(const std::string& path);

// A run of a file's bytes in the image.
struct Extent {
  uint64_t offset = 0;  // in the image
  uint64_t length = 0;
};
// A file inside the image, as its bytes lie in the image, in order.
struct DiscFile {
  uint64_t size = 0;
  std::vector<Extent> extents;
};

struct Track {
  enum Kind { Video, Audio, Subtitle } kind = Video;
  uint16_t pid = 0;
  uint8_t coding = 0;  // the stream coding type (bd_stream_type_e)
  std::string lang;    // ISO 639-2, e.g. "eng"; empty if not given
};

// One play item of a playlist: a span of one clip.
struct Clip {
  std::string id;       // "00000": STREAM/00000.m2ts, SSIF/00000.ssif
  double in = 0;        // its timestamps (seconds, 90 kHz / 90000) ...
  double out = 0;       // ... it plays between
  double start = 0;     // where it starts in the title (seconds)
  // A title's only play item that plays less than its whole clip (more
  // than a few frames cut at either end).
  bool trimmed = false;
};

struct Title {
  uint32_t playlist = 0;  // NNNNN.mpls
  double seconds = 0;
  std::vector<Clip> clips;
  std::vector<double> chapters;  // where each starts in the title (seconds)
  std::vector<Track> tracks;     // the first clip's (STN table)
  // Both views of each clip are on the disc (SSIF/<clip>.ssif), so the
  // title can play in 3D.
  bool stereo = false;
  bool baseViewRight = false;  // MVC: the base view is the right eye's
};

struct Disc {
  enum class Status {
    Ok,
    Unreadable,  // the image (or a file in it) couldn't be read
    NotBluray,   // no UDF file system with BDMV/ in it
    NoTitle,     // no playlist that plays
    Encrypted,   // AACS: the clips aren't plain transport streams
  } status = Status::Unreadable;
  // Unreadable because reading the image failed (worth another try later),
  // not because of what it holds.
  bool readFailed = false;
  uint64_t imageSize = 0;
  Title title;  // the main title
  // The title's clips' files: "STREAM/00000.m2ts" and "SSIF/00000.ssif"
  // (when the disc has it), by their path under BDMV/.
  std::map<std::string, DiscFile> files;
};

// Reads the disc in `source`: its main title and where its clip files are.
// Any failed read of the source makes it Unreadable, with readFailed set.
Disc readDisc(Source& source);

const char* statusName(Disc::Status status);

}  // namespace bdmv
