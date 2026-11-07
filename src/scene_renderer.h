#pragma once

#include "anti_shimmer.h"
#include "curved_panel.h"
#include "flat_screen.h"
#include "foveation.h"
#include "glass_backdrop.h"
#include "gpu_context.h"
#include "pipelines.h"
#include "stereo_correction.h"
#include "texture.h"
#include "video_format.h"
#include "video_memory.h"
#include "xmath.h"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

class DirectVideo;
class EyeTargets;
class GlInterop;
class MirrorWindow;
class Uploader;
struct EyeFrame;
struct EyeMatrices;
class XrContext;

// What the eye images are cleared to, and how passthrough is drawn.
enum class ClearMode {
  Dark,         // the empty scene
  KeyGreen,     // the room shows where the Frame's camera keyer sees key green (desktop, streaming)
  Transparent,  // native passthrough: alpha blended over the camera (premultiplied)
};

// The video, a photo or the empty flat screen.
struct VideoDraw {
  const Texture* texture = nullptr;  // none yet: the flat screen shows its placeholder colour
  VideoFormat format;
  float frameAspect = 16.0f / 9.0f;  // the whole frame's width / height (both eyes, if stereo)
  bool flip = false;
  ScreenPlacement screen;
  float adjust[3] = {0, 0, 0};  // brightness, contrast, saturation -1..1 (photos; mpv adjusts video)
  bool mipmaps = true;          // trilinear through the mipmaps, else the full-size level
  AntiShimmer antiShimmer = AntiShimmer::Off;  // ... sampled over each pixel's footprint
  bool look = false;            // tone curve and vignette
  float sharpen = 0;            // sharpening strength (Settings::sharpen / 100), 0 = off
  SeeThrough seeThrough;        // with passthrough on: the packed mask or a keyed green screen
  GreenScreen key;              // ... its softness, spill and shrink
  StereoCorrection correction;
};

// The menu button under the pointer, highlighted by the panel shader.
struct PanelHover {
  float rect[4] = {0, 0, 0, 0};  // x0, y0, x1, y1 in layout pixels
  float radius = 18;             // corner radius; half the size for a round button
  bool danger = false;           // destructive action (quit, delete): highlighted red
  float clip[4] = {-1e6f, -1e6f, 1e6f, 1e6f};  // cut off outside this (its list's edges)
  uint8_t rim[3] = {30, 120, 255};  // the rim's colour, sRGB: the menu's accent
};

// A canvas on a flat quad (text, markers) or on part of a cylinder (menus).
struct PanelItem {
  const Texture* texture = nullptr;  // premultiplied alpha
  Mat4 model;                        // flat: the unit quad (-0.5..0.5) into local space
  std::optional<CurvedPanel> curved;  // instead of model
  std::optional<PanelHover> hover;
  float opacity = 1;  // faded out toward 0
  // Glass menus: the panel's lowest coverage. On the desktop the scene drawn
  // before the first of them shows blurred behind their translucent parts.
  float glassAlpha = 0;
};
// Laser-coloured quads: the flat screen's corner handles.
struct QuadsItem {
  std::vector<Quad> quads;
};
// The pointer's beam, facing each eye, and its dot at the end.
struct LaserItem {
  Vec3 from, to;
  float dotRadius = 0;
};
// The seek preview's frame (seek_preview.h) on a flat quad: the left eye of
// a flat video, or of a surround one the view straight ahead.
struct PreviewItem {
  const Texture* texture = nullptr;  // the whole frame, both eyes if stereo
  Mat4 model;                        // the unit quad (-0.5..0.5) into local space
  VideoFormat format;
  bool flip = false;
  float aheadTangents[2] = {1.0f, 9.0f / 16.0f};  // surround: the view's half width and height
};
using OverlayDraw = std::variant<PanelItem, QuadsItem, LaserItem, PreviewItem>;

// Everything one frame draws, built by main.cpp, recorded by SceneRenderer.
struct FrameScene {
  ClearMode clear = ClearMode::Dark;
  bool sky = false;  // the room-fixed night sky behind a flat screen
  std::optional<VideoDraw> video;
  std::vector<OverlayDraw> overlays;  // over the video, in drawing order
  bool showFoveation = false;
  FoveationDebug foveation[2];
};

// The eye's part of every draw's push constants (shaders/common.glsl, EyeData).
struct EyeData {
  float orientation[4];  // quaternion x, y, z, w
  float position[4];     // w = the eye's index
  float tangents[4];     // left, right, down, up
  float area[4];         // render area in pixels: x, y, width, height
  float stereo[4];       // this eye's part of a stereo frame: scale xy, offset xy
  float viewer[4];       // stereo correction: eye from head centre, w = focus distance
  float camera[4];       // ... its camera from the rig centre
};
static_assert(sizeof(EyeData) == 112, "EyeData must match shaders/common.glsl");
// Where the draw's block starts in the push constants, and their size.
constexpr uint32_t kDrawPushOffset = sizeof(EyeData);
constexpr uint32_t kPushConstantBytes = 256;

// Draw flags (shaders/video.glsl).
enum VideoFlag : uint32_t {
  kFlip = 1,
  kBaseLevel = 2,
  kLook = 4,
  kKeyGreen = 8,
  kAlphaOut = 16,
  kPackedAlpha = 32,
  kChromaKey = 64,
  kCorrect = 128,
  kTexture = 256,
  kYuv = 512,
  kTvGamma = 1024,
};

// shaders/surround.frag, SurroundDraw.
struct SurroundPush {
  float toPicture[3][4];  // columns of the rotation from local space into the picture's axes
  float params[4];        // x fisheye field of view (radians), y zoom; zw Kr, Kb (kYuv)
  float adjust[4];  // w: sharpening strength
  float chroma[4];
  float alpha[4];    // zw: kYuv, the chroma lookup's offset
  uint32_t mode[4];  // x projection, y flags, z footprint taps (antiShimmerTaps)
  float yuv[4];      // kYuv: YuvColor::range
};
static_assert(sizeof(SurroundPush) == 144 && offsetof(SurroundPush, mode) == 112,
              "SurroundPush must match shaders/surround.frag");
static_assert(kDrawPushOffset + sizeof(SurroundPush) <= kPushConstantBytes, "push constants");

// shaders/screen.glsl, ScreenDraw.
struct ScreenPush {
  float pose[3][4];  // rows of the screen's pose
  float size[4];     // width, height, distance, radius
  float color[4];
  float adjust[4];  // w: sharpening strength
  uint32_t mode[4];  // y flags, z footprint taps (antiShimmerTaps)
  float yuv[4];      // kYuv: YuvColor
  float yuvMatrix[4];
};
static_assert(sizeof(ScreenPush) == 144 && offsetof(ScreenPush, mode) == 96,
              "ScreenPush must match shaders/screen.glsl");
static_assert(kDrawPushOffset + sizeof(ScreenPush) <= kPushConstantBytes, "push constants");

// shaders/panel.glsl, PanelDraw.
enum PanelFlag : uint32_t {
  kPanelCurved = 1,
  kPanelHover = 2,
  kPanelHoverDanger = 4,
  kPanelGlass = 16,
  kPanelPassthrough = 32,
  // Bits 8..31: the hover rim's colour, sRGB red, green, blue.
};
struct PanelPush {
  float model[3][4];  // rows
  float uv[4];        // u0, u1, v0, v1
  float arc[4];       // curved: angle left and right, height top and bottom
  float hover[4];
  float shape[4];  // radius, pixel scale, hover corner radius, glass alpha
  float opacity;
  float unused;
  uint32_t flags;
  float lodBias;
  float hoverClip[4];  // x0, y0, x1, y1
};
static_assert(sizeof(PanelPush) == 144 && offsetof(PanelPush, flags) == 120,
              "PanelPush must match shaders/panel.glsl");
static_assert(kDrawPushOffset + sizeof(PanelPush) <= kPushConstantBytes, "push constants");

// shaders/pointer.glsl, PointerDraw.
struct PointerPush {
  float corners[4][4];
  float color[4];
  uint32_t mode[4];  // x: a round dot
};
static_assert(sizeof(PointerPush) == 96 && offsetof(PointerPush, mode) == 80,
              "PointerPush must match shaders/pointer.glsl");

// shaders/preview.glsl, PreviewDraw.
enum PreviewFlag : uint32_t {
  kPreviewFlip = 1,
  kPreviewSurround = 2,
};
struct PreviewPush {
  float model[3][4];  // rows
  float picture[4];   // the eye's part: scale xy, offset xy
  float view[4];      // surround: tangents of the half width and height, fisheye field of view
  uint32_t mode[4];   // x projection, y flags
};
static_assert(sizeof(PreviewPush) == 96 && offsetof(PreviewPush, mode) == 80,
              "PreviewPush must match shaders/preview.glsl");

// Records the frame's eye passes into a frame slot's command buffer, with
// GPU timestamps around their parts (docs/vulkan-renderer.md, 2.9). It
// neither submits nor touches the XR frame: main.cpp does, once.
class SceneRenderer {
 public:
  // Timestamps written per frame, in this order.
  enum Timestamp : uint32_t { kStart, kUploaded, kGlassDone, kEye0Done, kEye1Done, kEnd, kCount };
  static_assert(kCount <= GpuContext::kTimestamps, "too many timestamps");

  // Milliseconds of GPU time of a finished frame.
  struct GpuTimes {
    double upload = 0;  // canvas copies and shared image acquires
    double glass = 0;   // the glass backdrop's passes
    double eye[2] = {};
    double mirror = 0;  // shared image releases and the desktop mirror's copy
    double frame = 0;   // all of it
  };

  SceneRenderer() = default;
  SceneRenderer(const SceneRenderer&) = delete;
  SceneRenderer& operator=(const SceneRenderer&) = delete;
  ~SceneRenderer();

  // Makes the pipelines and renders the sky (startup work on the GPU).
  bool init(GpuContext& gpu, GlInterop& interop, Uploader& uploader, EyeTargets& targets);
  // The desktop's glass menus: their blurred backdrop, made for xr's eye images.
  bool initGlass(const XrContext& xr);
  // --xr-test: the eyes are cleared to a slow pulse instead (left reddish,
  // right bluish), whatever the scene.
  void setTestPattern(bool on) { testPattern_ = on; }
  // --menu-sharpness: a mip level bias for the menu's panels (negative is sharper).
  void setMenuLodBias(float bias) { menuLodBias_ = bias; }
  // The desktop mirror: the left eye is copied into it after the eye
  // passes, when it has an image for the frame.
  void setMirror(MirrorWindow* mirror) { mirror_ = mirror; }
  // Frame: decoded frames sampled from their planes (Texture::yuv), Q10C
  // and Q08C ones by the pipelines for them, made here.
  void setDirectVideo(DirectVideo* direct);

  // Records the frame; `matrices`: the eyes' (XrContext::eye), `time`:
  // seconds, for the test pattern. Adds what the submission waits for.
  void record(GpuContext::FrameSlot& slot, const EyeFrame& eyes, const EyeMatrices* matrices,
              const FrameScene& scene, double time, std::vector<SemaphoreWait>& waits);
  // The times of the slot's last frame, once its slot is reused (never
  // waits); false if they are not there.
  bool readTimes(const GpuContext::FrameSlot& slot, GpuTimes& times);

 private:
  void timestamp(GpuContext::FrameSlot& slot, Timestamp t, VkPipelineStageFlagBits stage);
  bool createSky();
  void drawSky(VkCommandBuffer cmd, EyeData eye);
  // sharpen: the strength (VideoDraw::sharpen), 0 off.
  void drawVideo(VkCommandBuffer cmd, EyeData eye, int index, const VideoDraw& video,
                 ClearMode clear, float sharpen);
  // The scene's first `count` overlays; backdrop: behind glass panels.
  void drawOverlays(VkCommandBuffer cmd, const EyeData& eye, const EyeMatrices& matrices,
                    const FrameScene& scene, size_t count, const Texture* backdrop);
  void drawPanel(VkCommandBuffer cmd, const PanelItem& panel, ClearMode clear,
                 const Texture* backdrop);
  // The scene's pipeline, or its variant for the glass backdrop's pass.
  VkPipeline pipeline(Pipelines::Id id) const {
    return glassPass_ ? pipelines_.glass(id) : pipelines_.get(id, plainEye_);
  }
  void drawPointer(VkCommandBuffer cmd, const Quad& corners, bool dot);
  void drawPreview(VkCommandBuffer cmd, const PreviewItem& preview);
  void pushEye(VkCommandBuffer cmd, const EyeData& eye);
  template <typename T>
  void pushDraw(VkCommandBuffer cmd, const T& draw);

  GpuContext* gpu_ = nullptr;
  GlInterop* interop_ = nullptr;
  DirectVideo* direct_ = nullptr;
  Uploader* uploader_ = nullptr;
  EyeTargets* targets_ = nullptr;
  Pipelines pipelines_;
  GpuImage skyImage_;
  Texture sky_;
  bool testPattern_ = false;
  float menuLodBias_ = 0.0f;
  MirrorWindow* mirror_ = nullptr;
  std::unique_ptr<GlassBackdrop> glass_;
  bool glassPass_ = false;  // recording the glass backdrop's scene
  bool plainEye_ = false;   // recording an eye pass without a density map (EyeTargets)
};

// An eye's push-constant data for this frame (the whole frame as its part).
EyeData eyeData(const EyeMatrices& eye, const VkRect2D& area, int index);
