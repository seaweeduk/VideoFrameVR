#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <string>

class XrContext;

// How much the eye images are foveated on the Steam Frame
// (docs/vulkan-renderer.md, 5): full detail where you look, coarser shading
// further out. Persisted by name (settings.cpp).
enum class FoveationLevel { Off, Low, Medium, High, Custom };

constexpr int kDefaultSharpBins = 1;
constexpr bool kDefaultFoveationRing = true;

const char* foveationLevelName(FoveationLevel level);
bool parseFoveationLevel(const std::string& name, FoveationLevel& level);
void foveationPreset(FoveationLevel level, int& sharpBins, bool& ring, int& outside);
FoveationLevel foveationLevel(bool enabled, int sharpBins, bool ring, int outside);
// Off, followed by the eight combinations of sharp area, ring and outside.
int foveationMapIndex(bool enabled, int sharpBins, bool ring, int outside);

// Where the sharp area goes: where SteamVR's eye tracking puts it, straight
// ahead, or a fixed point (NDC, +y down) for repeatable headset runs
// (--foveation-gaze).
struct FoveationGaze {
  enum Mode { Tracked, Centre, Fixed } mode = Tracked;
  float x = 0, y = 0;
};
bool parseFoveationGaze(const std::string& text, FoveationGaze& gaze);

// shaders/foveation.frag, only filled while the debug view is on.
struct FoveationDebug {
  float gaze[4];  // gaze in pixels xy
};
static_assert(sizeof(FoveationDebug) == 16, "foveation push constants");

// The foveation of the eye images: the level in use, the app's density maps
// (density_map.h) where XrContext has them, and each frame the maps' offsets
// to SteamVR's foveation centre. The eye images and their density maps
// belong to XrContext; this decides what goes into them. All calls on the
// main thread.
class Foveation {
 public:
  // After XrContext::createEyeImages. Without foveated eye images (the
  // desktop, or the runtime or GPU lacks it) everything here does nothing.
  void init(XrContext& xr, FoveationLevel level, const FoveationGaze& gaze, int sharpBins, bool ring, int outside);
  bool active() const { return xr_ != nullptr; }
  // The level asked for, applied from the next prepare() (at a frame boundary).
  void setLevel(FoveationLevel level) { requested_ = level; }
  FoveationLevel level() const { return applied_; }
  void setShape(int sharpBins, bool ring, int outside) { sharpBins_ = sharpBins; ring_ = ring; outside_ = outside; }
  std::array<int, 3> binDegrees() const { return binDegrees_; }  // 1-bin width, 3-bin width, height
  const std::array<int, 9>& equivalentMaps() const { return equivalentMaps_; }

  // The eye size changed (XrContext::updateEyeSize): the maps are drawn
  // again for it by the next prepare().
  void resized() { resized_ = true; }
  // Between frames, with no frame being recorded (before
  // GpuContext::beginFrame): applies a changed level or size to both eyes.
  void prepare();
  // Once per drawn frame, after XrContext::beginFrame and before
  // acquireEyes(): asks SteamVR for the latest pattern, reads the foveation
  // centres and sets each eye's density map offset.
  void update();

  // For --timing, since the last call: "level=high map=app grid=... sharp=...
  // gaze_valid=100% ..." with the calls' CPU time and failures.
  std::string report();
  FoveationDebug debug(int eye) const;
  void sharpDegrees(float& width, float& height) const;
  // What a drawn frame's shading rates depend on (frame_reuse.h): the maps,
  // another number once prepare() changed them, the offset update() last
  // gave an eye's map, and its full-rate area's size in pixels (0 x 0 with
  // SteamVR's maps: not known).
  uint64_t mapsVersion() const { return mapsVersion_; }
  VkOffset2D offset(int eye) const { return offset_[eye]; }
  VkExtent2D sharpSize(int eye) const { return sharp_[eye].extent; }

 private:
  // Applies `level`: redraws the app's maps, or changes SteamVR's profile.
  bool apply(FoveationLevel level);
  void compareMaps();

  XrContext* xr_ = nullptr;
  FoveationLevel requested_ = FoveationLevel::High, applied_ = FoveationLevel::Off;
  bool resized_ = false;
  int sharpBins_ = kDefaultSharpBins, drawnSharpBins_ = kDefaultSharpBins;
  bool ring_ = kDefaultFoveationRing, drawnRing_ = kDefaultFoveationRing;
  int outside_ = 4, drawnOutside_ = 4;
  std::array<int, 9> equivalentMaps_{{0, 1, 2, 3, 4, 5, 6, 7, 8}};
  uint32_t bins_[2][3] = {};
  std::array<int, 3> binDegrees_{};
  // Where each eye's map puts the gaze (its offsets move that point to the
  // gaze) and its full-rate area, in pixels; the image centre and unknown
  // with SteamVR's maps.
  VkOffset2D anchor_[2] = {};
  VkRect2D sharp_[2] = {};
  VkOffset2D offset_[2] = {};  // the last offsets set
  uint64_t mapsVersion_ = 0;
  FoveationGaze gaze_;
  VkExtent2D granularity_{1, 1};
  // Since the last report().
  int frames_ = 0, valid_ = 0, failures_ = 0, malformed_ = 0;
  double cpuMs_ = 0, cpuMaxMs_ = 0;
  float centre_[2][2] = {};  // the last centres used, NDC
};
