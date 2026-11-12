#include "seek_preview.h"

#include "bluray.h"
#include "menu.h"
#include "strings.h"

#include <cairo.h>

#include <algorithm>
#include <cmath>

namespace {
// The preview's decoder holds as much GPU memory as the playing video's
// (8K: about 1.8 GB). Unused this long, it sleeps; waking costs about a seek.
constexpr std::chrono::seconds kDecoderIdle{5};
}  // namespace

bool SeekPreview::init(GlInterop& interop, const std::string& hwdec, Uploader& uploader,
                       const MpvGlContext& glContext) {
  ready_ = grabber_.init(interop, hwdec, 640, 640, glContext);
  if (!time_.init(uploader) || !placeholder_.init(uploader, 320, 180)) return false;
  cairo_set_source_rgba(placeholder_.cr(), 0.12, 0.12, 0.15, 1.0);
  cairo_paint(placeholder_.cr());
  placeholder_.upload();
  return true;
}

void SeekPreview::setEnabled(bool enabled) {
  enabled_ = enabled;
  if (enabled_) return;
  grabber_.close();
  shown_ = false;
}

void SeekPreview::videoChanged(const std::string& location, bool stream) {
  if (ready_ && enabled_ && stream)
    grabber_.open(location, "0");
  else
    grabber_.close();
  target_ = 0;
  lastUse_ = std::chrono::steady_clock::now();
}

void SeekPreview::close() { grabber_.close(); }

void SeekPreview::poll() {
  if (!ready_) return;
  grabber_.update();
  if (!grabber_.asleep() && std::chrono::steady_clock::now() - lastUse_ > kDecoderIdle)
    grabber_.sleep();
}

void SeekPreview::show(double fraction, const Playing& playing, const CurvedPanel& strip) {
  shown_ = ready_ && enabled_;
  if (!shown_) return;
  lastUse_ = std::chrono::steady_clock::now();
  const double target = fraction * playing.duration;
  if (grabber_.path() != playing.location) {
    grabber_.open(playing.location, std::to_string(target));
    target_ = target;
  } else if (std::abs(target - target_) > 0.5) {
    grabber_.seek(target);
    target_ = target;
  }
  format_ = playing.format;
  // A 3D disc's previews decode its base view alone (FrameGrabber::open()):
  // one picture, where playback shows both views side by side.
  if (playing.views == TwoViews::Both && bdmv::isDiscImage(playing.location)) {
    format_.stereo = StereoLayout::Mono;
    format_.packing = Packing::Auto;
  }
  flip_ = playing.flip;
  // Until the first frame arrives, a placeholder says it is loading.
  time_.setText(grabber_.hasFrame() ? clockTime(target)
                                    : clockTime(target) + "  ·  " + strings().loading);

  float aspect = 16.0f / 9.0f;
  if (grabber_.hasFrame()) {
    const VideoFormat& shown = format_;
    if (shown.projection == Projection::Flat) {
      aspect = eyeDisplayAspect(shown, static_cast<float>(grabber_.width()) / grabber_.height());
    }
  }
  place(fraction, aspect, strip);
}

// Just above the strip, over the seek bar where the pointer is (it runs 40
// pixels in from the strip's ends), facing the viewer.
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
  model_ = base * translation({0.0f, y, -r}) * scaling(w, h, 1);
  const float lw = 0.48f, lh = lw / SubtitleOverlay::kAspect;
  timeModel_ = base * translation({0.0f, y - h / 2 + lh / 2, -r + 0.002f}) * scaling(lw, lh, 1);
}

void SeekPreview::addTo(std::vector<OverlayDraw>& overlays) {
  if (!shown_) return;
  if (const Texture* image = grabber_.image()) {
    PreviewItem frame;
    frame.texture = image;
    frame.model = model_;
    frame.format = format_;
    frame.flip = flip_;
    overlays.push_back(frame);
  } else {
    PanelItem frame;
    frame.texture = placeholder_.texture();
    frame.model = model_;
    overlays.push_back(frame);
  }
  PanelItem time;
  time.texture = time_.texture();
  time.model = timeModel_;
  overlays.push_back(time);
}
