#pragma once

#include "cairo_canvas.h"
#include "curved_panel.h"
#include "frame_grabber.h"
#include "scene_renderer.h"
#include "subtitle_overlay.h"
#include "video_format.h"
#include "xmath.h"
#include "xr_context.h"

#include <chrono>
#include <string>
#include <vector>

// The seek bar preview: while the pointer is on the strip's seek bar, the
// frame of the video where it points floats above the strip, with its time
// under it. Flat video shows (one eye of) the frame; 180°/360° video the
// view straight ahead, rendered like in the headset. A second, silent mpv
// (FrameGrabber) finds the frames, so the video itself plays on undisturbed.
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
  };

  SeekPreview() = default;
  SeekPreview(const SeekPreview&) = delete;
  SeekPreview& operator=(const SeekPreview&) = delete;

  // hwdec: copy-back (e.g. "nvdec-copy") or "no", so frames can be shrunk on the
  // CPU. False if its canvases could not be made; without its mpv it never shows.
  bool init(GlInterop& interop, const std::string& hwdec, Uploader& uploader);
  // Off, it shows nothing and keeps no video open.
  void setEnabled(bool enabled);
  // Another video plays. Streams take seconds to open, so the preview opens
  // one right away; local files open fast enough when first pointed at.
  void videoChanged(const std::string& location, bool stream);
  // Lets go of the video (it was closed, or its file is about to be deleted).
  void close();
  // Every frame, inside GL's access window (gl_interop.h): takes a frame mpv
  // has ready, and puts the decoder to sleep once it has been unused for a while.
  void poll();
  bool busy() const { return ready_ && grabber_.busy(); }
  // This frame shows the frame at `fraction` (0..1) of the playing video,
  // above that spot of the strip.
  void show(double fraction, const Playing& playing, const CurvedPanel& strip);
  // ... or shows nothing.
  void hide() { shown_ = false; }
  // The corners of what it shows (local space), none while hidden.
  std::vector<Vec3> corners() const;
  // What is shown this frame, added to the scene's overlays: the frame (or
  // its placeholder) and the time.
  void addTo(std::vector<OverlayDraw>& overlays);

 private:
  void place(double fraction, float aspect, const CurvedPanel& strip);

  FrameGrabber grabber_;
  bool ready_ = false;  // the grabber's mpv runs
  bool enabled_ = true;
  SubtitleOverlay time_;     // the time under the frame
  CairoCanvas placeholder_;  // shown while the first frame is still loading
  double target_ = -1;  // seconds the frame was asked for
  std::chrono::steady_clock::time_point lastUse_;
  bool shown_ = false;
  VideoFormat format_;  // of the video shown
  bool flip_ = false;
  Mat4 model_, timeModel_;
};
