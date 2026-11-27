#pragma once

#include <string>

// VideoPlayer's partial download (isPartialDownload()) where its data ends:
// its duration only grows, a seek sent from the end of its data drops mpv's
// buffers first (more may have come since), and a seek that waits for data
// the download hasn't brought yet is a wait for it, as the end of the data
// is (VideoPlayer::waiting()). Such a seek is told from one that is only slow
// to decode by time alone: it has gone on longer than a seek that finds its
// data takes. It is never undone: the picture stays where it was, and a seek
// back plays on. Nothing changes for any other file. Same thread as
// VideoPlayer.
class DownloadEdge {
 public:
  void load(const std::string& path);
  void stop();
  bool partial() const { return partial_; }

  // mpv's duration, or a position or the cache's end it reached, for the
  // file mpv has loaded (`loaded`; earlier values may be the last file's):
  // a partial download's `duration` is the largest of them, 0 until one.
  void grow(double& duration, double reached, bool loaded);
  // A seek from here drops mpv's buffers first (SeekQueue::send()): its
  // demuxer remembers where the data ended.
  bool dropBeforeSeek(bool endReached) const { return partial_ && endReached; }
  // mpv's seeking flag, now (steady seconds).
  void seeking(bool on, double now);
  // A seek has waited for its data longer than `after` seconds.
  bool seekWaits(double now, double after) const;

 private:
  bool partial_ = false;
  double largest_ = 0;        // seconds, of this file
  double seekingSince_ = -1;  // steady seconds, -1 = not seeking
};
