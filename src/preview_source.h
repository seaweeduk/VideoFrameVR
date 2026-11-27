#pragma once

#include <chrono>
#include <string>

struct PlaybackState;
struct Texture;

// Where the seek bar preview's pictures come from: single pictures of the
// playing video, where the pointer is. A source is told the video and where
// the pointer is, and opens the one and asks for pictures of the other when
// it sees fit: with a hardware decoder (frame_grabber.h) a picture costs
// little and the open decoder much memory; on the CPU (keyframe_grabber.h)
// every picture costs time. All methods must be called on the thread that
// owns the GL context; update() inside GL's access window (gl_interop.h),
// before GlInterop::publish().
class PreviewSource {
 public:
  virtual ~PreviewSource() = default;

  // Another video plays, or none (empty). `stream`: a link or a server's
  // stream, not a file.
  virtual void setVideo(const std::string& location, bool stream) = 0;
  // Every frame the preview shows: the pointer is at `seconds` of the
  // video, `fraction` (0..1) of the seek bar.
  virtual void point(double seconds, double fraction) = 0;
  // Every frame: takes a picture that is ready. `playback`: the playing
  // video's.
  virtual void update(const PlaybackState& playback) = 0;

  enum class State {
    Loading,      // no picture of this video yet
    Ready,        // image() has one, of where the pointer is or was last
    Unavailable,  // this video gives none: it can't be opened, or can't seek
  };
  virtual State state() const = 0;
  // A picture asked for has not come yet.
  virtual bool busy() const = 0;
  // Of a video with two views, which playback shows side by side (a 3D
  // Blu-ray's): its pictures hold the base view alone.
  virtual bool baseViewOnly() const = 0;
  // The whole frame (both eyes of a 3D video; the scene picks one), while
  // Ready. It stays valid until the next update().
  virtual const Texture* image() const = 0;
  virtual int width() const = 0;
  virtual int height() const = 0;

 protected:
  // Not pointed at for this long, a source lets go of its decoder's memory
  // (a hardware decoder's is as much as the playing video's, for 8K about
  // 1.8 GB; a software one's frames are hundreds of MB).
  static constexpr std::chrono::seconds kIdle{5};
};
