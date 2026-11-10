#pragma once

#include "depth_estimator.h"  // DepthPatchDrawer
#include "gl_util.h"

#include <functional>
#include <string>

// Looks for a packed mask's second layer (VideoSettings::maskLayersFound) in
// the frames shown, for a layer that first shows after the keyframes the
// mask layer check looked at (video_check.h). Every half second while it is
// wanted, a grid of single taps over each eye's mask, from the picture's
// full-size level (no mipmaps made for it), is tested for the check's blue
// on the GPU and read back once done, never waited for: by GL with a fence,
// or for frames that stay out of GL by a DepthPatchDrawer (DirectDepth,
// DepthPatch::maskProbe). Two frames with the blue in both eyes find it.
// Main thread, with the GL context current.
class MaskProbe {
 public:
  static constexpr int kGrid = 256;  // taps across each eye's mask

  MaskProbe() = default;
  MaskProbe(const MaskProbe&) = delete;
  MaskProbe& operator=(const MaskProbe&) = delete;
  ~MaskProbe();

  bool init();
  // Each loop iteration, for the video shown (another starts over); while
  // `wanted`, probes its frame now and then: from `drawer` if there is one,
  // else from the GL `texture` after `prepare()`. True once, as the second
  // layer is found.
  bool update(double now, const std::string& video, bool wanted, GLuint texture,
              const std::function<bool()>& prepare, DepthPatchDrawer* drawer);
  // A drawer's probe is on its way.
  bool drawing() const { return drawer_ != nullptr; }

 private:
  bool count(const float* values);
  bool count(const uint8_t* values);
  bool tally(int left, int right);

  GLuint program_ = 0, vao_ = 0, sampler_ = 0, target_ = 0, fbo_ = 0, pbo_ = 0;
  GLint uTex_ = -1, uRect_ = -1, uAlphaCenter_ = -1;
  GLsync fence_ = nullptr;             // a GL probe on its way
  DepthPatchDrawer* drawer_ = nullptr;  // ... or a drawer's
  std::string video_;
  bool stale_ = false;  // the probe on its way is the last video's
  int found_ = 0;       // probes with the layer, for this video
  double next_ = 0;     // the next probe's time
};
