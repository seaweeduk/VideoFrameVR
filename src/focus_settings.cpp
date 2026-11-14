#include "focus_settings.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <sstream>
#include <vector>

namespace {

// Each button changes one stored unit, matching the slider's precision.
struct Range {
  int lo, hi;
  int clamp(int value) const { return std::clamp(value, lo, hi); }
  int next(int value, int direction) const { return clamp(value + direction); }
};
constexpr Range kDistance{10, 1000}, kRange{5, 100000}, kSpeed{10, 1000},
    kSensitivity{50, 200}, kAccuracy{1, 128};
constexpr int kMinBaselineMm = 10, kMaxBaselineMm = 120;
constexpr int kMaxOffsetDeg = 45;
constexpr int kMinIpd = 400, kMaxIpd = 900;  // tenths of a mm (4 to 9 cm)

const char* modeName(FocusMode m) {
  return m == FocusMode::Manual ? "manual" : m == FocusMode::Automatic ? "auto" : "off";
}

FocusMode modeFromName(const std::string& s) {
  return s == "manual" ? FocusMode::Manual : s == "off" ? FocusMode::Off : FocusMode::Automatic;
}

// Every field brought into its range, Nearest below Farthest.
FocusSettings sanitized(FocusSettings f) {
  f.mode = static_cast<FocusMode>(
      std::clamp(static_cast<int>(f.mode), 0, static_cast<int>(FocusMode::kCount) - 1));
  f.distanceCm = kDistance.clamp(f.distanceCm);
  f.baselineMm = std::clamp(f.baselineMm, kMinBaselineMm, kMaxBaselineMm);
  f.minCm = kRange.clamp(f.minCm);
  f.maxCm = kRange.clamp(f.maxCm);
  if (f.minCm >= f.maxCm) f.maxCm = kRange.next(f.minCm, 1);
  if (f.minCm >= f.maxCm) f.minCm = kRange.next(f.maxCm, -1);
  f.speed = kSpeed.clamp(f.speed);
  f.sensitivity = kSensitivity.clamp(f.sensitivity);
  f.accuracy = kAccuracy.clamp(f.accuracy);
  f.pitchDeg = std::clamp(f.pitchDeg, -kMaxOffsetDeg, kMaxOffsetDeg);
  f.yawDeg = std::clamp(f.yawDeg, -kMaxOffsetDeg, kMaxOffsetDeg);
  return f;
}

// The first version's fields (mode, distance, baseline, min, max, transition
// 0..100, sensitivity 0..100, accuracy 0..2, pitch, yaw; -1 = absent). Its
// defaults were written for every video and into settings.conf whether
// chosen or not, so they become today's defaults; other values keep their
// meaning in the new units.
FocusSettings fromFirstVersion(const int v[10]) {
  FocusSettings f;
  auto set = [&](int i, int oldDefault, int& field, int value) {
    if (v[i] != -1 && v[i] != oldDefault) field = value;
  };
  // Off was the old default: Automatic now. Manual stays manual.
  if (v[0] == static_cast<int>(FocusMode::Manual) || v[0] == static_cast<int>(FocusMode::Automatic))
    f.mode = static_cast<FocusMode>(v[0]);
  set(1, 150, f.distanceCm, v[1]);
  set(2, 64, f.baselineMm, v[2]);
  set(3, 30, f.minCm, v[3]);
  set(4, 1000, f.maxCm, v[4]);
  // The old glide was 0.5 × 32^(t/100) per second (in 1/distance); now 4 × speed.
  set(5, 50, f.speed,
      static_cast<int>(std::lround(100 * 0.5 * std::pow(32.0, v[5] / 100.0) / 4)));
  set(6, 50, f.sensitivity, 50 + v[6] * 3 / 2);
  const int accuracies[] = {4, 8, 16};
  if (v[7] >= 0 && v[7] <= 2) f.accuracy = accuracies[v[7]];
  if (v[8] != -1) f.pitchDeg = v[8];
  if (v[9] != -1) f.yawDeg = v[9];
  return sanitized(f);
}

std::vector<int> numbers(const std::string& list) {
  std::vector<int> values;
  std::istringstream in(list);
  std::string part;
  while (std::getline(in, part, ',')) values.push_back(std::atoi(part.c_str()));
  return values;
}

}  // namespace

int nextFocusStep(FocusControl control, int value, int direction) {
  switch (control) {
    case FocusControl::Distance: return kDistance.next(value, direction);
    case FocusControl::MinDistance:
    case FocusControl::MaxDistance: return kRange.next(value, direction);
    case FocusControl::Transition: return kSpeed.next(value, direction);
    case FocusControl::Sensitivity: return kSensitivity.next(value, direction);
    case FocusControl::Accuracy: return kAccuracy.next(value, direction);
    case FocusControl::Ipd: return std::clamp(value + direction, kMinIpd, kMaxIpd);
    case FocusControl::Baseline:
      return std::clamp(value + direction, kMinBaselineMm, kMaxBaselineMm);
    case FocusControl::Pitch:
    case FocusControl::Yaw: return std::clamp(value + direction, -kMaxOffsetDeg, kMaxOffsetDeg);
    default: return value + direction;
  }
}

bool applyFocusControl(FocusControl control, double value, bool& videoOwn, FocusSettings& video,
                       FocusSettings& defaults, FocusPreferences& preferences) {
  const FocusSettings current = videoOwn ? video : defaults;
  const int v = static_cast<int>(std::lround(value));
  switch (control) {
    case FocusControl::MakeDefault:
      defaults = current;
      return true;
    case FocusControl::UseDefault:
      videoOwn = false;
      return false;
    case FocusControl::Enabled:
      preferences.enabled = v != 0;
      return true;
    case FocusControl::Diagnostics:
      preferences.diagnostics = v != 0;
      return true;
    case FocusControl::Target:
      preferences.eyeTracking = v != 0;
      return true;
    case FocusControl::GazeDot:
      preferences.gazeDot = v != 0;
      return true;
    case FocusControl::GazeCorrect:
      preferences.gazeCorrect = v != 0;
      return true;
    case FocusControl::AutoIpd:
      preferences.autoIpd = v != 0;
      return true;
    case FocusControl::Ipd:  // a value of its own: no longer the headset's
      preferences.ipdTenthMm = std::clamp(v, kMinIpd, kMaxIpd);
      preferences.autoIpd = false;
      return true;
    default:
      break;
  }
  FocusSettings f = current;
  switch (control) {
    case FocusControl::Mode: f.mode = static_cast<FocusMode>(v); break;
    case FocusControl::Distance: f.distanceCm = v; break;
    case FocusControl::Baseline: f.baselineMm = v; break;
    case FocusControl::MinDistance:
      f.minCm = kRange.clamp(v);
      if (f.minCm >= f.maxCm) f.maxCm = kRange.next(f.minCm, 1);
      break;
    case FocusControl::MaxDistance:
      f.maxCm = kRange.clamp(v);
      if (f.minCm >= f.maxCm) f.minCm = kRange.next(f.maxCm, -1);
      break;
    case FocusControl::Transition: f.speed = v; break;
    case FocusControl::Sensitivity: f.sensitivity = v; break;
    case FocusControl::Accuracy: f.accuracy = v; break;
    case FocusControl::Pitch: f.pitchDeg = v; break;
    case FocusControl::Yaw: f.yawDeg = v; break;
    default: break;
  }
  video = sanitized(f);
  videoOwn = true;
  return false;
}

std::string focusToken(const FocusSettings& f) {
  std::ostringstream out;
  out << "focus2=" << static_cast<int>(f.mode) << ',' << f.distanceCm << ',' << f.baselineMm
      << ',' << f.minCm << ',' << f.maxCm << ',' << f.speed << ',' << f.sensitivity << ','
      << f.accuracy << ',' << f.pitchDeg << ',' << f.yawDeg;
  return out.str();
}

// Fields missing at the end keep their defaults.
bool parseFocusToken(const std::string& token, FocusSettings& f) {
  if (token.rfind("focus2=", 0) == 0) {
    const std::vector<int> v = numbers(token.substr(7));
    FocusSettings s;
    int* fields[] = {nullptr,     &s.distanceCm,  &s.baselineMm, &s.minCm,    &s.maxCm,
                     &s.speed,    &s.sensitivity, &s.accuracy,   &s.pitchDeg, &s.yawDeg};
    for (size_t i = 0; i < v.size() && i < std::size(fields); ++i) {
      if (i == 0) s.mode = static_cast<FocusMode>(v[0]);
      else *fields[i] = v[i];
    }
    f = sanitized(s);
    return true;
  }
  if (token.rfind("focus=", 0) == 0) {
    int old[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    const std::vector<int> v = numbers(token.substr(6));
    std::copy_n(v.begin(), std::min<size_t>(v.size(), 10), old);
    f = fromFirstVersion(old);
    // Only the old defaults: the video follows today's defaults.
    return f != FocusSettings{};
  }
  return false;
}

bool FocusSettingsReader::read(const std::string& key, const std::string& value) {
  const int v = std::atoi(value.c_str());
  if (key == "focus_enabled") {
    preferences_.enabled = value != "0";
    enabledSeen_ = true;
    return true;
  }
  if (key == "focus_diagnostics") {
    preferences_.diagnostics = value == "1";
    return true;
  }
  if (key == "focus_target") {
    preferences_.eyeTracking = value == "eyes";
    return true;
  }
  if (key == "focus_gaze_dot") {
    preferences_.gazeDot = value == "1";
    return true;
  }
  if (key == "gaze_correction") {
    preferences_.gazeCorrect = value != "0";
    return true;
  }
  if (key == "user_ipd_auto") {
    preferences_.autoIpd = value != "0";
    return true;
  }
  if (key == "user_ipd_tenth_mm") {
    preferences_.ipdTenthMm = std::clamp(v, kMinIpd, kMaxIpd);
    return true;
  }
  if (key.rfind("focus2_", 0) == 0) {
    const std::string k = key.substr(7);
    FocusSettings& f = current_;
    if (k == "mode") f.mode = modeFromName(value);
    else if (k == "distance_cm") f.distanceCm = v;
    else if (k == "camera_mm") f.baselineMm = v;
    else if (k == "min_cm") f.minCm = v;
    else if (k == "max_cm") f.maxCm = v;
    else if (k == "speed") f.speed = v;
    else if (k == "sensitivity") f.sensitivity = v;
    else if (k == "accuracy") f.accuracy = v;
    else if (k == "pitch") f.pitchDeg = v;
    else if (k == "yaw") f.yawDeg = v;
    else return false;
    currentSeen_ = true;
    return true;
  }
  static const char* kOldKeys[] = {"focus_mode",       "focus_distance_cm", "camera_baseline_mm",
                                   "focus_min_cm",     "focus_max_cm",      "focus_transition",
                                   "focus_sensitivity", "focus_accuracy",    "focus_pitch",
                                   "focus_yaw"};
  for (int i = 0; i < 10; ++i) {
    if (key != kOldKeys[i]) continue;
    old_[i] = i == 0 ? (value == "manual" ? 1 : value == "auto" ? 2 : 0) : v;
    oldSeen_ = true;
    return true;
  }
  return false;
}

void FocusSettingsReader::finish(FocusSettings& f, FocusPreferences& p) const {
  f = currentSeen_ ? sanitized(current_) : oldSeen_ ? fromFirstVersion(old_) : FocusSettings{};
  p = preferences_;
  // Before the switch, a default of Off was how it was turned off for all videos.
  if (!enabledSeen_ && f.mode == FocusMode::Off) {
    p.enabled = false;
    f.mode = FocusSettings{}.mode;
  }
}

std::string focusSettingLines(const FocusSettings& f, const FocusPreferences& p) {
  std::ostringstream out;
  out << "focus2_mode=" << modeName(f.mode) << "\nfocus2_distance_cm=" << f.distanceCm
      << "\nfocus2_camera_mm=" << f.baselineMm << "\nfocus2_min_cm=" << f.minCm
      << "\nfocus2_max_cm=" << f.maxCm << "\nfocus2_speed=" << f.speed
      << "\nfocus2_sensitivity=" << f.sensitivity << "\nfocus2_accuracy=" << f.accuracy
      << "\nfocus2_pitch=" << f.pitchDeg << "\nfocus2_yaw=" << f.yawDeg
      << "\nfocus_enabled=" << (p.enabled ? 1 : 0)
      << "\nfocus_diagnostics=" << (p.diagnostics ? 1 : 0)
      << "\nfocus_target=" << (p.eyeTracking ? "eyes" : "head")
      << "\nfocus_gaze_dot=" << (p.gazeDot ? 1 : 0)
      << "\ngaze_correction=" << (p.gazeCorrect ? 1 : 0)
      << "\nuser_ipd_auto=" << (p.autoIpd ? 1 : 0) << "\nuser_ipd_tenth_mm=" << p.ipdTenthMm
      << "\n";
  return out.str();
}
