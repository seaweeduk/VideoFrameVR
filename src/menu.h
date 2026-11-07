#pragma once

#include "cairo_canvas.h"
#include "colour_balance.h"
#include "controller_guide.h"
#include "folder_listing.h"
#include "key_share.h"
#include "library.h"
#include "library_index.h"
#include "offline.h"
#include "places.h"
#include "presets.h"
#include "diagnostics.h"
#include "release_update.h"
#include "menu_geometry.h"
#include "settings.h"
#include "remote.h"
#include "sftp.h"
#include "smb_discovery.h"
#include "stash_folders.h"
#include "strings.h"
#include "video_format.h"
#include "video_memory.h"
#include "video_player.h"
#include "whats_new.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

typedef struct _PangoLayout PangoLayout;

class Thumbnailer;
class Uploader;
class FolderMemory;
class Stash;
class StashCatalog;
class StashCovers;
struct StashMenu;
struct SetupMenu;
struct MenuHints;

struct Palette;

// Match the room's switch (room_light.h): not offered, the room not shown,
// ready, reading, or a reading that just failed.
enum class RoomMatch { Hidden, Off, Ready, Reading, Failed };

// Something the menu asks the app to do.
struct MenuCommand {
  enum Type { None, TogglePause, Seek, SeekTo, ToggleMute, PreviousVideo, NextVideo,
              SetProjection, SetStereo, Recenter, OpenFile, SelectAudio,
              SelectSubtitle, LoadSubtitle, ToggleFloatingSubtitles,
              SetVolume, RequestDelete,
              DeleteFile, CancelDelete, Quit, SetBrightness,
              SetContrast, SetSaturation, SetZoom, ResetPicture, Paste, SetTheme,
              SetAccent, SetAccentHue, SetAccentBrightness, SetAccentSaturation,
              SetSlideshow, ToggleBackground, SetSubtitleSize,
              SetSubtitleRaise, SetSubtitleDistance, TogglePassthrough, ToggleGreenScreen,
              SetKeyStrength, SetKeySoftness, SetKeySpill, SetKeyShrink, ResetPassthrough,
              SetPacking, ToggleVideoMipmaps, SetAntiShimmer, SetSharpen, SetVideoResolution,
              SetFoveation, ToggleShowFoveation, SetSharpBins, ToggleFoveationRing,
              SetFoveationOutside, ToggleAdvancedSettings, SetRefreshRate, SetFocus,
              ToggleMotion, SetMotionDistance, ToggleResume, ToggleToneLook, Stop,
              ToggleTvGamma, ToggleSeekPreview, ToggleThumbnails, ToggleShowPerformance, SetScreenDistance,
              SetScreenHeight, SetScreenCurve, ResetScreen, SetOriginForward, SetOpacity,
              SetWarmth, SetTint, SetExposure, ToggleMatchRoom, ReadRoomAgain,
              SetMatchColour, SetMatchBrightness,
              ToggleAutoPassthrough, ToggleVibration,
              // Stash (docs/stash.md): on or off, its server, its switches,
              // Sync now, and the format the Scenes page shows.
              ToggleStash, SetStashServer, ToggleStashCovers, ToggleStashTitles,
              ToggleStashMalePerformers, SyncStash, SetStashFormat,
              ToggleStashWriteBack, SetStashSections, SetStashScenesSort, SetStashPerformersSort,
              SetStashStudiosSort, SetLanguage, DownloadReleaseUpdate,
              SaveDiagnostics,
              SetDownloadsTo,  // the drive's root (path) new offline downloads go to
              ApplyPreset,  // a performance preset (presets.h)
              // The user's named presets: list them again, save the
              // settings as one, apply, rename or delete one.
              ListUserPresets, SaveUserPreset, ApplyUserPreset, RenameUserPreset,
              DeleteUserPreset, MoveUserPreset, TogglePackedMask,
              SetKeyboardLayout,
              EndSetup,  // first-run setup ended: value 1 finished, 0 Set up later
              SetupPlaces,  // ... the places ticked so far, one per line in path
              SetupStash,   // ... Stash connected: its address, then its API key, in path
              StashCard,  // the menu's "Use Stash?" card: value 1 used, 0 dismissed
              DismissSetupBanner,  // the Library's "You're all set" after setup
              ToggleShowWhatsNew };  // What's new opens by itself after updates
  Type type = None;
  // seconds (Seek, SeekTo), enum index, track id (0 = off), volume 0..100,
  // 1 = only the downloaded copy (DeleteFile), picture value
  // -100..100, zoom in percent, the surround viewpoint in percent of the
  // sphere's radius toward the picture, slideshow seconds per photo (0 = off), green
  // screen setting 0..100, Packing (SetPacking), the flat screen's distance
  // and height in centimeters, its curve in percent, StashFormat
  // (SetStashFormat), PerformancePreset (ApplyPreset)
  double value = 0;
  // OpenFile, LoadSubtitle, RequestDelete and DeleteFile.
  // RequestDelete: the strip's delete button, the dialog open (the app
  // looks the file up and calls askDelete()); DeleteFile: confirmed;
  // CancelDelete: not. SetStashServer: the URL, the API
  // key, Stash's folder and this computer's, one per line. SetStashSections:
  // the Scenes rail's sections and subgroups as stash_filters= keeps them.
  // SetStash*Sort: the stable sort name, with its direction in value
  // (1 = descending).
  // SaveUserPreset: its name; ApplyUserPreset and DeleteUserPreset: its file;
  // RenameUserPreset: its file and the new name, one per line;
  // MoveUserPreset: its file (value: places, -1 up).
  // SetKeyboardLayout: the layout and the other one, one per line.
  std::string path;
  int option = 0;  // SetFocus: the FocusControl; SeekTo: 1 = from the seek bar
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

// Everything the menu shows about the app.
struct MenuView {
  PlaybackState playback;
  VideoFormat format;
  bool halfPacked = false;  // flat 3D shown with half-squeezed eyes (chosen by hand)
  ScreenView screen;
  bool passthrough = false;
  GreenScreen greenScreen;   // of the running video
  bool packedAlpha = false;  // the running video brings its own mask (no keying needed)
  bool maskFormat = false;   // its format is the one a packed mask comes in (maskFormat())
  int opacity = 100;            // of the running video, percent
  bool opacityApplies = false;  // the headset blends the video over the room (its own camera)
  std::string currentFile;
  bool playbackFailed = false;  // the current video couldn't be played
  bool discEncrypted = false;   // ... because it is an encrypted disc image
  Theme theme = Theme::Glass;
  Accent accent;
  bool floatingSubtitles = false;
  bool streaming = false;  // the current video is a link, not a file
  int videoWidth = 0, videoHeight = 0;
  bool background = true;  // night sky around the flat screen
  bool autoPassthrough = false;  // passthrough switches itself on for videos made for it
  int subtitleSize = 100, subtitleRaise = 0, subtitleDistance = 150;  // see Settings
  bool photo = false;        // the current file is a photo
  int slideshowSeconds = 0;  // 0 = no slideshow
  // Picture settings of the running video.
  int brightness = 0, contrast = 0, saturation = 0;
  ColourBalance balance;  // the video's, or Match the room's while it is on
  RoomMatch roomMatch = RoomMatch::Hidden;
  bool roomMatched = false;  // Match the room is on
  bool roomRead = false;     // ... and has a reading of this run to use again
  int matchColour = 70, matchBrightness = 60;  // percent (Settings::matchColour)
  float zoom = 1.0f;
  int originForward = 0;  // surround viewpoint: percent of the sphere's radius toward the picture
  bool tvGamma = false;  // SDR video converted for a TV's gamma (darker shadows)
  bool videoMipmaps = true;  // global: video sampled through mipmaps
  AntiShimmer antiShimmer = AntiShimmer::On;  // this run's: without them (anti_shimmer.h)
  int sharpen = 0;           // global: video sharpening in percent, 0 = off
  bool toneLook = false;     // global: tone curve and vignette
  // Global eye size: the choice (pixels with stops, otherwise percent),
  // and the requested and allowed scales in percent.
  int videoResolution = 100, videoResolutionActive = 100, videoResolutionEffective = 100;
  const std::vector<int>* resolutionStops = nullptr;  // Frame; owned by the run
  int resolutionOverride = 0;  // eye image pixels across this run asks for from the command line, else 0
  int resolutionBase = 0;    // eye image pixels across at 100 %
  bool panelBase = false;    // resolutionBase is the headset's panel, else SteamVR's recommendation
  bool resolutionNotice = false;  // 1× has changed meaning; show the migrated size
  bool resolutionPending = false;  // videoResolution is chosen but not switched to yet
  // The Frame's foveation (FoveationLevel), where the eye images have it.
  bool foveationAvailable = false;
  FoveationLevel foveation = FoveationLevel::High;
  bool showFoveation = false;
  int sharpBins = kDefaultSharpBins;
  bool foveationRing = kDefaultFoveationRing;
  int foveationOutside = 4;
  bool advancedSettings = false;
  int refreshRate = 0;  // Frame: Hz, SteamVR's for this app (refresh_rate.h); 0: not changeable
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
  std::array<int, 9> foveationMaps{{0, 1, 2, 3, 4, 5, 6, 7, 8}};
  std::array<int, 3> sharpDegrees{};
  FocusView focus;  // stereo focus of the running video
  bool motionEmulation = true;  // global: head translation emulation (motion.h)
  int motionDistance = 200;     // ... cm
  bool resume = true;           // videos start where they were left
  bool seekPreview = true;      // the seek bar shows the frame where the pointer is
  bool seekPreviewAvailable = true;  // ... can at all (not on the Steam Frame)
  bool showPerformance = false;  // the performance readout, this run's (--show-performance)
  bool vibration = true;        // controllers buzz on clicks, seeks and grabs
  bool thumbnails = true;       // file browser and library: thumbnails, else a video icon
  StashSettings stash;          // Stash's settings (its pages show only while it is on)
  bool setupBanner = false;     // the Library's "You're all set" after setup (DismissSetupBanner)
  bool showWhatsNew = true;     // What's new opens after updates (Settings::showWhatsNew)
  int appFps = 0;               // frames the app drew in the last second (0 = not known yet)
  ReleaseUpdate::State releaseUpdate = ReleaseUpdate::State::Disabled;
  std::string releaseVersion;
  DiagnosticsExport::State diagnostics = DiagnosticsExport::State::Idle;
  std::string diagnosticsFile;  // Saved: the zip's name
  bool crashedLastRun = false;  // until its diagnostics are saved
  bool softwareDecoding = false;  // the video plays without hardware decoding
  std::string downloadsTo;      // Settings::downloadsTo
  // The strip's delete button (docs/alpha/delete-file.md): not shown
  // (nothing open, a link), shown, or shown disabled for a scene played
  // from Stash's stream: Send to Stash off, or Stash not answering.
  enum class Deletable : uint8_t { No, Yes, NeedsSendToStash, StashDown } deletable = Deletable::No;
};

// The delete dialog's (docs/alpha/delete-file.md): the file playing, and
// what the app found out about it in the background. Until `ready`, only
// Cancel can be pressed.
struct DeleteInfo {
  std::string file;  // the file playing: a path or a remote locator (remote.h)
  std::string name;  // its file name
  bool photo = false;
  bool ready = false;
  uint64_t size = 0;
  // A downloaded copy (`file`) of `source`, on the drive at `copyRoot`:
  // Remove download deletes the copy; Delete everywhere, once the source
  // was found, the source too.
  std::string source, copyRoot;
  bool sourceFound = false;
  bool sourceChanged = false;  // not the video downloaded any more
  // A Stash scene's file (with Send to Stash on): Stash deletes it, the
  // scene too if it has no other files (`otherFiles`), into its trash
  // folder if it has one (`stashTrash`); or it is deleted here and Stash
  // lists it until its Clean runs, also when Stash's files didn't show
  // which one it is (Unsure).
  enum class Stash : uint8_t { None, Deletes, Lists, Unsure } stash = Stash::None;
  int otherFiles = 0;
  bool stashTrash = false;
};

// Where a list of paths was left (the Library, Stash's scenes): the row
// kept in its place on the screen when it comes back, maybe changed
// meanwhile; if that row is gone, the one then at its index.
struct ListPlace {
  std::string path;  // the one playing if it showed, else the top one; none: nothing listed
  int index = 0;
  double rowsFromTop = 0;
};

// The VR menu: the playback strip (kMain), always shown while the menu is
// open; above it the browser (kBrowser): a column of sources (files, the
// library, Stash, settings) and the central page showing one of them,
// or the grouped video settings or stereo focus pages. Drawn with
// cairo/pango into textures that are only redrawn on changes. Where the
// panels float is in menu_geometry.h and MenuPanels.
class Menu {
 public:
  enum PanelId { kMain = 0, kBrowser = 1, kSetup = 2 };
  // Panel sizes in layout pixels, menu_geometry::kPixelsPerMeter in VR (their
  // textures have kRasterScale times as many).
  // The strip: its body, below room for its open lists and tooltips.
  static constexpr int kMainWidth = menu_geometry::kStripWidthPx;
  static constexpr int kStripBodyY = 220;
  static constexpr int kStripBodyHeight = 240;
  static constexpr int kMainHeight = kStripBodyY + kStripBodyHeight;
  // The browser: the sources column, a gap, then the central page.
  static constexpr int kPageX = menu_geometry::kSourcesWidthPx + menu_geometry::kSourcesGapPx;
  static constexpr int kPageWidth = menu_geometry::kPageWidthPx;
  static constexpr int kBrowserWidth = kPageX + kPageWidth;
  static constexpr int kBrowserHeight = menu_geometry::kPageHeightPx;
  static constexpr int kPageCenterX = kPageX + kPageWidth / 2;
  // First-run setup's panel: 1.36 × 0.9 m, and below it the room its
  // keyboard shows in.
  static constexpr int kSetupWidth = 1360, kSetupHeight = 900, kSetupKeyboardHeight = 370;
  // Glass theme: the panels' own coverage where they are most transparent;
  // the renderer shows the blurred scene through them.
  static constexpr float kGlassAlpha = 0.62f;

  Menu();
  ~Menu();
  bool init(Uploader& uploader);
  // Source of the file browser's thumbnails (optional, not owned). Before
  // its start(): this sets the sizes they are made at.
  void setThumbnailer(Thumbnailer* thumbnailer);
  // Where each folder's order and list/gallery choice is kept (optional, not owned).
  void setFolderMemory(FolderMemory* folders) { folders_ = folders; }
  // The media library, for the listed files' records (optional, not owned).
  void setLibrary(Library* library) { library_ = library; }
  // The videos in the pinned folders, for the library view (optional, not owned).
  void setLibraryIndex(LibraryIndex* index) { index_ = index; }
  // Offline downloads (optional, not owned): the Offline place, the marks
  // and download buttons on videos, the drive they go to (menu_offline.cpp).
  void setOffline(Offline* offline);
  // Stash (optional, not owned; null while it is off): the browser's and the
  // library's videos show its covers, titles and studio logos
  // (menu_stash_covers.cpp); its section, Scenes page and settings show
  // (menu_stash_*.cpp).
  void setStash(Stash* stash);
  // Whether `path` was opened from a list in the menu (the Library, Stash's scenes),
  // and then in `next` the video `step` places from it there (empty at the
  // list's ends). False: Previous and Next go through its folder.
  bool listNeighbor(const std::string& path, int step, std::string& next);
  // Where the file browser opens the first time (the last session's folder).
  void setStartDirectory(const std::string& dir) { startDirectory_ = dir; }
  // The folder the file browser shows (empty until it is first opened).
  const std::string& directory() const { return listing_.directory(); }
  // Videos as a grid of large thumbnails instead of a list (all folders).
  void setGallery(bool gallery) { gallery_ = gallery; }
  bool gallery() const { return gallery_; }

  void update(const MenuView& view);
  // Pointer on a panel in 0..1 of its texture (v = 0 at the top); panel
  // -1 = pointing at none.
  void setPointer(int panel, float u, float v);
  // Clicks at the pointer (the trigger pressed). Navigation inside the menu is
  // handled here; the rest is returned for the app to carry out. On a list, a
  // row's click waits for letting go (drag()).
  MenuCommand click();
  // What the pointing hand should feel since the last call, as on Steam's
  // dashboard: the pointer entering something it can click, and while a
  // slider, the volume or the seek bar is dragged, a tick each 40th of its
  // track, a bump on or across its middle value (0, or its marked stop) and
  // a knock at either end. Entering is quiet while something is dragged and
  // while the content moves under the pointer.
  enum class Feel { None, Tick, Enter, Detent, Edge };  // weakest first
  Feel takeFeel() {
    const Feel f = feel_;
    feel_ = Feel::None;
    return f;
  }
  // Call every frame with whether the trigger is held: while a slider grabbed
  // by click() is held, returns its new value as the pointer moves; a list
  // pressed follows the pointer, or on letting go unmoved returns its row's click.
  MenuCommand drag(bool held);
  // Input focus went elsewhere (Steam's dashboard): a press or drag ends
  // without what letting go does (a seek, a row's click, a rating).
  void cancelDrag();
  // The slider click() grabbed, while it is held; else None.
  MenuCommand::Type draggingSlider() const { return draggingSlider_; }
  // Scrolls a list page by (fractions of) rows, positive = down (the stick).
  void scroll(double rows);
  // While the pointer is over the strip's seek bar: where it points,
  // 0..1 of the video. For the preview frame.
  bool seekHover(double& fraction) const;
  // Where the seek bar is at `fraction` (0..1) of the video, as setPointer()
  // takes it on the strip (Menu::kMain).
  void seekBarPoint(double fraction, float& u, float& v) const;
  // Clears the rating the pointer is on (reset gesture: grip
  // held and a stick click); false if it is on none.
  bool clearRatingAtPointer();
  // Switches to the video file browser, next to the current video or in $HOME.
  void showFileBrowser();
  // Switches to the library view: every video in the pinned folders.
  void showLibrary();
  // Closes the browser, leaving the strip; its pages keep their state for later.
  void showMainPage() { showPage(Page::Main); }
  // Switches to the settings page.
  void showSettings() { showPage(Page::Settings); }
  // The update dialog, over the page shown (MenuView::releaseUpdate).
  void showUpdate() { openDialog(Dialog::Update, ""); }
  // What's new (menu_whats_new.cpp): these releases' notes, newest first,
  // over the page shown. Several came with an update from `from` to `to`;
  // otherwise it is headed by the app's name.
  void showWhatsNew(std::vector<ReleaseNotes> releases, const std::string& from = "",
                    const std::string& to = "");
  bool whatsNewOpen() const { return dialog_ == Dialog::WhatsNew; }
  // Any dialog is open over the page (or alone, with the browser closed).
  bool dialogOpen() const { return dialog_ != Dialog::None; }
  // Switches to the Offline place: the drives, downloads to go and those done.
  void showOffline();
  // The list the playing (or last) video was opened from, as it was left;
  // if it can't be shown, the browser where it was last. A video opened
  // from nothing in the menu (the command line) comes from its folder.
  void showPlayedFrom();
  void languageChanged();
  // First-run setup (menu_setup.cpp, docs/alpha/first-run.md): its panel
  // (kSetup) shows instead of the strip and the browser until it ends
  // (MenuCommand::EndSetup), from Welcome, with `places` ticked. holdWrites
  // (--first-run): a login it remembers is saved only when it ends.
  bool startSetup(bool holdWrites, const std::vector<std::string>& places);
  // Finished: the places ticked are pinned.
  void endSetup(bool finished);
  bool setupRunning() const { return setup_ != nullptr; }
  // Whether a controller is tracked: Welcome's button waits for one.
  void setSetupController(bool tracked);
  // Your controllers: what the actions are bound to, and what the hands
  // hold, as heldBit()s (controller_guide.h).
  void setSetupControls(const ControllerBindings& bindings);
  void setSetupHeld(uint32_t held);
  // The keyboard is up on a setup page; and in the image shown, a few frames
  // later: the panel is raised.
  bool setupTyping() const;
  bool setupKeyboardShown() const;
  // A list on its page scrolls (the stick goes to scroll()).
  bool setupScrolls() const;
  // Stash connected in setup (docs/alpha/first-run.md, "Copies of Stash's
  // videos"): Stash's folders are looked for in the background (not only
  // while Settings shows, never while a video is open) and what is found is
  // saved without a press; with `places` (setup's), there too and again.
  void findStashFolders(bool on, std::vector<std::string> places = {});
  // While setup is out of view: the arrow toward it (pointing right), its
  // label and the line, in front of the eyes; and the desktop window's
  // card until someone wears the headset. Sizes in layout pixels.
  enum class SetupCue { Arrow, Label, Hint, Mirror };
  const Texture* setupCue(SetupCue cue, int& width, int& height);
  // The hints after setup (hints.h, menu_hints.cpp): what the controls are
  // bound to (their SteamVR glyphs read off the render thread), and a
  // hint's line drawn for showing, each time it starts (layout pixels).
  void setHintBindings(const ControllerBindings& bindings);
  const Texture* drawHint(Hint hint, int& width, int& height);
  // A list longer than its page pointed at (`page`: the page shown); the
  // pointer on the strip's volume.
  bool pointingAtLongList(int& page) const;
  bool pointingAtVolume() const;
  // Welcome's intro (docs/alpha/first-run.md, step 1; setup_intro.h), due
  // until it starts, once a run. Its layers are drawn on the first call and
  // dropped with dropSetupIntro(): each the part of its texture it shows,
  // and where on the setup panel that ends up (layout pixels).
  enum class IntroLayer { Backdrop, Tile, Blue, Green, Lens, Play, Name, Black, Count };
  struct IntroPiece {
    const Texture* texture = nullptr;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    float x = 0, y = 0, w = 0, h = 0;
  };
  bool setupIntroDue() const;
  void startSetupIntro();
  IntroPiece setupIntroPiece(IntroLayer layer);
  void dropSetupIntro();
  // microSD cards or USB drives were mounted or taken out (removable_drives.h):
  // the navigation lists them again, and a folder on one taken out is left.
  void drivesChanged(const std::vector<std::string>& removed);

  // The delete dialog, opened by the strip's button. While it is open,
  // clicks outside it are ignored. askDelete(): what the app looked up,
  // shown if it is still open for that file. showDeleteError(): why it
  // can't be deleted, in its place. deleteEnded(): the deletion ended, and
  // the list the video came from shows; with `error`, the dialog says why
  // it failed; with `note`, something left to know once it was deleted.
  // cancelDelete(): playback moved on; closes it unless confirmed.
  void askDelete(const DeleteInfo& info);
  void cancelDelete();
  void showDeleteError(const std::string& error);
  void deleteEnded(const std::string& error, const std::string& note = std::string());
  // Re-reads the file browser's folder.
  void refreshDirectory();
  // True on pages with a scrollable list (files, the library, Stash's), or
  // a settings page too tall for the browser.
  bool listOpen() const {
    if (dialog_ == Dialog::WhatsNew) return whatsNew_.max > 0;  // its notes, over the page
    return page_ == Page::Files || page_ == Page::Library || page_ == Page::Network ||
           page_ == Page::Offline || stashListPage() || (groupedPage() && pageScrollMax_ > 0) ||
           (page_ == Page::StashSceneDetails && side_.w > 0 && side_.max > 0);
  }

  // While a search keyboard is shown, the desktop keyboard types into it too.
  bool textInputActive() const { return editingFilter(); }
  void typeText(const std::string& utf8);
  void backspace();
  // Moves the text cursor by characters (negative: left); far enough, to
  // the start or the end (Home, End).
  void moveCursor(int chars);
  // Enter: for the folder search, hides the keyboard (what matches stays);
  // Stash's fields are kept.
  MenuCommand submitText();
  // What the menu decided by itself since the last call (e.g. Stash's
  // folder found in the one chosen); None if nothing.
  MenuCommand takeCommand() { return std::exchange(command_, MenuCommand{}); }
  // Escape: leaves the keyboard (the folder search keeps its words), without
  // closing the menu.
  void stopEditing();
  // The on-screen keyboard's layout and the other one its layout key
  // switches to, as settings.conf keeps them ("" follows the menu language).
  void setKeyboardLayout(const std::string& active, const std::string& other) {
    keyboardLayout_ = active;
    keyboardOther_ = other;
  }

  // Whether a panel is shown: the strip always, the browser while a page is
  // open in it (or a dialog); during setup only its own.
  bool panelShown(PanelId panel) const;
  // The rows [first, end) of a panel that are shown now (in layout pixels), and its columns.
  void shownRows(int panel, int& first, int& end) const;
  void shownColumns(int panel, int& first, int& end) const;
  // Panel texture (sRGB, premultiplied alpha), redrawn first if needed.
  const Texture* texture(PanelId panel);
  // The button under the pointer on this panel, in layout pixels
  // (x0, y0, x1, y1), with its corner radius and whether it is destructive
  // (highlighted red), for the shader, and the edges of the list it scrolls
  // in, where the highlight is cut off. False if there is none.
  bool hoverRect(PanelId panel, float rect[4], float& radius, bool& danger, float clip[4]) const;
  bool glass() const { return view_.theme == Theme::Glass; }
  // The theme's colours with the chosen accent (menu_style.h), and the accent.
  Palette palette() const;
  const AccentColours& accent() const { return accent_; }

 private:
  enum class Page { Main, Settings, VideoSettings, Files, Library, Focus, StashScenes, StashHome, StashPerformers, StashPerformer,
                    StashStudios, StashStudio, Network, Offline, Places,
                    StashSceneDetails };  // the strip's Scene tab (menu_stash_scene.cpp)
  enum class FileKind { Video, Subtitle };
  enum class Icon { None, Play, Pause, Stop, Previous, Next, Back10, Forward10, VolumeUp,
                    Mute, Folder, FolderUp, Settings, Recenter, Trash, Power,
                    Globe, FormatDome, ProjFlat, Proj180, Proj360,
                    Fisheye180, Fisheye190, Fisheye200, Cube, StereoMono, StereoSbs, StereoTb, Curved,
                    Slideshow, JumpTop, JumpBottom, Grid, MenuLines, Focus, Audio, Subtitles, Chapters,
                    List, Star,
                    StarFilled, Passthrough, Search, Library, Video, Pin, MoveUp, MoveDown,
                    Download, Check, Card, Storage, Close, PauseLine, PlayLine, Pencil, Plus,
                    Refresh, Arrow, ArrowRight, Chevron, Trigger, Controller, Lock, Warning,
                    Headset, Computer };
  enum class Dialog { None, ConfirmDelete, Deleting, Error, Deleted, ConfirmQuit,
                      SaveTo, NoRoom, RemoveDownload,  // offline downloads' (menu_offline.cpp)
                      Update,  // a newer release (menu_settings.cpp)
                      WhatsNew,  // the release notes (menu_whats_new.cpp)
                      // An SFTP server's (menu_network.cpp): signed in with the
                      // password as its key was refused, its key changed, and
                      // Forget for a key login (and with the server not answering).
                      KeyRefused, HostKeyChanged, ForgetServer, ForgetOffline };
  // Small choice menus opened by their button.
  enum class Dropdown { None, Slideshow, Audio, Subtitles, Chapters, Sort, Language,
                        Presets };  // the user's own presets (menu_presets.cpp)
  // File browser order (folders always come first).
  using SortMode = FolderListing::Order;
  struct Button {
    int panel = kMain;
    double x = 0, y = 0, w = 0, h = 0;
    std::string label;
    Icon icon = Icon::None;
    bool selected = false;
    std::function<MenuCommand()> action;  // null = not clickable (list heading)
    bool listRow = false;
    std::string detail;    // Stash's cards and rows: a second line
    std::string byline;    // file browser: who and where (Stash), dim after or under the name
    std::string size, date;  // file browser: the list's Size and Date modified columns
    std::string plays;       // ... and its Plays column (empty: never played)
    std::string thumbPath; // file browser video row: whose thumbnail to show
    std::string tooltip;   // icon-only button: its name, shown after hovering a while
    bool volumeSlider = false;  // click/drag sets the volume from the pointer's x
    bool popup = false;         // part of the open dropdown or dialog, drawn on top
    bool danger = false;        // destructive action, drawn red
    bool dangerHover = false;   // destructive, but only shown red when hovered
    bool hoverOnly = false;     // drawn only while it or the button before it is pointed at
    bool plain = false;         // no background of its own (Material theme)
    bool bare = false;          // no background in any theme (unless selected)
    double radius = -1;         // corner radius, -1 = the default
    std::string caption;        // format tile: small label under the icon
    bool disabled = false;      // not relevant right now: greyed out, not clickable
    bool crossed = false;       // dimmed, its icon struck through (a server that doesn't answer)
    bool card = false;          // file browser gallery: large thumbnail, name under it (a
                                // rating: on a thumbnail)
    int fontSize = 0;           // label size in pixels, 0 = the default
    std::string badge;          // short mark on a bright disc at the top-right corner
    bool scrolls = false;       // part of a smoothly scrolled list: cut off at its edges
    bool scrollbar = false;     // a list's scroll bar: click or drag scrolls to the pointer
    bool field = false;         // a text field: drawn as one, its label is the text
    int caret = -1;             // ... typed into: its cursor, after this many characters of it
    bool key = false;           // an on-screen key: its action redraws only what it changes
    bool placeholder = false;   // ... and that is only a hint (dimmed)
    bool staticText = false;    // only text (a label or hint): fontSize, `lines` lines at most
    int lines = 1;
    bool dim = false;           // ... in the dimmer text colour
    bool toggle = false;        // a switch: the label, and a pill that is blue while `on`
    bool on = false;
    bool tab = false;           // a strip tab: a small icon, then the label
    bool nav = false;           // a navigation row: no background unless `on` (chosen)
    int swatch = -1;            // an accent colour to choose (Accent's preset), ringed while `on`
    bool alignLeft = false;     // its label from the left (a list of names)
    // A slider (not None): click or drag sets this command's value from the
    // pointer's x, sliderMin..sliderMax in sliderStep steps; it shows
    // sliderText, else the value.
    MenuCommand::Type slider = MenuCommand::None;
    int sliderValue = 0;
    int sliderMin = 0, sliderMax = 100, sliderStep = 1;
    int sliderOption = 0;
    bool sliderLog = false;
    int sliderDefault = -1;  // marked stop, or none
    std::string sliderText;
    // A rating control (menu_rating.cpp): this video's stars; the trigger held
    // along them chooses, letting go keeps it. The library view rates its
    // videos by their id (a video's path may change meanwhile).
    std::string ratingPath;
    int64_t ratingVideo = 0;
    float rating = 0;
    bool side = false;  // in the side column (side_): scrolls with it, cut off at its edges
    // Drawn by drawStashButton() (menu_stash_draw.cpp) instead: its StashLook
    // (not None), and its StashIcon.
    int stashLook = 0, stashIcon = 0;
    int64_t stashId = 0;  // ... and the Stash id it shows (a performer's portrait)
    // A video row or tile: the path whose download shows on it (its state on
    // the picture, the tick); a drive's root on a drive's card.
    std::string offlinePath;
    int offlineLook = 0;  // drawn by drawOfflineButton() instead (OfflineLook, not None)
    int64_t downloadId = 0;   // a download's row in the Offline place
  };
  struct ListItem {
    std::string label;
    bool selected;
    std::function<MenuCommand()> action;
    std::function<MenuCommand()> trash = nullptr;  // optional delete button at the row's end
    std::string thumbPath = {};                    // video file: show its thumbnail
    bool folder = false;
    std::string size, date;  // file: for the list's columns and under thumbnails
    std::string ratingPath;  // a video: its rating control, with this rating
    int64_t ratingVideo = 0;  // ... by the library's id, if it is rated by that
    float rating = 0;
    std::string plays;       // ... and how often it was played, if at all
    std::string byline;      // dim after the name (an SFTP server's account)
    std::string trashTip;    // the delete button's tooltip, if not tipDelete
  };
  ListItem folderItem(const std::string& label, const std::string& path);
  ListItem fileItem(const std::string& label, const std::string& path);
  const Strings& text() const { return strings(); }
  Button iconButton(Icon icon, std::function<MenuCommand()> action, bool selected = false) const;
  Button textButton(const std::string& label, Icon icon, std::function<MenuCommand()> action,
                    bool selected = false) const;
  void layout(bool browser = true);
  void layoutPage(bool browser);
  void layoutFilesPage();
  void layoutMain();
  // The grouped settings pages (menu_settings.cpp, menu_focus.cpp).
  void layoutSettings();
  void layoutDiagnostics(double top, int column);  // Help: Save diagnostics
  double layoutAdvancedSwitch();  // atop a page with rows it hides; returns where groups start
  void layoutVideoSettings();
  // Presets atop Video settings (menu_presets.cpp): the row (returns where
  // the groups start), the user's own in a list under its button, and a
  // name typed over the keyboard to save the settings as, or rename one to.
  double layoutPresetRow();
  void layoutPresetList(const Button& opener);
  void layoutPresetName();
  void openPresetName(const std::string& file, const std::string& name);
  MenuCommand submitPresetName();
  std::string presetSummary(const Settings& s) const;  // its covered settings, listed
  struct PresetName {
    bool open = false;
    std::string file;  // renaming this preset; empty: saving the settings as a new one
    std::string name;  // ... its name now
    std::string text;  // typed
  };
  PresetName presetName_;
  // What the name typed is: nothing, fine, a built-in's, or another of the
  // user's (`preset`: which), which saving replaces and renaming refuses.
  struct PresetNameCheck {
    enum Kind { Empty, Fine, BuiltIn, Other } kind = Empty;
    int preset = -1;
    bool operator==(const PresetNameCheck& o) const { return kind == o.kind && preset == o.preset; }
  };
  PresetNameCheck checkPresetName() const;
  std::string presetDeleting_;  // the preset whose delete is asked in its row (its file)
  // The flat screen's group on the video settings page, its box from y = top.
  void layoutScreenGroup(double top, bool disabled);
  void layoutFocus();
  // The browser: the sources column, the page's frame and its content.
  void layoutBrowser();
  void drawBrowser(cairo_t* cr);
  // Video settings and stereo focus show on the page alone, without the
  // sources column.
  bool pageAlone() const { return page_ == Page::VideoSettings || page_ == Page::Focus; }
  bool groupedPage() const {
    return page_ == Page::Settings || page_ == Page::Places || pageAlone();
  }
  void layoutNavigation();
  Places places_;
  LibraryIndex* index_ = nullptr;
  std::vector<std::string> indexPins_;  // the pins the index was last given
  void pinsChanged();
  int placeOffset_ = 0;
  double navLineY_ = 0;  // the line under the places
  double stashCardY_ = -1;  // the "Use Stash?" card's top, -1 none (StashPages::layoutCard())
  int placeOf(const std::string& path) const;
  // Edit places (menu_places.cpp): each place with its name, a switch and
  // its moves; or one being renamed, over the keyboard.
  void layoutPlacesPage();
  void layoutPlaceRename();
  void savePlaceName();
  struct PlaceRename {
    std::string path;  // empty: none is being renamed
    std::string ownName, text;
  } placeRename_;
  void openPlace(const std::string& path);
  void refreshPlaces();
  // Panel backgrounds (panelFrame()s at x, y, w, h), drawn once per theme:
  // the strip is redrawn every second, the browser's rows while scrolling.
  struct Frame {
    cairo_surface_t* surface = nullptr;
    Theme theme = Theme::Glass;
    Accent accent;
    std::vector<bool> rows;
  };
  struct FrameBox {
    int x, y, w, h;
  };
  Frame browserFrame_, stripFrame_;
  AccentColours accent_ = accentColours(Accent{});
  std::chrono::steady_clock::time_point accentChanged_;  // update()'s, for dragging
  void paintFrame(cairo_t* cr, Frame& frame, int width, int height,
                  const std::vector<FrameBox>& panels);
  // The hovered button's tooltip, if it is on this panel.
  void drawTooltip(cairo_t* cr, int panel, double panelWidth);
  void drawPopups(cairo_t* cr, int panel);
  void layoutTools();
  // Offline downloads (menu_offline.cpp).
  Offline* offline_ = nullptr;
  uint64_t offlineGeneration_ = 0;
  std::string offlineShape_;  // what the downloads were, but for their progress
  uint64_t offlineShapeGeneration_ = 0;  // ... counted as it changes
  // The download a dialog asks about: the video, and the drives.
  struct OfflineAsk {
    std::string path, oshash, root, other;
    uint64_t size = 0;
    int64_t id = 0;
  } offlineAsk_;
  void offlineChanged();
  void layoutOfflinePage();
  void drawOfflineHeading(cairo_t* cr);
  const Offline::Download* downloadOf(const std::string& path) const;
  std::string oshashFor(const std::string& path) const;
  uint64_t sizeFor(const std::string& path) const;
  bool downloadable(const std::string& path) const;
  bool downloadedMark(const std::string& path) const;
  // After a video's row or tile: its download button (while pointed at), or
  // what opens the Offline place: its tick, its picture's state. statusX:
  // a row's status column; lastLineY: where a tile's last line is.
  void addOfflineButtons(const Button& item, double statusX, double lastLineY);
  // On a row's or tile's picture: its download's state; and its tick.
  void drawOfflineState(cairo_t* cr, const Button& b, double x, double y, double w, double h) const;
  void drawOfflineTick(cairo_t* cr, double x, double cy, double size) const;
  void drawOfflineButton(cairo_t* cr, const Button& b, const double ink[4]) const;
  void drawDriveBar(cairo_t* cr, const Offline::Drive& d, double x, double y, double w,
                    double h) const;
  MenuCommand startDownload(const std::string& path);
  MenuCommand downloadTo(const std::string& root);
  std::string downloadRoot() const;
  std::string driveName(const Offline::Drive& d, bool full) const;
  std::string driveName(const std::string& root, bool full) const;
  Icon driveIcon(const Offline::Drive& d) const;
  double dialogHeight() const;
  // The delete dialog's buttons along its bottom (into `row`) and its text
  // (menu_delete.cpp).
  void layoutDeleteDialog(std::vector<Button>& row);
  void drawDeleteDialog(cairo_t* cr, double textX, double boxY);
  void layoutOfflineDialog(double boxY, std::vector<Button>& row);
  void layoutUpdateDialog(std::vector<Button>& row);
  void drawUpdateDialog(cairo_t* cr, double textX, double boxY);
  void drawOfflineDialog(cairo_t* cr, double boxX, double boxY);
  // What's new: the releases shown, the line under its title, and the
  // notes laid out once (measured as they are drawn) and how far they are
  // scrolled down (max 0: they fit, at the full width).
  struct WhatsNewPiece {
    enum Kind { Version, Section, Line, ReleaseRule, SectionRule } kind;
    std::string text, date;  // a version's date
    double y = 0, h = 0;     // from the notes' top
  };
  struct WhatsNewDialog {
    std::vector<ReleaseNotes> releases;
    std::string from, to;
    std::vector<WhatsNewPiece> pieces;
    double width = 0, height = 0;  // as laid out; width 0: not yet
    double scroll = 0, max = 0;
  } whatsNew_;
  // Where the dialog is, as its notes were laid out: its left, top and
  // height, and its notes' height.
  struct WhatsNewBox {
    double x, y, h, notesH;
  };
  WhatsNewBox whatsNewBox() const;
  void layoutWhatsNewNotes(double width);
  void layoutWhatsNew();
  void drawWhatsNew(cairo_t* cr);
  void scrollWhatsNew(double pixels);
  bool overWhatsNewNotes() const;
  // The Scene tab's box (menu_stash_scene.cpp): Download, or the state.
  void addOfflineBox(const std::string& path, double x, double y, double w, double h);
  // The tab "Main menu": the browser where it was last.
  void showBrowser();
  Page lastBrowserPage_ = Page::Files;
  // A list page shown again as it was left; false for any other page.
  bool showListPage(Page page);
  // Opens a video from the page shown, which is then where it was opened
  // from (Previous and Next keep it), and left with it in its place.
  MenuCommand openVideo(const std::string& path);
  Page playedFrom_ = Page::Files;
  std::string opening_;
  const std::string& playingFile() const { return opening_.empty() ? view_.currentFile : opening_; }
  double offlineScroll_ = 0;  // the Offline place's rows, where it was left
  std::string infoText_;  // what is playing, along the strip's top
  double infoX_ = 0;
  long clockMinute_ = -1;  // the minute the strip's clock shows
  // Height of the strip's seek bar line.
  double seekLineY() const;
  // Adds a round icon button with a tooltip; returns it (valid until the next add).
  Button& addRound(Icon icon, const std::string& tooltip, std::function<MenuCommand()> action,
                   double x, double y, double size = 88, bool selected = false);
  // A strip of choices above the opener, on the opener's panel.
  void addDropdown(const Button& opener, std::vector<Button> entries);
  void addDropdownBelow(const Button& opener, std::vector<Button> entries);
  bool onPopups(int panel, double px, double py) const;
  void layoutDialog();
  void drawDialog(cairo_t* cr);
  void openDialog(Dialog dialog, const std::string& path, const std::string& error = "");
  // At the bottom of the browser's page; on another panel centred on cx
  // from y, its keys keyW wide.
  void layoutKeyboard(int panel = kBrowser, double cx = kPageCenterX, double y = -1,
                      double keyW = 100);
  // Typing into the folder's (or the library's) search: the keyboard shows
  // under its results. On Stash's pages, into what stashText() is; in
  // setup, its SMB form.
  bool editingFilter() const {
    if (setup_) return (smbForm_.kind != SmbForm::Kind::None || setupText()) && editing_;
    return (page_ == Page::Files || page_ == Page::Library || page_ == Page::Network ||
            page_ == Page::Places || stashTextPage() ||
            (page_ == Page::VideoSettings && presetName_.open)) &&
           editing_;
  }
  bool editing_ = false;
  void setFilter(const std::string& text);
  const std::string& filterText() const {
    if (setup_ && setupText()) return *setupText();
    if (smbForm_.kind != SmbForm::Kind::None) return smbForm_.text[smbForm_.field];
    if (page_ == Page::Places) return placeRename_.text;
    if (page_ == Page::VideoSettings && presetName_.open) return presetName_.text;
    return stashTextPage() ? stashText() : page_ == Page::Library ? libraryFilter_ : fileFilter_;
  }
  // The text cursor in the field typed into: a byte of filterText(), kept
  // with the text it was placed in. In other text (another field, or this
  // one changed from elsewhere) it is at the end.
  struct Cursor {
    const std::string* text = nullptr;
    std::string was;
    size_t at = 0;
  } cursor_;
  size_t cursorAt() const;
  void setCursor(size_t at);
  int caret() const;  // the characters before it
  // A field's text: from x, w wide, in `size`; drawn with its caret, kept in
  // view by scrolling the text (fieldShift_) when it is longer.
  void fieldTextBox(const Button& b, double& x, double& w, int& size) const;
  void drawFieldText(cairo_t* cr, const Button& b, const double ink[4]) const;
  mutable double fieldShift_ = 0;
  // A field pressed: the one typed into puts its cursor where the pointer
  // is; another opens, its cursor there too if it shows the text typed.
  MenuCommand clickField(const Button& b);
  bool keyboardShown_ = false;
  double keyboardRect_[4] = {0, 0, 0, 0};  // its panel: x, y, w, h
  // Centred on x = cx (default: the strip's and the former menu's middle).
  void addCenteredRow(double y, double h, std::vector<Button> items, double cx = kMainWidth / 2.0);
  void addRow(double y, double h, std::vector<Button> items, double x0);
  // Grouped settings, on the browser's page (menu_widgets.cpp): a titled box,
  // and rows of kSettingRowH in it, a label on the left and the control on
  // the right from x + labelW.
  struct Group {
    double x, y, w, h;
    std::string title;
    bool rule = false;  // only a thin line (between choices in a row)
  };
  std::vector<Group> groups_;  // on the browser
  void addGroup(double x, double y, double w, double h, const std::string& title);
  void addRule(double x, double y, double h) { groups_.push_back({x, y, 1.5, h, {}, true}); }
  Button& addLabel(double x, double y, double w, double h, const std::string& text,
                   int size = kTextSize, bool dim = false, int lines = 1);
  Button& addToggleRow(double x, double y, double w, const std::string& label, bool on,
                       std::function<MenuCommand()> action, bool disabled = false);
  void addValueRow(double x, double y, double w, double labelW, const std::string& label,
                   const std::string& value, int now, int lo, int hi,
                   std::function<MenuCommand()> less, std::function<MenuCommand()> more,
                   bool disabled = false, bool logarithmic = false, const char* tooltip = "");
  void addChoiceRow(double x, double y, double w, double labelW, const std::string& label,
                    const std::vector<std::string>& options, int chosen,
                    const std::function<MenuCommand(int)>& choose, bool disabled = false,
                    const char* tooltip = "");
  void drawGroups(cairo_t* cr);
  // A grouped page taller than the browser scrolls (by pixels, not rows):
  // moves its groups and the browser's buttons from `first` up by pageScroll_.
  void scrollGroups(size_t first);
  double pageScroll_ = 0;     // how far the grouped page is scrolled down
  double pageScrollMax_ = 0;  // ... and can be, 0 = it fits
  // A scroll bar's extent: where its list or page is scrolled to, how much
  // of it shows and how much there is (in rows, or pixels for a grouped page).
  struct ScrollExtent {
    double position, shown, total;
  };
  ScrollExtent scrollExtent() const;
  // Wrapped text of at most `lines` lines from y, ellipsized after the last;
  // with inkBottom, fewer lines if they would reach below it.
  void drawTextLines(cairo_t* cr, const std::string& text, double x, double y, double w, int size,
                     int lines, const double rgba[4], bool center, double inkBottom = 0,
                     bool bold = false) const;
  // How many lines drawTextLines() wraps `text` to, at most `lines`.
  int wrappedLines(const std::string& text, double w, int size, int lines, bool bold = false) const;
  void addList(const std::vector<ListItem>& items, double x0 = 40, double rowH = 58,
               int rows = 9);
  // The file browser as a grid of large thumbnails.
  void addGallery(const std::vector<ListItem>& items, double x0, int rows);
  void addScrollbar();
  void scrollToPointer(const Button& bar);
  void drawScrollbar(cairo_t* cr, const Button& bar) const;
  // The columns of the scroll bar's track and thumb, narrower than its button.
  static void scrollbarTrack(const Button& bar, double& x, double& w) {
    w = std::min(12.0, bar.w / 2);
    x = bar.x + (bar.w - w) / 2;
  }
  bool draggingScroll_ = false;
  // Dragging a list or a tall grouped page by its content, as on a touch
  // screen (menu.cpp): a press on it is a click only when let go before the
  // pointer moved kDragStart along it; beyond that the content follows the
  // pointer, and letting go while moving lets it glide on for a moment.
  struct ListDrag {
    bool pressed = false;  // the trigger is held since a press on the list ...
    bool moving = false;   // ... and the list follows the pointer
    bool gliding = false;  // let go while moving: the list slows to a stop
    bool away = false;     // the pointer is off the browser (the list waits)
    std::function<MenuCommand()> action;  // the row pressed, carried out on letting go
    double x = 0, y = 0, w = 0, h = 0;    // ... where that row is
    double pressY = 0, lastY = 0;         // the pointer: at the press, in the last frame
    double travel = 0;                    // how far it moved along the list since
    std::deque<std::pair<double, double>> recent;  // (seconds, travel): its speed at letting go
    double velocity = 0;  // gliding: layout pixels a second, positive = down
    double lastTime = 0;  // seconds (steady clock) of its last step
    bool side = false;    // it is the side column (side_) that is dragged
  } listDrag_;
  // A column beside a list page's list that scrolls on its own, by layout
  // pixels (the Stash filter rail): the stick and dragging scroll whichever
  // of the two the pointer is over. As the list, it moves on the GPU and
  // only what comes into view is drawn. None (w = 0) on other pages.
  struct SideColumn {
    double x = 0, y = 0, w = 0, h = 0;  // where it shows
    double position = 0, max = 0;       // how far it is scrolled down, and can be
  } side_;
  bool overSide() const;
  void scrollSide(double pixels);
  double sideRemainder_ = 0;  // as scrollRemainder_, in layout pixels
  bool sideMoved_ = false;  // only its position changed since the last redraw
  double drawnSide_ = -1;   // where the texture shows it, -1 = not known
  bool shiftSide();
  bool listDragging() const { return listDrag_.moving || listDrag_.gliding; }
  // Whether the button pressed (-1: none) is on a list's content, not its
  // scroll bar or the controls around it.
  bool onListContent(int index) const;
  void pressList(const Button* row);
  MenuCommand dragList(bool held);
  void glideList();
  // Scrolls by (fractions of) rows as scroll(), or by layout pixels.
  void scrollRows(double rows);
  void scrollPixels(double pixels);
  double listBottom_ = 0;  // the list's viewport ends here (rows below are cut off)
  bool listColumns_ = false;  // the list shows the folder's Size and Date modified columns
  void layoutListHeader(double x0, double rowW);
  double searchRight_ = 0;  // where the folder search's text ends (its count and Clear follow)
  // A list row's parts: the thumbnail (or icon) box, then the name, then the
  // columns at its right end.
  // 16:9 for thumbnails (or Stash's covers); without them, a square for the icon.
  double listThumbW(const Button& row) const {
    return view_.thumbnails || stashCoversShown() ? (row.h - 4) * 16 / 9 : row.h - 4;
  }
  double listNameWidth(const Button& row) const;
  // A video's picture drawn w layout pixels wide: its Stash cover, else its
  // frame thumbnail; null until there is one. Drawing asks for it, and what
  // it got is kept in shownPictures_.
  cairo_surface_t* pictureOf(const std::string& path, double w) const;
  // Whether a picture of path may still come: its Stash cover, or a frame
  // thumbnail not tried yet. Without one, its box shows the video icon.
  bool pictureComing(const std::string& path) const;
  cairo_surface_t* findPicture(const std::string& path, double w) const;
  struct ShownPicture {
    double w = 0;
    cairo_surface_t* surface = nullptr;
    bool icon = false;  // none, and none coming: the video icon
  };
  mutable std::unordered_map<std::string, ShownPicture> shownPictures_;  // by path
  mutable std::unordered_set<std::string> logosComing_;  // tiles drawn before their studio's logo
  // Pages whose only pictures are those of pictureOf().
  bool picturesAlone() const {
    return page_ == Page::Files || page_ == Page::Library || page_ == Page::StashScenes ||
           page_ == Page::Offline;
  }
  // New thumbnails or covers: redraws only the rows and cards on screen whose
  // picture changed. False on pages that also show pictures of other kinds.
  bool picturesChanged();
  void drawThumbnail(cairo_t* cr, const std::string& path, double x, double y, double w, double h,
                     double radius) const;  // the scroll bar follows the pointer while the trigger is held
  // Asks for the thumbnails of the items on screen [first, end) and, bounded,
  // of those around them and at both ends of the folder. `page` = items per
  // screen, `pictureW` how wide they are drawn.
  void wantThumbnails(const std::vector<ListItem>& items, int first, int end, int page,
                      double pictureW);
  // Whether these thumbnails and covers were just asked for, as they are
  // while a list scrolls within its rows: then they needn't be again yet.
  bool wantedAlready(const std::vector<std::string>& wanted, size_t shown, double pictureW);
  struct Wanted {
    std::vector<std::string> paths;
    size_t shown = 0;
    double pictureW = 0, at = -1;
    bool thumbnails = false, covers = false;
  } wanted_;
  void setThumbnailOrIcon(Button& b, const ListItem& item) const;
  // Whether a folder's or the library's search lists an entry: by its name,
  // or with Stash's titles shown by what Stash knows of it too.
  bool matchesFilter(const FolderListing::Entry& entry, const std::string& filter) const;
  int folderMatchCount() const;
  // Stash in these views (menu_stash_covers.cpp); none while it is off.
  std::unique_ptr<StashCovers> stashCovers_;
  bool stashCoversShown() const;
  void updateStash();
  // Stash thumbnail previews (menu_stash_preview.cpp). Only the pointed-at
  // picture changes, at the preview's frame rate, without relaying out the menu.
  struct HoverPreview {
    std::string path;
    double x = 0, y = 0, w = 0, h = 0;
    double since = 0;
    bool requested = false;
  } preview_;
  void updatePreview();
  void stopPreview();
  void drawPreview();
  cairo_surface_t* previewFrame(const std::string& path, double x, double y) const;
  // A video's row or tile with Stash's title as its label and its
  // performers and studio as its byline, if Stash knows it and titles are
  // on; returns the file's name when the title replaced it.
  std::string describeFromStash(Button& b, const std::string& path) const;
  void wantStashImages(const std::vector<std::string>& paths, size_t shown, double pictureW);
  // The studio's logo (or name) in the top right corner of a tile's picture.
  void drawStudioMark(cairo_t* cr, const std::string& path, double x, double y, double w,
                      double h) const;
  void arrangeByTitles();
  // The column right of the file list: jump to top / bottom, sort, list or gallery.
  void layoutFileTools();
  std::vector<ListItem> fileItems();
  std::string trackName(const Track& track) const;
  // Choices stacked above their opener, in columns of five if there are many.
  void addDropdownList(const Button& opener, std::vector<Button> entries);
  void openFiles(FileKind kind);
  void openDirectory(const std::string& dir, const std::vector<std::string>& fallbacks = {});
  void sortBy(SortMode mode);
  ListPlace listPlace(const std::vector<std::string>& paths, int columns) const;
  void scrollToPlace(const ListPlace& place, const std::vector<std::string>& paths, int columns);
  // The library view (menu_library.cpp): every video in the pinned folders,
  // from the library index's memory, and while Stash is on every scene of
  // its catalog, sorted and searched like a folder.
  struct LibraryItem {
    FolderListing::Entry entry;  // the file shown (away from home: its copy, or none)
    std::string path;            // the file in the pinned folder, or the scene's location
    bool scene = false;          // a Stash scene with no file in the pins
    std::string oshash;          // ... its file's
    int64_t id = 0;              // the library's, what its rating and plays are read by
    VideoRecord record;
    int64_t addedAt = 0;
    bool listed = true;  // shown by the Show choice (as of the last sort: a rating doesn't move it)
    std::string title;   // Stash's, while shown instead of the file's name
    const std::string& name() const { return title.empty() ? entry.name : title; }
  };
  std::vector<LibraryItem> libraryItems_;  // sorted
  std::string libraryFilter_;
  SortMode librarySort_ = SortMode::AddedNewest;  // newest additions first
  bool libraryDescending_ = true;
  int libraryShow_ = 0;  // Strings::libraryShows: all, not played, played, rated
  int libraryCounts_[4] = {};  // ... how many of each the search finds
  ListPlace libraryPlace_;  // where it was left
  std::vector<std::string> libraryPaths();  // the rows' paths, as listed
  uint64_t libraryGeneration_ = 0;
  std::string libraryStatus_;  // the heading's status as drawn
  // The catalog the scenes are from (none: Stash off or not synced yet).
  std::weak_ptr<const StashCatalog> libraryCatalog_;
  bool noPins() const;
  // Nothing to list, not even Stash's scenes: the page says how to pin a
  // folder, or how Stash's first sync is getting on.
  bool libraryPinHelp() const { return noPins() && libraryItems_.empty(); }
  // Stash's first sync, while there is no catalog yet: under way, or failed
  // (a later one failing keeps the catalog there was, and says so in Settings).
  enum class FirstSync { None, Running, Failed };
  FirstSync firstSync() const;
  std::string firstSyncText(bool line) const;  // its step, or why it failed
  // Under the Library's heading: the banner after setup and the first sync's
  // line (with pins), their tops (-1: not shown); the tools and the list
  // move down by `shift`.
  struct LibraryBands {
    double banner = -1, sync = -1, shift = 0;
  };
  LibraryBands libraryBands() const;
  double toolsShift() const { return page_ == Page::Library ? libraryBands().shift : 0; }
  void layoutLibraryBands(const LibraryBands& bands);
  void layoutLibraryCentre();
  void drawLibraryBands(cairo_t* cr, const LibraryBands& bands);
  void drawLibraryCentre(cairo_t* cr);
  void updateLibraryFirstSync();
  std::string allSetText() const;  // the banner's, before "Change it any time in"
  FirstSync libraryFirstSync_ = FirstSync::None;  // as drawn
  std::string libraryFirstSyncText_;
  void loadLibrary();
  LibraryItem libraryItem(const std::string& path, const LibraryIndex::File& file) const;
  const std::shared_ptr<const StashCatalog>& stashCatalog() const;
  bool libraryCatalogChanged() const;
  void addLibraryScenes();
  void libraryFilesChanged(const std::vector<std::string>& changed,
                           const std::vector<std::string>& removed);
  void refreshLibraryRecords();
  bool libraryBefore(const LibraryItem& a, const LibraryItem& b) const;
  bool libraryShows(const LibraryItem& item) const;
  void sortLibrary();
  bool placeLibraryItems();
  std::string libraryStatus() const;
  void layoutLibraryShow(double& x);
  // What the list shows, kept between redraws (a redraw while scrolling would
  // otherwise make all of them again); remade when stale.
  const std::vector<ListItem>& libraryListItems();
  std::vector<ListItem> libraryList_;
  bool libraryListStale_ = true;
  std::string libraryListPlaying_;  // the playing file it marks
  int libraryMatchCount() const;
  void layoutLibraryPage();
  void drawLibraryHeading(cairo_t* cr);
  // Previous / next: the locations of the list a video was opened from (the
  // Library, Stash's scenes), and the one it is at.
  std::vector<std::string> playList_;
  std::string playAt_;
  // Sorts by a field of Strings::sortFields: the one sorted by turns around,
  // another keeps the direction.
  void sortByField(int field);
  bool sortDescending_ = false;  // the direction last chosen
  // Library orders: the files by their records (the listing has them by name).
  void arrangeByRecords();
  // Where the browser was in a folder, to return there (this session only).
  struct Browsing {
    std::string filter;
    std::string anchor;  // the item in view (see listAnchor())
    double rowsFromTop = 0;
    int index = -1;  // ... its index: the item there then takes its place if it is gone
  };
  std::map<std::string, Browsing> browsing_;  // by folder
  bool listShowsFolder_ = false;  // the last layout listed the folder
  void saveBrowsing();
  Browsing returnTo_;        // where openDirectory() returns to, once the folder is read
  std::string revealPath_;   // ... and what it then scrolls into view
  void folderRead();
  int folderColumns() const;  // of the folder's list now: 1, or the thumbnails' columns
  int folderRows() const;     // ... and its rows shown at once
  std::vector<std::string> listedPaths() const;
  bool listAnchor(std::string& path, double& rowsFromTop, int* index = nullptr) const;
  std::string lastAnchor_;  // the item scrollToItem() last brought into view
  void scrollToItem(const std::string& path, double rowsFromTop, int index = -1);
  void scrollIntoView(const std::string& path);
  void showPage(Page page);
  int buttonAt(int panel, double px, double py) const;
  void redraw();
  void drawMain(cairo_t* cr);
  void drawButtons(cairo_t* cr, int panel, bool popups = false);
  void drawVolumeSlider(cairo_t* cr, const Button& b) const;
  void drawTrack(cairo_t* cr, double x0, double x1, double y, double fraction, double buffered,
                 double alpha) const;
  void drawButtonContent(cairo_t* cr, const Button& b, const double ink[4]) const;
  MenuCommand volumeAtPointer(const Button& slider) const;
  MenuCommand sliderAtPointer(const Button& slider) const;
  void drawSlider(cairo_t* cr, const Button& b) const;
  void drawSwatch(cairo_t* cr, const Button& b) const;
  MenuCommand::Type draggingSlider_ = MenuCommand::None;
  int draggingSliderOption_ = 0;
  bool resolutionHeld_ = false;  // a new Resolution chosen, its slider still held
  static void drawIcon(cairo_t* cr, Icon icon, double cx, double cy, double s, double r = 1,
                       double g = 1, double b = 1);
  // The button's fill and its icon/text colour in the current theme.
  void buttonColors(const Button& b, double fill[4], double ink[4]) const;
  void drawMainPage(cairo_t* cr);
  bool settingsShown() const;
  void drawFilesHeading(cairo_t* cr);
  void drawListStates(cairo_t* cr);
  // The ordinary text size in pixels: about 40 reference layout units.
  static constexpr int kTextSize = 25;
  double textWidth(const std::string& text, int size, bool bold) const;
  mutable std::map<std::pair<std::string, int>, double> textWidths_;  // by text, size and boldness
  void drawText(cairo_t* cr, const std::string& text, double x, double y, double w, double h,
                int size, bool bold, bool center, double r, double g, double b, double a = 1.0) const;
  // Text laid out for drawText() (lines 0) or drawTextLines(), kept to draw
  // again: scrolled lists draw the same titles in strip after strip, and
  // laying them out is about half of drawing them.
  PangoLayout* textLayout(cairo_t* cr, const std::string& text, int size, bool bold, double w,
                          int lines, bool center) const;
  mutable std::unordered_map<std::string, PangoLayout*> textLayouts_;

  CairoCanvas canvases_[2];
  bool dirty_ = true;          // everything is redrawn
  bool stripDirty_ = false;    // only the strip (the time moved on)
  bool browserDirty_ = false;  // only the browser (scrolling, thumbnails) ...
  int browserFirst_ = 0, browserEnd_ = 0;  // ... and only these rows of it
  int browserLeft_ = 0, browserRight_ = 0;
  struct Damage { int x0, y0, x1, y1; };
  std::vector<Damage> browserDamage_;
  int browserRow_ = -1;  // next strip of a complete replacement; -1 when idle
  int browserColumn_ = 0;  // visible page's left edge until its replacement is ready
  MenuView browserView_;
  int browserOffset_ = 0;
  double browserFraction_ = 0, browserSide_ = 0, browserPage_ = 0;
  // Redraws the browser's rows [y0, y1) (all of them by default) at the next texture().
  void browserChanged(double y0 = 0, double y1 = kBrowserHeight,
                      double x0 = 0, double x1 = kBrowserWidth);
  void sliderChanged(MenuCommand::Type type, double below = 0, int option = -1);
  void focusChanged(const FocusView& view);
  void drawBrowserPart(int y0, int y1, int x0, int x1);
  // Scrolling: the list's new position, in rows, and whether only that changed.
  void setListPosition(double position);
  bool listMoved_ = false;
  double drawnScroll_ = -1;     // where the browser's texture shows the list, -1 = not known
  double scrollRemainder_ = 0;  // scrolling of less than a texture pixel, kept for the next
  bool shiftList();

  MenuView view_;

  int pointerPanel_ = -1;
  double pointerX_ = 0, pointerY_ = 0;  // layout pixels of pointerPanel_
  int hovered_ = -1;
  // Finds the button under the pointer (none while a list is dragged or
  // glides), and shows or takes away its tooltip.
  void updateHover();
  double hoverStart_ = 0;  // seconds (steady clock) since hovered_ points at its button
  int tooltip_ = -1;       // button whose tooltip is drawn, -1 = none
  // Redraws what the button's tooltip covers, as it appears or goes.
  void tooltipChanged(int button);
  // The list or the side column moved: a browser tooltip goes, and the next
  // waits until it has stood still for a while.
  void hideTooltipWhileScrolling();
  Feel feel_ = Feel::None;
  void feel(Feel f) {
    if (f > feel_) feel_ = f;
  }
  // The control the pointer is on, by where it is (indices change whenever
  // a page is laid out again); panel -1 = none.
  struct HoverSpot {
    int panel = -1;
    double x = 0, y = 0, w = 0, h = 0;
    bool operator==(const HoverSpot& o) const {
      return panel == o.panel && x == o.x && y == o.y && w == o.w && h == o.h;
    }
  };
  HoverSpot hoverSpot_;
  bool hoverQuiet_ = false;  // something dragged or moved: the next spot is taken without a feel
  void feelHover(int hovered);
  bool dragging() const;
  // The dragged slider's, volume's or seek bar's value at the last frame, and
  // the feel of moving it to `value` (fraction: where a value is along its track).
  double trackValue_ = 0;
  void feelTrack(double value, double lo, double hi, bool detented, double detent,
                 const std::function<double(double)>& fraction);
  static double sliderFraction(const Button& slider, double value);
  bool draggingVolume_ = false;
  bool draggingSeek_ = false;  // the seek bar follows the pointer while the trigger is held
  double lastScrub_ = -1;      // seconds last sent while dragging it
  double seekAtPointer() const;  // seconds under the pointer on the seek bar
  Dropdown dropdown_ = Dropdown::None;
  bool thumbnailsPending_ = false;  // new thumbnails wait for the next redraw
  double lastThumbnailRedraw_ = 0;  // seconds, steady clock
  Thumbnailer* thumbnailer_ = nullptr;
  FolderMemory* folders_ = nullptr;
  void rememberFolderView();
  Library* library_ = nullptr;
  // The library's records of listing_'s entries, one each, and the
  // library's generation they are from.
  std::vector<VideoRecord> records_;
  uint64_t recordsGeneration_ = 0;
  void lookUpRecords();
  void recordsChanged(const std::vector<VideoRecord>& before);
  VideoRecord current_;  // the playing file's record, for the strip's stars ...
  uint64_t currentGeneration_ = 0;  // ... as of this library generation
  // Ratings (menu_rating.cpp). Pointed at, stars show the rating a press
  // would choose; while the trigger is held on them, the one letting go
  // there keeps (or clears, on the saved one); leaving them restores it.
  struct RatingDrag {
    bool active = false;
    std::string path;
    int64_t video = 0;  // rated by the library's id instead, if not 0
    int panel = kMain;
    double x = 0, y = 0, w = 0, h = 0;  // the control
    bool card = false;                  // ... on a picture's strip
    double scroll = 0;                  // where its list was scrolled to
    float saved = 0;                    // its rating
    float rating = 0;                   // shown meanwhile
    // The same control (a list relaid out makes it anew).
    bool is(const Button& b) const {
      return panel == b.panel && path == b.ratingPath && std::abs(x - b.x) < 0.5 &&
             std::abs(y - b.y) < 0.5;
    }
    bool is(const RatingDrag& o) const {
      return panel == o.panel && path == o.path && std::abs(x - o.x) < 0.5 &&
             std::abs(y - o.y) < 0.5;
    }
  } ratingDrag_;
  RatingDrag ratingHover_;  // the stars pointed at, while the trigger is not held
  double listMovedAt_ = 0;  // steadySeconds() the list last scrolled: no pointing meanwhile
  Button ratingButton(const std::string& path, float rating, int panel, double x, double y,
                      double starW, double h) const;
  Button cardRating(const std::string& path, float rating, const Button& card) const;
  RatingDrag ratingControl(const Button& control) const;
  float ratingAtPointer(const RatingDrag& control, float kept) const;
  void startRating(const Button& control);
  MenuCommand dragRating(bool held);
  void pointAtRating(int hovered);
  void rate(const std::string& path, int64_t video, float rating);
  void ratingChanged(const RatingDrag& control);
  void drawRating(cairo_t* cr, const Button& b) const;
  void drawCoverStrip(cairo_t* cr, const Button& card, double x, double y, double w,
                      double h) const;
  enum class StarInk { Gold, White };
  cairo_surface_t* starImage(int size, int fill, StarInk ink) const;
  mutable std::map<int, cairo_surface_t*> starImages_;  // by size, fill and ink
  Dialog dialog_ = Dialog::None;
  std::string dialogPath_;
  std::string dialogError_;
  DeleteInfo deleteInfo_;  // the delete dialog's

  Page page_ = Page::Main;
  FileKind fileKind_ = FileKind::Video;
  std::string startDirectory_;
  FolderListing listing_;
  // The folder search: only entries whose names hold all its words are listed
  // (empty = all). Opening another folder clears it.
  std::string fileFilter_;
  SortMode sortMode_ = SortMode::NameUp;
  bool gallery_ = false;  // file browser as a grid of large thumbnails
  int scrollOffset_ = 0;
  double scrollFraction_ = 0;  // smooth scrolling: part of a row scrolled past scrollOffset_
  double listTop_ = 0;         // y of the list's first visible row
  // Clamps the scroll position to `count` rows of which `rows` fit, and
  // returns the first and last (exclusive) row to lay out.
  void clampScroll(int count, int rows, int& first, int& end);
  int listCount_ = 0;     // items (gallery: grid rows) on the current list page, for scrolling
  int visibleRows_ = 9;   // rows the current list page shows at once
  double rowHeight_ = 58;
  double listRight_ = 1560;  // right edge of the list (left of the scroll bar)
  double listLeft_ = 0;      // ... and its left edge on the browser

  // The Network page and the SMB and SFTP forms (menu_network.cpp).
  // A sign-in on a thread of its own, or for SFTP the connect job (probe,
  // trust a new key, try this device's key), or Forget's key removal.
  struct RemoteJob {
    enum class Task { SignIn, Connect, RemoveKey } task = Task::SignIn;
    std::atomic<bool> done{false};
    remote::Status status = remote::Status::Ok;
    sftp::Probe probe;
    std::string publicKey;  // Connect for a server taking keys only: this device's
    std::string trusted;    // HostKeyChanged: the key trusted before
  };
  struct SmbForm {
    // KeyOnly, Changed (in setup; a dialog on the Network page) and
    // KeyRefused (setup's) are an SFTP server's.
    enum class Kind { None, AddServer, SignIn, Share, KeyOnly, Changed, KeyRefused } kind = Kind::None;
    bool sftp = false;  // an SFTP server (sftp://), else SMB
    std::string root;  // the server's (the one signing in to, once Sign in is chosen): remote::root()
    std::string then;  // Add server: the share (and folders) typed after the server
    int field = 0;     // typed into: 0 server (or share), 1 user name, 2 password, 3 port
    std::array<std::string, 4> text{{"", "", "", "22"}};
    bool remember = true, showPassword = false;
    bool password = false;  // SFTP's sign-in: password offered (the probe's) ...
    bool keyChoice = false;  // ... and the key too: the choice row shows
    bool byKey = false;      // ... the method chosen
    std::string publicKey, trusted, fingerprint;  // KeyOnly's; Changed's two keys
    double keyBox[4] = {0, 0, 0, 0};  // where the key is shown, as laid out
    double commandBox[4] = {0, 0, 0, 0};  // ... and the command, if it is
    // KeyOnly: the key served for the server to fetch (key_share.h), the
    // command fetching it while it is, the fetches seen; after one, signing
    // in by itself at retryAt until retryUntil.
    std::shared_ptr<KeyShare> share;
    std::string command;
    int fetches = 0;
    std::chrono::steady_clock::time_point retryAt, retryUntil;
    std::string error, hint;  // hint: dim, under the error
    std::shared_ptr<RemoteJob> signing;  // under way
  };
  SmbForm smbForm_;
  SmbDiscovery discovery_;
  std::map<std::string, std::string> sftpUsers_;  // the user name last tried on each SFTP server
  void showNetwork();
  std::vector<remote::Server> networkServers() const;
  void openServer(const remote::Server& server);
  void layoutNetworkPage();
  const char* networkState() const;
  void openSmbForm(SmbForm::Kind kind, const std::string& root);
  void layoutSmbForm(double top);
  void layoutSftpForm(double x, double& y, double inner);
  void drawSmbForm(cairo_t* cr);
  std::vector<int> smbFields() const;
  MenuCommand submitSmbForm();
  void typedIntoSmbForm(const std::string& text);
  // smbForm_'s SFTP server, its user (text[1]): the connect job; after a
  // key changed, trusting `trust` first. A sign-in with its fields.
  void connectSftp(const std::string& trust = "");
  void signInSftp(const remote::Login& login, bool remember);
  void updateNetwork();
  void updateKeyShare();
  bool keyFetched() const;
  void sftpDone(const RemoteJob& job);
  void openSignedIn();
  void smbFolderRead();
  void layoutNetworkDialog(std::vector<Button>& row);
  void drawNetworkDialog(cairo_t* cr, double textX, double boxY);
  // What's typed as a server: "alex@nas:2222/media"'s parts (port 0:
  // none, -1: ":0"), and whether it names an SFTP server (1), an SMB one (0) or
  // neither (-1).
  struct TypedServer {
    std::string host, user, path;
    int port = 0;
  };
  static TypedServer splitServer(std::string text);
  // The SFTP root of a host typed (an IPv6 address bracketed) and a port.
  static std::string sftpRoot(const std::string& host, int port);
  static int serverType(const std::string& text);
  // A key or a fingerprint in the monospace font, wrapped anywhere, drawn
  // from x, y (or with cr null only measured): its height.
  double drawMono(cairo_t* cr, const std::string& text, double x, double y, double w, int size,
                  const double rgba[4]) const;
  std::string serverName(const std::string& root) const;
  std::string shownLocation(const std::string& dir) const;

  // Where Stash's videos are here (menu_stash_folders.cpp): looked for once
  // per sync, or in the folder chosen in the browser.
  struct StashFolders {
    bool picking = false;    // the browser chooses where one of Stash's folders is
    std::string folder;      // ... which (StashCatalogFolder::folder)
    bool checking = false;   // ... and looks in the one chosen
    bool notThere = false;   // ... which hasn't its videos
    uint64_t searches = 0;   // Stash::folderSearches() last seen
    // Stash connected in first-run setup (findStashFolders()): looked for
    // in these places too, and saved as found.
    bool automatic = false;
    std::vector<std::string> places;
    uint64_t saved = 0;  // ... the search last saved
  };
  StashFolders stashFolders_;
  MenuCommand command_;
  void updateStashFolders();
  // SetStashServer with these mappings, the server and key as they are.
  MenuCommand stashFolders(const std::vector<StashFolderMap>& folders) const;
  void chooseStashFolder(const std::string& folder);
  double layoutStashFolderChoice();
  double headingRight_ = 0;  // where the Files page's location ends

  bool shift_ = false;  // the on-screen keyboard's Shift
  bool capsLock_ = false;  // ... tapped twice: it stays on
  double shiftAt_ = 0;     // when Shift was last tapped
  bool symbols_ = false;  // ... and its symbols instead of the letters
  std::string keyboardLayout_, keyboardOther_;  // see setKeyboardLayout()
  // The key pressed, lit while the trigger is held on it (Backspace repeats).
  // Held on, a letter shows its accents and the layout key all layouts.
  struct KeyDown {
    double x = 0, y = 0, w = 0, at = -1, next = 0;
    std::function<void()> repeat;  // held, done again and again (Backspace, the arrows)
    enum class Hold { None, Accents, Layouts } hold = Hold::None;
    std::string letter;  // Accents: the letter typed, as typed
    std::string before;  // ... and the text before it
    size_t cursor = 0;   // ... and where in it
    bool done = false;   // its hold (or the layout key's tap) acted on
  } keyDown_;
  // A row of keys over the one held: its accents, or the layouts.
  struct KeyPopup {
    bool open = false;
    bool accents = false;
    double x = 0, y = 0;            // the middle and top of the key it came from
    std::vector<std::string> keys;  // what each types, or the layouts' codes
    std::string before, after;      // Accents: the text before and after the letter
    size_t at = 0;                  // ... and where it went in
    bool dragging = false;          // opened by holding: closes when let go
  } keyPopup_;
  void keyHeld(bool held);
  void openKeyPopup(bool accents, bool dragging);
  void closeKeyPopup();
  MenuCommand pickFromKeyPopup(size_t index);
  int activeLayout() const;
  int otherLayout() const;  // 0 (English) when there is none
  MenuCommand useLayouts(int active, int other);
  void keysChanged();   // the keyboard alone needs drawing again
  void typedChanged();  // ... and the field typed into

  // Stash (docs/stash.md): its section in the navigation, its pages and the
  // Settings group are laid out and drawn by StashPages (menu_stash.h), only
  // while stash_ is set; stashMenu_ keeps their state for the session.
  friend struct StashPages;
  Stash* stash_ = nullptr;
  std::unique_ptr<StashMenu> stashMenu_;
  // Stash's text fields: the scenes' and its lists' searches, a pinned
  // list's name, the server's settings, the Scene tab's tag search.
  bool stashTextPage() const {
    return stashMenu_ && (stashPage() || page_ == Page::Settings);
  }
  // Stash's pages with a scrolling list or grid (and a side column).
  bool stashListPage() const {
    return page_ == Page::StashScenes || page_ == Page::StashPerformers ||
           page_ == Page::StashPerformer || page_ == Page::StashStudios ||
           page_ == Page::StashStudio || page_ == Page::StashHome;
  }
  bool stashPage() const { return stashListPage() || page_ == Page::StashSceneDetails; }
  const std::string& stashText() const;

  // First-run setup's pages, laid out and drawn by SetupPages (menu_setup.h)
  // while setup_ is set; the Stash step's field typed into, if any.
  friend struct SetupPages;
  std::unique_ptr<SetupMenu> setup_;
  std::string* setupText() const;
  std::unique_ptr<MenuHints> hints_;  // once setHintBindings() or drawHint() is called
  Uploader* uploader_ = nullptr;

  std::vector<Button> buttons_;
};
