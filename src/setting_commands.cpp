#include "setting_commands.h"

#include "menu_command.h"
#include "motion.h"
#include "settings.h"

#include <algorithm>
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
