#pragma once

#include "accent.h"
#include "anti_shimmer.h"
#include "flat_screen.h"
#include "focus_settings.h"
#include "foveation.h"
#include "stash_settings.h"

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

// The look of the VR menus.
enum class Theme { Glass, Material };

// One choice of resolution, foveation, mipmaps, anti-shimmer and sharpening
// (presets.h); Custom when they match none.
enum class PerformancePreset { Low, Balanced, High, Custom };

// What happens when a video ends: it stops on its last frame (the menu
// opens), starts over, or playback goes on with the next video of the folder
// or list it was opened from, in its order or shuffled. Stop and Next are
// kept as the setting; Repeat and Shuffle last until the player quits.
enum class AfterVideo { Stop, Repeat, Next, Shuffle };

// Where the player keeps its files: ~/.config/videoframevr (or under
// $XDG_CONFIG_HOME). A folder from the former name "vrplayer" is moved there.
std::filesystem::path configDir();
// Where it keeps what it collects (the media library): ~/.local/share/videoframevr
// (or under $XDG_DATA_HOME).
std::filesystem::path dataDir();
// Where it keeps what can be fetched again (Stash's images): ~/.cache/videoframevr
// (or under $XDG_CACHE_HOME).
std::filesystem::path cacheDir();

// User preferences, kept in settings.conf in configDir().
struct Settings {
  Theme theme = Theme::Glass;
  Accent accent;                  // the menu's colour
  std::string language = "auto";
  // The on-screen keyboard's layout and the other one its layout key
  // switches to ("en", "de", "fr", "es", "pt"; empty follows the language).
  std::string keyboardLayout, keyboardOther;
  bool floatingSubtitles = false;  // 2D video: subtitles float in front (3D/surround always do)
  bool background = true;         // night sky around the flat screen
  bool tvGamma = false;           // SDR video converted for a TV's gamma (BT.1886)
  std::string lastFolder;         // the file browser's folder when the player last ran
  bool gallery = false;           // file browser: video thumbnails in a grid, else a list
  int subtitleSize = 100;         // percent
  int subtitleRaise = 0;          // how much higher than usual, percent of the picture
  int subtitleDistance = 150;     // floating subtitles of surround video: centimetres away
  bool videoMipmaps = true;       // video sampled through mipmaps (trilinear), else bilinear full size
  AntiShimmer antiShimmer = AntiShimmer::Off;  // ... over each pixel's footprint (anti_shimmer.h); off: the owner saw no gain
  int sharpen = 0;                // video sharpening strength in percent (100 = 1.0), 0 = off
  bool toneLook = false;          // video tone curve: brighter, a little more contrast, vignette
  // Eye images (video and menus), percent of what 1× is (eye_resolution.h),
  // applied while running; 0 = not settled yet (settleEyeResolution).
  int videoResolution = 0;
  int videoResolutionPixels = 0;  // Frame: exact stop; percent kept for older builds
  int legacyRenderScale = 0;      // render_scale of older versions: percent of SteamVR's recommended size
  bool resolutionNotice = false;  // Frame: say that 1× now means the panel, until the size is changed
  FoveationLevel foveation = FoveationLevel::High;  // Frame: the eye images' (foveation.h)
  bool showFoveation = false;
  bool foveationLearned = true;  // apply the learned gaze correction, independent of stereo focus
  int sharpBins = kDefaultSharpBins;
  bool foveationRing = kDefaultFoveationRing;
  int foveationOutside = 4;
  // Frame: a frame that would look like the last one drawn is not drawn
  // (frame_reuse.h); elsewhere only --frame-reuse=on turns it on.
  bool frameReuse = true;
  bool advancedSettings = false;
  // Match the room (colour_balance.h): how much of the room's measured
  // warmth and tint, and of its exposure, it applies (percent).
  int matchColour = 70, matchBrightness = 60;
  // Frame: Hz, SteamVR's for this app (refresh_rate.h), read from it at start.
  int refreshRate = 120;
  // The performance preset the settings above match (presets.h), named in
  // settings.conf; a profile from before presets reads as Custom.
  PerformancePreset performancePreset = PerformancePreset::Custom;
  FocusSettings focus;            // stereo focus of videos without their own
  FocusPreferences focusPrefs;
  std::string gazeLearned;        // what the eye tracker was off by in earlier sittings (gaze_correction.h)
  bool motionEmulation = true;    // surround video: head translation shown as parallax (motion.h)
  int motionDistance = 200;       // ... of things this far away, cm
  bool resume = true;             // videos start where they were left (library.h)
  AfterVideo afterVideo = AfterVideo::Next;  // Stop or Next
  bool seekPreview = true;        // pointing along the seek bar shows the frame there (seek_preview.h)
  bool thumbnails = true;         // file browser and library: video thumbnails, else a video icon
  bool showPerformance = false;   // frame rates and drops shown over the video (performance_readout.h)
  bool vibration = true;          // controllers buzz on clicks, seeks, turns and grabs (main.cpp)
  int volume = 100;               // mpv's 0..130 for all videos: the last one set
  // Passthrough switches itself on for videos made for it (see_through.h):
  // on the Frame; on a PC only for headsets whose camera keys the green.
  bool autoPassthrough = false;
  bool passthroughNoticeSeen = false;  // its notice is shown the first time only
  bool setupBanner = false;  // the Library's "You're all set" after setup, until dismissed or a video starts
  // What's new (whats_new.h): it opens after an update, and the highest
  // version seen (empty: none saved, as by 0.1.x).
  bool showWhatsNew = true;
  std::string versionSeen;
  // The hints after setup still to show, each with the times shown (hints.h);
  // empty: none (a profile from before setup has none).
  std::string hints;
  // Where offline downloads go (offline.h): a drive's root, the home folder
  // for internal storage; empty until chosen (internal storage meanwhile).
  std::string downloadsTo;
  // The flat screen of videos without their own size: the last one set.
  ScreenSize screen;
  StashSettings stash;  // the optional Stash integration (stash_settings.h)
};

// The defaults; on the Frame (platform.h) what suits its hardware.
// settings.conf keeps only what differs from them.
Settings defaultSettings();
Settings loadSettings();
void saveSettings(const Settings& settings);
// While held (--first-run's setup), saveSettings() writes nothing, and
// Stash's sync keeps no local copy (stash_sync.cpp).
void holdSettings(bool hold);
bool settingsHeld();

// settings.conf's lines of only `keys`, and such lines read into `settings`,
// other keys ignored (presets.h).
std::string settingLines(const Settings& settings, const std::vector<std::string>& keys);
void readSettingLines(std::istream& in, const std::vector<std::string>& keys, Settings& settings);
