#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
class RemoteAvio;

// FFmpeg's interrupt callback for decoding off the frame loop: a blocking
// open, read or seek stops once its deadline (from construction) passes or
// its owner stops it.
class Interrupt {
 public:
  // Stopped once `quit` (or `also`) is set.
  Interrupt(std::chrono::seconds timeout, const std::atomic<bool>& quit,
            const std::atomic<bool>* also = nullptr)
      : quit_(&quit), also_(also), deadline_(Clock::now() + timeout) {}
  // Stopped once `generation` is no longer `mine`.
  Interrupt(std::chrono::seconds timeout, const std::atomic<uint64_t>& generation, uint64_t mine)
      : generation_(&generation), mine_(mine), deadline_(Clock::now() + timeout) {}

  static int check(void* opaque) {
    const auto& self = *static_cast<Interrupt*>(opaque);
    return (self.quit_ && self.quit_->load()) || (self.also_ && self.also_->load()) ||
           (self.generation_ && self.generation_->load() != self.mine_) ||
           Clock::now() >= self.deadline_;
  }

 private:
  using Clock = std::chrono::steady_clock;
  const std::atomic<bool>* quit_ = nullptr;
  const std::atomic<bool>* also_ = nullptr;
  const std::atomic<uint64_t>* generation_ = nullptr;
  uint64_t mine_ = 0;
  Clock::time_point deadline_;
};

// Opens `path` (local, or through `remote`, which outlives the result) with
// `interrupt`, and reads its streams' parameters. Null on failure; else the
// caller closes it (avformat_close_input()).
AVFormatContext* openMedia(const std::string& path, RemoteAvio& remote, Interrupt& interrupt);

// The decoder for `format`'s best video stream (`stream`), set up by
// `configure` before it opens. Null on failure; else the caller frees it.
AVCodecContext* openVideoDecoder(AVFormatContext* format, int& stream,
                                 const std::function<void(AVCodecContext*)>& configure);

// Feeds `stream`'s packets (at most `limit` of them) to `codec` until it
// gives a frame, draining it at the end of the input. False without one or
// once interrupted.
bool decodeFirstFrame(AVFormatContext* format, AVCodecContext* codec, int stream,
                      AVPacket* packet, AVFrame* frame, int limit, Interrupt& interrupt);

// The picture of `stream`'s next packet, the keyframe a seek went to: its
// packet alone, then the end of input, as video_check.cpp reads keyframes.
// Fed on instead, the decoder keeps the picture until the frames after it
// are decoded too, which for 8K takes as long again as the keyframe did.
// If the packet gives no whole picture that way (the file calls it a
// keyframe and it isn't one, or it is one field of two), it decodes on as
// decodeFirstFrame() does. Flush `codec` before it decodes anything else.
bool decodeKeyframe(AVFormatContext* format, AVCodecContext* codec, int stream, AVPacket* packet,
                    AVFrame* frame, int limit, Interrupt& interrupt);
