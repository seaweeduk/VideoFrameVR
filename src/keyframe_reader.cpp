#include "keyframe_reader.h"

#include "bluray.h"
#include "frame_scaler.h"
#include "remote_avio.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
// A file's keyframe that isn't one is given up on after this many packets
// (decodeKeyPacket()): each of 8K costs a tenth of a second or more.
constexpr int kPackets = 120;
// Where a seek lands between keyframes (MPEG-TS), the next one is read on
// to, for at most this many packets: ten seconds of a 60 fps video. They
// are read, not decoded. With none before the file's end, the one read on
// to from this much earlier, or from the start.
constexpr int kReadOn = 600;
constexpr int64_t kStepBack = 10 * AV_TIME_BASE;
}  // namespace

KeyframeReader::KeyframeReader(Interrupt& interrupt)
    : interrupt_(interrupt), packet_(av_packet_alloc()), frame_(av_frame_alloc()) {}

KeyframeReader::~KeyframeReader() {
  close();
  sws_freeContext(sws_);
  av_frame_free(&frame_);
  av_packet_free(&packet_);
}

bool KeyframeReader::open(const std::string& path) {
  close();
  if (!packet_ || !frame_) return false;
  remote_ = std::make_unique<RemoteAvio>();
  format_ = openMedia(path, *remote_, interrupt_);
  if (format_) stream_ = av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  bool ok = format_ && stream_ >= 0 && format_->duration > 0 &&
            !(format_->ctx_flags & AVFMTCTX_UNSEEKABLE) &&
            (!format_->pb || (format_->pb->seekable & AVIO_SEEKABLE_NORMAL));
  if (ok) origin_ = format_->start_time != AV_NOPTS_VALUE ? format_->start_time : 0;
  if (ok && bdmv::isDiscImage(path)) {
    // The title's time counts from where its clip plays from, which the
    // title gives in the clip's own timestamps.
    const std::shared_ptr<const bdmv::Disc> disc = bdmv::read(path);
    ok = disc && disc->status == bdmv::Disc::Status::Ok && disc->title.clips.size() == 1;
    if (ok) origin_ = std::llround(disc->title.clips[0].in * AV_TIME_BASE);
  }
  if (!ok) {
    close();
    return false;
  }
  // Nothing of the sound or the subtitles between keyframes is read.
  for (unsigned i = 0; i < format_->nb_streams; ++i)
    if (static_cast<int>(i) != stream_) format_->streams[i]->discard = AVDISCARD_ALL;
  return true;
}

void KeyframeReader::close() {
  rest();
  given_ = false;
  avformat_close_input(&format_);
  remote_.reset();
  stream_ = -1;
}

void KeyframeReader::rest() {
  if (frame_) av_frame_unref(frame_);
  avcodec_free_context(&codec_);
}

KeyframeReader::Read KeyframeReader::read(double seconds, int maxWidth, int maxHeight,
                                          Picture& out) {
  if (!format_) return Read::Failed;
  const int64_t at =
      origin_ + std::clamp<int64_t>(std::llround(seconds * AV_TIME_BASE), 0, format_->duration);
  if (!seekToKey(at) && !(at > origin_ && seekToKey(std::max(origin_, at - kStepBack))))
    return Read::Failed;
  // Moments apart in the video mostly share a keyframe (seconds lie between
  // two): the one just given isn't decoded again. It is the same if the
  // packet read is; a file's index doesn't say, where it lists only what
  // was read so far. One with neither a time nor a place can't be told.
  const Key key{packet_->pos, packet_->dts, packet_->pts, packet_->size};
  const bool told = key.pos >= 0 || key.dts != AV_NOPTS_VALUE || key.pts != AV_NOPTS_VALUE;
  if (given_ && told && key.pos == key_.pos && key.dts == key_.dts && key.pts == key_.pts &&
      key.size == key_.size) {
    av_packet_unref(packet_);
    return Read::Same;
  }
  if (!codec_) {
    int stream = -1;
    codec_ = openVideoDecoder(format_, stream, [](AVCodecContext* c) {
      // One thread: one keyframe is all it decodes, and bursts on more
      // cores make the headset's frames late. Without the loop filter it
      // decodes faster, unnoticeably at this size.
      c->thread_count = 1;
      c->skip_loop_filter = AVDISCARD_ALL;
    });
    if (!codec_ || stream != stream_) {
      av_packet_unref(packet_);
      return Read::Failed;
    }
  }
  avcodec_flush_buffers(codec_);
  av_frame_unref(frame_);
  const bool ok = decodeKeyPacket(format_, codec_, stream_, packet_, frame_, kPackets, interrupt_) &&
                  shrink(maxWidth, maxHeight, out);
  av_frame_unref(frame_);
  if (!ok) return Read::Failed;
  given_ = true;
  key_ = key;
  return Read::New;
}

// Seeks to `at` and reads on to the video's next packet marked a keyframe,
// which a seek mostly lands on.
bool KeyframeReader::seekToKey(int64_t at) {
  if (av_seek_frame(format_, -1, at, AVSEEK_FLAG_BACKWARD) < 0 &&
      av_seek_frame(format_, -1, at, 0) < 0)
    return false;
  for (int reads = 0; reads < kReadOn; ++reads) {
    if (Interrupt::check(&interrupt_) || av_read_frame(format_, packet_) < 0) return false;
    if (packet_->stream_index == stream_ && (packet_->flags & AV_PKT_FLAG_KEY)) return true;
    av_packet_unref(packet_);
  }
  return false;
}

// The decoded frame at the size and the way round it is displayed, never
// enlarged.
bool KeyframeReader::shrink(int maxWidth, int maxHeight, Picture& out) {
  if (frame_->width < 2 || frame_->height < 2) return false;
  double sar = av_q2d(frame_->sample_aspect_ratio);
  if (!(sar > 0)) sar = 1;
  const double w = frame_->width * sar, h = frame_->height;
  const int turn = frameRotation(frame_);
  if (turn % 180 == 90) std::swap(maxWidth, maxHeight);
  const double scale = std::min({1.0, maxWidth / w, maxHeight / h});
  int width = std::max(2, static_cast<int>(w * scale));
  int height = std::max(2, static_cast<int>(h * scale));
  sws_ = frameScaler(sws_, frame_, frame_->width, width, height, AV_PIX_FMT_BGRA, SWS_BILINEAR);
  std::vector<uint32_t> pixels;
  if (!sws_ || !scaleInto(sws_, frame_, width, height, pixels)) return false;
  rotatePixels(pixels, width, height, turn);
  out.pixels.swap(pixels);
  out.width = width;
  out.height = height;
  return true;
}
