#pragma once

#include "control_map.h"
#include "menu_command.h"
#include "settings.h"

#include <cstdint>
#include <string>
#include <unordered_map>

class Library;
class RunningVideo;
class VideoPlayer;
struct JobInput;

// What a press of a button's job does, in this order: what it says, then the
// command, as if the menu sent it.
struct JobAction {
  std::string said;          // empty: nothing
  bool showOpacity = false;  // See-through: the opacity's value shows
  GestureValue showRing = GestureValue::Count;  // this value's ring shows (Zoom in and out)
  MenuCommand command;       // None: nothing
};

// The buttons' other jobs (controls.h), as the menu's or the strip's
// controls do them, and what they turn back on. Main thread only.
class ButtonJobs {
 public:
  // What else is going on at the press.
  struct Context {
    double now = 0;
    bool photo = false;
    bool slideshow = false;  // a photo's slideshow runs
    bool passthroughAvailable = false;
    bool alphaBlend = false;    // the room can show (XrContext::alphaBlendSupported())
    bool roomReadable = false;  // Passthrough::roomReadable
    bool roomShown = false;     // Passthrough::roomShown()
    bool roomMatched = false;   // Passthrough::matched()
    AfterVideo afterVideo = AfterVideo::Stop;
  };

  ButtonJobs(const VideoPlayer& player, RunningVideo& running, const Settings& settings,
             const Library& library)
      : player(player), running(running), settings(settings), library(library) {}

  // A press of a job that has what it needs (JobInfo::needs), by that hand.
  // Frame back and forward, Rating up and down and Delete get only their
  // command: the caller does them.
  JobAction press(Job job, int hand, const Context& context);
  // After the press's command: the switches whose change doesn't show by
  // itself, as they are now; empty for the rest.
  std::string saidAfter(Job job, AfterVideo afterVideo) const;
  // Each frame, before the presses: the running video's see-through, for
  // See-through to go back to.
  void keepSeeThrough();
  // Each frame, after the presses: a held Zoom in or out goes on. True if
  // the zoom changed (its ring shows).
  bool holdZoom(const JobInput& jobs, double now);
  // A zoom is held: it is stored once let go.
  bool zoomHeld() const { return zoomHold.hand >= 0; }

 private:
  const VideoPlayer& player;
  RunningVideo& running;
  const Settings& settings;
  const Library& library;

  std::string seeThroughKey() const;

  // The buttons' Subtitles and Opacity jobs: what they turn back on; the
  // opacity, each video's last see-through value however it was set, for
  // this run (by its library id, else where it is).
  int64_t lastSubtitle = 0;
  std::unordered_map<std::string, int> lastSeeThrough;
  std::string seeThroughFile;
  int seeThroughSeen = 100;
  // Zoom in or out held: by which hand (-1 none), since when, and how far
  // it has gone, on which file. It is stored once, when let go.
  struct ZoomHold {
    Job job = Job::None;
    int hand = -1;
    double start = 0, at = 0;
    std::string file;
  } zoomHold;
};
