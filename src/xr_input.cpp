#include "xr_input.h"

#include "platform.h"
#include "vr_input.h"
#include "xr_util.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// Defines /interaction_profiles/valve/frame_controller_valve; not in the
// Khronos headers. SteamVR offers it on the Frame and on the desktop.
const char* const kFrameControllerExtension = "XR_VALVE_frame_controller_interaction";

}  // namespace

bool XrInput::check(XrResult result, const char* what) const {
  return xrCheck(instance_, result, what);
}

void XrInput::enableExtensions(const std::function<bool(const char*)>& offered,
                               std::vector<const char*>& enabledExts) {
  // Eye gaze is optional: enabled only where the runtime offers it.
  gazeExtension_ = offered(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
  if (gazeExtension_) enabledExts.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
  frameControllerExtension_ = offered(kFrameControllerExtension);
  if (frameControllerExtension_) enabledExts.push_back(kFrameControllerExtension);
  triggerThreshold_ = frameControllerExtension_ &&
                      offered(XR_KHR_BINDING_MODIFICATION_EXTENSION_NAME) &&
                      offered(XR_VALVE_ANALOG_THRESHOLD_EXTENSION_NAME);
  if (triggerThreshold_) {
    enabledExts.push_back(XR_KHR_BINDING_MODIFICATION_EXTENSION_NAME);
    enabledExts.push_back(XR_VALVE_ANALOG_THRESHOLD_EXTENSION_NAME);
  }
}

void XrInput::logSupport() const {
  std::printf("Eye tracking: %s\n", gaze_.available  ? "available (XR_EXT_eye_gaze_interaction)"
                                    : gazeExtension_ ? "not supported by this headset"
                                                     : "not offered by the OpenXR runtime");
  if (frameControllerExtension_)
    std::printf("Controllers: %s enabled%s\n", kFrameControllerExtension,
                triggerThreshold_ ? ", trigger clicks at 0.7 (XR_VALVE_analog_threshold)" : "");
}

// One action per physical control (controls.h), each bound to itself in
// every controller type's profile; what a control does is the app's
// (ControlMap). The Steam Frame controllers use their own profile where
// SteamVR offers it, else SteamVR maps the Touch one onto them.
bool XrInput::create(XrInstance instance, XrSession session, XrSpace localSpace) {
  instance_ = instance;
  session_ = session;
  localSpace_ = localSpace;
  XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strncpy(setInfo.actionSetName, "player", XR_MAX_ACTION_SET_NAME_SIZE - 1);
  std::strncpy(setInfo.localizedActionSetName, "Player", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
  XR_TRY(xrCreateActionSet(instance_, &setInfo, &actionSet_));

  XR_TRY(xrStringToPath(instance_, "/user/hand/left", &handPaths_[0]));
  XR_TRY(xrStringToPath(instance_, "/user/hand/right", &handPaths_[1]));

  auto createAction = [&](XrActionType type, const char* name, const char* localized,
                          XrAction& out) {
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    info.actionType = type;
    std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(info.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    info.countSubactionPaths = 2;
    info.subactionPaths = handPaths_;
    return check(xrCreateAction(actionSet_, &info, &out), name);
  };
  if (!createAction(XR_ACTION_TYPE_POSE_INPUT, "aim", "Pointer", aimAction_) ||
      !createAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick", "Thumbstick", stickAction_) ||
      !createAction(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Vibration", hapticAction_))
    return false;
  for (int c = 0; c < kControls; ++c) {
    const ControlInfo& info = controlInfo(static_cast<Control>(c));
    if (!createAction(XR_ACTION_TYPE_BOOLEAN_INPUT, info.id, info.name, controlActions_[c]))
      return false;
  }

  for (int t = 0; t < kControllerTypes; ++t) {
    const ControllerType type = static_cast<ControllerType>(t);
    if (type == ControllerType::Frame && !frameControllerExtension_) continue;
    const ControllerLayout& layout = controllerLayout(type);
    std::vector<std::pair<XrAction, std::string>> bindings;
    for (const char* hand : {"/user/hand/left", "/user/hand/right"}) {
      bindings.push_back({aimAction_, std::string(hand) + "/input/aim/pose"});
      bindings.push_back({hapticAction_, std::string(hand) + "/output/haptic"});
      if (layout.stick) bindings.push_back({stickAction_, std::string(hand) + "/input/thumbstick"});
    }
    for (const LayoutControl& c : layout.controls)
      bindings.push_back({controlActions_[static_cast<int>(c.control)],
                          std::string(c.hand ? "/user/hand/right" : "/user/hand/left") + c.input});
    if (type != ControllerType::Frame || !triggerThreshold_) {
      suggestBindings(layout.profile, bindings);
      continue;
    }
    // The trigger clicks at 0.7 and lets go at 0.65, as it did through
    // SteamVR's Touch remapping.
    XrInteractionProfileAnalogThresholdVALVE thresholds[2];
    const XrBindingModificationBaseHeaderKHR* modifications[2];
    for (int h = 0; h < 2; ++h) {
      thresholds[h] = {XR_TYPE_INTERACTION_PROFILE_ANALOG_THRESHOLD_VALVE};
      thresholds[h].action = controlActions_[static_cast<int>(Control::Trigger)];
      XR_TRY(xrStringToPath(instance_, h ? "/user/hand/right/input/trigger/value"
                                         : "/user/hand/left/input/trigger/value",
                            &thresholds[h].binding));
      thresholds[h].onThreshold = 0.7f;
      thresholds[h].offThreshold = 0.65f;
      modifications[h] = reinterpret_cast<const XrBindingModificationBaseHeaderKHR*>(&thresholds[h]);
    }
    XrBindingModificationsKHR modified{XR_TYPE_BINDING_MODIFICATIONS_KHR};
    modified.bindingModificationCount = 2;
    modified.bindingModifications = modifications;
    suggestBindings(layout.profile, bindings, &modified);
  }

  // One gaze pose, not per hand. A failure only loses eye tracking.
  if (gaze_.available) {
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    info.actionType = XR_ACTION_TYPE_POSE_INPUT;
    std::strncpy(info.actionName, "gaze", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(info.localizedActionName, "Eye gaze", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    spaceInfo.poseInActionSpace.orientation.w = 1.0f;
    if (check(xrCreateAction(actionSet_, &info, &gazeAction_), "gaze")) {
      suggestBindings("/interaction_profiles/ext/eye_gaze_interaction",
                      {{gazeAction_, "/user/eyes_ext/input/gaze_ext/pose"}});
      spaceInfo.action = gazeAction_;
      if (!check(xrCreateActionSpace(session_, &spaceInfo, &gazeSpace_), "gaze space"))
        gazeSpace_ = XR_NULL_HANDLE;
    }
    if (gazeSpace_ == XR_NULL_HANDLE) gaze_.available = false;
  }

  for (int h = 0; h < 2; ++h) {
    XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    spaceInfo.action = aimAction_;
    spaceInfo.subactionPath = handPaths_[h];
    spaceInfo.poseInActionSpace.orientation.w = 1.0f;
    XR_TRY(xrCreateActionSpace(session_, &spaceInfo, &aimSpaces_[h]));
  }

  XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  attachInfo.countActionSets = 1;
  attachInfo.actionSets = &actionSet_;
  XR_TRY(xrAttachSessionActionSets(session_, &attachInfo));
  return true;
}

// A rejected profile only loses that controller type, so failures just warn.
void XrInput::suggestBindings(const char* profile,
                              const std::vector<std::pair<XrAction, std::string>>& bindings,
                              const void* next) {
  XrPath profilePath;
  if (!check(xrStringToPath(instance_, profile, &profilePath), profile)) return;
  std::vector<XrActionSuggestedBinding> suggested;
  for (const auto& [action, path] : bindings) {
    XrPath binding;
    if (!check(xrStringToPath(instance_, path.c_str(), &binding), path.c_str())) return;
    suggested.push_back({action, binding});
  }
  XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
  info.next = next;
  info.interactionProfile = profilePath;
  info.countSuggestedBindings = static_cast<uint32_t>(suggested.size());
  info.suggestedBindings = suggested.data();
  check(xrSuggestInteractionProfileBindings(instance_, &info), profile);
}

void XrInput::destroy() {
  for (XrSpace space : aimSpaces_)
    if (space != XR_NULL_HANDLE) xrDestroySpace(space);
  if (gazeSpace_ != XR_NULL_HANDLE) xrDestroySpace(gazeSpace_);
  if (actionSet_ != XR_NULL_HANDLE) xrDestroyActionSet(actionSet_);
}

void XrInput::sync(XrTime time) {
  // Returns XR_SESSION_NOT_FOCUSED (a success code) while unfocused.
  const XrActiveActionSet activeSet{actionSet_, XR_NULL_PATH};
  XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
  syncInfo.countActiveActionSets = 1;
  syncInfo.activeActionSets = &activeSet;
  const XrResult synced = xrSyncActions(session_, &syncInfo);
  check(synced, "xrSyncActions");
  const bool focused = synced == XR_SUCCESS && isFocused();
  if (focused != controllers_.focused) {
    char sync[XR_MAX_RESULT_STRING_SIZE] = "";
    xrResultToString(instance_, synced, sync);
    std::printf("OpenXR: input focus %s (session state %d, xrSyncActions %s)\n",
                focused ? "on" : "off", static_cast<int>(sessionState_), sync);
  }
  updateControllers(focused, time);
  updateGaze(time);
}

void XrInput::updateControllers(bool focused, XrTime time) {
  if (!focused) {
    const uint32_t losses = controllers_.focusLosses + (controllers_.focused ? 1 : 0);
    controllers_ = {};
    controllers_.focusLosses = losses;
    inputHeld_ = true;
    for (PointerFilter& f : pointFilters_) f.reset();
    return;
  }
  bool held = false;
  for (int h = 0; h < 2; ++h) {
    HandState& hand = controllers_.hands[h];
    hand.controls = hand.pressed = 0;
    hand.present = false;
    for (int c = 0; c < kControls; ++c) {
      XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
      info.action = controlActions_[c];
      info.subactionPath = handPaths_[h];
      XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
      if (XR_FAILED(xrGetActionStateBoolean(session_, &info, &state)) || !state.isActive) continue;
      hand.present = true;
      if (!state.currentState) continue;
      hand.controls |= 1u << c;
      if (state.changedSinceLastSync) hand.pressed |= 1u << c;
    }
    const uint32_t trigger = controlBit(Control::Trigger), grip = controlBit(Control::Grip);
    hand.select = hand.controls & trigger;
    hand.selectPressed = hand.pressed & trigger;
    hand.grip = hand.controls & grip;
    hand.gripPressed = hand.pressed & grip;

    XrActionStateGetInfo stickInfo{XR_TYPE_ACTION_STATE_GET_INFO};
    stickInfo.action = stickAction_;
    stickInfo.subactionPath = handPaths_[h];
    XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
    xrGetActionStateVector2f(session_, &stickInfo, &stick);
    hand.stickX = stick.isActive ? stick.currentState.x : 0.0f;
    hand.stickY = stick.isActive ? stick.currentState.y : 0.0f;

    XrActionStateGetInfo aimInfo{XR_TYPE_ACTION_STATE_GET_INFO};
    aimInfo.action = aimAction_;
    aimInfo.subactionPath = handPaths_[h];
    XrActionStatePose aim{XR_TYPE_ACTION_STATE_POSE};
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    const XrSpaceLocationFlags needed =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    hand.poseValid =
        XR_SUCCEEDED(xrGetActionStatePose(session_, &aimInfo, &aim)) && aim.isActive &&
        XR_SUCCEEDED(xrLocateSpace(aimSpaces_[h], localSpace_, time, &location)) &&
        (location.locationFlags & needed) == needed;
    if (hand.poseValid) {
      hand.aimPosition = toVec3(location.pose.position);
      hand.aimOrientation = toQuat(location.pose.orientation);
      hand.pointDir = pointFilters_[h].update(aimDirection(hand), time * 1e-9);
    } else {
      pointFilters_[h].reset();
    }
    held = held || hand.controls || std::abs(hand.stickX) > 0.15f || std::abs(hand.stickY) > 0.15f;
  }
  controllers_.focused = true;

  // Back from the dashboard: what was held over it presses nothing here
  // until all of it is let go, and no gesture starts from it.
  if (!inputHeld_) return;
  inputHeld_ = held;
  if (!held) return;
  for (HandState& hand : controllers_.hands) {
    hand.select = hand.selectPressed = hand.grip = hand.gripPressed = false;
    hand.controls = hand.pressed = 0;
    hand.stickX = hand.stickY = 0;
  }
}

void XrInput::vibrate(int hand, float amplitude, double seconds, float frequency) {
  if (!isFocused()) return;
  XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
  vibration.amplitude = amplitude;
  vibration.duration = static_cast<XrDuration>(seconds * 1e9);
  vibration.frequency = frequency > 0 ? frequency : XR_FREQUENCY_UNSPECIFIED;
  XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
  info.action = hapticAction_;
  info.subactionPath = hand < 0 ? XR_NULL_PATH : handPaths_[hand];
  xrApplyHapticFeedback(session_, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
}

// Logged, it shows whether the Frame controllers come through their own
// profile or SteamVR's Touch remapping. SteamVR sends the event several
// times per change.
void XrInput::readInteractionProfiles() {
  XrPath profiles[2] = {XR_NULL_PATH, XR_NULL_PATH};
  for (int h = 0; h < 2; ++h) {
    XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
    if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(session_, handPaths_[h], &state)))
      profiles[h] = state.interactionProfile;
  }
  if (profiles[0] == handProfiles_[0] && profiles[1] == handProfiles_[1]) return;
  ++profilesVersion_;
  for (int h = 0; h < 2; ++h) {
    handProfiles_[h] = profiles[h];
    char name[XR_MAX_PATH_LENGTH];
    uint32_t length = 0;
    handProfileNames_[h] =
        profiles[h] != XR_NULL_PATH &&
                XR_SUCCEEDED(xrPathToString(instance_, profiles[h], sizeof(name), &length, name))
            ? name
            : "";
  }
  std::printf("Controllers: left %s, right %s\n",
              handProfileNames_[0].empty() ? "none" : handProfileNames_[0].c_str(),
              handProfileNames_[1].empty() ? "none" : handProfileNames_[1].c_str());
}

void XrInput::pairedProfiles(std::string out[2]) const {
  // Controllers come in pairs: the one on (the right, if they differ) stands
  // for both; none on, the Frame's own.
  std::string pair = !handProfileNames_[1].empty() ? handProfileNames_[1] : handProfileNames_[0];
  if (pair.empty() && kStandaloneHeadset)
    pair = controllerLayout(frameControllerExtension_ ? ControllerType::Frame : ControllerType::Touch)
               .profile;
  for (int h = 0; h < 2; ++h) out[h] = handProfileNames_[h].empty() ? pair : handProfileNames_[h];
}

void XrInput::updateGaze(XrTime time) {
  if (!gaze_.available) return;
  XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
  info.action = gazeAction_;
  XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
  const bool active = XR_SUCCEEDED(xrGetActionStatePose(session_, &info, &state)) && state.isActive;
  if (active != gaze_.active) std::printf("Eye tracking: %s\n", active ? "active" : "inactive");
  gaze_.active = active;
  gaze_.valid = gaze_.tracked = gaze_.ageKnown = false;
  if (!active) return;
  XrEyeGazeSampleTimeEXT sampleTime{XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT};
  XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
  location.next = &sampleTime;
  if (XR_FAILED(xrLocateSpace(gazeSpace_, localSpace_, time, &location))) return;
  const XrSpaceLocationFlags valid =
      XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  gaze_.valid = (location.locationFlags & valid) == valid;
  // The direction is what matters; runtimes may only estimate the gaze origin.
  gaze_.tracked = gaze_.valid &&
                  (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0;
  if (!gaze_.valid) return;
  gaze_.origin = toVec3(location.pose.position);
  gaze_.orientation = toQuat(location.pose.orientation);
  // 0 = unknown; a sample time after the display time is a prediction.
  gaze_.ageKnown = sampleTime.time != 0;
  if (gaze_.ageKnown) {
    gaze_.ageSeconds = static_cast<double>(time - sampleTime.time) * 1e-9;
    gaze_.sampleSeconds = static_cast<double>(sampleTime.time) * 1e-9;
  }
}
