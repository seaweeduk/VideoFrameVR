#pragma once

#include "cairo_canvas.h"
#include "curved_panel.h"
#include "playback_state.h"
#include "preview_source.h"
#include "scene_renderer.h"
#include "subtitle_overlay.h"
#include "video_format.h"
#include "xmath.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

class GlInterop;
class MpvGlContext;
class Uploader;

// The seek bar preview: while the pointer is on the strip's seek bar, the
// frame of the video where it points floats above the strip, with its time
// under it. Flat video shows (one eye of) the frame; 180°/360° video the
// middle 16:9 of its front 180° as a 180° video stores it, whatever its
// projection (fisheye unwarped, 360° cut to the front), so every kind looks
// alike. A reader of its own finds the frames (PreviewSource: with a
// hardware decoder a second, silent mpv; on the CPU the file's keyframes),
// so the video itself plays on undisturbed. When it opens the video and
// which frames it asks for are the source's business; this draws them.
// All methods must be called on the thread that owns the GL context.
class SeekPreview {
 public:
  // What is playing, as far as the preview needs to know.
  struct Playing {
    std::string location;
    double duration = 0;  // seconds
    VideoFormat format;
    TwoViews views = TwoViews::No;  // as playback decodes them
    bool flip = false;
    bool stereo = false;  // each eye its own picture of a 3D video, else both the left
  };

  SeekPreview() = default;
  SeekPreview(const SeekPreview&) = delete;
  SeekPreview& operator=(const SeekPreview&) = delete;

  // hwdec: copy-back (e.g. "nvdec-copy"), so frames can be shrunk on the
  // CPU, or "no": the file's keyframes, decoded on the CPU. False if its
  // canvases could not be made; without its mpv it never shows.
  bool init(GlInterop& interop, const std::string& hwdec, Uploader& uploader,
            const MpvGlContext& glContext);
  // Off, it shows nothing and keeps no video open.
  void setEnabled(bool enabled);
  // Another video plays: a link or a server's stream (`stream`), or a file.
  // A photo has no preview, nor a partial download: it would be another
  // reader of a growing file, which may have no data where it points.
  void videoChanged(const std::string& location, bool stream);
  // Lets go of the video (it was closed, or its file is about to be deleted).
  void close() { videoChanged({}, false); }
  // Every frame, inside GL's access window (gl_interop.h): takes a frame
  // that is ready. `playback`: the playing video's.
  void poll(const PlaybackState& playback);
  bool busy() const { return source_ && source_->busy(); }
  // This frame shows the frame at `fraction` (0..1) of the playing video,
  // above that spot of the strip (Menu::seekHover()'s, steadied).
  void show(double fraction, const Playing& playing, const CurvedPanel& strip);
  // ... or shows nothing.
  void hide() { shown_ = false; }
  // What is shown this frame, added to the scene's overlays: the frame (or
  // its placeholder) and the time, at `opacity` (fading with the menu).
  void addTo(std::vector<OverlayDraw>& overlays, float opacity = 1);

 private:
  void place(double fraction, float aspect, const CurvedPanel& strip);

  std::unique_ptr<PreviewSource> source_;  // none: it never shows
  bool enabled_ = true;
  std::string video_;    // the playing video, if it has a preview
  bool stream_ = false;  // ... and is a stream
  bool stereo_ = false;
  SubtitleOverlay time_;     // the time under the frame
  CairoCanvas placeholder_;  // shown while the first frame is still loading
  std::chrono::steady_clock::time_point timeSetAt_;  // the time's text last set
  bool timeOnly_ = false;  // the video shown gives no frames (PreviewSource::State::Unavailable)
  bool shown_ = false;
  VideoFormat format_;  // of the video shown
  bool flip_ = false;
  Mat4 model_, timeModel_;
  float radius_ = 0, halfAngle_ = 0;  // PreviewItem's
};
