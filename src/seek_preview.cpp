#include "seek_preview.h"

#include "file_util.h"
#include "frame_grabber.h"
#include "keyframe_grabber.h"
#include "menu.h"
#include "menu_style.h"
#include "strings.h"

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
// A 180° video's eye 640 pixels square: the preview shows its middle 101°.
constexpr int kFrameSize = 1280;
// The time is drawn anew for each change, on the thread that draws the
// headset's frames: while the pointer runs along the bar, at most this often.
constexpr std::chrono::milliseconds kTimeInterval{50};
// The bar's dead zone (Menu::seekHover()), of the bar's width.
constexpr double kSteady = strip::kSeekSteady / (strip::kRight - strip::kLeft);
}  // namespace

bool SeekPreview::init(GlInterop& interop, const std::string& hwdec, Uploader& uploader,
                       const MpvGlContext& glContext) {
  if (hwdec == "no") {
    source_ = std::make_unique<KeyframeGrabber>(uploader, kFrameSize, kFrameSize, kSteady);
  } else {
    auto grabber = std::make_unique<FrameGrabber>();
    if (grabber->init(interop, hwdec, kFrameSize, kFrameSize, glContext))
      source_ = std::move(grabber);
  }
  if (!time_.init(uploader) || !placeholder_.init(uploader, 320, 180)) return false;
  cairo_set_source_rgba(placeholder_.cr(), 0.12, 0.12, 0.15, 1.0);
  cairo_paint(placeholder_.cr());
  placeholder_.upload();
  return true;
}

void SeekPreview::setEnabled(bool enabled) {
  if (enabled == enabled_) return;
  enabled_ = enabled;
  if (!enabled_) shown_ = false;
  if (source_) source_->setVideo(enabled_ ? video_ : std::string(), stream_);
}

void SeekPreview::videoChanged(const std::string& location, bool stream) {
  const bool previewed = !isPhotoFile(location) && !isPartialDownload(location);
  video_ = previewed ? location : std::string();
  stream_ = stream;
  if (source_ && enabled_) source_->setVideo(video_, stream_);
}

void SeekPreview::poll(const PlaybackState& playback) {
  if (source_) source_->update(playback);
}

void SeekPreview::show(double fraction, const Playing& playing, const CurvedPanel& strip) {
  shown_ = source_ && enabled_ && !video_.empty() && playing.location == video_;
  if (!shown_) return;
  const double target = fraction * playing.duration;
  source_->point(target, fraction);
  const PreviewSource::State state = source_->state();
  timeOnly_ = state == PreviewSource::State::Unavailable;
  format_ = playing.format;
  // One picture, where playback shows both views side by side.
  if (playing.views == TwoViews::Both && source_->baseViewOnly()) {
    format_.stereo = StereoLayout::Mono;
    format_.packing = Packing::Auto;
  }
  flip_ = playing.flip;
  stereo_ = playing.stereo;
  // Until the first frame arrives, a placeholder says it is loading; later
  // the last frame stays until the next is ready. One that can't come (no
  // seeking): the time alone.
  const auto now = std::chrono::steady_clock::now();
  if (now - timeSetAt_ >= kTimeInterval) {
    const bool loading = state == PreviewSource::State::Loading;
    time_.setText(loading ? clockTime(target) + "  ·  " + strings().loading : clockTime(target));
    timeSetAt_ = now;
  }

  float aspect = 16.0f / 9.0f;
  if (state == PreviewSource::State::Ready && format_.projection == Projection::Flat)
    aspect = eyeDisplayAspect(format_, static_cast<float>(source_->width()) / source_->height());
  place(fraction, aspect, strip);
}

// Just above the strip, over the seek bar where the pointer is (it runs 40
// pixels in from the strip's ends), bent like the strip.
void SeekPreview::place(double fraction, float aspect, const CurvedPanel& strip) {
  float h = 0.26f, w = h * aspect;
  if (w > 0.58f) {
    w = 0.58f;
    h = w / aspect;
  }
  const float stripW = strip.right - strip.left;
  const float margin = w / 2 / stripW;
  const float u = std::clamp(
      (40.0f + static_cast<float>(fraction) * (Menu::kMainWidth - 80.0f)) / Menu::kMainWidth,
      margin, 1 - margin);
  const float angle = (strip.left + u * stripW) / strip.radius;
  const float y = strip.top - static_cast<float>(Menu::kStripBodyY) / Menu::kMainHeight *
                                  (strip.top - strip.bottom) + 0.02f + h / 2;
  const float r = strip.radius - 0.03f;
  const Mat4 base = strip.model() * rotationY(-angle);
  model_ = base * translation({0.0f, y, 0.0f}) * scaling(1, h, 1);
  radius_ = r;
  halfAngle_ = w / 2 / r;
  const float lw = 0.48f, lh = lw / SubtitleOverlay::kAspect;
  timeModel_ = base * translation({0.0f, y - h / 2 + lh / 2, -r + 0.002f}) * scaling(lw, lh, 1);
}

void SeekPreview::addTo(std::vector<OverlayDraw>& overlays, float opacity) {
  if (!shown_) return;
  if (timeOnly_) {
    PanelItem time;
    time.texture = time_.texture();
    time.model = timeModel_;
    time.opacity = opacity;
    overlays.push_back(time);
    return;
  }
  PreviewItem frame;
  frame.model = model_;
  frame.radius = radius_;
  frame.halfAngle = halfAngle_;
  frame.stereo = stereo_;
  frame.opacity = opacity;
  if (const Texture* image = source_->image()) {
    frame.texture = image;
    frame.format = format_;
    frame.flip = flip_;
  } else {
    frame.texture = placeholder_.texture();
  }
  overlays.push_back(frame);
  PanelItem time;
  time.texture = time_.texture();
  time.model = timeModel_;
  time.opacity = opacity;
  overlays.push_back(time);
}
