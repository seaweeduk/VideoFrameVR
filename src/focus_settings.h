#pragma once

#include <string>

// Stereo focus: the depth at which both eyes of a 3D video are made to line
// up for the viewer's eyes and head pose (see stereo_correction.h). Automatic
// estimates that depth where the viewer looks (autofocus.h). Defaults, units
// and ranges are described in docs/autofocus.md.
enum class FocusMode { Off, Manual, Automatic, kCount };

// What is set per video (or as the default for videos without their own).
struct FocusSettings {
  FocusMode mode = FocusMode::Automatic;
  int distanceCm = 200;  // manual focal distance, from the camera
  int baselineMm = 65;   // camera separation (calibration)
  // Automatic only.
  int minCm = 10, maxCm = 1000;  // accepted depth range; estimates outside it are held, not taken
  int speed = 100;        // transition speed in hundredths (1.00 = an angle filter of 4 per second)
  int sensitivity = 100;  // hundredths, 0.50..2.00: closer probes, so switching near edges
  int accuracy = 8;       // 1..128: 64 × accuracy candidate depths
  int pitchDeg = 0, yawDeg = 0;  // target offset from the viewing direction (up, right)

  bool operator==(const FocusSettings& o) const {
    return mode == o.mode && distanceCm == o.distanceCm && baselineMm == o.baselineMm &&
           minCm == o.minCm && maxCm == o.maxCm && speed == o.speed &&
           sensitivity == o.sensitivity && accuracy == o.accuracy && pitchDeg == o.pitchDeg &&
           yawDeg == o.yawDeg;
  }
  bool operator!=(const FocusSettings& o) const { return !(*this == o); }
};

// User preferences for the focus that are the same for every video.
struct FocusPreferences {
  bool enabled = true;       // off: every video keeps its own stereo, whatever its mode
  bool diagnostics = false;  // target marker and the estimate's state in the view
  bool eyeTracking = false;  // aim automatic focus with the eyes (else the head direction)
  bool gazeDot = false;      // a dot where the eyes look, for checking eye tracking
  // Undo the eye tracker's measured error. Off, not saved and not in the menu
  // for now: the fixed fit is to be replaced by a calibration.
  bool gazeCorrect = false;
  // The viewer's IPD for the correction: the
  // headset's eye separation, or this value (tenths of a mm, 40-90 mm).
  bool autoIpd = true;
  int ipdTenthMm = 640;

  bool operator==(const FocusPreferences& o) const {
    return enabled == o.enabled && diagnostics == o.diagnostics && eyeTracking == o.eyeTracking &&
           autoIpd == o.autoIpd && ipdTenthMm == o.ipdTenthMm && gazeDot == o.gazeDot &&
           gazeCorrect == o.gazeCorrect;
  }
};

// What the focus menu pages show.
struct FocusView {
  FocusSettings settings;  // in effect for the running video
  FocusPreferences preferences;
  bool own = false;        // the video has its own (else the defaults)
  bool supported = false;  // the running video's format can be corrected
  std::string status;      // the live state, one line
  std::string targetStatus;  // what aims it (head, eyes, fallback), one line
  int headsetIpdTenthMm = 0;  // the headset's eye separation now (0 = unknown)

  bool operator==(const FocusView& o) const {
    return settings == o.settings && preferences == o.preferences && own == o.own &&
           supported == o.supported && status == o.status && targetStatus == o.targetStatus &&
           headsetIpdTenthMm == o.headsetIpdTenthMm;
  }
};

// The focus menu's controls (MenuCommand::SetFocus carries one of them).
enum class FocusControl { Mode, Distance, Baseline, MakeDefault, UseDefault, MinDistance,
                          MaxDistance, Transition, Sensitivity, Accuracy, Pitch, Yaw,
                          Diagnostics, Target, GazeDot, AutoIpd, Ipd, GazeCorrect, Enabled };

// Carries out a focus control for the running video: changes its own
// settings (it has them from then on), copies them to the defaults, drops
// them so it follows the defaults again, or changes a preference. Returns
// true if the defaults or preferences changed (settings.conf).
bool applyFocusControl(FocusControl control, double value, bool& videoOwn, FocusSettings& video,
                       FocusSettings& defaults, FocusPreferences& preferences);

// One unit up or down within the control's range, in its stored unit
// (cm, mm, hundredths, candidates, degrees); direction +1 or -1.
int nextFocusStep(FocusControl control, int value, int direction);

// Persistence: a "focus2=..." field in a videos.conf line (an older
// "focus=..." one is converted; false if that leaves only defaults), and settings.conf keys (older focus_* keys
// are converted when no focus2_* key is present). Without a focus_enabled key,
// a default mode of Off becomes stereo focus off for all videos.
std::string focusToken(const FocusSettings& f);
bool parseFocusToken(const std::string& token, FocusSettings& f);
// Reads the focus keys of settings.conf, then fills in the settings.
class FocusSettingsReader {
 public:
  bool read(const std::string& key, const std::string& value);
  void finish(FocusSettings& f, FocusPreferences& p) const;

 private:
  FocusSettings current_;
  bool currentSeen_ = false;
  int old_[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};  // focus_* keys of the first version
  bool oldSeen_ = false;
  bool enabledSeen_ = false;
  FocusPreferences preferences_;
};
std::string focusSettingLines(const FocusSettings& f, const FocusPreferences& p);
