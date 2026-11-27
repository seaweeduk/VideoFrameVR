#include "menu_command.h"

#include "control_map.h"
#include "focus_settings.h"
#include "refresh_rate.h"
#include "server_kind.h"
#include "stash_settings.h"
#include "value_ranges.h"
#include "video_format.h"

#include <algorithm>
#include <cstring>
#include <iterator>

const char* commandName(MenuCommand::Type type) {
  using C = MenuCommand;
  switch (type) {
    case C::None: return "None";
    case C::TogglePause: return "TogglePause";
    case C::Seek: return "Seek";
    case C::SeekTo: return "SeekTo";
    case C::ToggleMute: return "ToggleMute";
    case C::PreviousVideo: return "PreviousVideo";
    case C::NextVideo: return "NextVideo";
    case C::SetProjection: return "SetProjection";
    case C::SetStereo: return "SetStereo";
    case C::Recenter: return "Recenter";
    case C::OpenFile: return "OpenFile";
    case C::SelectAudio: return "SelectAudio";
    case C::SelectSubtitle: return "SelectSubtitle";
    case C::LoadSubtitle: return "LoadSubtitle";
    case C::ToggleFloatingSubtitles: return "ToggleFloatingSubtitles";
    case C::SetVolume: return "SetVolume";
    case C::RequestDelete: return "RequestDelete";
    case C::DeleteFile: return "DeleteFile";
    case C::CancelDelete: return "CancelDelete";
    case C::Quit: return "Quit";
    case C::SetBrightness: return "SetBrightness";
    case C::SetContrast: return "SetContrast";
    case C::SetSaturation: return "SetSaturation";
    case C::SetZoom: return "SetZoom";
    case C::ResetPicture: return "ResetPicture";
    case C::Paste: return "Paste";
    case C::SetAccent: return "SetAccent";
    case C::SetAccentHue: return "SetAccentHue";
    case C::SetAccentBrightness: return "SetAccentBrightness";
    case C::SetAccentSaturation: return "SetAccentSaturation";
    case C::SetPointerColour: return "SetPointerColour";
    case C::SetPointerHue: return "SetPointerHue";
    case C::SetSlideshow: return "SetSlideshow";
    case C::SetAfterVideo: return "SetAfterVideo";
    case C::ToggleBackground: return "ToggleBackground";
    case C::SetSubtitleSize: return "SetSubtitleSize";
    case C::SetSubtitleRaise: return "SetSubtitleRaise";
    case C::SetSubtitleDistance: return "SetSubtitleDistance";
    case C::TogglePassthrough: return "TogglePassthrough";
    case C::ToggleGreenScreen: return "ToggleGreenScreen";
    case C::SetKeyStrength: return "SetKeyStrength";
    case C::SetKeySoftness: return "SetKeySoftness";
    case C::SetKeySpill: return "SetKeySpill";
    case C::SetKeyShrink: return "SetKeyShrink";
    case C::SetKeyBackground: return "SetKeyBackground";
    case C::PickKeyColour: return "PickKeyColour";
    case C::CancelKeyPick: return "CancelKeyPick";
    case C::ResetPassthrough: return "ResetPassthrough";
    case C::SetPacking: return "SetPacking";
    case C::ToggleVideoMipmaps: return "ToggleVideoMipmaps";
    case C::SetAntiShimmer: return "SetAntiShimmer";
    case C::SetSharpen: return "SetSharpen";
    case C::SetVideoResolution: return "SetVideoResolution";
    case C::SetFoveation: return "SetFoveation";
    case C::ToggleShowFoveation: return "ToggleShowFoveation";
    case C::ToggleFoveationLearned: return "ToggleFoveationLearned";
    case C::SetSharpBins: return "SetSharpBins";
    case C::ToggleFoveationRing: return "ToggleFoveationRing";
    case C::SetFoveationOutside: return "SetFoveationOutside";
    case C::ToggleAdvancedSettings: return "ToggleAdvancedSettings";
    case C::SetRefreshRate: return "SetRefreshRate";
    case C::SetFocus: return "SetFocus";
    case C::ToggleMotion: return "ToggleMotion";
    case C::SetMotionDistance: return "SetMotionDistance";
    case C::ToggleResume: return "ToggleResume";
    case C::ToggleToneLook: return "ToggleToneLook";
    case C::ToggleFrameReuse: return "ToggleFrameReuse";
    case C::Stop: return "Stop";
    case C::ToggleTvGamma: return "ToggleTvGamma";
    case C::ToggleSeekPreview: return "ToggleSeekPreview";
    case C::ToggleSeekPreview3d: return "ToggleSeekPreview3d";
    case C::ToggleThumbnails: return "ToggleThumbnails";
    case C::ToggleShowPerformance: return "ToggleShowPerformance";
    case C::SetScreenDistance: return "SetScreenDistance";
    case C::SetScreenHeight: return "SetScreenHeight";
    case C::SetScreenCurve: return "SetScreenCurve";
    case C::ResetScreen: return "ResetScreen";
    case C::SetOriginForward: return "SetOriginForward";
    case C::SetOpacity: return "SetOpacity";
    case C::SetWarmth: return "SetWarmth";
    case C::SetTint: return "SetTint";
    case C::SetExposure: return "SetExposure";
    case C::ToggleMatchRoom: return "ToggleMatchRoom";
    case C::ReadRoomAgain: return "ReadRoomAgain";
    case C::SetMatchColour: return "SetMatchColour";
    case C::SetMatchBrightness: return "SetMatchBrightness";
    case C::ToggleAutoPassthrough: return "ToggleAutoPassthrough";
    case C::ToggleKeyGreen: return "ToggleKeyGreen";
    case C::ToggleVibration: return "ToggleVibration";
    case C::UseServer: return "UseServer";
    case C::SetStashServer: return "SetStashServer";
    case C::SetStashFolders: return "SetStashFolders";
    case C::ToggleStashCovers: return "ToggleStashCovers";
    case C::ToggleStashTitles: return "ToggleStashTitles";
    case C::ToggleStashMalePerformers: return "ToggleStashMalePerformers";
    case C::SyncStash: return "SyncStash";
    case C::SetStashFormat: return "SetStashFormat";
    case C::ToggleStashWriteBack: return "ToggleStashWriteBack";
    case C::SetStashSections: return "SetStashSections";
    case C::SetStashScenesSort: return "SetStashScenesSort";
    case C::SetStashPerformersSort: return "SetStashPerformersSort";
    case C::SetStashStudiosSort: return "SetStashStudiosSort";
    case C::SetLanguage: return "SetLanguage";
    case C::DownloadReleaseUpdate: return "DownloadReleaseUpdate";
    case C::FreeDecoder: return "FreeDecoder";
    case C::ToggleFreeDecoder: return "ToggleFreeDecoder";
    case C::SaveDiagnostics: return "SaveDiagnostics";
    case C::SetDownloadsTo: return "SetDownloadsTo";
    case C::ApplyPreset: return "ApplyPreset";
    case C::ListUserPresets: return "ListUserPresets";
    case C::SaveUserPreset: return "SaveUserPreset";
    case C::ApplyUserPreset: return "ApplyUserPreset";
    case C::RenameUserPreset: return "RenameUserPreset";
    case C::DeleteUserPreset: return "DeleteUserPreset";
    case C::MoveUserPreset: return "MoveUserPreset";
    case C::TogglePackedMask: return "TogglePackedMask";
    case C::ToggleSecondLayer: return "ToggleSecondLayer";
    case C::SetKeyboardLayout: return "SetKeyboardLayout";
    case C::EndSetup: return "EndSetup";
    case C::SetupPlaces: return "SetupPlaces";
    case C::SetupStash: return "SetupStash";
    case C::StashCard: return "StashCard";
    case C::DismissSetupBanner: return "DismissSetupBanner";
    case C::ToggleShowWhatsNew: return "ToggleShowWhatsNew";
    case C::ToggleScripts: return "ToggleScripts";
    case C::SetScriptAddress: return "SetScriptAddress";
    case C::SetScriptOffset: return "SetScriptOffset";
    case C::SetScriptRangeLow: return "SetScriptRangeLow";
    case C::SetScriptRangeHigh: return "SetScriptRangeHigh";
    case C::SetButtonJob: return "SetButtonJob";
    case C::ResetButtons: return "ResetButtons";
    case C::SetSkipSeconds: return "SetSkipSeconds";
    case C::SetBigSkipSeconds: return "SetBigSkipSeconds";
    case C::RestoreButton: return "RestoreButton";
    case C::AddGesture: return "AddGesture";
    case C::ChangeGesture: return "ChangeGesture";
    case C::RemoveGesture: return "RemoveGesture";
    case C::RestoreGestures: return "RestoreGestures";
    case C::ToggleMenu: return "ToggleMenu";
    case C::SetPaused: return "SetPaused";
    case C::OpenFileAt: return "OpenFileAt";
    case C::FrameStep: return "FrameStep";
    case C::StepRating: return "StepRating";
    case C::ToggleRemoteControl: return "ToggleRemoteControl";
  }
  return nullptr;
}

bool commandType(const char* name, MenuCommand::Type& type) {
  for (int t = 0; const char* n = commandName(static_cast<MenuCommand::Type>(t)); ++t)
    if (std::strcmp(n, name) == 0) {
      type = static_cast<MenuCommand::Type>(t);
      return true;
    }
  return false;
}

namespace {

bool within(double v, double least, double most) { return v >= least && v <= most; }
bool within(double v, const ValueRange& r) { return within(v, r.least, r.most); }
bool indexBelow(double v, int count) { return v >= 0 && v < count; }

bool slotValid(int packed) {
  const ControlSlot slot = ControlSlot::unpack(packed);
  return slot.type < ControllerType::Count && slot.control < Control::Count;
}

// option: the ControllerType << 17 | Gesture::pack().
bool gestureValid(int option) {
  const int type = option >> 17;
  const Gesture g = Gesture::unpack(option & 0x1ffff);
  return type >= 0 && type < kControllerTypes && g.first < Control::Count && g.second <= Control::Count &&
         g.movement < Movement::Count && g.value < GestureValue::Count;
}

}  // namespace

bool commandInRange(const MenuCommand& c) {
  using C = MenuCommand;
  const double v = c.value;
  switch (c.type) {
    case C::SetProjection:
      return indexBelow(v, static_cast<int>(Projection::kCount));
    case C::SetStereo:
      return indexBelow(v, static_cast<int>(StereoLayout::kCount));
    case C::SetVolume:
      return within(v, value_range::kKeptVolume);
    case C::DeleteFile:
      return v == 0 || v == 1;
    case C::SetBrightness: case C::SetContrast: case C::SetSaturation:
      return within(v, value_range::kPicture);
    case C::SetZoom:
      return within(v, value_range::kZoom);
    case C::SetSlideshow:
      return v >= 0;
    case C::SetSubtitleSize:
      return within(v, value_range::kSubtitleSize);
    case C::SetSubtitleRaise:
      return within(v, value_range::kSubtitleRaise);
    case C::SetSubtitleDistance:
      return within(v, value_range::kSubtitleDistance);
    case C::SetKeyStrength: case C::SetKeySoftness: case C::SetKeySpill: case C::SetKeyShrink:
      return within(v, value_range::kPercent);
    case C::SetPacking:
      return indexBelow(v, static_cast<int>(Packing::kCount));
    case C::SetRefreshRate:
      return std::find(std::begin(kRefreshRates), std::end(kRefreshRates), v) != std::end(kRefreshRates);
    case C::SetFocus:
      return c.option >= 0 && c.option <= static_cast<int>(FocusControl::Enabled);
    case C::SetScreenDistance:
      return within(v, value_range::kScreenDistance);
    case C::SetScreenHeight:
      return within(v, value_range::kScreenHeight);
    case C::SetScreenCurve:
      return within(v, value_range::kScreenCurve);
    case C::SetOpacity:
      return within(v, value_range::kPercent);
    case C::UseServer:
      return within(v, -1, static_cast<int>(ServerKind::Xbvr));
    case C::SetStashServer: case C::SetupStash:
      return within(v, 0, static_cast<int>(ServerKind::Xbvr));
    case C::SetStashFormat:
      return within(v, 0, static_cast<int>(StashFormat::Flat));
    case C::SetButtonJob:
      return slotValid(c.option) && within(v, -1, kJobs - 1);
    case C::RestoreButton:
      return slotValid(c.option);
    case C::AddGesture: case C::ChangeGesture:
      return gestureValid(c.option);
    // Any value: none, or one its handler keeps in range itself.
    case C::None: case C::TogglePause: case C::Seek: case C::SeekTo: case C::ToggleMute:
    case C::PreviousVideo: case C::NextVideo: case C::Recenter: case C::OpenFile:
    case C::SelectAudio: case C::SelectSubtitle: case C::LoadSubtitle:
    case C::ToggleFloatingSubtitles: case C::RequestDelete: case C::CancelDelete: case C::Quit:
    case C::ResetPicture: case C::Paste: case C::SetAccent: case C::SetAccentHue:
    case C::SetAccentBrightness: case C::SetAccentSaturation: case C::SetPointerColour:
    case C::SetPointerHue: case C::SetAfterVideo:
    case C::ToggleBackground: case C::TogglePassthrough: case C::ToggleGreenScreen:
    case C::SetKeyBackground: case C::PickKeyColour: case C::CancelKeyPick:
    case C::ResetPassthrough: case C::ToggleVideoMipmaps: case C::SetAntiShimmer:
    case C::SetSharpen: case C::SetVideoResolution: case C::SetFoveation:
    case C::ToggleShowFoveation: case C::ToggleFoveationLearned: case C::SetSharpBins:
    case C::ToggleFoveationRing: case C::SetFoveationOutside: case C::ToggleAdvancedSettings:
    case C::ToggleMotion: case C::SetMotionDistance: case C::ToggleResume: case C::ToggleToneLook:
    case C::ToggleFrameReuse: case C::Stop: case C::ToggleTvGamma: case C::ToggleSeekPreview:
    case C::ToggleSeekPreview3d: case C::ToggleThumbnails: case C::ToggleShowPerformance: case C::ResetScreen:
    case C::SetOriginForward: case C::SetWarmth: case C::SetTint: case C::SetExposure:
    case C::ToggleMatchRoom: case C::ReadRoomAgain: case C::SetMatchColour:
    case C::SetMatchBrightness: case C::ToggleAutoPassthrough: case C::ToggleKeyGreen:
    case C::ToggleVibration: case C::SetStashFolders: case C::ToggleStashCovers:
    case C::ToggleStashTitles: case C::ToggleStashMalePerformers: case C::SyncStash:
    case C::ToggleStashWriteBack: case C::SetStashSections: case C::SetStashScenesSort:
    case C::SetStashPerformersSort: case C::SetStashStudiosSort: case C::SetLanguage:
    case C::DownloadReleaseUpdate: case C::FreeDecoder: case C::ToggleFreeDecoder:
    case C::SaveDiagnostics: case C::SetDownloadsTo: case C::ApplyPreset: case C::ListUserPresets:
    case C::SaveUserPreset: case C::ApplyUserPreset: case C::RenameUserPreset:
    case C::DeleteUserPreset: case C::MoveUserPreset: case C::TogglePackedMask:
    case C::ToggleSecondLayer: case C::SetKeyboardLayout: case C::EndSetup: case C::SetupPlaces:
    case C::StashCard: case C::DismissSetupBanner: case C::ToggleShowWhatsNew:
    case C::ToggleScripts: case C::SetScriptAddress: case C::SetScriptOffset:
    case C::SetScriptRangeLow: case C::SetScriptRangeHigh: case C::ResetButtons:
    case C::SetSkipSeconds: case C::SetBigSkipSeconds: case C::RemoveGesture:
    case C::RestoreGestures: case C::ToggleMenu: case C::SetPaused: case C::OpenFileAt:
    case C::FrameStep: case C::StepRating: case C::ToggleRemoteControl:
      return true;
  }
  return false;
}
