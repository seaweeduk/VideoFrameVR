#pragma once

#include "colour_balance.h"
#include "flat_screen.h"
#include "focus_settings.h"
#include "video_format.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>

// A colour the background key removes: 8-bit display values of the decoded
// picture as it came, before TV gamma (which seeThrough() converts it for)
// and any picture settings.
struct KeyColour {
  uint8_t r = 0, g = 0, b = 0;
  bool operator==(const KeyColour& o) const { return r == o.r && g == o.g && b == o.b; }
  bool operator!=(const KeyColour& o) const { return !(*this == o); }
};

// What the background key removes (stored by number: new values go at the
// end): Automatic is a flat colour the fill check found in the video
// (VideoSettings::fillFound), else a green screen the green screen check
// found, else nothing; or the green screen, or a colour the user picked.
enum class KeySource { Automatic, GreenScreen, Picked, kCount };

// A background key's sliders, 0..100, scaled to each key (SceneRenderer::drawVideo).
struct KeySliders {
  int strength = 0;  // how much is cut away: more also eats into edges
  // How gradual the edge is. Soft edges are half green, and after the
  // stream's compression the headset's keyer turns them into a green or
  // yellow fringe, so this stays small.
  int softness = 0;
  int spill = 0;     // how much of the background's tint is taken out of what stays
  int shrink = 0;    // how far the cut moves into the subject, past what is keyed
  bool operator==(const KeySliders& o) const {
    return strength == o.strength && softness == o.softness && spill == o.spill &&
           shrink == o.shrink;
  }
  bool operator!=(const KeySliders& o) const { return !(*this == o); }
};

// Removing a video's background in passthrough: the player cuts it out
// itself (the headset only sees the result), with the green keyer (a green
// screen, of any shade) or a colour key (one flat colour, exactly), each
// with sliders of its own.
struct GreenScreen {
  bool on = false;
  KeySource source = KeySource::Automatic;
  KeyColour picked;   // KeySource::Picked's
  // Real rooms vary, so the green keyer errs on cutting enough; software
  // fills often fade into the subject through a halo of the fill's colour,
  // which the colour key cuts into.
  KeySliders green{55, 0, 55, 0};
  KeySliders colour{60, 25, 20, 20};

  KeySliders& sliders(bool colourKey) { return colourKey ? colour : green; }
  const KeySliders& sliders(bool colourKey) const { return colourKey ? colour : green; }
  // Nothing chosen for the key in use: off, Automatic, its sliders at their defaults.
  bool isDefault(bool colourKey) const {
    return !on && source == KeySource::Automatic &&
           sliders(colourKey) == GreenScreen().sliders(colourKey);
  }
  bool operator==(const GreenScreen& o) const {
    return on == o.on && source == o.source &&
           (source != KeySource::Picked || picked == o.picked) && green == o.green &&
           colour == o.colour;
  }
  bool operator!=(const GreenScreen& o) const { return !(*this == o); }
};

// What the player remembers about one video.
struct VideoSettings {
  bool hasFormat = false;  // the format was chosen by hand
  VideoFormat format;
  int brightness = 0, contrast = 0, saturation = 0;  // mpv's -100..100
  // Warmth, tint and exposure: Match the room's, while it is on, are never
  // stored here.
  ColourBalance balance;
  float zoom = 1.0f;  // 180°/360° video: magnification
  float originForward = 0;  // surround viewpoint, in sphere radii toward the picture
  int opacity = 100;  // percent, over the room (blended by the headset itself)
  // Persisted control range. The combined origin and head motion are eased
  // inside the sphere by stereo_correction.cpp, not clamped at this value.
  static constexpr float kMaxOriginForward = 0.8f;
  bool passthrough = false;
  // Passthrough was set by hand for this video, on or off: it is then never
  // switched automatically (autoPassthrough(), see_through.h).
  bool passthroughChosen = false;
  // What the green screen check found (video_check.h), and the
  // version of it that looked; 0 = not checked.
  int greenCheckVersion = 0;
  bool greenScreenFound = false;
  // What the fill check found (a flat background colour, for the key's
  // Automatic), and the version of it that looked; 0 = not checked.
  int fillCheckVersion = 0;
  bool fillFound = false;
  KeyColour fillColour;
  // The packed mask (DeoVR alpha) chosen by hand for this video; else the
  // file name decides (packedMask(), see_through.h).
  std::optional<bool> packedMask;
  // Whether the packed mask has a second layer, in blue (SLR's layered
  // masks): found by the keyframe check (video_check.h, which stores its
  // version; 0 = not checked) or while the video played (MaskProbe), and
  // then never looked for again.
  int maskLayersVersion = 0;
  bool maskLayersFound = false;
  bool secondLayerHidden = false;  // ... hidden by hand (it shows otherwise)
  std::optional<bool> legacyTvGamma;  // kept in the library, no longer applied
  // A volume stored by older versions for this video (-1 none): kept as it
  // is, no longer applied (the volume is a setting for all videos).
  int volume = -1;
  GreenScreen greenScreen;
  bool hasFocus = false;  // stereo focus set for this video, else the defaults apply
  FocusSettings focus;
  // A flat screen size stored by older versions for this video: kept as it
  // is, no longer applied (the size is a setting for all videos).
  bool hasScreen = false;
  ScreenSize screen;

  // Whether the background key is a colour key, and on what colour; else
  // it is the green keyer.
  bool colourKeyed() const {
    return greenScreen.source == KeySource::Picked ||
           (greenScreen.source == KeySource::Automatic && fillCheckVersion > 0 && fillFound);
  }
  KeyColour keyColour() const {
    return greenScreen.source == KeySource::Picked ? greenScreen.picked : fillColour;
  }
  // Whether the key removes anything: Automatic doesn't until a check
  // found a flat colour or a green screen.
  bool keyRemoves() const {
    return greenScreen.source != KeySource::Automatic || colourKeyed() ||
           (greenCheckVersion > 0 && greenScreenFound);
  }
  // The sliders of the key in use.
  KeySliders& keySliders() { return greenScreen.sliders(colourKeyed()); }
  const KeySliders& keySliders() const { return greenScreen.sliders(colourKeyed()); }

  bool pictureIsDefault() const {
    return brightness == 0 && contrast == 0 && saturation == 0 && balance.neutral() &&
           zoom == 1.0f;
  }
  // Nothing remembered: the video follows the defaults.
  bool isDefault() const {
    return !hasFormat && pictureIsDefault() && volume < 0 && greenScreen == GreenScreen() &&
           !hasFocus && !hasScreen && !passthrough && !passthroughChosen && originForward == 0 &&
           greenCheckVersion == 0 && fillCheckVersion == 0 && !packedMask && opacity == 100 &&
           maskLayersVersion == 0 &&
           !maskLayersFound && !secondLayerHidden;
  }

  // The parts the user changes one at a time.
  enum Part : unsigned { kFormat = 1, kBrightness = 2, kContrast = 4, kSaturation = 8, kZoom = 16,
                         kVolume = 32, kGreenScreen = 64, kFocus = 128, kPassthrough = 256,
                         kScreen = 512, kOrigin = 1024, kGreenCheck = 2048, kPackedMask = 4096,
                         kOpacity = 8192, kWarmth = 16384, kTint = 32768, kExposure = 65536,
                         kMaskLayers = 131072, kSecondLayer = 262144, kFillCheck = 524288,
                         kAll = 1048575,
                         kBalance = kWarmth | kTint | kExposure,
                         kPicture = kBrightness | kContrast | kSaturation | kBalance | kZoom };
  // Takes these parts from `other`.
  void take(const VideoSettings& other, unsigned parts) {
    if (parts & kFormat) {
      hasFormat = other.hasFormat;
      format = other.format;
    }
    if (parts & kBrightness) brightness = other.brightness;
    if (parts & kContrast) contrast = other.contrast;
    if (parts & kSaturation) saturation = other.saturation;
    if (parts & kWarmth) balance.warmth = other.balance.warmth;
    if (parts & kTint) balance.tint = other.balance.tint;
    if (parts & kExposure) balance.exposure = other.balance.exposure;
    if (parts & kZoom) zoom = other.zoom;
    if (parts & kOrigin) originForward = other.originForward;
    if (parts & kPassthrough) {
      passthrough = other.passthrough;
      passthroughChosen = other.passthroughChosen;
    }
    if (parts & kGreenCheck) {
      greenCheckVersion = other.greenCheckVersion;
      greenScreenFound = other.greenScreenFound;
    }
    if (parts & kFillCheck) {
      fillCheckVersion = other.fillCheckVersion;
      fillFound = other.fillFound;
      fillColour = other.fillColour;
    }
    if (parts & kPackedMask) packedMask = other.packedMask;
    if (parts & kMaskLayers) {
      maskLayersVersion = other.maskLayersVersion;
      maskLayersFound = other.maskLayersFound;
    }
    if (parts & kSecondLayer) secondLayerHidden = other.secondLayerHidden;
    if (parts & kOpacity) opacity = other.opacity;
    if (parts & kVolume) volume = other.volume;
    if (parts & kGreenScreen) greenScreen = other.greenScreen;
    if (parts & kFocus) {
      hasFocus = other.hasFocus;
      focus = other.focus;
    }
    if (parts & kScreen) {
      hasScreen = other.hasScreen;
      screen = other.screen;
    }
  }
  bool operator==(const VideoSettings& o) const {
    return hasFormat == o.hasFormat && format.projection == o.format.projection &&
           format.stereo == o.format.stereo && format.packing == o.format.packing &&
           brightness == o.brightness && contrast == o.contrast && saturation == o.saturation &&
           balance == o.balance &&
           zoom == o.zoom && passthrough == o.passthrough && volume == o.volume &&
           greenScreen == o.greenScreen &&
           hasFocus == o.hasFocus && focus == o.focus && hasScreen == o.hasScreen &&
           screen == o.screen && originForward == o.originForward &&
           passthroughChosen == o.passthroughChosen && greenCheckVersion == o.greenCheckVersion &&
           greenScreenFound == o.greenScreenFound && fillCheckVersion == o.fillCheckVersion &&
           fillFound == o.fillFound && fillColour == o.fillColour && packedMask == o.packedMask &&
           opacity == o.opacity && maskLayersVersion == o.maskLayersVersion &&
           maskLayersFound == o.maskLayersFound && secondLayerHidden == o.secondLayerHidden;
  }
  bool operator!=(const VideoSettings& o) const { return !(*this == o); }
};

// Per-video settings as earlier versions kept them, in videos.conf in
// configDir(), by a hash of the path or link (earlier versions hashed
// "youtube:<id>" for YouTube videos; those entries are never asked for now).
// Only read now, to carry them over into the library (library.h); the file
// is never written again.
class VideoMemory {
 public:
  void load();
  // False if nothing is remembered for the video.
  bool get(const std::string& location, VideoSettings& settings) const;

 private:
  std::map<std::string, VideoSettings> entries_;  // by hash
};

// How the file browser shows a folder (its order and list or gallery), in
// folders.conf in configDir(); folders are stored only as hashes too.
struct FolderView {
  int sort = 0;  // Menu's SortMode
  bool gallery = false;
};

class FolderMemory {
 public:
  void load();
  // False if nothing is remembered for the folder.
  bool get(const std::string& folder, FolderView& view) const;
  void set(const std::string& folder, const FolderView& view);

 private:
  void save() const;
  std::map<std::string, FolderView> entries_;  // by hash
};
