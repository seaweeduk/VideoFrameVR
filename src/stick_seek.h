#pragma once

#include "vr_input.h"

#include <functional>

struct MenuPanels;
struct PlaybackState;
class VideoPlayer;

// The stick's seeks stop this far before the end: a seek into it would end
// the video (and open the menu).
constexpr double kSeekEndMargin = 5.0;

// Either stick sideways, or the arrow keys, seek (HeldSeek): a tap jumps, a
// hold pauses the video and shows the strip while its target moves on, and
// the video goes there when it lets go. Also the Scrub gesture's picture,
// which follows its target as a hold's does.
class StickSeek {
 public:
  // What else is going on this frame.
  struct Frame {
    double now = 0;
    bool seekable = false;     // a video is open, not a photo
    bool sticksTaken = false;  // typing, or a stick steps a value: no stick seeks
    bool backKey = false, forwardKey = false;  // the arrow keys held down
    int skipSeconds = 10;      // Skip's length, a stick's tap
    bool headValid = false;
    Vec3 headPos;
    Mat4 menuAt;  // where the menu opens (menuFrame): a hold's strip goes there
  };

  StickSeek(VideoPlayer& player, MenuPanels& panel) : player(player), panel(panel) {}

  // A pulse on a controller; set once, before the frames.
  std::function<void(int hand, float amplitude, double seconds)> buzz;

  // Each frame. True if a seek of the user's started.
  bool update(const ControllerState& input, const Frame& frame);
  // Ends a seek at once, sending nothing (another video, or none).
  void cancel();

  // The input is down.
  bool active() const { return heldSeek.active(); }
  // A hold goes on, or the video isn't yet where it let go.
  bool held() const { return heldSeek.holding() || heldSeekLanding; }
  // The hold paused the video.
  bool pausedVideo() const { return heldSeekPaused; }
  // The playback the strip shows: while held(), where it goes.
  void fill(PlaybackState& shown) const;
  // The strip placed for a hold, the menu closed, still shown.
  bool stripShown(double now) const { return heldSeekBar && (heldSeek.holding() || now < heldSeekBarUntil); }

  // The Scrub gesture (GestureInput::Host): where it takes the video.
  void beginScrub(double now);
  void endScrub(bool released);
  void scrubTo(double target) { scrub.target = target; }
  double scrubValue() const;
  // Each frame, after the gestures: the picture follows a scrub.
  void followScrub(double now);

 private:
  VideoPlayer& player;
  MenuPanels& panel;

  // Scrub: where it takes the video, and the last picture shown on the way
  // there (its target, when sent), as the stick's held seek.
  struct Scrub {
    bool on = false, sent = false;  // ... a picture on the way
    double from = 0, target = 0, picture = 0, pictureAt = 0;
  } scrub;
  // The stick's seek (HeldSeek): from where, whether it paused the video,
  // and whether the bar shows where it goes until the video is there.
  HeldSeek heldSeek;
  double heldSeekFrom = 0, heldSeekTarget = 0;
  bool heldSeekPaused = false, heldSeekLanding = false;
  bool heldSeekBar = false;  // the strip placed for it, the menu closed
  double heldSeekPicture = 0, heldSeekPictureAt = 0;  // the last picture's target, when sent
  double heldSeekBarUntil = 0;  // ... shown after the stick let go
  // Driven by a stick (not the arrow keys), it buzzes that hand: a tick each
  // 10 s it goes, at most 8 a second, and a bump at the start or the end.
  bool heldSeekStick = false, heldSeekAtLimit = false;
  int heldSeekHand = 1;  // the stick's
  double heldSeekTens = 0, heldSeekTickAt = 0;
};
