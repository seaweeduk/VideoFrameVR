#include "media_decode.h"

#include "remote_avio.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

AVFormatContext* openMedia(const std::string& path, RemoteAvio& remote, Interrupt& interrupt) {
  AVFormatContext* format = avformat_alloc_context();
  if (!format) return nullptr;
  format->interrupt_callback = {Interrupt::check, &interrupt};
  if (!remote.attach(format, path)) {
    avformat_free_context(format);
    return nullptr;
  }
  if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0) return nullptr;
  if (avformat_find_stream_info(format, nullptr) < 0) {
    avformat_close_input(&format);
    return nullptr;
  }
  return format;
}

AVCodecContext* openVideoDecoder(AVFormatContext* format, int& stream,
                                 const std::function<void(AVCodecContext*)>& configure) {
  const AVCodec* decoder = nullptr;
  stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
  if (stream < 0 || !decoder) return nullptr;
  AVCodecContext* codec = avcodec_alloc_context3(decoder);
  if (!codec || avcodec_parameters_to_context(codec, format->streams[stream]->codecpar) < 0) {
    avcodec_free_context(&codec);
    return nullptr;
  }
  configure(codec);
  if (avcodec_open2(codec, decoder, nullptr) < 0) avcodec_free_context(&codec);
  return codec;
}

bool decodeFirstFrame(AVFormatContext* format, AVCodecContext* codec, int stream,
                      AVPacket* packet, AVFrame* frame, int limit, Interrupt& interrupt) {
  bool got = false, ended = false;
  for (int reads = 0; !got && reads < limit && !Interrupt::check(&interrupt); ++reads) {
    if (!ended && av_read_frame(format, packet) < 0) {
      ended = true;
      avcodec_send_packet(codec, nullptr);  // drain what the decoder holds
    } else if (!ended) {
      if (packet->stream_index == stream) avcodec_send_packet(codec, packet);
      av_packet_unref(packet);
    }
    got = avcodec_receive_frame(codec, frame) == 0;
    if (ended && !got) break;
  }
  return got && !Interrupt::check(&interrupt);
}
