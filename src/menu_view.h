#pragma once

// What the app hands the menu to show (Menu::update()), and the notices'
// icons and rings it draws.

#include "accent.h"
#include "anti_shimmer.h"
#include "colour_balance.h"
#include "control_map.h"
#include "diagnostics.h"
#include "flat_screen.h"
#include "focus_settings.h"
#include "foveation_level.h"
#include "playback_state.h"
#include "presets.h"
#include "release_update.h"
#include "script_player.h"
#include "settings.h"
#include "stash_settings.h"
#include "video_format.h"
#include "video_memory.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

// Match the room's switch (room_light.h): not offered, the room not shown,
// ready, reading, or a reading that just failed.
enum class RoomMatch { Hidden, Off, Ready, Reading, Failed };

// A message's icon (notices.h), from the menu's set.
enum class NoticeIcon { None, Recenter, Viewpoint, Passthrough, Card, Usb, Drive, Warning };

// A gesture's value as its notice shows it (Menu::drawRingNotice()): a ring
// in the pill's round start, a knob's travel of 270° open at the bottom,
// filled clockwise to the value, and its name and number beside it.
struct RingNotice {
  // A value; a rating in ten half-star steps; a place in the video.
  enum class Kind : uint8_t { Value, Stars, Position };
  Kind kind = Kind::Value;
  // Along the ring, 0..1: the value, where its fill starts (0, or the
  // middle's for a value centred on 0, which gets a tick), its default
  // (-1: none), and Position's chapters.
  double at = 0, from = 0, mark = -1;
  bool centred = false;
  bool limit = false;  // at an end: the end brighter
  std::vector<double> ticks;
  int halves = 0;  // Stars: the rating
  // Its name, then its number in the semibold, then dim what follows it
  // ("Volume" "64 %"; Position: "12:40" "/ 48:10").
  std::string name, number, after;
  // Equal as drawn: the fill to a 300th of the ring.
  bool operator==(const RingNotice& o) const {
    const auto step = [](double x) { return std::lround(x * 300); };
    return kind == o.kind && step(at) == step(o.at) && step(from) == step(o.from) &&
           step(mark) == step(o.mark) && centred == o.centred && limit == o.limit &&
           ticks == o.ticks && halves == o.halves && name == o.name && number == o.number &&
           after == o.after;
  }
  bool operator!=(const RingNotice& o) const { return !(*this == o); }
};

// The flat screen's size (ScreenSize) as the menu shows it.
struct ScreenView {
  int distanceCm = 0, heightCm = 0, curvePercent = 0;

  static ScreenView of(const ScreenSize& size) {
    return {static_cast<int>(std::lround(size.distance * 100)),
            static_cast<int>(std::lround(size.height * 100)),
            static_cast<int>(std::lround(size.curve * 100))};
  }
  bool operator==(const ScreenView& o) const {
    return distanceCm == o.distanceCm && heightCm == o.heightCm &&
           curvePercent == o.curvePercent;
  }
  bool operator!=(const ScreenView& o) const { return !(*this == o); }
};

// Why a press while picking the background key's colour took none: the
// patch pointed at isn't one colour, or the picture can't be read.
enum class KeyPickNote { None, Uneven, Unreadable };

// What a view's pointers point at until it is filled: T's defaults, for the
// whole run.
template <class T>
const T* defaultShown() {
  static const T value{};
  return &value;
}

// Settings' Controls (menu_controls.cpp): the controller type it shows,
// what the buttons do, its gestures, and how far Skip and Big skip go. The
// buttons' and gestures' are the settings' own, by pointer (MenuViewCopies).
struct ControlsView {
  ControllerType type = ControllerType::Touch;
  const ControlMap* map = defaultShown<ControlMap>();
  const GestureMap* gestures = defaultShown<GestureMap>();
  int skipSeconds = 10, bigSkipSeconds = 60;
  bool operator==(const ControlsView& o) const {
    return type == o.type && *map == *o.map && *gestures == *o.gestures && skipSeconds == o.skipSeconds &&
           bigSkipSeconds == o.bigSkipSeconds;
  }
  bool operator!=(const ControlsView& o) const { return !(*this == o); }
};

// Everything the menu shows about the app (MenuView, below), in groups by
// what a change of a field redraws (Menu::update()). A new field goes in
// the group that says what it needs.

// A change of any of these lays the whole menu out again.
struct MenuViewLayout {
  bool halfPacked = false;  // flat 3D shown with half-squeezed eyes (chosen by hand)
  bool passthrough = false;
  bool colourKey = false;    // the running video's key is a colour key (VideoSettings::colourKeyed()) ...
  KeyColour keyColour;       // ... on this colour
  bool fillChecked = false;  // ... and the video was checked for a flat background
  bool fillChecking = false;  // ... or is being, and hasn't answered yet
  bool greenScreenFound = false;  // ... and its green screen check found one
  bool keyPicking = false;   // a colour for it is being picked: the strip asks to point at it
  KeyPickNote keyPickNote = KeyPickNote::None;  // ... and why the last press took none
  bool packedAlpha = false;  // the running video brings its own mask (no keying needed)
  bool maskFormat = false;   // its format is the one a packed mask comes in (maskFormat())
  bool maskLayers = false;   // its mask has a second layer (VideoSettings::maskLayersFound)
  bool secondLayerHidden = false;  // ... hidden by hand
  bool opacityApplies = false;  // the runtime blends the video over the room
  std::string currentFile;
  bool playbackFailed = false;  // the current video couldn't be played
  bool discEncrypted = false;   // ... because it is an encrypted disc image
  bool decoderBusy = false;     // ... because the decoder is busy (decoder_holders.h)
  bool floatingSubtitles = false;
  bool streaming = false;  // the current video is a link, not a file
  int videoWidth = 0, videoHeight = 0;
  bool background = true;  // night sky around the flat screen
  bool autoPassthrough = false;  // passthrough switches itself on for videos made for it
  bool keyGreen = false;         // a PC whose runtime doesn't blend: passthrough draws the room green
  bool passthroughAvailable = false;  // the runtime blends, or keyGreen is on in its place
  bool photo = false;        // the current file is a photo
  int slideshowSeconds = 0;  // 0 = no slideshow
  AfterVideo afterVideo = AfterVideo::Next;  // what happens when a video ends
  RoomMatch roomMatch = RoomMatch::Hidden;
  bool roomMatched = false;  // Match the room is on (applied only while the room is shown)
  bool tvGamma = false;  // SDR video converted for a TV's gamma (darker shadows)
  bool videoMipmaps = true;  // global: video sampled through mipmaps
  bool toneLook = false;     // global: tone curve and vignette
  // The Frame's foveation (FoveationLevel), where the eye images have it.
  bool foveationAvailable = false;
  FoveationLevel foveation = FoveationLevel::High;
  bool showFoveation = false;
  bool foveationLearned = false;
  int sharpBins = kDefaultSharpBins;
  bool foveationRing = kDefaultFoveationRing;
  int foveationOutside = 4;
  bool frameReuse = true;  // Frame: repeated frames not drawn, this run's (frame_reuse.h)
  bool advancedSettings = false;
  int refreshRate = 0;  // Frame: Hz, SteamVR's for this app (refresh_rate.h); 0: not changeable
  std::array<int, 9> foveationMaps{{0, 1, 2, 3, 4, 5, 6, 7, 8}};
  std::array<int, 3> sharpDegrees{};
  bool resume = true;           // videos start where they were left
  bool freeDecoder = false;     // Frame: the decoder is freed from Steam without asking
  bool seekPreview = true;      // the seek bar shows the frame where the pointer is
  bool showPerformance = false;  // the performance readout, this run's (--show-performance)
  bool vibration = true;        // controllers buzz on clicks, seeks and grabs
  bool thumbnails = true;       // file browser and library: thumbnails, else a video icon
  bool setupBanner = false;     // the Library's "You're all set" after setup (DismissSetupBanner)
  bool showWhatsNew = true;     // What's new opens after updates (Settings::showWhatsNew)
  ReleaseUpdate::State releaseUpdate = ReleaseUpdate::State::Disabled;
  std::string releaseVersion;
  DiagnosticsExport::State diagnostics = DiagnosticsExport::State::Idle;
  std::string diagnosticsFile;  // Saved: the zip's name
  bool crashedLastRun = false;  // until its diagnostics are saved
  bool softwareDecoding = false;  // the video plays without hardware decoding
  ScriptStatus scriptStatus;    // funscripts: how far the way to a device has got, and whether the video has a script
  std::string downloadsTo;      // Settings::downloadsTo
  // The strip's delete button (docs/alpha/delete-file.md): not shown
  // (nothing open, a link), shown, or shown disabled for a scene played
  // from Stash's stream: Send to Stash off, or Stash not answering.
  enum class Deletable : uint8_t { No, Yes, NeedsSendToStash, StashDown } deletable = Deletable::No;
};
// Every field, in order: one added to MenuViewLayout and not named here
// doesn't compile.
inline bool operator==(const MenuViewLayout& a, const MenuViewLayout& b) {
  const auto fields = [](const MenuViewLayout& v) {
    const auto& [halfPacked, passthrough, colourKey, keyColour, fillChecked, fillChecking,
                 greenScreenFound, keyPicking, keyPickNote, packedAlpha, maskFormat, maskLayers,
                 secondLayerHidden, opacityApplies, currentFile, playbackFailed, discEncrypted,
                 decoderBusy, floatingSubtitles, streaming, videoWidth, videoHeight, background,
                 autoPassthrough, keyGreen, passthroughAvailable, photo, slideshowSeconds, afterVideo,
                 roomMatch, roomMatched, tvGamma, videoMipmaps, toneLook, foveationAvailable, foveation,
                 showFoveation, foveationLearned, sharpBins, foveationRing, foveationOutside,
                 frameReuse, advancedSettings, refreshRate, foveationMaps, sharpDegrees, resume,
                 freeDecoder, seekPreview, showPerformance, vibration, thumbnails, setupBanner,
                 showWhatsNew, releaseUpdate, releaseVersion, diagnostics, diagnosticsFile,
                 crashedLastRun, softwareDecoding, scriptStatus, downloadsTo, deletable] = v;
    return std::tie(halfPacked, passthrough, colourKey, keyColour, fillChecked, fillChecking,
                    greenScreenFound, keyPicking, keyPickNote, packedAlpha, maskFormat, maskLayers,
                    secondLayerHidden, opacityApplies, currentFile, playbackFailed, discEncrypted,
                    decoderBusy, floatingSubtitles, streaming, videoWidth, videoHeight, background,
                    autoPassthrough, keyGreen, passthroughAvailable, photo, slideshowSeconds, afterVideo,
                    roomMatch, roomMatched, tvGamma, videoMipmaps, toneLook, foveationAvailable, foveation,
                    showFoveation, foveationLearned, sharpBins, foveationRing, foveationOutside,
                    frameReuse, advancedSettings, refreshRate, foveationMaps, sharpDegrees, resume,
                    freeDecoder, seekPreview, showPerformance, vibration, thumbnails, setupBanner,
                    showWhatsNew, releaseUpdate, releaseVersion, diagnostics, diagnosticsFile,
                    crashedLastRun, softwareDecoding, scriptStatus, downloadsTo, deletable);
  };
  return fields(a) == fields(b);
}
inline bool operator!=(const MenuViewLayout& a, const MenuViewLayout& b) { return !(a == b); }

// A change of one of these redraws only its own part: a slider's row, or
// the strip (appFps). Menu::update() names each field: a new one doesn't
// compile until it says what it redraws.
struct MenuViewRows {
  // Picture settings of the running video.
  int brightness = 0, contrast = 0, saturation = 0;
  ColourBalance balance;  // the video's, or Match the room's while it is on and applies
  int matchColour = 70, matchBrightness = 60;  // percent (Settings::matchColour)
  float zoom = 1.0f;
  int originForward = 0;  // surround viewpoint: percent of the sphere's radius toward the picture
  int opacity = 100;            // of the running video, percent
  int sharpen = 0;           // global: video sharpening in percent, 0 = off
  ScreenView screen;
  int subtitleSize = 100, subtitleRaise = 0, subtitleDistance = 150;  // see Settings
  int appFps = 0;               // frames the app drew in the last second (0 = not known yet)
};

// Shown on one page only, which compares them itself (Menu::PageKind's
// update): Video settings' Display, Focus, and the Library's banner.
struct MenuViewPage {
  // Global eye size: the choice (pixels with stops, otherwise percent),
  // and the requested and allowed scales in percent.
  int videoResolution = 100, videoResolutionActive = 100, videoResolutionEffective = 100;
  const std::vector<int>* resolutionStops = nullptr;  // Frame; owned by the run
  int resolutionOverride = 0;  // eye image pixels across this run asks for from the command line, else 0
  int resolutionBase = 0;    // eye image pixels across at 100 %
  bool resolutionNotice = false;  // 1× has changed meaning; show the migrated size
  // What the resolution, foveation, mipmaps, anti-shimmer and sharpening
  // match (presets.h), else Custom.
  PerformancePreset performancePreset = PerformancePreset::High;
  // The user's named presets as last listed (ListUserPresets), owned by the
  // run; userPresetsListed counts the listings. The one shown as chosen
  // (-1: a performance preset, or Custom), and this device's presets.
  const std::vector<UserPreset>* userPresets = nullptr;
  int userPresetsListed = 0;
  int userPresetShown = -1;
  const Presets* presets = nullptr;
  FocusView focus;  // stereo focus of the running video
  bool motionEmulation = true;  // global: head translation emulation (motion.h)
  int motionDistance = 200;     // ... cm
};

// Parts whose fields redraw in different ways: Menu::update() (and
// StashPages::update(), updateControls()) compares them a field at a time.
struct MenuViewParts {
  PlaybackState playback;
  VideoFormat format;
  GreenScreen greenScreen;   // of the running video
  Accent accent;
  ScriptSettings scripts;       // funscripts on a device (script_player.h)
  // The media server's setup in use (its pages show only while it is on):
  // the settings' own, by pointer (MenuViewCopies).
  const StashSettings* stash = defaultShown<StashSettings>();
  ControlsView controls;
};

// Read when the menu is drawn, but a change of one redraws nothing by
// itself.
struct MenuViewQuiet {
  bool hardwareAv1 = false;  // the headset's decoder takes AV1 (SteamOS 0.5.10)
  AntiShimmer antiShimmer = AntiShimmer::On;  // this run's: without them (anti_shimmer.h)
  bool seekPreviewAvailable = true;  // the seek bar's preview can show at all (not on the Steam Frame)
  unsigned serversSetUp = 0;    // a bit per ServerKind with a server set up, in use or not
};

// Everything the menu shows about the app. No fields of its own: each is in
// the group above for what a change of it redraws.
struct MenuView : MenuViewLayout, MenuViewRows, MenuViewPage, MenuViewParts, MenuViewQuiet {};

// The menu's own copies of what a view has by pointer, so that it keeps
// drawing what it was handed, as with the rest of the view; copied only
// when they change.
struct MenuViewCopies {
  StashSettings stash;
  ControlMap controls;
  GestureMap gestures;
  // `view` pointed at these.
  void point(MenuView& view) {
    view.stash = &stash;
    view.controls.map = &controls;
    view.controls.gestures = &gestures;
  }
  // `view`'s taken (where they changed), and `view` pointed at these.
  void take(MenuView& view) {
    if (*view.stash != stash) stash = *view.stash;
    if (*view.controls.map != controls) controls = *view.controls.map;
    if (*view.controls.gestures != gestures) gestures = *view.controls.gestures;
    point(view);
  }
};
