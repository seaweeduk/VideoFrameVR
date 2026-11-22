#include "subtitle_picture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// The canvas's height in pixels: a Blu-ray's subtitles are laid out on 1080
// rows, a DVD's are scaled to it.
constexpr int kCanvasHeight = 1080;
// Each part starts on a row that is a multiple of this, with at least as
// many empty rows above it: the texture's smallest mipmap (an eighth) and
// its anisotropic filtering, up to 8 of those texels along a row, then
// don't reach from one part into the next.
constexpr int kAlign = 64;

int alignUp(int n) { return (n + kAlign - 1) / kAlign * kAlign; }

bool usable(const subtitle_export_part& s) {
  return s.bitmap && s.w > 0 && s.h > 0 && s.dw > 0 && s.dh > 0 && s.stride >= s.w * 4;
}

}  // namespace

bool isPictureSubtitle(const std::string& codec) {
  return codec == "hdmv_pgs_subtitle" || codec == "dvd_subtitle" || codec == "dvb_subtitle" ||
         codec == "xsub";
}

const SubtitlePicture& noSubtitlePicture() {
  static const SubtitlePicture none;
  return none;
}

SubtitleExport::SubtitleExport() {
  export_.magic = SUBTITLE_EXPORT_MAGIC;
  export_.size = sizeof(export_);
}

int SubtitleExport::canvasWidth(float aspect) {
  if (!std::isfinite(aspect) || aspect <= 0) aspect = 16.0f / 9.0f;
  return static_cast<int>(std::clamp(std::lround(kCanvasHeight * aspect), 64L, 8192L));
}

subtitle_export* SubtitleExport::param(bool take, float canvasAspect) {
  export_.take = take ? 1 : 0;
  export_.canvas_h = kCanvasHeight;
  export_.canvas_w = canvasWidth(canvasAspect);
  export_.handled = 0;
  return &export_;
}

void SubtitleExport::taken() {
  if (!export_.handled) return;
  handled_ = true;
  SubtitlePicture& p = picture_;
  const int count = export_.parts ? std::max(export_.num_parts, 0) : 0;
  if (!export_.take) {
    // mpv draws them again: a later take starts afresh.
    change_ = -1;
    if (!p.parts.empty()) {
      p.parts.clear();
      p.width = p.height = 0;
      ++p.version;
    }
    return;
  }
  // Nothing new, or still nothing to show.
  if ((export_.change_id == change_ && export_.canvas_w == p.canvasWidth &&
       export_.canvas_h == p.canvasHeight) ||
      (count == 0 && p.parts.empty())) {
    change_ = export_.change_id;
    return;
  }
  change_ = export_.change_id;
  p.canvasWidth = export_.canvas_w;
  p.canvasHeight = export_.canvas_h;
  p.parts.clear();
  p.width = p.height = 0;
  std::vector<int>& from = from_;  // the export's part each one is
  from.clear();
  for (int i = 0; i < count; ++i) {
    const subtitle_export_part& s = export_.parts[i];
    if (!usable(s)) continue;
    SubtitlePicture::Part part;
    part.top = p.parts.empty() ? 0 : alignUp(p.height + kAlign);
    if (s.w > kSubtitlePictureMax || part.top + s.h > kSubtitlePictureMax) {
      if (!tooLargeLogged_)
        std::printf("Subtitles: a picture subtitle too large to show whole (%d parts)\n", count);
      tooLargeLogged_ = true;
      continue;
    }
    part.width = s.w;
    part.height = s.h;
    part.x = s.x;
    part.y = s.y;
    part.w = s.dw;
    part.h = s.dh;
    p.height = part.top + s.h;
    p.width = std::max(p.width, s.w);
    p.parts.push_back(part);
    from.push_back(i);
  }
  p.pixels.assign(static_cast<size_t>(p.width) * p.height, 0);
  for (size_t i = 0; i < p.parts.size(); ++i) {
    const subtitle_export_part& s = export_.parts[from[i]];
    const SubtitlePicture::Part& part = p.parts[i];
    for (int row = 0; row < part.height; ++row)
      std::memcpy(&p.pixels[static_cast<size_t>(part.top + row) * p.width],
                  s.bitmap + static_cast<size_t>(row) * s.stride, static_cast<size_t>(part.width) * 4);
  }
  ++p.version;
}
