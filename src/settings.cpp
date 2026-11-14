#include "settings.h"

#include "eye_resolution.h"
#include "file_util.h"
#include "motion.h"
#include "platform.h"
#include "presets.h"
#include "refresh_rate.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path settingsPath() { return configDir() / "settings.conf"; }

// The file keeps only what differs from the defaults, so a default that
// changes (or differs on the Frame) reaches everyone who left it alone. Older
// files have every key; this line tells the two apart.
constexpr const char* kFormatLine = "settings_version=2";

// Stereo focus keys are kept from older files whatever their value: whether
// one is missing changes how the others read (FocusSettingsReader).
bool focusKey(const std::string& key) {
  return key.rfind("focus", 0) == 0 || key.rfind("user_ipd", 0) == 0 ||
         key == "camera_baseline_mm";
}

using Entries = std::vector<std::pair<std::string, std::string>>;

Entries entries(std::istream& in) {
  Entries out;
  std::string line;
  while (std::getline(in, line)) {
    const size_t eq = line.find('=');
    if (eq != std::string::npos) out.emplace_back(line.substr(0, eq), line.substr(eq + 1));
  }
  return out;
}

// Every key, as written.
std::string allLines(const Settings& s) {
  std::ostringstream out;
  out << "theme=" << (s.theme == Theme::Material ? "material" : "glass") << "\n";
  out << "accent=" << accentCode(s.accent.preset) << "\n";
  out << "accent_hue=" << s.accent.hue << "\n";
  out << "accent_brightness=" << s.accent.brightness << "\n";
  out << "accent_saturation=" << s.accent.saturation << "\n";
  out << "language=" << s.language << "\n";
  out << "keyboard_layout=" << s.keyboardLayout << "\n";
  out << "keyboard_layout_other=" << s.keyboardOther << "\n";
  out << "floating_subtitles_2d=" << (s.floatingSubtitles ? 1 : 0) << "\n";
  out << "background=" << (s.background ? 1 : 0) << "\n";
  out << "tv_gamma=" << (s.tvGamma ? 1 : 0) << "\n";
  out << "last_folder=" << s.lastFolder << "\n";
  out << "gallery=" << (s.gallery ? 1 : 0) << "\n";
  out << "subtitle_size=" << s.subtitleSize << "\n";
  out << "subtitle_height=" << s.subtitleRaise << "\n";
  out << "subtitle_distance_cm=" << s.subtitleDistance << "\n";
  out << "video_mipmaps=" << (s.videoMipmaps ? 1 : 0) << "\n";
  out << "anti_shimmer=" << antiShimmerName(s.antiShimmer) << "\n";
  out << "sharpen=" << s.sharpen << "\n";
  out << "tone_look=" << (s.toneLook ? 1 : 0) << "\n";
  if (s.videoResolution > 0)
    out << "video_resolution=" << s.videoResolution << "\n";
  else if (s.legacyRenderScale > 0)
    out << "render_scale=" << s.legacyRenderScale << "\n";  // not settled yet: kept as it was
  if (s.videoResolutionPixels > 0)
    out << "video_resolution_pixels=" << s.videoResolutionPixels << "\n";
  if (s.resolutionNotice) out << "resolution_notice=1\n";
  out << "foveation=" << foveationLevelName(s.foveation) << "\n";
  out << "show_foveation=" << s.showFoveation << "\n";
  out << "foveation_learned=" << s.foveationLearned << "\n";
  out << "sharp_bins=" << s.sharpBins << "\n";
  out << "foveation_ring=" << s.foveationRing << "\n";
  out << "foveation_outside=" << s.foveationOutside << "\n";
  out << "frame_reuse=" << (s.frameReuse ? 1 : 0) << "\n";
  out << "advanced_settings=" << s.advancedSettings << "\n";
  out << "match_colour=" << s.matchColour << "\n";
  out << "match_brightness=" << s.matchBrightness << "\n";
  out << "refresh_rate=" << s.refreshRate << "\n";
  out << "performance_preset=" << performancePresetName(s.performancePreset) << "\n";
  out << focusSettingLines(s.focus, s.focusPrefs);
  out << "gaze_correction_learned=" << s.gazeLearned << "\n";
  out << "motion_emulation=" << (s.motionEmulation ? 1 : 0) << "\n";
  out << "motion_distance_cm=" << s.motionDistance << "\n";
  out << "resume=" << (s.resume ? 1 : 0) << "\n";
  out << "after_video=" << (s.afterVideo == AfterVideo::Stop ? "stop" : "next") << "\n";
  out << "seek_preview=" << (s.seekPreview ? 1 : 0) << "\n";
  out << "auto_passthrough=" << (s.autoPassthrough ? 1 : 0) << "\n";
  out << "passthrough_notice_seen=" << (s.passthroughNoticeSeen ? 1 : 0) << "\n";
  out << "setup_banner=" << (s.setupBanner ? 1 : 0) << "\n";
  out << "show_whats_new=" << (s.showWhatsNew ? 1 : 0) << "\n";
  out << "version_seen=" << s.versionSeen << "\n";
  out << "hints=" << s.hints << "\n";
  out << "thumbnails=" << (s.thumbnails ? 1 : 0) << "\n";
  out << "show_performance=" << (s.showPerformance ? 1 : 0) << "\n";
  out << "vibration=" << (s.vibration ? 1 : 0) << "\n";
  out << "volume=" << s.volume << "\n";
  out << "downloads_to=" << s.downloadsTo << "\n";
  out << "screen_distance_cm=" << s.screen.distance * 100 << "\n";
  out << "screen_height_cm=" << s.screen.height * 100 << "\n";
  out << "screen_curve=" << s.screen.curve << "\n";
  out << stashSettingLines(s.stash);
  return out.str();
}

std::map<std::string, std::string> lineMap(const Settings& s) {
  std::istringstream in(allLines(s));
  std::map<std::string, std::string> out;
  for (auto& [key, value] : entries(in)) out[key] = value;
  return out;
}

}  // namespace

static Settings readEntries(Settings s, const Entries& file, bool oldFoveation);

fs::path configDir() {
  fs::path base;
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
    base = xdg;
  } else {
    const char* home = std::getenv("HOME");
    base = fs::path(home ? home : ".") / ".config";
  }
  const fs::path dir = base / kAppName;
  // Once: take over what was kept under the player's former name.
  static bool checked = false;
  if (!checked) {
    checked = true;
    std::error_code ec;
    const fs::path old = base / "vrplayer";
    if (!fs::exists(dir, ec) && fs::is_directory(old, ec)) fs::rename(old, dir, ec);
  }
  return dir;
}

fs::path dataDir() {
  // The XDG spec ignores a relative path.
  if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg == '/')
    return fs::path(xdg) / kAppName;
  const char* home = std::getenv("HOME");
  return fs::path(home ? home : ".") / ".local" / "share" / kAppName;
}

fs::path cacheDir() {
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg == '/')
    return fs::path(xdg) / kAppName;
  const char* home = std::getenv("HOME");
  return fs::path(home ? home : ".") / ".cache" / kAppName;
}

Settings defaultSettings() {
  Settings s;
  s.videoResolution = kDefaultVideoResolution;
  s.performancePreset = kDefaultPerformancePreset;
  // The Frame's own (docs/frame-plan.md, W4); its resolutions are in eye_resolution.h.
  if (kStandaloneHeadset) {
    s.theme = Theme::Material;
    s.videoMipmaps = false;  // at 2160 they cost 8K playback's headroom; 2592 drops frames
    s.seekPreview = false;   // not available there (main.cpp)
    s.background = false;    // the night sky: a non-essential cost behind flat videos
    s.autoPassthrough = true;  // the Frame shows the room itself
  }
  return s;
}

Settings loadSettings() {
  Settings s = defaultSettings();
  // Without video_resolution, settled at start: from render_scale or the default.
  s.videoResolution = 0;
  std::ifstream in(settingsPath());
  if (in && chmod(settingsPath().c_str(), 0600) != 0)
    std::fprintf(stderr, "Could not restrict settings permissions\n");
  Entries file = entries(in);
  // A profile from before presets keeps its values (settlePerformancePreset).
  if (in.is_open()) s.performancePreset = PerformancePreset::Custom;
  // Sparse old profiles also used three sharp bins. Keep that picture once;
  // the separate marker disambiguates the renamed Medium and High levels.
  const bool oldFoveation = in.is_open() && std::none_of(file.begin(), file.end(), [](const auto& e) {
    return e.first == "foveation_version" && e.second == "2";
  });
  if (oldFoveation) s.sharpBins = 3;
  const bool current = std::any_of(file.begin(), file.end(), [](const auto& e) {
    return e.first + "=" + e.second == kFormatLine;
  });
  if (!current && kStandaloneHeadset) {
    // An older file has every key, chosen or not (on the Frame, often copied
    // from the desktop): a value that is the desktop's default was most
    // likely left alone, so the Frame's default applies instead.
    const auto shared = lineMap(Settings{});
    file.erase(std::remove_if(file.begin(), file.end(),
                              [&](const auto& e) {
                                const auto d = shared.find(e.first);
                                return !focusKey(e.first) && e.first != "foveation" &&
                                       e.first != "sharp_bins" && e.first != "foveation_ring" &&
                                       e.first != "ring" && d != shared.end() &&
                                       d->second == e.second;
                              }),
               file.end());
  }
  return readEntries(s, file, oldFoveation);
}

// Reads settings.conf's entries over `s`. oldFoveation: the file is from
// before foveation_version=2.
static Settings readEntries(Settings s, const Entries& file, bool oldFoveation) {
  FocusSettingsReader focus;
  bool focusRead = false;  // else the focus stays as in `s`
  for (const auto& [key, value] : file) {
    if (key == "theme") s.theme = value == "material" ? Theme::Material : Theme::Glass;
    else if (key == "accent") s.accent.preset = accentPreset(value);
    else if (key == "accent_hue") s.accent.hue = std::clamp(std::atoi(value.c_str()), 0, 359);
    else if (key == "accent_brightness")
      s.accent.brightness = std::clamp(std::atoi(value.c_str()), 0, 100);
    else if (key == "accent_saturation")
      s.accent.saturation = std::clamp(std::atoi(value.c_str()), 0, 100);
    else if (key == "language") s.language = value;
    else if (key == "gaze_correction_learned") s.gazeLearned = value;
    else if (key == "keyboard_layout") s.keyboardLayout = value;
    else if (key == "keyboard_layout_other") s.keyboardOther = value;
    else if (key == "floating_subtitles_2d") s.floatingSubtitles = value == "1";
    else if (key == "background") s.background = value != "0";
    else if (key == "passthrough") continue;  // old global value is not carried into videos
    else if (key == "tv_gamma") s.tvGamma = value == "1";
    else if (key == "last_folder") s.lastFolder = value;
    else if (key == "downloads_to") s.downloadsTo = value;
    else if (key == "gallery") s.gallery = value == "1";
    else if (key == "subtitle_size") s.subtitleSize = std::atoi(value.c_str());
    else if (key == "subtitle_height") s.subtitleRaise = std::atoi(value.c_str());
    else if (key == "subtitle_distance_cm") s.subtitleDistance = std::atoi(value.c_str());
    else if (key == "video_mipmaps") s.videoMipmaps = value != "0";
    else if (key == "anti_shimmer") parseAntiShimmer(value, s.antiShimmer);
    else if (key == "sharpen") s.sharpen = std::clamp(std::atoi(value.c_str()), 0, 300);
    else if (key == "match_colour") s.matchColour = std::clamp(std::atoi(value.c_str()), 0, 100);
    else if (key == "match_brightness")
      s.matchBrightness = std::clamp(std::atoi(value.c_str()), 0, 100);
    else if (key == "tone_look") s.toneLook = value == "1";
    else if (key == "render_scale")
      s.legacyRenderScale = std::clamp(std::atoi(value.c_str()), 50, 200);
    else if (key == "video_resolution")
      s.videoResolution = std::clamp(std::atoi(value.c_str()), kMinResolution, kMaxResolution);
    else if (key == "video_resolution_pixels")
      s.videoResolutionPixels = std::max(0, std::atoi(value.c_str()));
    else if (key == "resolution_notice") s.resolutionNotice = value == "1";
    else if (key == "foveation") parseFoveationLevel(value, s.foveation);
    else if (key == "show_foveation") s.showFoveation = std::atoi(value.c_str()) > 0;
    else if (key == "foveation_learned") s.foveationLearned = value == "1";
    else if (key == "sharp_bins") s.sharpBins = value == "1" ? 1 : 3;
    else if (key == "foveation_ring" || key == "ring") s.foveationRing = value != "0" && value != "off";
    else if (key == "foveation_outside") s.foveationOutside = value == "2" ? 2 : 4;
    else if (key == "frame_reuse") s.frameReuse = value != "0";
    else if (key == "advanced_settings") s.advancedSettings = value == "1";
    else if (key == "refresh_rate") {
      const int hz = std::atoi(value.c_str());
      if (std::count(std::begin(kRefreshRates), std::end(kRefreshRates), hz)) s.refreshRate = hz;
    }
    else if (key == "performance_preset") s.performancePreset = parsePerformancePreset(value);
    else if (key == "motion_emulation") s.motionEmulation = value != "0";
    else if (key == "motion_distance_cm")
      s.motionDistance = clampMotionDistance(std::atoi(value.c_str()));
    else if (key == "resume") s.resume = value != "0";
    else if (key == "after_video") s.afterVideo = value == "stop" ? AfterVideo::Stop : AfterVideo::Next;
    else if (key == "seek_preview") s.seekPreview = value != "0";
    else if (key == "auto_passthrough") s.autoPassthrough = value != "0";
    else if (key == "passthrough_notice_seen") s.passthroughNoticeSeen = value == "1";
    else if (key == "setup_banner") s.setupBanner = value == "1";
    else if (key == "show_whats_new") s.showWhatsNew = value != "0";
    else if (key == "version_seen") s.versionSeen = value;
    else if (key == "hints") s.hints = value;
    else if (key == "thumbnails") s.thumbnails = value != "0";
    else if (key == "show_performance") s.showPerformance = value == "1";
    else if (key == "vibration") s.vibration = value != "0";
    else if (key == "volume") s.volume = std::clamp(std::atoi(value.c_str()), 0, 130);
    else if (key == "screen_distance_cm") s.screen.distance = std::atof(value.c_str()) / 100;
    else if (key == "screen_height_cm") s.screen.height = std::atof(value.c_str()) / 100;
    else if (key == "screen_curve") s.screen.curve = static_cast<float>(std::atof(value.c_str()));
    else if (!readStashSetting(key, value, s.stash)) focusRead |= focus.read(key, value);
  }
  // stash_path_from left empty: none (the same paths on both).
  auto& folders = s.stash.folders;
  folders.erase(std::remove_if(folders.begin(), folders.end(),
                               [](const StashFolderMap& m) { return m.from.empty(); }),
                folders.end());
  if (oldFoveation)
    s.foveationOutside = s.foveation == FoveationLevel::Medium ? 2 : 4;
  s.foveation = foveationLevel(s.foveation != FoveationLevel::Off, s.sharpBins,
                              s.foveationRing, s.foveationOutside);
  if (focusRead) focus.finish(s.focus, s.focusPrefs);
  s.screen = s.screen.clamped();
  return s;
}

static void writeSettings(const Settings& s) {
  const fs::path path = settingsPath();
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  FILE* file = nullptr;
  if (fd >= 0 && fchmod(fd, 0600) == 0) file = fdopen(fd, "w");
  if (!file) {
    if (fd >= 0) close(fd);
    std::fprintf(stderr, "Could not save settings\n");
    return;
  }
  std::ostringstream out;
  const auto defaults = lineMap(defaultSettings());
  std::istringstream lines(allLines(s));
  out << kFormatLine << "\nfoveation_version=2\n";
  for (const auto& [key, value] : entries(lines)) {
    // focus_enabled stays: without it, an Off focus mode reads as focus off
    // for all; performance_preset too: without it, the profile is from before presets.
    const auto d = defaults.find(key);
    if (key == "focus_enabled" || key == "performance_preset" || key == "foveation" ||
        key == "sharp_bins" || key == "foveation_ring" || key == "foveation_outside" ||
        d == defaults.end() || d->second != value)
      out << key << "=" << value << "\n";
  }
  const std::string contents = out.str();
  std::fwrite(contents.data(), 1, contents.size(), file);
  std::fclose(file);
}

static std::atomic<bool> held{false};  // read by Stash's sync worker too

void saveSettings(const Settings& s) {
  if (!held) queueConfigWork([s] { writeSettings(s); });
}

void holdSettings(bool hold) { held = hold; }

bool settingsHeld() { return held; }

std::string settingLines(const Settings& s, const std::vector<std::string>& keys) {
  std::istringstream lines(allLines(s));
  std::string out;
  for (const auto& [key, value] : entries(lines))
    if (std::find(keys.begin(), keys.end(), key) != keys.end()) out += key + "=" + value + "\n";
  return out;
}

void readSettingLines(std::istream& in, const std::vector<std::string>& keys, Settings& s) {
  Entries file = entries(in);
  file.erase(std::remove_if(file.begin(), file.end(),
                            [&](const auto& e) {
                              return std::find(keys.begin(), keys.end(), e.first) == keys.end();
                            }),
             file.end());
  auto find = [&](const char* key) {
    return std::find_if(file.begin(), file.end(), [&](const auto& e) { return e.first == key; });
  };
  // A foveation level without its shape (foveation=low): the level's.
  const auto level = find("foveation");
  if (level != file.end() && find("sharp_bins") == file.end() &&
      find("foveation_ring") == file.end() && find("foveation_outside") == file.end()) {
    FoveationLevel l = FoveationLevel::High;
    int bins, outside;
    bool ring;
    parseFoveationLevel(level->second, l);
    foveationPreset(l, bins, ring, outside);
    file.insert(file.end(), {{"sharp_bins", std::to_string(bins)},
                             {"foveation_ring", ring ? "1" : "0"},
                             {"foveation_outside", std::to_string(outside)}});
  }
  // Only so that an Off focus mode stays Off (FocusSettingsReader::finish):
  // the focus preferences are not preset keys.
  const bool focus = std::any_of(file.begin(), file.end(),
                                 [](const auto& e) { return e.first.rfind("focus2_", 0) == 0; });
  if (focus) file.emplace_back("focus_enabled", "1");
  const FocusPreferences preferences = s.focusPrefs;
  s = readEntries(std::move(s), file, false);
  s.focusPrefs = preferences;
}
