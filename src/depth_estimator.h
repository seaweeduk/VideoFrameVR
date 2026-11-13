#pragma once

#include "focus_settings.h"
#include "gl_util.h"
#include "video_format.h"
#include "xmath.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// Why a depth measurement has no usable distance.
enum class DepthStatus {
  Ok,
  Blank,      // too little structure to match (a plain wall, darkness)
  Ambiguous,  // several equally good matches (a repeating pattern)
  Mismatch,   // nothing matches well (occlusion, highlights, misaligned eyes)
  Edge,       // the target is outside the picture or too close to its edge
  Clear,      // passthrough: the room shows through most of the window
};

// What DepthEstimator's GL program draws for a request, for a frame GL
// cannot read: its uniforms. Rows 0.. of a `width`-wide image hold the left
// eye's patch (height rows), then the right eye's, then (thumbnail) a
// kThumbW x kThumbH thumbnail of the whole left eye.
struct DepthPatch {
  int width = 0, height = 0;
  bool thumbnail = true;
  float start[2] = {};  // alpha, beta at the first pixel's corner
  float step = 0;
  int projection = 0;  // 0 = 180, 2 = fisheye
  float fisheyeFov = 0;
  bool flip = false, sbs = false;
  // The packed mask, read as surround.frag's kSeeThrough: 1 its red, 3 red
  // and blue (maskRule()); 0 = none.
  int alphaPacked = 0;
  float alphaScale[2] = {};
  float greenKey = 0;
  // Else a flat background's key (above 0): clear within keyCut of
  // keyColour (SeeThrough::packedKeyColour()).
  float keyColour = 0, keyCut = 0;
  // MaskProbe's instead: rows 0.. of a width x height image cover the left
  // eye's packed mask (inside its circle; -1 outside), the next ones the
  // right eye's, one tap of the full-size picture per pixel, 1 where it has
  // VideoCheck's blue, else 0 (no thumbnail).
  bool maskProbe = false;
  // KeyPicker's instead: red, green and blue in a 4 x 1 image (width 4,
  // height 1), each the mean display value (as the frame came, before TV
  // gamma) of the 5 x 5 texels of the full-size picture around pickUv
  // (frame coordinates), within pickPart (the eye's part or its cube face:
  // its offset and size, as EyeData::stereo's zw and xy), then the largest
  // difference between them in any channel, as the colour key compares
  // them.
  bool keyPick = false;
  float pickUv[2] = {};
  float pickPart[4] = {};
};

// Draws a DepthPatch from a frame that stays out of GL (DirectDepth).
// One at a time: draw() queues it, ready() says when its values are there.
class DepthPatchDrawer {
 public:
  virtual ~DepthPatchDrawer() = default;
  virtual bool draw(const DepthPatch& patch) = 0;
  virtual bool ready() = 0;
  virtual const float* values() = 0;  // width x the patch's rows, once ready()
};

struct DepthRequest {
  GLuint texture = 0;  // the stereo frame (with mipmaps generated, whatever its filter)
  DepthPatchDrawer* drawer = nullptr;  // instead of texture, for a frame that stays out of GL
  // Makes the texture GL's and its mipmaps up to date, run only if it is
  // read (empty: they are).
  std::function<bool()> prepareRead;
  VideoFormat format;  // stereoCorrectionSupported()
  bool flip = false;   // the picture is shown flipped vertically
  Vec3 direction;      // the target ray from the rig centre, in video axes
  float baseline = 0.064f;                     // meters between the cameras
  int accuracy = 8;  // 64 × accuracy candidate depths, accuracy 1..128
  float window = 0;  // the match window across (radians); 0 = the usual 2.9°
  SeeThrough seeThrough;  // passthrough: what shows the room is left out of the match
  // Passthrough: if the room shows through most of the window, the nearest
  // spot within this angle (radians) that is mostly video is measured
  // instead; 0 = none (status Clear).
  float reach = 0;
  // The focus now held, as a half-convergence angle (radians; 0 = none). A
  // match that several depths fit (Ambiguous) is still Ok if it is farther
  // than this and the picture rules this depth out.
  float held = 0;
  uint64_t tag = 0;  // handed back with the estimate
};

struct DepthEstimate {
  uint64_t tag = 0;
  DepthStatus status = DepthStatus::Edge;
  // Status Ok: the candidate depth matched, as a half-convergence angle
  // (radians; distance = baseline / 2 / tan(angle)) and in meters.
  float angle = 0, distance = 0;
  float confidence = 0;     // 0..1 (shown in the diagnostics only)
  // How far the window was moved off the target (DepthRequest::reach), in
  // epipolar angles (radians): alpha from the baseline, beta around it.
  float reachAlpha = 0, reachBeta = 0;
  // 0..1 difference of the whole eye image to its thumbnail before; -1 unknown
  // (no thumbnail with this measurement, or none before).
  float sceneChange = -1;
};

// The candidate depth nearest to `distance` (meters), as a half-convergence
// angle: 64 × accuracy candidates from 0.0001 rad in steps of
// 0.008 / accuracy rad.
float candidateAngle(float distance, float baseline, int accuracy);

// How much higher the right eye's picture is than the left one's (radians), as
// learned from clear matches of the video so far: a pitch between the cameras
// shifts every point alike, a roll by roll × u with u = cot(alpha) cos(beta).
// Decaying sums of the samples, fitted by least squares.
struct VerticalOffset {
  double w = 0, u = 0, v = 0, uu = 0, uv = 0;
  int samples = 0;
  void add(float u, float v);
  float predict(float u) const;
  // Videos shot with converged cameras show far things beyond the infinity
  // of parallel ones, at disparities below 0. beyond: how far below (radians)
  // the video is known to reach, from two clear matches in a row within 1°
  // of each other; pending: the last clear match beyond it. Until then such
  // a match is more likely a mistake.
  float beyond = 0, pending = 0;
  void addBeyond(float disparity);
};

// Measures the depth of a stereo video around a target direction: a small
// patch of each eye, resampled along epipolar lines (so matching points only
// differ horizontally) at a reduced resolution set by the accuracy, is drawn
// on the GPU and read back asynchronously; a worker thread matches the
// patches over the whole candidate range (coarse, then its best few peaks
// refined, sub-pixel), judges the match and rounds it to the nearest
// candidate depth. The render
// thread only issues one small draw and one asynchronous copy per request.
// All methods are called on the GL thread.
class DepthEstimator {
 public:
  // The largest patch image (DepthPatch), in pixels.
  static constexpr int kMaxPatchWidth = 2048, kMaxPatchRows = 1024;
  // The whole left eye, for scene cuts. Its taps are spread over the whole
  // frame, which costs the Frame's GPU several times the patches' (0.64 ms
  // of an 8K frame against 0.1 ms): it is drawn with a measurement only
  // once kThumbEvery seconds have passed since the last, which still
  // notices a cut within a few frames.
  static constexpr int kThumbW = 64, kThumbH = 32;
  static constexpr double kThumbEvery = 0.05;

  ~DepthEstimator();
  bool init();
  // Starts a measurement of the frame now in the texture, unless one is
  // still under way (false).
  bool request(const DepthRequest& request);
  // A finished measurement (each once).
  bool poll(DepthEstimate& out);
  // The next estimate has no earlier picture to compare with (sceneChange -1).
  void forgetScene();
  // Another video: also its eyes' vertical offset is learned anew.
  void forgetVideo();

 private:
  struct Job {
    std::vector<float> pixels;  // left patch, right patch, then the thumbnail (if any)
    bool thumbnail = true;
    // The rows of pixels.
    int rows() const { return 2 * height + (thumbnail ? kThumbH : 0); }
    int stride = 0, width = 0, height = 0, window = 0, low = 0, high = 0, accuracy = 8;
    bool masked = false;  // passthrough: clear pixels are marked
    int reach = 0;        // pixels of extra patch on every side, for DepthRequest::reach
    float step = 0, alpha = 0, beta = 0, baseline = 0, held = 0;
    bool forget = false, forgetVideo = false;
    DepthEstimate result;
  };
  void work();

  GLuint program_ = 0, vao_ = 0, target_ = 0, fbo_ = 0, pbo_ = 0;
  GLuint sampler_ = 0;  // trilinear, whatever filter the video texture is set to
  GLint uTex_ = -1, uMode_ = -1, uRect_ = -1, uStart_ = -1, uStep_ = -1, uProjection_ = -1,
        uFisheyeFov_ = -1, uFlip_ = -1, uEyeScale_ = -1, uEyeOffset_ = -1, uAlphaPacked_ = -1,
        uAlphaCenter_ = -1, uAlphaScale_ = -1, uGreenKey_ = -1, uKeyCut_ = -1,
        uKey_ = -1;
  GLsync fence_ = nullptr;
  DepthPatchDrawer* drawer_ = nullptr;  // drawing pending_ instead of GL
  Job pending_;  // read back on the GPU
  bool forgetScene_ = false, forgetVideo_ = false;
  std::chrono::steady_clock::time_point thumbnailAt_{};  // the last request with one

  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable wake_;
  enum class Stage { Idle, Gpu, Working, Ready } stage_ = Stage::Idle;
  Job job_;  // with the worker from Working until Ready
  bool quit_ = false;
  std::vector<float> lastThumbnail_;  // worker only
  VerticalOffset vertical_;           // worker only
};
