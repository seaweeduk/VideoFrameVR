#include "download_edge.h"

#include "file_util.h"

#include <algorithm>

void DownloadEdge::load(const std::string& path) {
  partial_ = isPartialDownload(path);
  largest_ = 0;
  seekingSince_ = -1;
}

void DownloadEdge::stop() {
  partial_ = false;
  seekingSince_ = -1;
}

void DownloadEdge::grow(double& duration, double reached, bool loaded) {
  if (!partial_) return;
  if (loaded) largest_ = std::max(largest_, reached);
  duration = largest_;
}

void DownloadEdge::seeking(bool on, double now) {
  seekingSince_ = !on ? -1 : seekingSince_ >= 0 ? seekingSince_ : now;
}

bool DownloadEdge::seekWaits(double now, double after) const {
  return partial_ && seekingSince_ >= 0 && now - seekingSince_ > after;
}
