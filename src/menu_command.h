#pragma once

#include <string>

// Something the menu asks the app to do.
struct MenuCommand {
  enum Type { None, TogglePause, Seek, SeekTo, ToggleMute, PreviousVideo, NextVideo,
              SetProjection, SetStereo, Recenter, OpenFile, SelectAudio,
              SelectSubtitle, LoadSubtitle, ToggleFloatingSubtitles,
              SetVolume, RequestDelete,
              DeleteFile, CancelDelete, Quit, SetBrightness,
              SetContrast, SetSaturation, SetZoom, ResetPicture, Paste,
              SetAccent, SetAccentHue, SetAccentBrightness, SetAccentSaturation,
              SetSlideshow, SetAfterVideo, ToggleBackground, SetSubtitleSize,
              SetSubtitleRaise, SetSubtitleDistance, TogglePassthrough, ToggleGreenScreen,
              SetKeyStrength, SetKeySoftness, SetKeySpill, SetKeyShrink,
              SetKeyBackground,  // the key removes this KeySource (Automatic or GreenScreen)
              PickKeyColour, CancelKeyPick,  // ... or a colour pointed at: start or cancel that
              ResetPassthrough,
              SetPacking, ToggleVideoMipmaps, SetAntiShimmer, SetSharpen, SetVideoResolution,
              SetFoveation, ToggleShowFoveation, ToggleFoveationLearned, SetSharpBins, ToggleFoveationRing,
              SetFoveationOutside, ToggleAdvancedSettings, SetRefreshRate, SetFocus,
              ToggleMotion, SetMotionDistance, ToggleResume, ToggleToneLook, ToggleFrameReuse, Stop,
              ToggleTvGamma, ToggleSeekPreview, ToggleThumbnails, ToggleShowPerformance, SetScreenDistance,
              SetScreenHeight, SetScreenCurve, ResetScreen, SetOriginForward, SetOpacity,
              SetWarmth, SetTint, SetExposure, ToggleMatchRoom, ReadRoomAgain,
              SetMatchColour, SetMatchBrightness,
              ToggleAutoPassthrough, ToggleKeyGreen, ToggleVibration,
              // Stash (docs/stash.md): on or off, its server, its switches,
              // its folders here, Sync now, and the format the Scenes page
              // shows. UseServer: which kind of media server is used, or none.
              UseServer, SetStashServer, SetStashFolders, ToggleStashCovers, ToggleStashTitles,
              ToggleStashMalePerformers, SyncStash, SetStashFormat,
              ToggleStashWriteBack, SetStashSections, SetStashScenesSort, SetStashPerformersSort,
              SetStashStudiosSort, SetLanguage, DownloadReleaseUpdate,
              FreeDecoder,  // end Steam's holders of the decoder, open the video again;
                            // value 1: from now on without asking
              ToggleFreeDecoder,  // ... always, or ask first
              SaveDiagnostics,
              SetDownloadsTo,  // the drive's root (path) new offline downloads go to
              ApplyPreset,  // a performance preset (presets.h)
              // The user's named presets: list them again, save the
              // settings as one, apply, rename or delete one.
              ListUserPresets, SaveUserPreset, ApplyUserPreset, RenameUserPreset,
              DeleteUserPreset, MoveUserPreset, TogglePackedMask,
              ToggleSecondLayer,     // the packed mask's second layer, shown or hidden
              SetKeyboardLayout,
              EndSetup,  // first-run setup ended: value 1 finished, 0 Set up later
              SetupPlaces,  // ... the places ticked so far, one per line in path
              SetupStash,   // ... a server connected: its address, then its API key, in
                            // path; its kind in value; option 1: its certificate
                            // accepted unverified
              StashCard,  // the menu's "Use Stash?" card, used or dismissed
              DismissSetupBanner,  // the Library's "You're all set" after setup
              ToggleShowWhatsNew,  // What's new opens by itself after updates
              // Funscripts (script_player.h): on or off, Intiface's address
              // (path), and the timing offset and stroke range (value).
              ToggleScripts, SetScriptAddress, SetScriptOffset, SetScriptRangeLow,
              SetScriptRangeHigh,
              // Settings' Controls (controls.h): a button's job (option: its
              // ControlSlot, value: the Job; with grip held, -1: none of its
              // own), a controller type's buttons back to their defaults
              // (value: the ControllerType), how far Skip and Big skip go
              // (value: seconds), and a button's jobs alone and with grip
              // held put back (option: its ControlSlot, value:
              // ControlMap::button()). ResetButtons resets the type's
              // gestures too.
              SetButtonJob, ResetButtons, SetSkipSeconds, SetBigSkipSeconds, RestoreButton,
              // A controller type's gestures (controls.h): one added in place
              // of any with its hold, or (value: its index) one changed
              // into it (option: the ControllerType << 17 | Gesture::pack());
              // the index-th removed (option: the ControllerType, value: the
              // index); a whole list put back, for Undo (option: the
              // ControllerType, path: GestureMap::listLine()).
              AddGesture, ChangeGesture, RemoveGesture, RestoreGestures };
  Type type = None;
  // seconds (Seek, SeekTo), enum index, track id (0 = off), volume 0..100,
  // 1 = only the downloaded copy (DeleteFile), picture value
  // -100..100, zoom in percent, the surround viewpoint in percent of the
  // sphere's radius toward the picture, slideshow seconds per photo (0 = off),
  // AfterVideo (SetAfterVideo), green
  // screen setting 0..100, Packing (SetPacking), the flat screen's distance
  // and height in centimeters, its curve in percent, StashFormat
  // (SetStashFormat), PerformancePreset (ApplyPreset), ServerKind
  // (SetStashServer, SetupStash; UseServer, -1 for none)
  double value = 0;
  // OpenFile, LoadSubtitle, RequestDelete and DeleteFile.
  // RequestDelete: the strip's delete button, the dialog open (the app
  // looks the file up and calls askDelete()); DeleteFile: confirmed;
  // CancelDelete: not. SetStashServer: the URL and the API key, one per
  // line (option 1: the server's certificate accepted unverified; 2: the
  // key isn't one typed for this kind, which keeps its own at the address
  // it had). SetStashFolders: each mapping's folder on the server and the
  // folder here, one per line. SetStashSections:
  // the Scenes rail's sections and subgroups as <id>_filters= keeps them.
  // SetStash*Sort: the stable sort name, with its direction in value
  // (1 = descending).
  // SaveUserPreset: its name; ApplyUserPreset and DeleteUserPreset: its file;
  // RenameUserPreset: its file and the new name, one per line;
  // MoveUserPreset: its file (value: places, -1 up).
  // SetKeyboardLayout: the layout and the other one, one per line.
  std::string path;
  // SetFocus: the FocusControl; SeekTo: 1 = from the seek bar; SetupStash,
  // SetStashServer; OpenFile: 1 = Shuffle from it, for the list it is opened from
  int option = 0;
};
