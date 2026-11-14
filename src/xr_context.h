#pragma once

#include "controller_guide.h"
#include "density_map.h"
#include "foveation.h"
#include "gpu_context.h"
#include "tile_grid.h"
#include "xmath.h"

#include <vulkan/vulkan.h>
#include <openxr/openxr.h>

#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <utility>
#include <vector>

struct EyeMatrices {
  Mat4 projection;
  Mat4 view;
  // The same view as raw orientation + field-of-view tangents, for shaders
  // that compute a view ray per pixel.
  Quat orientation;
  Vec3 position;
  float tanLeft = 0, tanRight = 0, tanUp = 0, tanDown = 0;
};

struct HandState {
  bool poseValid = false;
  Vec3 aimPosition;      // pointing ray origin, local space
  Quat aimOrientation;   // the ray points along -Z of this orientation
  bool select = false;          // trigger held
  bool selectPressed = false;   // trigger went down this frame
  bool grip = false;            // grip button held
  bool gripPressed = false;     // grip went down this frame
  float stickX = 0, stickY = 0;
};

// Eye gaze from XR_EXT_eye_gaze_interaction (Steam Frame), updated by beginFrame().
struct GazeState {
  bool available = false;  // the runtime offers it and the system supports it
  bool active = false;     // the input is bound and delivering (permission, tracker on)
  bool valid = false;      // a pose this frame
  bool tracked = false;    // ... measured, not estimated
  Vec3 origin;             // local space
  Quat orientation;        // the gaze looks along -Z of this orientation
  bool ageKnown = false;
  double ageSeconds = 0;   // sample time before the frame's display time (negative: predicted)
  double sampleSeconds = 0;  // the sample time itself (XrTime in seconds)
};

struct ControllerState {
  // The app has input focus (not Steam's dashboard or other system UI).
  // Without it, or until every button and stick is let go after it comes
  // back, the hands press nothing; without it, they have no pose either.
  bool focused = false;
  HandState hands[2];  // 0 = left, 1 = right
  bool menuPressed = false;
  bool playPausePressed = false;
  bool recenterPressed = false;
};

// One eye's swapchain image of this frame, acquired and waited for.
struct EyeImage {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  uint32_t index = 0;  // in the eye's swapchain
  int width = 0, height = 0;
  VkRect2D area{};  // what is drawn and submitted: the size in use, at the origin
  // Foveated eye images: the density map drawn with (none while foveation
  // is off), and where its sharp area is moved to, in pixels
  // (XrContext::setDensityOffset).
  VkImageView densityMap = VK_NULL_HANDLE;
  VkOffset2D densityOffset{};
};

// The eye images of a frame (XrContext::acquireEyes), until releaseEyes().
struct EyeFrame {
  static constexpr int kMaxEyes = 2;
  int count = 0;
  EyeImage eyes[kMaxEyes];
};

// Owns the OpenXR instance, session, swapchains and reference spaces, and the
// Vulkan instance and device, made through the runtime (GpuContext uses them).
//
// SteamVR on Linux only composites Vulkan images reliably, so the session uses
// Vulkan.
class XrContext {
 public:
  ~XrContext();

  // Diagnostic (--fixed-view): the eyes are located and submitted relative to
  // the head, so the scene stays in front of the view wherever the headset
  // points; headPose() is then the origin, looking along -Z. Set before init().
  void setHeadLocked(bool locked) { headLocked_ = locked; }

  // Before init(): log refresh rates at session start; optionally request Hz
  // for this run (-1: query only, 0: no preference).
  void setRefreshRateDiagnostic(float hz = -1) { refreshDiagnostic_ = true; refreshRequest_ = hz; }

  // Starts OpenXR and makes the Vulkan device with what `gpu` asks for, up to
  // the eye images, which createEyeImages() makes once their size is chosen.
  // Foveation is enabled only if the runtime and the GPU offer all of it.
  bool init(const GpuRequirements& gpu);
  // The Vulkan device and what it was made with.
  const GpuDevice& gpuDevice() const { return gpu_; }
  // The runtime's recommended eye image size (of the first eye).
  int recommendedEyeWidth() const;
  int recommendedEyeHeight() const;
  // The OpenXR system's name, as the runtime reports it.
  const std::string& systemName() const { return systemName_; }
  // The eye images, at videoScale times baseWidth × baseHeight per eye
  // (0 × 0: the runtime's recommended size). They keep their shape and are
  // lowered to fit the runtime's maximum and the GPU's limits. Sizes up to
  // capScale are drawn in images of that scale (requestEyeScale). `gpu`
  // retires what a size change replaces.
  bool createEyeImages(GpuContext& gpu, int baseWidth, int baseHeight, float videoScale, float capScale);
  // A new eye size, as a scale like createEyeImages', from the next
  // updateEyeSize() on (docs/vulkan-renderer.md, 5.5): at once where the
  // images hold it, drawn in a rectangle at their origin; otherwise once a
  // worker thread has made images of that size (or, coming back from above
  // the cap, of the cap's), the old size drawn meanwhile.
  void requestEyeScale(float scale);
  // The scale asked for, lowered to what fits the runtime and the GPU.
  float requestedEyeScale() const { return requestedScale_; }
  // Between frames (no image acquired, no frame being recorded): applies
  // what can be of the size asked for. True if the size in use or the
  // images changed; their density maps then need drawing again
  // (drawDensityMaps).
  bool updateEyeSize();
  // Bumped whenever the eye images, the size in use or the density maps
  // change: eye framebuffers made before are stale (EyeTargets).
  uint64_t eyeTargetsVersion() const { return targetsVersion_; }
  // The eye images' format (sRGB: shaders output linear colour) and, per
  // eye, the size in use, the images' own size and a view of each image.
  VkFormat eyeFormat() const { return static_cast<VkFormat>(swapchainFormat_); }
  int eyeWidth(int eye) const { return swapchains_[eye].width; }
  int eyeHeight(int eye) const { return swapchains_[eye].height; }
  int eyeImageWidth(int eye) const { return swapchains_[eye].imageWidth; }
  int eyeImageHeight(int eye) const { return swapchains_[eye].imageHeight; }
  const std::vector<VkImageView>& eyeViews(int eye) const { return swapchains_[eye].views; }

  // Foveation (docs/vulkan-renderer.md, 5): the eye images have the
  // runtime's density maps (XR_FB_foveation_vulkan), one per swapchain image,
  // and a foveation profile for every level. False on the desktop, or where
  // the runtime or the GPU lacks any of it.
  bool foveated() const { return foveated_; }
  // Before createEyeImages(): draw with the app's own density maps, one per
  // eye (density_map.h), where the GPU's bins are known (tile_grid.h); else,
  // or if false, with the runtime's.
  void preferAppDensityMaps(bool prefer) { preferAppMaps_ = prefer; }
  bool appDensityMaps() const { return appMaps_; }
  // The bins of eye 0's pass, where known.
  const TileGrid& tileGrid() const { return tileGrid_; }
  // The views drawn with, by swapchain image.
  const std::vector<VkImageView>& densityMapViews(int eye) const {
    return appMaps_ ? swapchains_[eye].appMap.views : swapchains_[eye].densityViews;
  }
  VkExtent2D densityMapSize(int eye) const {
    return appMaps_ ? swapchains_[eye].appMap.pixels.size : swapchains_[eye].densitySize;
  }
  // App maps: new ones for the size in use, which `draw` draws for each eye,
  // used from the next frame on; the old ones are retired. Between frames,
  // as updateEyeSize().
  bool drawDensityMaps(const std::function<void(int eye, const DensityMapPixels&)>& draw);
  // Both eyes to `level`'s profile, from the next frame on. On a failure
  // both keep the one they had (or go unfoveated if even that fails);
  // foveationLevel() is what they have. With app maps the profile only
  // keeps the runtime's eye-tracked centres coming: it stays High.
  bool setFoveationLevel(FoveationLevel level);
  FoveationLevel foveationLevel() const { return foveationLevel_; }
  // Asks the runtime for the latest pattern of the profile in use
  // (xrUpdateSwapchainFB), right before foveationCentres().
  bool refreshFoveation();
  // Each eye's foveation centre in NDC (+y down), as the runtime reports it
  // (XR_META_foveation_eye_tracked); valid: its gaze data is current.
  bool foveationCentres(float centres[2][2], bool& valid);
  // Where eye `eye`'s sharp area goes in the next acquired image, in pixels
  // from where the density map has it.
  void setDensityOffset(int eye, VkOffset2D offset) { swapchains_[eye].densityOffset = offset; }
  // Whether the next acquired images are drawn with their density maps;
  // without (foveation Off), in a pass that has none (EyeTargets).
  void useDensityMaps(bool use) { useDensityMaps_ = use; }

  // Handles session state changes. Sets quit when the runtime wants us to exit.
  void pollEvents(bool& quit);
  bool isRunning() const { return running_; }
  // Rendering can continue underneath the runtime's dashboard without input focus.
  bool isFocused() const { return sessionState_ == XR_SESSION_STATE_FOCUSED; }
  bool isVisible() const {
    return sessionState_ == XR_SESSION_STATE_VISIBLE || sessionState_ == XR_SESSION_STATE_FOCUSED;
  }
  // Whether someone wears the headset (XR_EXT_user_presence): 1 yes, 0 no,
  // -1 not known (the runtime or the headset can't tell, or hasn't yet).
  int userPresence() const { return userPresence_; }
  // The session or instance was lost, or the runtime hung: the app stops.
  bool failed() const { return failed_; }

  // Waits for and begins the next frame. Returns true if the eyes should be drawn.
  bool beginFrame();
  // Acquires and waits for both eye images, as late as possible before
  // drawing them; nullptr if they cannot be had (the frame then ends
  // without them).
  const EyeFrame* acquireEyes();
  // Gives the images back to the runtime, once all GPU work on them is
  // submitted on the queue.
  void releaseEyes();
  // Ends the frame begun, with the eyes if `drawn` (released after a
  // successful submission), else without any layer.
  void endFrame(bool drawn);
  // Whether the runtime still holds the eye images of the frame before, to
  // show again: that frame ended with them, and nothing replaced or acquired
  // them since.
  bool canShowAgain() const;
  // Ends the frame begun with the eye images of the frame before, as they
  // were drawn (their poses, fields of view and areas): the runtime
  // reprojects them to where the head is now. Only while canShowAgain().
  void endFrameShownAgain();

  // Native passthrough: the eye images' alpha blends them over the camera
  // (XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND), if the runtime offers it.
  bool alphaBlendSupported() const { return alphaBlend_; }
  // For the next endFrame(); only while alphaBlendSupported().
  void setAlphaBlend(bool on) { useAlphaBlend_ = on && alphaBlend_; }

  int eyeCount() const { return static_cast<int>(eyes_.size()); }
  const EyeMatrices& eye(int i) const { return eyes_[i]; }
  // Eye image pixels per degree straight ahead, across or (vertical) up.
  float eyePixelsPerDegree(int i, bool vertical) const;

  // Head pose in the local reference space for the frame begun. Position uses
  // the current stereo midpoint; orientation prefers VIEW over the eye average.
  // Each component falls back independently when its preferred source is invalid.
  bool headPose(Vec3& position, Quat& orientation) const;
  // The current frame's predicted display time, in seconds.
  double displayTime() const { return frameState_.predictedDisplayTime * 1e-9; }
  // The runtime's frame period in seconds, 0 before the first frame.
  double displayPeriod() const { return frameState_.predictedDisplayPeriod * 1e-9; }

  // Controller input, updated by beginFrame().
  const ControllerState& controllers() const { return controllers_; }
  const GazeState& gaze() const { return gaze_; }
  // Whether Menu, PlayPause or Recenter is held on a hand (0 left, 1 right),
  // read from the runtime on each call (controllers() has their presses).
  bool held(ControlAction action, int hand) const;
  // Each hand's interaction profile and what the actions are bound to, read
  // from the runtime again only after it reported a change, which bumps
  // bindingsVersion(). A hand without a controller gets the other hand's
  // profile (none on: on the Frame, its controllers') and the app's
  // suggested bindings on it.
  const ControllerBindings& controllerBindings();
  uint64_t bindingsVersion() const { return bindingsVersion_; }
  // A short pulse on a hand's controller (0 left, 1 right, -1 both), while
  // the app has input focus. 0 seconds is SteamVR's single impulse, its
  // length set by the amplitude (about 21 ms at 0.16 on the Frame);
  // frequency 0 leaves it to the runtime.
  void vibrate(int hand, float amplitude, double seconds, float frequency = 0);

 private:
  // The app's density map, host-written, linear, for all of an eye's images.
  struct AppMap {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    bool coherent = true;
    DensityMapPixels pixels;
    std::vector<VkImageView> views;  // `view` once per swapchain image
  };
  struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    int width = 0, height = 0;            // in use
    int imageWidth = 0, imageHeight = 0;  // of the images
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkImageView> densityViews;  // foveated: of each image's density map
    VkExtent2D densitySize{};
    AppMap appMap;
    VkOffset2D densityOffset{};
    VkRect2D area{};
    uint32_t acquired = 0;
    // This frame's image: acquired, then successfully waited for.
    bool isAcquired = false, isWaited = false;
  };

  bool createActions();
  void suggestBindings(const char* profile,
                       const std::vector<std::pair<XrAction, const char*>>& bindings,
                       const void* next = nullptr);
  void updateControllers(bool focused);
  void logInteractionProfiles();
  void updateGaze();
  bool createVulkanDevice(const GpuRequirements& gpu);
  bool queryViewConfiguration();
  void queryBlendModes();
  // Records a lost session or instance (failed()).
  bool checkFrame(XrResult result, const char* what);
  // xrEndFrame for the frame begun: with projViews_ as its layer, or none.
  void submitFrame(bool layerReady);
  // The largest scale up to `scale` whose images fit the runtime and the GPU.
  float fittingScale(float scale, int baseWidth, int baseHeight, bool& runtimeLimited) const;
  int scaledSize(int base, int recommended, float scale) const;
  bool createSwapchain(int width, int height, bool foveated, Swapchain& out) const;
  void destroySwapchain(Swapchain& sc) const;
  // After their last use on the GPU: their views and maps at once, the
  // swapchains on a worker thread.
  void retireSwapchains(std::vector<Swapchain> old);
  void destroyAppMap(AppMap& map) const;
  bool createAppDensityMap(const Swapchain& sc, AppMap& out);
  // Whether images made at scale `images` hold the size at `size`, on every eye.
  bool holds(float images, float size) const;
  // The images' scale for a request, given the images' scale now.
  float imageScaleFor(float request, float images) const;
  // Makes images of `scale` on a worker thread (making_).
  void startMakingImages(float scale);
  // Sets the size in use to `scale` of the base, on every eye, within the
  // images; true if it changed.
  bool useScale(float scale);
  bool createFoveationProfiles();
  bool applyFoveationProfile(XrSwapchain swapchain, XrFoveationProfileFB profile) const;
  void logFieldOfView();
  void diagnoseRefreshRate();
  bool check(XrResult result, const char* what) const;

  XrInstance instance_ = XR_NULL_HANDLE;
  XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
  XrSession session_ = XR_NULL_HANDLE;
  XrSpace localSpace_ = XR_NULL_HANDLE;
  XrSpace viewSpace_ = XR_NULL_HANDLE;
  XrActionSet actionSet_ = XR_NULL_HANDLE;
  XrAction aimAction_ = XR_NULL_HANDLE;
  XrAction selectAction_ = XR_NULL_HANDLE;
  XrAction gripAction_ = XR_NULL_HANDLE;
  XrAction menuAction_ = XR_NULL_HANDLE;
  XrAction playPauseAction_ = XR_NULL_HANDLE;
  XrAction recenterAction_ = XR_NULL_HANDLE;
  XrAction stickAction_ = XR_NULL_HANDLE;
  XrAction hapticAction_ = XR_NULL_HANDLE;
  XrPath handPaths_[2] = {XR_NULL_PATH, XR_NULL_PATH};
  XrSpace aimSpaces_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  XrPath handProfiles_[2] = {XR_NULL_PATH, XR_NULL_PATH};  // each hand's, as last logged
  ControllerState controllers_;
  ControllerBindings bindings_;
  uint64_t bindingsVersion_ = 1, bindingsRead_ = 0;
  std::vector<std::pair<std::string, std::pair<XrAction, std::string>>> suggested_;  // profile, binding
  bool inputHeld_ = true;  // since focus came back, something is still held
  bool gazeExtension_ = false;  // XR_EXT_eye_gaze_interaction enabled
  bool frameControllerExtension_ = false;  // XR_VALVE_frame_controller_interaction enabled
  bool triggerThreshold_ = false;  // ... and XR_VALVE_analog_threshold, for its trigger
  bool presenceExtension_ = false;  // XR_EXT_user_presence enabled
  int userPresence_ = -1;
  XrAction gazeAction_ = XR_NULL_HANDLE;
  XrSpace gazeSpace_ = XR_NULL_HANDLE;
  GazeState gaze_;
  XrFrameState frameState_{XR_TYPE_FRAME_STATE};
  XrSessionState sessionState_ = XR_SESSION_STATE_UNKNOWN;
  bool running_ = false;
  bool failed_ = false;
  // Native SteamVR launch only: one dismissal before first focus, never rearmed.
  bool launchDashboardPending_ = false;
  bool sceneSubmitted_ = false;
  bool layerHeld_ = false;  // the last frame ended with the eyes, still the swapchains' newest
  std::future<void> launchDashboard_;
  bool frameBegun_ = false;
  bool viewsValid_ = false;  // this frame's eyes are located and should be drawn
  XrViewStateFlags viewStateFlags_ = 0;  // current locate only, even if partly valid
  bool alphaBlend_ = false, useAlphaBlend_ = false;
  bool headLocked_ = false;
  bool refreshDiagnostic_ = false, refreshExtension_ = false;
  float refreshRequest_ = -1;
  // Foveation: the runtime's extensions are enabled, the eye images have
  // density maps, the profiles by FoveationLevel.
  bool foveationExtensions_ = false;
  bool foveated_ = false;
  XrFoveationProfileFB profiles_[5] = {};
  FoveationLevel foveationLevel_ = FoveationLevel::Off;
  bool useDensityMaps_ = true;
  bool preferAppMaps_ = false, appMaps_ = false;
  TileGrid tileGrid_;
  PFN_xrCreateFoveationProfileFB createFoveationProfile_ = nullptr;
  PFN_xrDestroyFoveationProfileFB destroyFoveationProfile_ = nullptr;
  PFN_xrUpdateSwapchainFB updateSwapchain_ = nullptr;
  PFN_xrGetFoveationEyeTrackedStateMETA getFoveationState_ = nullptr;
  // Eye sizes, as scales of the base size: in use, of the images, asked for
  // (fitted), and the cap below which larger images are made at the cap.
  GpuContext* context_ = nullptr;
  int baseWidth_ = 0, baseHeight_ = 0;
  float activeScale_ = 1.0f, imageScale_ = 1.0f, requestedScale_ = 1.0f, capScale_ = 1.0f;
  uint64_t targetsVersion_ = 0;
  // Images being made on a worker, and old ones being destroyed.
  std::future<std::vector<Swapchain>> making_;
  float makingScale_ = 0;
  std::chrono::steady_clock::time_point makingSince_;
  std::vector<std::future<void>> unmaking_;
  std::string systemName_;
  std::vector<XrViewConfigurationView> configViews_;
  int64_t swapchainFormat_ = 0;
  uint32_t imageLimit_ = 0;  // the GPU's largest image side

  GpuDevice gpu_;
  EyeFrame eyeFrame_;
  std::vector<Swapchain> swapchains_;
  std::vector<XrView> views_;
  std::vector<XrCompositionLayerProjectionView> projViews_;
  std::vector<EyeMatrices> eyes_;
  std::vector<XrFovf> loggedFov_;
};
