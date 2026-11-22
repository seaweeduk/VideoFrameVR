#include "setting_commands.h"

#include "control_map.h"
#include "menu_command.h"
#include "menu_view.h"
#include "motion.h"
#include "settings.h"
#include "value_ranges.h"

#include <algorithm>
#include <cmath>
#include <string>

bool applySettingCommand(const MenuCommand& c, Settings& settings) {
  switch (c.type) {
    case MenuCommand::SetAccent:
    case MenuCommand::SetAccentHue:
    case MenuCommand::SetAccentBrightness:
    case MenuCommand::SetAccentSaturation: {
      Accent& accent = settings.accent;
      const int value = static_cast<int>(c.value);
      if (c.type == MenuCommand::SetAccent) accent.preset = std::clamp(value, 0, kAccentCustom);
      else if (c.type == MenuCommand::SetAccentHue) accent.hue = (value % 360 + 360) % 360;
      else if (c.type == MenuCommand::SetAccentBrightness) accent.brightness = std::clamp(value, 0, 100);
      else accent.saturation = std::clamp(value, 0, 100);
      saveSettings(settings);
      break;
    }
    case MenuCommand::ToggleBackground:
      settings.background = !settings.background;
      saveSettings(settings);
      break;
    case MenuCommand::ToggleAdvancedSettings:
      settings.advancedSettings = !settings.advancedSettings;
      saveSettings(settings);
      break;
    case MenuCommand::StashCard:
      // The menu's "Use Stash?" card, gone for good: dismissed, or used
      // (Settings open where a server is chosen).
      settings.stash.card = false;
      saveSettings(settings);
      break;
    case MenuCommand::SetKeyboardLayout: {
      const size_t line = c.path.find('\n');
      settings.keyboardLayout = c.path.substr(0, line);
      settings.keyboardOther = line == std::string::npos ? "" : c.path.substr(line + 1);
      saveSettings(settings);
      break;
    }
    case MenuCommand::ToggleMotion:
      settings.motionEmulation = !settings.motionEmulation;
      saveSettings(settings);
      break;
    case MenuCommand::SetMotionDistance:
      settings.motionDistance = clampMotionDistance(static_cast<int>(c.value));
      saveSettings(settings);
      break;
    case MenuCommand::ToggleResume:
      settings.resume = !settings.resume;
      saveSettings(settings);
      break;
    case MenuCommand::SetDownloadsTo:
      settings.downloadsTo = c.path;
      saveSettings(settings);
      break;
    case MenuCommand::ToggleThumbnails:
      settings.thumbnails = !settings.thumbnails;
      saveSettings(settings);
      break;
    case MenuCommand::SetSkipSeconds:
      settings.skipSeconds = kSkipRange.snap(c.value);
      saveSettings(settings);
      break;
    case MenuCommand::SetBigSkipSeconds:
      settings.bigSkipSeconds = kBigSkipRange.snap(c.value);
      saveSettings(settings);
      break;
    case MenuCommand::ToggleVibration:
      settings.vibration = !settings.vibration;
      saveSettings(settings);
      break;
    case MenuCommand::ToggleShowWhatsNew:
      settings.showWhatsNew = !settings.showWhatsNew;
      saveSettings(settings);
      break;
    case MenuCommand::ToggleFloatingSubtitles:
      settings.floatingSubtitles = !settings.floatingSubtitles;
      saveSettings(settings);
      break;
    case MenuCommand::SetStashSections:
      readServerSetting("filters", c.path, settings.stash);
      saveSettings(settings);
      break;
    case MenuCommand::SetStashFormat:
      settings.stash.format = static_cast<StashFormat>(static_cast<int>(c.value));
      saveSettings(settings);
      break;
    case MenuCommand::SetStashScenesSort:
      readServerSetting("scenes_sort", c.path, settings.stash);
      settings.stash.scenesDescending = c.value != 0;
      saveSettings(settings);
      break;
    case MenuCommand::SetStashPerformersSort:
      readServerSetting("performers_sort", c.path, settings.stash);
      settings.stash.performersDescending = c.value != 0;
      saveSettings(settings);
      break;
    case MenuCommand::SetStashStudiosSort:
      readServerSetting("studios_sort", c.path, settings.stash);
      settings.stash.studiosDescending = c.value != 0;
      saveSettings(settings);
      break;
    case MenuCommand::ToggleFreeDecoder:
      settings.freeDecoder = !settings.freeDecoder;
      saveSettings(settings);
      break;
    default: return false;
  }
  return true;
}

bool changeControls(const MenuCommand& c, Settings& settings) {
  switch (c.type) {
    case MenuCommand::SetButtonJob: {
      const ControlSlot slot = ControlSlot::unpack(c.option);
      const int job = static_cast<int>(std::lround(c.value));  // -1 with grip held: none of its own
      if (slot.type == ControllerType::Count || slot.hand > 1 || job >= kJobs) return false;
      ControlMap& map = settings.controls;
      const bool done =
          job < 0 ? slot.grip && map.clearGrip(slot.type, slot.hand, slot.control)
                  : (slot.grip || map.allowed(slot.type, slot.hand, slot.control, static_cast<Job>(job))) &&
                        map.set(slot, static_cast<Job>(job));
      if (!done) return false;
      break;
    }
    case MenuCommand::RestoreButton: {
      const ControlSlot slot = ControlSlot::unpack(c.option);
      const int button = static_cast<int>(std::lround(c.value));
      ControlMap& map = settings.controls;
      if (slot.type == ControllerType::Count || slot.hand > 1 || button < 0 ||
          !map.allowed(slot.type, slot.hand, slot.control, static_cast<Job>(button % kJobs)) ||
          !map.restore(slot.type, slot.hand, slot.control, button))
        return false;
      break;
    }
    case MenuCommand::ResetButtons: {
      const int type = static_cast<int>(std::lround(c.value));
      if (type < 0 || type >= kControllerTypes) return false;
      settings.controls.reset(static_cast<ControllerType>(type));
      settings.gestures.reset(static_cast<ControllerType>(type));
      break;
    }
    case MenuCommand::AddGesture:
    case MenuCommand::ChangeGesture:
    case MenuCommand::RemoveGesture:
    case MenuCommand::RestoreGestures: {
      const bool packed = c.type == MenuCommand::AddGesture || c.type == MenuCommand::ChangeGesture;
      const int type = packed ? c.option >> 17 : c.option;
      if (type < 0 || type >= kControllerTypes) return false;
      const ControllerType t = static_cast<ControllerType>(type);
      const size_t index = static_cast<size_t>(std::max(0L, std::lround(c.value)));
      GestureMap& map = settings.gestures;
      bool done = true;
      if (c.type == MenuCommand::AddGesture) done = map.add(t, Gesture::unpack(c.option & 0x1ffff));
      else if (c.type == MenuCommand::ChangeGesture) done = map.replace(t, index, Gesture::unpack(c.option & 0x1ffff));
      else if (c.type == MenuCommand::RemoveGesture) done = map.remove(t, index);
      else map.read(t, c.path);
      if (!done) return false;
      break;
    }
    default: return false;
  }
  saveSettings(settings);
  return true;
}

void changeScripts(const MenuCommand& c, Settings& settings) {
  ScriptSettings& s = settings.scripts;
  const int value = static_cast<int>(std::lround(c.value));
  if (c.type == MenuCommand::ToggleScripts) {
    s.enabled = !s.enabled;
  } else if (c.type == MenuCommand::SetScriptAddress) {
    const size_t first = c.path.find_first_not_of(" \t");
    s.address = first == std::string::npos
                    ? std::string()
                    : c.path.substr(first, c.path.find_last_not_of(" \t") - first + 1);
  } else if (c.type == MenuCommand::SetScriptOffset) {
    s.offsetMs = std::clamp(value, -kScriptOffsetLimit, kScriptOffsetLimit);
  } else if (c.type == MenuCommand::SetScriptRangeLow) {
    // The other end keeps its distance.
    s.rangeLow = std::clamp(value, 0, 100 - kScriptRangeGap);
    s.rangeHigh = std::max(s.rangeHigh, s.rangeLow + kScriptRangeGap);
  } else {
    s.rangeHigh = std::clamp(value, kScriptRangeGap, 100);
    s.rangeLow = std::min(s.rangeLow, s.rangeHigh - kScriptRangeGap);
  }
  saveSettings(settings);
}

void fillSettings(MenuView& view, const Settings& settings) {
  view.scripts = settings.scripts;
  view.accent = settings.accent;
  view.floatingSubtitles = settings.floatingSubtitles;
  view.background = settings.background;
  view.subtitleSize = settings.subtitleSize;
  view.subtitleRaise = settings.subtitleRaise;
  view.subtitleDistance = settings.subtitleDistance;
  view.advancedSettings = settings.advancedSettings;
  view.motionEmulation = settings.motionEmulation;
  view.motionDistance = settings.motionDistance;
  view.resume = settings.resume;
  view.freeDecoder = settings.freeDecoder;
  view.downloadsTo = settings.downloadsTo;
  view.seekPreview = settings.seekPreview;
  view.thumbnails = settings.thumbnails;
  view.vibration = settings.vibration;
  view.controls.map = settings.controls;
  view.controls.gestures = settings.gestures;
  view.controls.skipSeconds = settings.skipSeconds;
  view.controls.bigSkipSeconds = settings.bigSkipSeconds;
  view.showWhatsNew = settings.showWhatsNew;
}
