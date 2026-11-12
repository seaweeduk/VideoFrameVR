#pragma once

#include "video_format.h"

#include <cstdint>
#include <memory>
#include <string>

typedef struct _cairo_surface cairo_surface_t;

// One silent Stash preview, downloaded into RAM and decoded on a worker.
// No disk cache or playback/history changes. Main thread only, except stop().
class StashPreview {
 public:
  // `url`: Stash's address as typed; `unverified`: its certificate is
  // accepted unverified.
  StashPreview(const std::string& url, std::string apiKey, bool unverified);
  ~StashPreview();
  StashPreview(const StashPreview&) = delete;
  StashPreview& operator=(const StashPreview&) = delete;

  // Starts the worker on first use. Repeating the same request does nothing;
  // a different scene replaces it. Stereo previews show just the first eye.
  // `width`: frames are made to cover a 16:9 picture that many pixels wide,
  // to be painted at their own size; 0: at most 640 pixels.
  void request(int64_t scene, const VideoFormat& format, int width = 0);
  // Drops the picture immediately and cancels work without waiting.
  void cancel();
  // Takes the newest frame without waiting. True if the picture changed.
  bool update();
  cairo_surface_t* frame() const;

  // At shutdown: wakes the worker and cancels its download. Destruction
  // waits for it, so Stash hands this to its shutdown thread.
  void stop();

 private:
  struct State;
  std::unique_ptr<State> state_;
};
