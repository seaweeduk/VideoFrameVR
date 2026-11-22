#pragma once

// The controllers' buttons and what they do. OpenXR is asked for the
// physical controls (Control), each controller type's own (ControllerLayout);
// the app decides what each button does (Job), from the layout's defaults or
// the user's choice (ControlMap), and ControlInput turns a frame's presses
// into jobs.
//
// A new job: a Job, its entry in kJobTable (controls.cpp) and its label's
// string, and where no menu command does it, its case in main.cpp. A new
// control: a Control, its entry in kControlTable and its paths in the
// layouts that have it.

#include "control_map.h"
#include "controller_guide.h"
#include "controller_state.h"
#include "menu_command.h"
#include "value_ranges.h"

#include <cstdint>
#include <string>
#include <vector>

struct Strings;

struct ControlInfo {
  const char* id;    // the OpenXR action's name, and the saved bindings'
  const char* name;  // the action's localized name
};
const ControlInfo& controlInfo(Control control);

// The chooser's sections (Nothing is in none).
enum class JobSection : uint8_t { None, General, Playback, Videos, Sound, Picture };
// What a job needs to do anything: nothing, a file open (a video or a
// photo), or a video.
enum class JobNeeds : uint8_t { Nothing, File, Video };
// The seek length a job goes by: none, Skip's or Big skip's (settings.h).
enum class JobLength : uint8_t { None, Skip, BigSkip };

struct JobInfo {
  const char* id;  // saved; never changed once released
  JobSection section;
  const char* (*label)(const Strings& t);  // printf: the length, where it has one
  JobNeeds needs;
  JobLength length;
  // How it runs: this command, as if the menu sent it (its value times the
  // length's seconds, where it has one); None: main.cpp does it itself.
  MenuCommand command;
  // It goes on while its button is held (Peek at the room, Zoom in and out),
  // where others happen once a press.
  bool held = false;
};
const JobInfo& jobInfo(Job job);
// Its label as the chooser shows it, with the lengths' seconds.
std::string jobLabel(Job job, const Strings& t, int skipSeconds, int bigSkipSeconds);
// A skip's length as said: "10 s", "2 min".
std::string skipLength(int seconds, const Strings& t);
// What the chooser's tooltip says about it; null where its label says enough.
const char* jobTip(Job job, const Strings& t);
const char* jobSectionTitle(JobSection section, const Strings& t);

struct LayoutControl {
  Control control;
  int hand;           // 0 left, 1 right
  const char* input;  // its path under the hand: "/input/b/click"
  Job job;            // what it does unless the user chose another
  bool fixed;         // ... always: no other can be chosen
};
struct ControllerLayout {
  const char* id;       // the saved bindings'
  const char* profile;  // the OpenXR interaction profile
  bool stick;           // it has a thumbstick on each hand
  std::vector<LayoutControl> controls;
};
const ControllerLayout& controllerLayout(ControllerType type);
// The type of an interaction profile; Count for none, or one without a layout.
ControllerType controllerType(const std::string& profile);
ControllerType controllerTypeById(const std::string& id);
// Every control of a type's layout as sources, for SteamVR's drawings of
// them and their glyphs (loadControllerArt()).
ControllerBindings layoutSources(ControllerType type);

// The value chooser's sections.
enum class GestureSection : uint8_t { Sound, Room, Picture, Place, Playback, Funscripts, Subtitles, Count };
// Where a value can be changed: anywhere, with a video or photo open, a
// video, 180°/360° video or the flat screen. What else it needs (passthrough
// on, Match the room on ...) main.cpp says.
enum class GestureWhere : uint8_t { Anywhere, File, Video, Surround, Flat };
// How its number is said (gestureValueText()).
enum class GestureFormat : uint8_t {
  Number, Signed, Percent, Centimetres, Metres, Hundredths, Viewpoint, Milliseconds, Clock, Stars
};

struct GestureValueInfo {
  const char* id;  // saved; never changed once released
  GestureSection section;
  const char* (*label)(const Strings& t);
  // Said before the label where it says too little alone ("Subtitles" before
  // "Size"); null for none. The chooser shows the label under it as a heading.
  const char* (*group)(const Strings& t);
  GestureWhere where;
  // In the units its command carries (value_ranges.h); Rating's in half
  // stars, Scrub's in seconds (to the video's end, main.cpp's).
  ValueRange range;
  double perDegree;  // ... a degree of twist changes it by
  int detent;        // a buzz at each multiple of this, from 0 or the range's start
  bool centred;      // it goes both ways from 0, which the ring marks
  int mark;          // its default, which the ring marks; kNoMark for none
  GestureFormat format;
  // How it is set: this command with the value, as the menu's slider sends
  // it; None: main.cpp sets it itself (Scrub, Rating).
  MenuCommand::Type command;
};
constexpr int kNoMark = -100000;
const GestureValueInfo& gestureValueInfo(GestureValue value);
const char* gestureSectionTitle(GestureSection section, const Strings& t);
// Its name as the ring and the Controls page say it ("Subtitles: Size").
std::string gestureValueLabel(GestureValue value, const Strings& t);
// Its number as the ring says it ("64 %", "−12", "+20 ms", "1.00"); Scrub's
// and Rating's are drawn otherwise.
std::string gestureValueText(GestureValue value, double number);

// Settings' Controls lists a type's gestures in three columns of
// kGestureRows rows: the left controller's (0), either's (1) and the right
// one's (2). Either's starts with the gestures that can't be changed and
// ends with Add gesture. A column holds no more than it has room for.
constexpr int kGestureRows = 11;
int gestureColumn(const Gesture& g);
int gestureColumnRoom(ControllerType type, int column);

// A button of the gesture that chooses on that hand (with grip held where
// the gesture has the grip), the trigger aside, else Control::Count. It
// selects as it goes down, as the trigger does, so the gesture doesn't run
// there.
Control choosingButton(const ControlMap& map, ControllerType type, int hand, const Gesture& g);

// The bindings as first-run's Your controllers and the hints show them, for
// each hand's interaction profile, with the gestures.
ControllerBindings describeControls(const std::string profiles[2], const ControlMap& map,
                                    const GestureMap& gestures);
void logControls(const ControllerBindings& bindings);

// A frame's controller input as jobs.
struct JobInput {
  // As read, with a hand's select held by the buttons that choose.
  ControllerState state;
  uint8_t pressed[kJobs] = {};  // a bit per hand that pressed it this frame
  uint8_t held[kJobs] = {};     // ... that holds it
  // A bit per hand whose presses were dropped: the app lost input focus, or
  // the controller went or changed. What it held does nothing until let go.
  uint8_t lost = 0;
  bool wasPressed(Job job) const { return pressed[static_cast<int>(job)] != 0; }
  bool isHeld(Job job, int hand) const { return held[static_cast<int>(job)] >> hand & 1; }
  // The hand that pressed it (the right one if both did), else -1.
  int pressedBy(Job job) const {
    const uint8_t bits = pressed[static_cast<int>(job)];
    return bits & 2 ? 1 : bits & 1 ? 0 : -1;
  }
};

// Turns each frame's controls into jobs by the map, for each hand's
// controller type.
// A button's job is the one it had when pressed (with grip held or not)
// until it is let go. A hand's select goes down only with a press of a
// button that chooses.
// A press of a gesture's button (GestureMap::tapOnRelease()) waits: let go
// within kHoldStart, its job is done then, as a tap. Held longer, a held
// job (JobInfo::held) starts and goes on until it is let go, and no gesture
// starts with that press (holdingJob()); another job waits for the release.
// Once the gesture gets under way (cancelTap()), the press does no job.
// A hand that loses its controller or the app's focus isn't letting go:
// its presses do no job, and what it held stays without one until let go
// (JobInput::lost).
class ControlInput {
 public:
  static constexpr double kHoldStart = 0.3;  // seconds

  ControlInput();
  // gestures: null while no press waits (setup shows what each button does).
  const JobInput& update(const ControllerState& raw, const ControllerType types[2],
                         const ControlMap& map, const GestureMap* gestures, double now);
  // Before update(): a press this frame that did something else (grip and a
  // stick's click on the stars clear a rating; a press skips setup's
  // intro). It does nothing, now or while held, and isn't held in the
  // state update() gives (no gesture starts with it).
  void consume(int hand, Control control) { consume_[hand] |= controlBit(control); }
  // A gesture got under way with this button held: its press does no job.
  void cancelTap(int hand, Control control);
  // Another video or none: the presses waiting to do a job that needs one
  // (JobInfo::needs) do none.
  void dropFileTaps();
  // Its press is doing its held job (past kHoldStart): no gesture of it now.
  bool holdingJob(int hand, Control control) const {
    return tap_[hand][static_cast<int>(control)] == Tap::Holding;
  }

 private:
  static constexpr uint8_t kFree = 0xff, kConsumed = 0xfe;
  // A press that waits: not one, waiting, its held job going, or no more.
  enum class Tap : uint8_t { None, Waiting, Holding, Done };
  JobInput jobs_;
  uint8_t active_[2][kControls];  // each held button's Job, as pressed; kFree, kConsumed
  Tap tap_[2][kControls] = {};
  double pressedAt_[2][kControls] = {};
  uint32_t consume_[2] = {0, 0};
  ControllerType types_[2] = {ControllerType::Count, ControllerType::Count};
  uint32_t focusLosses_ = 0;
};
