#pragma once

#include <memory>
#include <string>

// Plays a video's funscript (funscript.h) on a device through Intiface
// (intiface.h): the device strokes in time with the video. Off by default;
// while off none of this runs: no thread, no connection, and no script is
// looked for.
//
// The app tells it which video plays and, every frame, where that video is.
// A worker thread does the rest: it keeps the connection to Intiface (trying
// again every few seconds while nothing answers), and at each of the
// script's actions sends the move to the next one, to arrive when the video
// reaches it. The device is held where it is as soon as the video stops
// moving, for whatever reason: paused, seeking, buffering, at its end, the
// headset taken off.

// Kept in settings.conf as script_* keys.
struct ScriptSettings {
  bool enabled = false;
  // Where Intiface listens: "host" or "host:port" (port 12345 when none is
  // given). Empty: this computer on a PC; not set up on a headset, where
  // Intiface runs on another device.
  std::string address;
  // The device is sent each move this much earlier, milliseconds (later if
  // negative): what it and its connection take to react.
  int offsetMs = 0;
  // The part of the device's stroke the script's 0..100 is played on, percent.
  int rangeLow = 0, rangeHigh = 100;

  bool operator==(const ScriptSettings& o) const {
    return enabled == o.enabled && address == o.address && offsetMs == o.offsetMs &&
           rangeLow == o.rangeLow && rangeHigh == o.rangeHigh;
  }
  bool operator!=(const ScriptSettings& o) const { return !(*this == o); }
};
constexpr int kScriptOffsetLimit = 1000;  // offsetMs is within this of 0
constexpr int kScriptRangeGap = 10;       // rangeHigh is at least this above rangeLow

// The address Intiface is looked for at: the one set, else this computer's
// on a PC; empty if there is none to try.
std::string scriptAddress(const ScriptSettings& settings);

// How far the way to a device has got.
enum class ScriptLink {
  Off,         // the setting is off
  NoAddress,   // no address to look for Intiface at
  Connecting,  // the first try is under way
  NoServer,    // nothing answers as Intiface there (tried again every few seconds)
  NoDevice,    // Intiface answers, but has no device that strokes
  Ready,       // a device is there
};

struct ScriptStatus {
  ScriptLink link = ScriptLink::Off;
  std::string device;  // Ready: its name
  int devices = 0;     // ... and how many there are (all of them move)
  bool script = false;  // the video playing has a script
  bool operator==(const ScriptStatus& o) const {
    return link == o.link && device == o.device && devices == o.devices && script == o.script;
  }
  bool operator!=(const ScriptStatus& o) const { return !(*this == o); }
};

// All calls from one thread (the render loop's); none waits on the network
// or a disk.
class ScriptPlayer {
 public:
  ScriptPlayer() = default;
  ~ScriptPlayer();
  ScriptPlayer(const ScriptPlayer&) = delete;
  ScriptPlayer& operator=(const ScriptPlayer&) = delete;

  // The settings as they are now: starts or stops the worker with `enabled`.
  void configure(const ScriptSettings& settings);
  // The video playing is now the one at `location` (a path, a remote
  // locator or a Stash scene's; empty: none): its script is looked for.
  void open(const std::string& location);
  // Every frame: where the video is, seconds, and whether it is meant to be
  // moving (not paused, seeking or loading). One that says so but whose
  // position stands still counts as stopped.
  void sync(double seconds, bool playing);
  ScriptStatus status() const;

  struct Shared;

 private:
  void start();
  void stop(bool wait);
  void load();

  std::shared_ptr<Shared> shared_;  // while enabled
  ScriptSettings settings_;
  std::string location_;
  // sync()'s own: the position last seen and when it changed, and what the
  // worker was last told.
  double lastSeconds_ = -1, lastChange_ = 0;
  bool playing_ = false;
  double base_ = 0;
};
