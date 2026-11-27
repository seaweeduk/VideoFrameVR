#pragma once

#include "media_decode.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct SwsContext;

// A video opened for pictures of its keyframes, decoded on the CPU on one
// thread and shrunk, for the seek preview on the Frame (keyframe_grabber.h).
// Local files, remote ones, Stash scenes, links and disc images
// (remote_avio.h). Everything blocks: a worker's.
class KeyframeReader {
 public:
  // Upright, rows from the top, cairo's byte order, opaque.
  struct Picture {
    std::vector<uint32_t> pixels;
    int width = 0, height = 0;
  };

  // `interrupt` stops its reads and outlives it; its owner sets it anew
  // (its deadline) before each call.
  explicit KeyframeReader(Interrupt& interrupt);
  ~KeyframeReader();
  KeyframeReader(const KeyframeReader&) = delete;
  KeyframeReader& operator=(const KeyframeReader&) = delete;

  // False if it can't be opened, has no video or duration, or can't seek (a
  // server without ranges). A disc's title of several clips can't either:
  // only its first clip is read.
  bool open(const std::string& path);
  bool isOpen() const { return format_ != nullptr; }
  void close();
  enum class Read {
    Failed,
    Same,  // the keyframe the last New gave: read again, not decoded
    New,
  };
  // The last keyframe up to `seconds` (in a file whose seeks land between
  // keyframes, MPEG-TS, the next one after), with both eyes of a 3D video,
  // shrunk to fit maxWidth × maxHeight. `out` changes only with New.
  Read read(double seconds, int maxWidth, int maxHeight, Picture& out);
  // Frees the decoder and its frames (for 8K, hundreds of MB); the next
  // read() opens it again, which takes no time to speak of.
  void rest();

 private:
  bool seekToKey(int64_t at);
  bool shrink(int maxWidth, int maxHeight, Picture& out);

  Interrupt& interrupt_;
  std::unique_ptr<RemoteAvio> remote_;
  AVFormatContext* format_ = nullptr;
  AVCodecContext* codec_ = nullptr;
  AVPacket* packet_ = nullptr;
  AVFrame* frame_ = nullptr;
  SwsContext* sws_ = nullptr;
  int stream_ = -1;
  int64_t origin_ = 0;  // the video's start in the file's timestamps (AV_TIME_BASE)
  // The packet of the keyframe read() gave last, if it gave one.
  struct Key {
    int64_t pos = 0, dts = 0, pts = 0;
    int size = 0;
  };
  bool given_ = false;
  Key key_;
};
