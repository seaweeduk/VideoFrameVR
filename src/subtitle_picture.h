#pragma once

#include "subtitle_export.h"

#include <cstdint>
#include <string>
#include <vector>

// Whether a subtitle codec (mpv's track-list codec) draws pictures rather
// than text: Blu-ray PGS, DVD VobSub, DVB.
bool isPictureSubtitle(const std::string& codec);

// The largest width or height of the image of a SubtitlePicture: parts
// beyond it are left out.
constexpr int kSubtitlePictureMax = 4096;

// The picture subtitles showing, as mpv handed them out (subtitle_export.h):
// their parts stacked in one image, with where each goes on the canvas (one
// eye's picture).
struct SubtitlePicture {
  struct Part {
    int top = 0, width = 0, height = 0;  // its pixels: rows from top, columns from 0
    int x = 0, y = 0, w = 0, h = 0;      // where it goes on the canvas, scaled to w × h
  };
  uint64_t version = 0;  // counts changes
  int canvasWidth = 0, canvasHeight = 0;
  std::vector<Part> parts;  // none: nothing showing
  int width = 0, height = 0;
  std::vector<uint32_t> pixels;  // BGRA, premultiplied alpha, width × height
};

// One with nothing showing.
const SubtitlePicture& noSubtitlePicture();

// VideoPlayer's side of it: the render parameter, and a copy of what mpv
// hands out whenever it changes (the bitmaps last only until the next call).
class SubtitleExport {
 public:
  SubtitleExport();
  // For each render call that draws: mpv hands the subtitles out instead of
  // drawing them into the video while `take`, laid out on a canvas of the
  // given aspect (one eye's picture as shown).
  subtitle_export* param(bool take, float canvasAspect);
  // Whether param() would ask for something else than the last one did.
  bool changes(bool take, float canvasAspect) const {
    return export_.take != (take ? 1 : 0) || export_.canvas_w != canvasWidth(canvasAspect);
  }
  // After that call.
  void taken();
  // This mpv knows the parameter: once true after a call, it stays.
  bool handled() const { return handled_; }
  const SubtitlePicture& picture() const { return picture_; }

 private:
  static int canvasWidth(float aspect);

  subtitle_export export_{};
  bool handled_ = false;
  int64_t change_ = -1;
  SubtitlePicture picture_;
  std::vector<int> from_;  // taken()'s, kept for its capacity
  bool tooLargeLogged_ = false;
};
