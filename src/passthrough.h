#pragma once

#include "colour_balance.h"
#include "platform.h"
#include "room_light.h"
#include "video_check.h"

#include <optional>
#include <set>
#include <string>

class Library;
class XrContext;
enum class AutoPassthrough;
struct MenuCommand;
struct MenuView;
struct Settings;

// The room behind the video: passthrough switching itself on for videos
// made for it, the checks that tell, and Match the room. `video`,
// `currentFile` and `format` are the playing video's, kept by the app.
// Main thread only.
class Passthrough {
 public:
  // Its two workers, and whether the room can be read: built before the
  // library, so the workers stop after its last writes.
  struct Workers {
    VideoCheck videoCheck;
    RoomLight roomLight;
    const bool roomReadable = kStandaloneHeadset && RoomLight::available();
  };

  Passthrough(Settings& settings, Library& library, const XrContext& xr, Workers& workers,
              VideoSettings& video, const std::string& currentFile, const VideoFormat& format)
      : roomReadable(workers.roomReadable), settings(settings), library(library), xr(xr),
        video(video), currentFile(currentFile), format(format), videoCheck(workers.videoCheck),
        roomLight(workers.roomLight) {}

  // The eye images are blended over the room (the headset's cameras, where
  // the runtime blends): with passthrough on, or Opacity below 100, with or
  // without passthrough (whose mask or key it doesn't bring). Match the room
  // applies only then.
  bool roomShown() const;

  // A video opens (`stream`: a link): it settles again, without the last
  // one's checks and notice.
  void videoChanged(bool stream);
  // Automatic passthrough or Key green switched: for the video playing too.
  void resettle() { passthroughSettled = false; }
  // Each frame. Once the video's size (so its format) and its settings are
  // known (`ready`): passthrough for a video made for it, and the checks it
  // needs. `autoPassthroughOn`: the setting, where the room can show.
  void settle(bool autoPassthroughOn, bool ready);
  // Each frame: a check that is due starts, and one that finished is kept.
  void poll(bool autoPassthroughOn);
  // Passthrough switched by hand (it is already switched).
  void switchedByHand();
  // MaskProbe found the playing video's second layer.
  void layersFound() { layersSeen.insert(currentFile); }
  // The notice's first line while passthrough switched on by itself says
  // so; else null.
  const char* notice(double now) const;

  const bool roomReadable;
  bool matched() const { return roomMatched; }
  // The command, if it is one of Match the room's or the automatic
  // passthrough's settings; false for any other.
  bool handle(const MenuCommand& c);
  // The menu's ToggleMatchRoom and ReadRoomAgain.
  void toggle();
  void readAgain();
  // The menu's SetWarmth, SetTint or SetExposure: the video's part to save,
  // or 0 if the room's balance took it.
  unsigned setBalance(const MenuCommand& c);
  // Picture reset the video's balance.
  void pictureReset();
  // The passthrough group's reset: Match the room's strengths, where shown.
  void resetMatch();
  // The strengths changed.
  void applyRoomMatch();
  // The balance drawn: the room's while `matching` (on, the room shown),
  // else the video's own.
  const ColourBalance& balance(bool matching) const {
    return matching ? roomBalance : video.balance;
  }
  // Each frame: a room reading that ended.
  void pollRoom();
  bool roomBlank() const { return roomLight.blank(); }
  void blankSubmitted() { roomLight.blankSubmitted(); }
  // `blended` and `roomMatching` as the frame found them.
  void fill(MenuView& view, bool blended, bool roomMatching, double now) const;

 private:
  void passthroughByItself(AutoPassthrough why);
  void readRoom();

  Settings& settings;
  Library& library;
  const XrContext& xr;
  VideoSettings& video;
  const std::string& currentFile;
  const VideoFormat& format;

  // Passthrough switching itself on for videos made for it (see_through.h):
  // once per video, when its size and settings are known.
  // The green screen check, or the mask layer check for a video with a
  // packed mask (video_check.h).
  VideoCheck& videoCheck;
  bool passthroughSettled = false;
  bool stream = false;  // the video is a link
  bool greenCheckAsked = false;  // the green screen check was asked for, for this video
  bool fillCheckPending = false;  // ... and its fill check hasn't answered yet
  double videoCheckAt = 0;  // glfwGetTime() the check starts, a moment after the video opens
  VideoCheck::Kind videoCheckKind = VideoCheck::Kind::GreenScreen;
  std::set<std::string> layersSeen;  // videos whose mask's second layer MaskProbe found
  const char* passthroughWhy = nullptr;  // the notice's first line
  double passthroughShownUntil = 0;
  // The colour balance: the video's own (RunningVideo::video.balance, saved with it),
  // or while Match the room is on and the room is shown the room's, never
  // saved: the room's light belongs to the room, not to a video. The switch
  // stays as set for the run; with the room hidden (another video, passthrough
  // off, Opacity at 100 %) the video's own is drawn, shown and edited until
  // the room shows again.
  RoomLight& roomLight;
  double roomFailedUntil = 0;  // glfwGetTime() Match the room stops saying it failed
  // Match the room, a switch: on, the room's reading (the run's first, or
  // Read again's) times the strengths, from the video's own balance (grey
  // cameras and a dark room leave the warmth and tint as they are). Moved by
  // hand while on, the sliders stay as moved until the match is applied again
  // (a strength, Read again). A reading is kept when it ends, but applied
  // only if they were not moved by hand or reset after it was asked for.
  ColourBalance roomBalance;
  std::optional<RoomLight::Reading> roomReading;  // this run's latest
  bool roomMatched = false;
  // Hand moves while on, and Picture resets; their count when last asked.
  unsigned balanceEdits = 0, roomAsked = 0;
};
