#pragma once

// Settings' Controls rules that need no menu (menu_controls.cpp draws the
// page): the controls' and gestures' names as the page says them, and what
// the gestures' editor allows.

#include "controls.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct ControlsPageStrings;
struct GesturePageStrings;

const LayoutControl* layoutControl(ControllerType type, int hand, Control control);
std::string glyphOf(const LayoutControl& c);
// A button of the type can take a job with grip held.
bool gripCombos(ControllerType type);

inline const char* printArg(const std::string& s) { return s.c_str(); }
inline const char* printArg(const char* s) { return s; }
inline int printArg(int n) { return n; }
template <typename... Args>
std::string printed(const char* format, const Args&... args) {
  char line[512];
  std::snprintf(line, sizeof(line), format, printArg(args)...);
  return line;
}

const char* typeTitle(ControllerType type, const ControlsPageStrings& t);
// A button's name as a label (the chooser's header, tooltips): "B",
// "Bumper", "Stick click".
std::string buttonName(const LayoutControl& c, const Strings& t);
// ... and in the notice's sentence: "the left bumper", "the Menu button".
std::string buttonNoun(ControllerType type, const LayoutControl& c, const Strings& t);
// A fixed control's or a gesture's glyph's name, where the glyph is missing.
const char* glyphName(const std::string& glyph, const ControlsPageStrings& cp);

// ---------------------------------------------------------------- gestures

// Its controls, in the order said and shown: the grip first, the trigger last.
std::vector<Control> gestureControls(const Gesture& g);
// A hand that has the gesture's controls (the right one where both have them).
int gestureHand(const Gesture& g);
// Both controllers have its controls: one hand's says which.
bool bothHave(ControllerType type, const Gesture& g);
std::string controlGlyphName(ControllerType type, int hand, Control control);
// A control's name in a gesture's: as a label ("Grip", "Y"), or in a sentence
// and after another ("grip", "trigger", "Y").
std::string controlName(ControllerType type, int hand, Control control, bool sentence, const Strings& t);
// Its controls as said: "Y", "Grip + trigger", "Right A"; in a sentence
// "grip + trigger", "the right A". In some languages it starts with an
// article ("la poignée"): capitalized() where it starts a title or a line.
std::string gestureName(ControllerType type, const Gesture& g, const Strings& t, bool sentence = false);
// Where a value works, beside its name: none where it works anywhere, or
// where its name says it (Zoom (180°/360°)).
const char* gestureWhere(GestureValue value, const Strings& t);
// What a value needs to change, as the value chooser's tooltip says it (as
// main.cpp's gestureHost.why checks it); "" where a file open is enough.
const char* gestureNeeds(GestureValue value, const GesturePageStrings& gp);

// The gestures that can't be changed, as the list heads Either's column.
struct FixedGesture {
  const char* name;
  std::vector<std::string> glyphs;
  bool plus;  // a + before its movement's word
  const char* move;
  bool turn;  // ... and the twist after it
  const char* where;
  const char* tip;
  bool clicks = false;  // its movement is a click of its last control (the stick's)
};
std::vector<FixedGesture> fixedGestures(ControllerType type, const Strings& t);

// A type can have gestures: a grip, or a button that can be changed.
bool gesturesFor(ControllerType type);
// The controls of a hand that can be a gesture's.
uint32_t gestureBits(ControllerType type, int hand);

// ---------------------------------------------------------------- the gestures' editor

// What is wrong with the buttons held for a gesture, or with their place.
enum class GestureMistake : uint8_t { None, Hands, Many, Trigger, Chooses, RoomLeft, RoomRight, RoomEither };

// What's wrong with these held, while they are held.
GestureMistake heldMistake(const ControlMap& map, ControllerType type, const uint32_t held[2]);
// One for either controller with a button that chooses on one of them.
bool choosesOnEither(const ControlMap& map, ControllerType type, Gesture g);
// A hand's held controls in the order said and shown: grip first, the
// trigger last, the others as the layout has them.
std::vector<Control> orderedControls(ControllerType type, int hand, uint32_t bits);
// No room for `g` in its column: those there, but the one changed and those
// it replaces, fill it.
bool noRoom(ControllerType type, const std::vector<Gesture>& list, const Gesture& g, int edit);
const char* mistakeText(GestureMistake m, const GesturePageStrings& gp);

// The value step's columns: values, and "" for its heading (`heading`).
// Values in groups and others: the others first, the group under its name
// beside them; Place's flat screen's beside the rest; else at most five in
// one column, more half and half.
struct ValueColumns {
  std::vector<GestureValue> columns[2];
  std::string heading;
};
ValueColumns valueColumns(GestureSection section, const Strings& t);
