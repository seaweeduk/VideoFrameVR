#pragma once

#include "video_format.h"

#include <string>

// How many of the video's pixels reach each eye, as one console line: the
// eye's part of the frame, its pixels per degree of view straight ahead, and
// the mip level that roughly picks there against the eye image's density.
// screenWidthOverDistance: the flat screen's width over its distance;
// mipmaps: the picture is sampled through its mipmaps (else level 0 only).
// Written into `line`, which keeps its memory.
void describeEyeSampling(std::string& line, const VideoFormat& format, int frameWidth,
                         int frameHeight, float zoom, float screenWidthOverDistance,
                         float eyePixelsPerDegree, bool mipmaps);

// Prints a line once it has stayed the same for a second, so dragging a
// slider or the screen does not flood the console. Each update() takes
// line(), written in place before it.
class SettledLine {
 public:
  std::string& line() { return line_; }
  void update(double now);

 private:
  std::string line_, printed_, pending_;
  double since_ = 0;
};
