#pragma once

#include "depth_estimator.h"  // DepthPatchDrawer
#include "gl_util.h"
#include "video_memory.h"  // KeyColour
#include "xmath.h"

#include <functional>

// The colour the background key is to remove, read where the user pointed
// (Video settings → Passthrough → Background → Pick a colour): the mean of
// the 5 x 5 texels of the picture's full-size level around a point, in
// display values of the picture as it came (KeyColour), unless they differ
// too much, as the scene's colour key compares them, to make one colour.
// Once per press, read back when done, never waited for: by GL with a
// fence (its program made at startup), or for frames that stay out of GL
// by a DepthPatchDrawer (DirectDepth, DepthPatch::keyPick). Main thread,
// with the GL context current.
class KeyPicker {
 public:
  KeyPicker() = default;
  KeyPicker(const KeyPicker&) = delete;
  KeyPicker& operator=(const KeyPicker&) = delete;
  ~KeyPicker();
  // At startup, the GL read's program and buffers; false if they failed
  // (the GL read is then Unavailable).
  bool init();

  // Where the left eye's picture of a surround video shows `direction`
  // (the picture's axes, ScreenPlacement::rotation()'s inverse applied):
  // uv in frame coordinates and the eye's part, or a cubemap's face, it is
  // read within (DepthPatch::pickPart). False where the picture has nothing.
  static bool pointOf(const VideoFormat& format, float frameAspect, bool flip, Vec3 direction,
                      float uv[2], float part[4]);
  // Reads around uv within part: from `drawer` if there is one, else the GL
  // `texture` after `prepare()`, which mpv converted for TV gamma if
  // `tvGamma` (VideoPlayer::tvGammaImage()). Later: not now (a read is on its way, or
  // there is nothing to read yet); Unavailable: GL can't read it.
  enum class Read { Started, Later, Unavailable };
  Read read(const float uv[2], const float part[4], GLuint texture, bool tvGamma,
            const std::function<bool()>& prepare, DepthPatchDrawer* drawer);
  bool busy() const { return fence_ || drawer_; }
  // The read, once, as it comes: the colour (Picked), or texels too
  // different to be one (Uneven: the patch crosses an edge), or nothing
  // read back (Failed). None: no read has come.
  enum class Result { None, Picked, Uneven, Failed };
  Result result(KeyColour& colour);
  // Forgets a read on its way.
  void cancel();

 private:
  bool glReady_ = false;
  GLuint program_ = 0, vao_ = 0, target_ = 0, fbo_ = 0, pbo_ = 0;
  GLint uTex_ = -1, uUv_ = -1, uPart_ = -1, uTv_ = -1;
  GLsync fence_ = nullptr;              // a GL read on its way
  DepthPatchDrawer* drawer_ = nullptr;  // ... or a drawer's
};
