#pragma once

#include "cairo_canvas.h"
#include "scene_renderer.h"
#include "subtitle_picture.h"

#include <cstdint>
#include <memory>
#include <vector>

class Uploader;
struct ScreenPlacement;

// Picture subtitles (Blu-ray PGS, DVD VobSub, DVB) drawn where the disc puts
// them, as a player would: on the flat screen, in each eye's picture (so 3D
// video shows them at the screen's depth); for surround video on a picture
// of the same shape ahead of the viewer. Their parts are copied into one
// texture as they change (VideoPlayer::subtitlePicture()), made only once
// a video shows picture subtitles.
class PictureSubtitles {
 public:
  explicit PictureSubtitles(Uploader& uploader) : uploader_(uploader) {}

  // What shows now; uploaded if it changed.
  void update(const SubtitlePicture& picture);
  bool empty() const { return parts_.empty() || !canvas_ || !canvas_->texture(); }
  // Another number whenever what is drawn changes; never 0.
  uint64_t version() const { return version_; }

  // On the flat screen (FrameScene::onPicture): each part where it goes on
  // one eye's picture, eyeAspect its width / height, faded with the video.
  void addTo(std::vector<OverlayDraw>& items, const ScreenPlacement& screen, float eyeAspect,
             float opacity) const;
  // Surround video (FrameScene::overlays): the picture `canvas` places, the
  // unit quad (-0.5..0.5) into local space.
  void addTo(std::vector<OverlayDraw>& items, const Mat4& canvas) const;

 private:
  // Where a part shows, as fractions of the canvas, and its texture coordinates.
  struct Shown {
    float x0, x1, y0, y1;
    float u0, u1, v0, v1;
  };
  bool shown(const SubtitlePicture::Part& part, Shown& out) const;
  bool upload(const SubtitlePicture& picture);

  Uploader& uploader_;
  std::unique_ptr<CairoCanvas> canvas_;
  int uploadedWidth_ = 0, uploadedHeight_ = 0;  // of the canvas, what holds a picture
  uint64_t shown_ = 0;    // SubtitlePicture::version of what was uploaded
  uint64_t version_ = 1;
  int canvasWidth_ = 0, canvasHeight_ = 0;
  std::vector<SubtitlePicture::Part> parts_;
};
