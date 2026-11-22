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

#include "controller_guide.h"
#include "controller_state.h"
#include "menu_command.h"
#include "value_ranges.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct Strings;

// The buttons OpenXR is asked for, one boolean action each, on both hands.
// The thumbstick (a vector) and the pointer's pose are read on their own.
enum class Control : uint8_t {
  Trigger, Bumper, Grip, A, B, X, Y, Menu, View, DpadUp, DpadDown, DpadLeft, DpadRight, StickClick,
  Count
};
constexpr int kControls = static_cast<int>(Control::Count);
constexpr uint32_t controlBit(Control c) { return 1u << static_cast<int>(c); }

struct ControlInfo {
  const char* id;    // the OpenXR action's name, and the saved bindings'
  const char* name;  // the action's localized name
};
const ControlInfo& controlInfo(Control control);

// What a button can do, in the order the chooser shows them.
enum class Job : uint8_t {
  None,
  Menu, Recenter, Choose,
  PlayPause, Stop, SkipBack, SkipForward, BigSkipBack, BigSkipForward, FrameBack, FrameForward,
  PreviousChapter, NextChapter, FromStart, Scripts,
  PreviousVideo, NextVideo, AfterVideo, RatingUp, RatingDown, DeleteVideo,
  VolumeUp, VolumeDown, Mute, NextAudioTrack, Subtitles,
  Passthrough, PeekRoom, SeeThrough, MatchRoom, RemoveBackground, ToneLook, TvGamma, Motion, ZoomIn, ZoomOut,
  StereoFocus, ShowFocusTarget, ResetScreen, ResetPicture,
  Count
};
constexpr int kJobs = static_cast<int>(Job::Count);

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

// The controller types with a layout of their own, by OpenXR interaction
// profile. Others reach the app through SteamVR's remapping onto one of
// them.
enum class ControllerType : uint8_t { Frame, Index, Touch, Simple, Count };
constexpr int kControllerTypes = static_cast<int>(ControllerType::Count);

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

// A changeable control of a type, alone or with grip held, as a menu
// command carries it (MenuCommand::option).
struct ControlSlot {
  ControllerType type = ControllerType::Count;
  int hand = 0;
  Control control = Control::Count;
  bool grip = false;
  int pack() const {
    return static_cast<int>(type) << 6 | grip << 5 | hand << 4 | static_cast<int>(control);
  }
  static ControlSlot unpack(int packed) {
    return {static_cast<ControllerType>(packed >> 6 & 7), packed >> 4 & 1,
            static_cast<Control>(packed & 15), (packed >> 5 & 1) != 0};
  }
  bool operator==(const ControlSlot& o) const {
    return type == o.type && hand == o.hand && control == o.control && grip == o.grip;
  }
};

// What each controller type's buttons do: the user's choices, else the
// layout's defaults. Kept in the settings as each type's changes. With
// that hand's grip held, a button does its grip job if it has one (none by
// default; Nothing is one), else its own.
class ControlMap {
 public:
  ControlMap();
  // Job::None for a control the type hasn't on that hand; on an unknown
  // type, the trigger chooses and nothing else does anything.
  Job job(ControllerType type, int hand, Control control) const;
  // A changeable control's job; false if it is fixed or not there.
  bool set(ControllerType type, int hand, Control control, Job job);
  // How many of the type's controls do it.
  int count(ControllerType type, Job job) const;
  // A change Menu allows: it always keeps a button (one of them fixed, or
  // the last one left).
  bool allowed(ControllerType type, int hand, Control control, Job job) const;
  // A grip job of its own (Nothing too), and which: Job::None without
  // one. A stick's click (with grip held it clears a rating) and a button
  // that chooses (with grip held it gives volume, as the trigger does) take
  // none; a button's grip job goes when it is set to choose.
  bool hasGrip(ControllerType type, int hand, Control control) const;
  Job gripJob(ControllerType type, int hand, Control control) const;
  bool takesGrip(ControllerType type, int hand, Control control) const;
  bool setGrip(ControllerType type, int hand, Control control, Job job);
  // Back to doing with grip held what it does alone.
  bool clearGrip(ControllerType type, int hand, Control control);
  // What a press does, with that hand's grip held or not.
  Job pressJob(ControllerType type, int hand, Control control, bool gripHeld) const {
    return gripHeld && hasGrip(type, hand, control) ? gripJob(type, hand, control)
                                                     : job(type, hand, control);
  }
  // A change of a slot's job, alone or with grip held.
  bool set(const ControlSlot& s, Job job) {
    return s.grip ? setGrip(s.type, s.hand, s.control, job) : set(s.type, s.hand, s.control, job);
  }
  // A button's jobs alone and with grip held (none of its own: -1) as one
  // number, and those put back: an Undo's.
  int button(ControllerType type, int hand, Control control) const;
  bool restore(ControllerType type, int hand, Control control, int button);
  // Every button alone and with grip held back to its default.
  void reset(ControllerType type);
  // The changes from the defaults as one line ("right_b_button:none,...",
  // grip jobs as "grip_left_x_button:stop"), and such a line read: unknown
  // controls and jobs are left out, so they keep their defaults.
  std::string line(ControllerType type) const;
  void read(ControllerType type, const std::string& line);
  bool operator==(const ControlMap& o) const;
  bool operator!=(const ControlMap& o) const { return !(*this == o); }

 private:
  static constexpr uint8_t kDefault = 0xff;
  uint8_t chosen_[kControllerTypes][2][kControls];  // a Job, or kDefault
  uint8_t grip_[kControllerTypes][2][kControls];    // a Job, or kDefault: none
  // Menu always keeps a button: a type's read without one gets the layout's.
  void keepMenu(ControllerType type);
};

// How a gesture's hand moves while its buttons are held: only the wrist's
// twist for now. Saved by id, so others can be added.
enum class Movement : uint8_t { Twist, Count };
constexpr int kMovements = static_cast<int>(Movement::Count);
const char* movementId(Movement movement);

// What a gesture changes: a value the menu also sets, in the chooser's
// order.
enum class GestureValue : uint8_t {
  Volume,
  Opacity, MatchBrightness, MatchColour, KeyStrength, KeyEdges, KeySpill, KeyShrink,
  Brightness, Contrast, Saturation, Warmth, Tint, Exposure, Sharpen, Zoom,
  Viewpoint, ScreenDistance, ScreenHeight, ScreenCurve,
  Scrub, Rating,
  ScriptTiming,
  SubtitleSize, SubtitleRaise, SubtitleDistance,
  Count
};
constexpr int kGestureValues = static_cast<int>(GestureValue::Count);

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

// A gesture: one button, or two on one controller, held, and the hand's
// movement changes a value. The trigger is only ever the second; the
// trigger alone isn't one (it chooses and drags).
struct Gesture {
  uint8_t hands = 0;  // a bit per hand that does it: 1 left, 2 right, 3 either
  Control first = Control::Count;
  Control second = Control::Count;  // Count: one button
  Movement movement = Movement::Twist;
  GestureValue value = GestureValue::Count;

  bool has(Control c) const { return c != Control::Count && (first == c || second == c); }
  int controls() const { return second == Control::Count ? 1 : 2; }
  // Its buttons as bits (controlBit()).
  uint32_t bits() const {
    return controlBit(first) | (second == Control::Count ? 0u : controlBit(second));
  }
  // The same buttons and movement on a hand of both: one replaces the other.
  bool sameHold(const Gesture& o) const {
    return (hands & o.hands) != 0 && bits() == o.bits() && movement == o.movement;
  }
  // As a menu command carries it (MenuCommand::option, with the type).
  int pack() const {
    return static_cast<int>(value) << 12 | static_cast<int>(movement) << 10 |
           static_cast<int>(second) << 6 | static_cast<int>(first) << 2 | hands;
  }
  static Gesture unpack(int packed) {
    Gesture g;
    g.hands = static_cast<uint8_t>(packed & 3);
    g.first = static_cast<Control>(packed >> 2 & 15);
    g.second = static_cast<Control>(packed >> 6 & 15);
    g.movement = static_cast<Movement>(packed >> 10 & 3);
    g.value = static_cast<GestureValue>(packed >> 12 & 31);
    return g;
  }
  bool operator==(const Gesture& o) const {
    return hands == o.hands && first == o.first && second == o.second && movement == o.movement &&
           value == o.value;
  }
  bool operator!=(const Gesture& o) const { return !(*this == o); }
};
constexpr int kMaxGestures = 12;  // a controller type's
// Settings' Controls lists a type's gestures in three columns of
// kGestureRows rows: the left controller's (0), either's (1) and the right
// one's (2). Either's starts with the gestures that can't be changed and
// ends with Add gesture. A column holds no more than it has room for.
constexpr int kGestureRows = 11;
int gestureColumn(const Gesture& g);
int gestureColumnRoom(ControllerType type, int column);

// Each controller type's gestures: the user's, else the layout's defaults.
// Kept in the settings as a type's whole list once it differs from them.
// Each type's list is shared between copies until one of them changes, so
// the menu's copy each frame costs no more than a pointer's.
class GestureMap {
 public:
  GestureMap();
  const std::vector<Gesture>& list(ControllerType type) const;
  static const std::vector<Gesture>& defaults(ControllerType type);
  // Whether the type can have it: a known value and movement, its buttons
  // on each of its hands and changeable or the grip, the trigger only
  // second, not one button twice.
  static bool valid(ControllerType type, const Gesture& g);
  // A new one, in place of any with the same hold (those go into
  // `replaced`, for Undo; one of either hand's stays on the hand `g` leaves
  // it); false if it isn't valid, or that would be more than kMaxGestures or
  // than its column has room for.
  bool add(ControllerType type, const Gesture& g, std::vector<Gesture>* replaced = nullptr);
  // The index-th changed into `g` (another value, or other buttons), also in
  // place of any other with its hold, as add() does; `replaced` gets the old
  // one first.
  bool replace(ControllerType type, size_t index, const Gesture& g,
               std::vector<Gesture>* replaced = nullptr);
  bool remove(ControllerType type, size_t index);
  // A list put back (Undo); those not valid are left out.
  void restore(ControllerType type, const std::vector<Gesture>& gestures);
  void reset(ControllerType type);  // the defaults again
  bool full(ControllerType type) const { return list(type).size() >= kMaxGestures; }
  // A button that is one of a gesture's on that hand (the Controls page marks it).
  bool hasGesture(ControllerType type, int hand, Control control) const;
  // The gesture of exactly these buttons on that hand (b: Count for one), or null.
  const Gesture* find(ControllerType type, int hand, Control a, Control b = Control::Count,
                      Movement movement = Movement::Twist) const;
  // A press of `control` on that hand waits to do its job until it is let go
  // (and does it only if no gesture got under way meanwhile): it is a
  // gesture's button, with the grip only while that is held (`held`:
  // controlBit()s). Never the trigger's.
  bool tapOnRelease(ControllerType type, int hand, Control control, uint32_t held) const;
  // The list as one line ("right:y_button:twist:opacity,either:grip+trigger:
  // twist:volume"); "none" without any. line() is empty while it is the
  // defaults. A line read: entries with unknown ids, or not valid, are left
  // out; an empty one is the defaults.
  std::string line(ControllerType type) const;
  std::string listLine(ControllerType type) const;
  void read(ControllerType type, const std::string& line);
  bool operator==(const GestureMap& o) const;
  bool operator!=(const GestureMap& o) const { return !(*this == o); }

 private:
  using List = std::shared_ptr<const std::vector<Gesture>>;
  static const List& defaultList(ControllerType type);
  List lists_[kControllerTypes];
};

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
