#pragma once

#include "controller_state.h"
#include "controls.h"
#include "eye_state.h"
#include "pointer_filter.h"

#include <openxr/openxr.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// Controller and eye input through OpenXR actions: the controls, aim poses,
// thumbsticks and haptics of both hands, and the eye gaze. XrContext makes it
// with its session and reads it each frame (XrContext::input()).
class XrInput {
 public:
  // `sessionState`: the session's, as XrContext keeps it.
  explicit XrInput(const XrSessionState& sessionState) : sessionState_(sessionState) {}
  XrInput(const XrInput&) = delete;
  XrInput& operator=(const XrInput&) = delete;

  // Before the instance: adds the input extensions the runtime offers to
  // `enabledExts`.
  void enableExtensions(const std::function<bool(const char*)>& offered,
                        std::vector<const char*>& enabledExts);
  bool gazeExtension() const { return gazeExtension_; }
  // From the system's properties: whether the headset tracks the eyes.
  void setGazeSupported(bool supported) { gaze_.available = gazeExtension_ && supported; }
  // What of it the runtime and the headset offer, logged at start.
  void logSupport() const;
  // On XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED.
  void readInteractionProfiles();

  // Controller input, updated by XrContext::beginFrame().
  const ControllerState& controllers() const { return controllers_; }
  const GazeState& gaze() const { return gaze_; }
  // Each hand's interaction profile ("" none), read from the runtime again
  // after it reported a change, which bumps profilesVersion().
  const std::string& interactionProfile(int hand) const { return handProfileNames_[hand]; }
  uint64_t profilesVersion() const { return profilesVersion_; }
  // ... as the controllers' pictures want them: a hand without a controller
  // gets the other hand's (none on: on the Frame, its controllers').
  void pairedProfiles(std::string out[2]) const;
  // A short pulse on a hand's controller (0 left, 1 right, -1 both), while
  // the app has input focus. 0 seconds is SteamVR's single impulse, its
  // length set by the amplitude (about 21 ms at 0.16 on the Frame);
  // frequency 0 leaves it to the runtime.
  void vibrate(int hand, float amplitude, double seconds, float frequency = 0);

 private:
  friend class XrContext;  // its handles live and die with XrContext's session
  // Once the session and its local space exist: the actions, their bindings
  // and spaces.
  bool create(XrInstance instance, XrSession session, XrSpace localSpace);
  // Before the session and the instance are destroyed.
  void destroy();
  // After xrBeginFrame: syncs the actions and reads the controllers and the
  // gaze, located at `time`.
  void sync(XrTime time);

  bool isFocused() const { return sessionState_ == XR_SESSION_STATE_FOCUSED; }
  bool check(XrResult result, const char* what) const;
  void suggestBindings(const char* profile,
                       const std::vector<std::pair<XrAction, std::string>>& bindings,
                       const void* next = nullptr);
  void updateControllers(bool focused, XrTime time);
  void updateGaze(XrTime time);

  const XrSessionState& sessionState_;
  XrInstance instance_ = XR_NULL_HANDLE;
  XrSession session_ = XR_NULL_HANDLE;
  XrSpace localSpace_ = XR_NULL_HANDLE;
  XrActionSet actionSet_ = XR_NULL_HANDLE;
  XrAction aimAction_ = XR_NULL_HANDLE;
  XrAction controlActions_[kControls] = {};  // by Control
  XrAction stickAction_ = XR_NULL_HANDLE;
  XrAction hapticAction_ = XR_NULL_HANDLE;
  XrPath handPaths_[2] = {XR_NULL_PATH, XR_NULL_PATH};
  XrSpace aimSpaces_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  XrPath handProfiles_[2] = {XR_NULL_PATH, XR_NULL_PATH};  // each hand's, as last read
  std::string handProfileNames_[2];
  uint64_t profilesVersion_ = 1;
  ControllerState controllers_;
  PointerFilter pointFilters_[2];  // each hand's pointDir
  bool inputHeld_ = true;  // since focus came back, something is still held
  bool gazeExtension_ = false;  // XR_EXT_eye_gaze_interaction enabled
  bool frameControllerExtension_ = false;  // XR_VALVE_frame_controller_interaction enabled
  bool triggerThreshold_ = false;  // ... and XR_VALVE_analog_threshold, for its trigger
  XrAction gazeAction_ = XR_NULL_HANDLE;
  XrSpace gazeSpace_ = XR_NULL_HANDLE;
  GazeState gaze_;
};
