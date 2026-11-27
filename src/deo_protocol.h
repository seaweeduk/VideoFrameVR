#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// DeoVR's remote control protocol, as its documentation and sample client
// describe it and DeoVR itself behaves: TCP, port 23554, the player
// listening. A packet is a 4-byte little-endian length, then that many bytes
// of UTF-8 JSON, one object with any of path, duration, currentTime,
// playbackSpeed and playerState (0 playing, 1 paused, 2 at the end, which
// the player alone sends). A packet of length 0 (or, from a client, a
// negative length) is a ping from a client, and from the player says
// nothing is open. Each side sends at least one a second, the player also
// at once when what plays changes, and a connection with no whole packet
// from a client for 3 seconds is dropped.
//
// Bytes in and out only: no sockets, threads or app types (deo_remote.h has
// those).
namespace deo {

constexpr uint16_t kPort = 23554;
// A longer packet isn't one: the connection is dropped, and nothing that
// size is ever kept on a client's say.
constexpr uint32_t kMaxPacket = 64 * 1024;
// Nothing longer is opened, so the status naming it stays well under
// kMaxPacket.
constexpr size_t kMaxPath = 8 * 1024;
// A later time isn't one (about 30 years): seconds stay within an int.
constexpr double kMaxTime = 1e9;

// Whole packets from bytes as they arrive, split anywhere or several at once.
class Reader {
 public:
  void feed(const char* data, size_t size);
  enum class Next {
    None,    // no whole packet yet
    Packet,  // one, in `body` (empty for a ping) until the next feed()
    Broken,  // a length over kMaxPacket: drop the connection
  };
  Next next(std::string_view& body);

 private:
  std::string buffer_;
  size_t at_ = 0;  // the next packet's start in buffer_
};

// What a client's packet asks for, as DeoVR takes it. Its fields are found
// by name exactly, else in any case. A number may be a number or a string
// of one; a packet with a field that is neither (or a path that isn't a
// string) asks for nothing, as DeoVR drops it. A missing or null field is
// absent, as are a path that is empty once spaces around it are trimmed,
// times that aren't finite or are over kMaxTime, and a playerState other
// than 0 or 1. A negative time is 0. A path opens playing: the packet's
// playerState is dropped. duration and playbackSpeed are checked, never
// taken.
struct Request {
  bool hasPath = false;
  std::string path;
  bool hasTime = false;
  double time = 0;  // currentTime, seconds
  bool hasState = false;
  bool paused = false;  // playerState 1
};
// False if the body isn't a JSON object or DeoVR would drop it (then
// `request` asks for nothing).
bool parse(std::string_view body, Request& request);
// Requests not yet acted on, as one: a later path replaces an earlier one
// with its own time or none and no state (an earlier seek, pause or play was
// for the video before), a later time or state an earlier one.
void merge(Request& into, Request&& later);

// What the player says about itself.
struct Report {
  bool open = false;  // nothing open: a packet of length 0
  std::string path;
  double duration = 0, position = 0;
  bool paused = true;
  bool ended = false;  // at the end: playerState 2
};
// Appends the report's packet to `out`. Its speed is always 1.
void write(const Report& report, std::string& out);

}  // namespace deo
