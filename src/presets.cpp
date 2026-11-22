#include "presets.h"

#include "app_dirs.h"
#include "eye_resolution.h"
#include "file_util.h"
#include "platform.h"

#include <glib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace {

// The settings a performance preset covers, by their settings.conf keys. A
// new one: its key here, and its live path in main.cpp's applyPreset.
const std::vector<std::string> kPerformanceKeys = {
    "video_resolution", "video_resolution_pixels", "foveation", "sharp_bins", "foveation_ring",
    "foveation_outside", "video_mipmaps", "anti_shimmer", "sharpen", "tone_look", "tv_gamma",
    "refresh_rate"};
// A user's preset also keeps the stereo focus defaults, a viewer's choice
// rather than performance. Never paths, Stash, credentials, language or
// library data.
const std::vector<std::string> kUserPresetKeys = [] {
  std::vector<std::string> keys = kPerformanceKeys;
  keys.insert(keys.end(), {"focus2_mode", "focus2_distance_cm", "focus2_camera_mm", "focus2_min_cm",
                           "focus2_max_cm", "focus2_speed", "focus2_sensitivity", "focus2_accuracy",
                           "focus2_pitch", "focus2_yaw"});
  return keys;
}();

// Low, Balanced, High: each one's lines, as in settings.conf; a covered
// setting it doesn't name has its default. None: not offered. Provisional
// (owner, 29 Oct). The Frame's resolution in pixels (a stop), the
// desktop's in percent of SteamVR's 1×.
using Lines = std::vector<const char*>;
const Lines kFramePresets[kPerformancePresetCount] = {
    {"video_resolution_pixels=3072", "foveation=high", "video_mipmaps=0", "anti_shimmer=off", "sharpen=0",
     "refresh_rate=90"},
    {"video_resolution_pixels=3456", "foveation=high", "video_mipmaps=0", "anti_shimmer=off", "sharpen=0",
     "refresh_rate=90"},
    {"video_resolution_pixels=3780", "foveation=high", "video_mipmaps=0", "anti_shimmer=off", "sharpen=0",
     "refresh_rate=120"},
};
const Lines kDesktopPresets[kPerformancePresetCount] = {
    {},
    {"video_resolution=100", "video_mipmaps=1", "anti_shimmer=off", "sharpen=0"},
    {"video_resolution=140", "video_mipmaps=1", "anti_shimmer=off", "sharpen=0"},
};

const char* const kNames[] = {"low", "balanced", "high", "custom"};

// A resolution from a preset or another device in this device's choices:
// the nearest stop, else the menu's range and step (the Frame's pixels in
// percent of its panel).
void fitResolution(Settings& s, const std::vector<int>& stops) {
  if (!stops.empty()) {
    settleEyeResolution(s, 0, stops);
    return;
  }
  const int pixels = s.videoResolutionPixels;
  if (pixels > 0)
    s.videoResolution = kPanelWidth > 0 ? static_cast<int>(std::lround(pixels * 100.0 / kPanelWidth))
                                        : s.videoResolution;
  s.videoResolutionPixels = 0;
  const int nearest = (s.videoResolution + kResolutionStep / 2) / kResolutionStep * kResolutionStep;
  s.videoResolution = std::clamp(nearest, kResolutionLow, kResolutionHigh);
}

fs::path presetDir() { return configDir() / "presets"; }
// The presets' file names in the user's order, one per line.
fs::path orderPath() { return presetDir() / "order"; }

// Only a file name, never a way out of the folder.
fs::path presetFile(const std::string& file) { return presetDir() / fs::path(file).filename(); }

}  // namespace

std::string presetName(const std::string& name) {
  std::string out;
  for (const char c : name)
    if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f) out += c;
  const size_t start = out.find_first_not_of(' ');
  if (start == std::string::npos) return {};
  return out.substr(start, out.find_last_not_of(' ') + 1 - start);
}

bool samePresetName(const std::string& a, const std::string& b) {
  auto fold = [](const std::string& name) {
    std::string kept = presetName(name);
    if (!g_utf8_validate(kept.data(), static_cast<gssize>(kept.size()), nullptr)) {
      for (char& c : kept) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return kept;
    }
    gchar* folded = g_utf8_casefold(kept.c_str(), static_cast<gssize>(kept.size()));
    std::string out = folded;
    g_free(folded);
    return out;
  };
  return fold(a) == fold(b);
}

namespace {

UserPreset readPreset(const fs::path& path) {
  std::ifstream in(path);
  std::ostringstream text;
  text << in.rdbuf();
  UserPreset preset;
  preset.file = path.filename().string();
  preset.name = path.stem().string();
  std::istringstream lines(text.str());
  for (std::string line; std::getline(lines, line);)
    if (line.rfind("name=", 0) == 0 && line.size() > 5) preset.name = line.substr(5);
  lines.clear();
  lines.seekg(0);
  preset.values = defaultSettings();
  readSettingLines(lines, kUserPresetKeys, preset.values);
  return preset;
}

std::vector<UserPreset> readPresets() {
  std::vector<UserPreset> presets;
  std::error_code ec;
  for (fs::directory_iterator it(presetDir(), ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code typeError;
    if (it->path().extension() == ".conf" && it->is_regular_file(typeError))
      presets.push_back(readPreset(it->path()));
  }
  std::sort(presets.begin(), presets.end(),
            [](const UserPreset& a, const UserPreset& b) { return fileNameLess(a.name, b.name); });
  // In the user's order; files not in it (copied in by hand) after, by name.
  std::vector<std::string> order;
  std::ifstream in(orderPath());
  for (std::string line; std::getline(in, line);) order.push_back(line);
  auto rank = [&](const UserPreset& p) { return std::find(order.begin(), order.end(), p.file) - order.begin(); };
  std::stable_sort(presets.begin(), presets.end(),
                   [&](const UserPreset& a, const UserPreset& b) { return rank(a) < rank(b); });
  return presets;
}

std::vector<std::string> fileNames(const std::vector<UserPreset>& presets) {
  std::vector<std::string> files;
  for (const UserPreset& p : presets) files.push_back(p.file);
  return files;
}

void writeOrder(const std::vector<std::string>& files) {
  std::string text;
  for (const std::string& file : files) text += file + "\n";
  replaceFile(orderPath(), text);
}

// Where a preset called `name` goes: the name made safe for a file name,
// numbered past other presets' files; `own` may be replaced.
fs::path presetPath(const std::string& name, const fs::path& own) {
  std::string stem;
  for (const char c : name)
    stem += std::string("/\\:*?\"<>|").find(c) == std::string::npos ? c : '_';
  if (stem[0] == '.') stem[0] = '_';
  constexpr size_t kLongest = 80;  // bytes, cut between UTF-8 characters
  size_t length = std::min(stem.size(), kLongest);
  while (length < stem.size() && (static_cast<unsigned char>(stem[length]) & 0xc0) == 0x80) --length;
  stem.resize(length);
  for (int i = 1;; ++i) {
    const fs::path path = presetDir() / (stem + (i > 1 ? "-" + std::to_string(i) : "") + ".conf");
    std::error_code ec;
    if (path == own || !fs::exists(path, ec)) return path;
  }
}

bool writePreset(const fs::path& path, const std::string& name, const std::string& lines) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (replaceFile(path, "name=" + name + "\n" + lines)) return true;
  std::fprintf(stderr, "Could not save the preset\n");
  return false;
}

std::future<std::vector<UserPreset>> queued(std::function<void()> change) {
  auto done = std::make_shared<std::promise<std::vector<UserPreset>>>();
  auto result = done->get_future();
  queueConfigWork([done, change = std::move(change)] {
    if (change) change();
    done->set_value(readPresets());
  });
  return result;
}

}  // namespace

const char* performancePresetName(PerformancePreset preset) {
  return kNames[static_cast<int>(preset)];
}

PerformancePreset parsePerformancePreset(const std::string& name) {
  if (name == "battery") return PerformancePreset::Low;
  for (int i = 0; i < kPerformancePresetCount; ++i)
    if (name == kNames[i]) return static_cast<PerformancePreset>(i);
  return PerformancePreset::Custom;
}

Presets::Presets(const std::vector<int>& stops) : stops_(stops) {
  for (int i = 0; i < kPerformancePresetCount; ++i) {
    std::string text;
    for (const char* line : (kStandaloneHeadset ? kFramePresets : kDesktopPresets)[i])
      text += std::string(line) + "\n";
    if (text.empty()) continue;
    std::istringstream in(text);
    values_[i] = defaultSettings();
    readSettingLines(in, kPerformanceKeys, values_[i]);
    fitResolution(values_[i], stops_);
    lines_[i] = covered(values_[i], kPerformanceKeys);
  }
}

bool Presets::offered(PerformancePreset preset) const {
  const int i = static_cast<int>(preset);
  return i >= 0 && i < kPerformancePresetCount && !lines_[i].empty();
}

Settings Presets::with(PerformancePreset preset, const Settings& current) const {
  if (!offered(preset)) return current;
  Settings s = with(values_[static_cast<int>(preset)], kPerformanceKeys, current);
  s.performancePreset = preset;
  return s;
}

Settings Presets::with(const UserPreset& preset, const Settings& current) const {
  Settings values = preset.values;
  fitResolution(values, stops_);
  return with(values, kUserPresetKeys, current);
}

Settings Presets::with(const Settings& preset, const std::vector<std::string>& keys, Settings s) const {
  std::istringstream in(settingLines(preset, keys));
  readSettingLines(in, keys, s);
  return s;
}

// Without stops the pixels are not used.
std::string Presets::covered(Settings s, const std::vector<std::string>& keys) const {
  if (stops_.empty()) s.videoResolutionPixels = 0;
  return settingLines(s, keys);
}

PerformancePreset Presets::matching(const Settings& s) const {
  const std::string lines = covered(s, kPerformanceKeys);
  if (offered(s.performancePreset) && lines_[static_cast<int>(s.performancePreset)] == lines)
    return s.performancePreset;
  for (int i = 0; i < kPerformancePresetCount; ++i)
    if (!lines_[i].empty() && lines_[i] == lines) return static_cast<PerformancePreset>(i);
  return PerformancePreset::Custom;
}

int Presets::shownUserPreset(const std::vector<UserPreset>& presets, const Settings& s,
                             const std::string& picked) const {
  const std::string lines = covered(s, kUserPresetKeys);
  int first = -1;
  for (size_t i = 0; i < presets.size(); ++i) {
    Settings values = presets[i].values;
    fitResolution(values, stops_);
    if (covered(values, kUserPresetKeys) != lines) continue;
    if (presets[i].name == picked) return static_cast<int>(i);
    if (first < 0) first = static_cast<int>(i);
  }
  return matching(s) == PerformancePreset::Custom ? first : -1;
}

bool Presets::settle(Settings& s) const {
  const PerformancePreset named = s.performancePreset;
  if (offered(named) && matching(s) != named) {
    s = with(named, s);
    return true;
  }
  s.performancePreset = matching(s);
  return s.performancePreset != named;
}

std::future<std::vector<UserPreset>> listUserPresets() { return queued(nullptr); }

bool sameUserPresets(const std::vector<UserPreset>& a, const std::vector<UserPreset>& b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const UserPreset& x, const UserPreset& y) {
    return x.name == y.name && x.file == y.file &&
           settingLines(x.values, kUserPresetKeys) == settingLines(y.values, kUserPresetKeys);
  });
}

std::future<std::vector<UserPreset>> saveUserPreset(const std::string& name, const Settings& s) {
  return queued([name = presetName(name), lines = settingLines(s, kUserPresetKeys)] {
    if (name.empty()) return;
    const std::vector<UserPreset> before = readPresets();
    fs::path own;
    for (const UserPreset& preset : before)
      if (samePresetName(preset.name, name)) own = presetFile(preset.file);
    const fs::path path = presetPath(name, own);
    if (!writePreset(path, name, lines)) return;
    std::vector<std::string> files = fileNames(before);
    if (own.empty()) {
      files.push_back(path.filename().string());
    } else if (path != own) {  // the name's case changed: it takes the old one's place
      std::error_code ec;
      fs::remove(own, ec);
      std::replace(files.begin(), files.end(), own.filename().string(), path.filename().string());
    }
    writeOrder(files);
  });
}

std::future<std::vector<UserPreset>> renameUserPreset(const std::string& file, const std::string& name) {
  return queued([from = presetFile(file), name = presetName(name)] {
    std::ifstream in(from);
    if (!in || name.empty()) return;
    std::string kept;
    for (std::string line; std::getline(in, line);)
      if (line.rfind("name=", 0) != 0) kept += line + "\n";
    in.close();
    std::vector<std::string> files = fileNames(readPresets());
    const fs::path to = presetPath(name, from);
    if (!writePreset(to, name, kept)) return;
    std::error_code ec;
    if (to != from) fs::remove(from, ec);
    std::replace(files.begin(), files.end(), from.filename().string(), to.filename().string());
    writeOrder(files);
  });
}

std::future<std::vector<UserPreset>> deleteUserPreset(const std::string& file) {
  return queued([path = presetFile(file)] {
    std::error_code ec;
    fs::remove(path, ec);
    std::vector<std::string> files = fileNames(readPresets());
    writeOrder(files);
  });
}

std::future<std::vector<UserPreset>> moveUserPreset(const std::string& file, int by) {
  return queued([file = presetFile(file).filename().string(), by] {
    std::vector<std::string> files = fileNames(readPresets());
    const auto it = std::find(files.begin(), files.end(), file);
    const long i = it - files.begin(), j = i + by;
    if (it == files.end() || j < 0 || j >= static_cast<long>(files.size())) return;
    std::swap(files[i], files[j]);
    writeOrder(files);
  });
}
