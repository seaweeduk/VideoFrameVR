#pragma once

// Settings' Controls section (menu_controls.cpp): SteamVR's drawings of the
// controllers shown, with a label for each control and a line to it; the
// gestures as a list; the chooser of a button's job and the gestures' editor.

#include "control_rules.h"
#include "menu.h"
#include "setup_places.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

struct ControlsPage {
  // SteamVR's drawings and glyphs for `type`, read on a thread of their own.
  ControllerType type = ControllerType::Count;
  std::shared_ptr<const ControllerArt> art;
  setup_places::Async<std::shared_ptr<const ControllerArt>> loading;
  uint32_t held[2] = {0, 0};  // each hand's controlBit()s

  // What the page draws itself, or a button draws (Menu::Button::controlPart).
  struct Part {
    enum Kind : uint8_t {
      Label,    // a button's: raised, its glyph and job
      Fixed,    // a control that can't be changed: flush, its glyph, what it does and how
      Gesture,  // a gesture's row: its controls' glyphs joined by +, the movement, → what it does
      Job,      // a job or a gesture's value in a chooser: its label, the glyphs of the buttons doing it
      Add,      // Add gesture
    } kind = Label;
    double x = 0, y = 0, w = 0, h = 0;
    std::vector<std::string> glyphs;  // by controlGlyph() name
    std::vector<std::string> names;   // each glyph's button, where the glyph is missing
    std::vector<std::string> tags;    // Job: before each glyph, its hand's letter, or ""
    // Job: the glyph of the button held with each (grip), or "", and its name.
    std::vector<std::string> withs, withNames;
    std::string text, how;
    // Fixed: where `text` is too long for its place, its lines (smaller,
    // one under another), each in its `shorter` form where still too long.
    std::vector<std::string> lines, shorter;
    char align = 'l';  // Fixed: its content's side ('l', 'r') or 'c' centred
    bool faint = false;      // Label, Gesture: it does nothing
    bool inherited = false;  // Label, with grip held: no grip job, its own job (dim)
    bool twist = false;      // Label: its button is a gesture's (a mark at its end)
    std::string name;        // Label: the button's, where its glyph is missing
    // Gesture: one that can't be changed (flush, dim); a word after its
    // glyphs (Horizon's drag); the movement: a + before it, its word, the
    // twist's mark after it.
    bool fixed = false, plus = false, turn = false;
    std::string word, move;
    // Gesture: its movement is in its last control's name (Clear rating's
    // stick click), so the word is left out where that name shows.
    bool moveNamed = false;
    // Gesture: where its controls end, its arrow and what it does start; a
    // column's rows line up (Menu::placeGestureRows()).
    double controlsEnd = 0, arrowX = 0, valueX = 0;
    int hands = 0;       // a bit per hand whose `control` held lights it
    Control control = Control::Count;
  };
  std::vector<Part> parts;
  // The lines from the labels to their controls on the drawings.
  struct Lead {
    double ax, ay, x, y;
    bool fixed;
    int hand;
    Control control;  // Count: none (a stick's directions)
  };
  std::vector<Lead> leads;
  bool picture = false;  // with the drawings, else the labels as a list
  // The well under the drawings: the buttons' jobs with grip held, or the
  // gestures, shown instead of the buttons' own.
  bool gripView = false, gestureView = false;
  double drawX[2] = {0, 0}, drawY = 0, artTop = 0, artBottom = 0;
  std::vector<std::string> headings;  // the list's: each column's, at headingX
  std::vector<double> headingX;

  // The chooser: its button, the section it shows, its header as placed.
  ControlSlot open;
  JobSection section = JobSection::General;
  double headX = 0, headY = 0, headW = 0;
  // A job, or with grip held kInherit: none of its own.
  static constexpr int kInherit = -1;

  // The gestures' editor: first the buttons (held on the controller, then let
  // go), then the value.
  struct Editor {
    using Mistake = GestureMistake;
    int step = 1;
    int edit = -1;    // the gesture it changes (its index), else a new one
    bool back = false;  // the buttons again, from the value: Back goes back
    // The value step's: its hands and buttons, and its value now (or Count).
    Gesture gesture;
    int side = 1;  // the hand its buttons were held on
    // Held as the step began (not its buttons until let go), held since the
    // first press, and what the step shows: those, or what went wrong.
    uint32_t ignore[2] = {0, 0}, seen[2] = {0, 0}, shown[2] = {0, 0};
    Mistake mistake = Mistake::None;
    GestureSection section = GestureSection::Sound;
    // Where the first step's drawings go (each hand's left edge, their top)
    // and what is held between them; the second's header.
    double drawX[2] = {0, 0}, drawY = 0, heldX = 0, heldW = 0;
    double headX = 0, headY = 0, headW = 0;
    // The second step's heading inside a column of values, if any.
    std::string heading;
    double headingX = 0, headingY = 0, headingW = 0;
  };
  Editor editor;

  // The notice after a change replaced a job, with Undo: the jobs the slot
  // did before and does now (with grip held, its own if none of its own),
  // and the button's jobs before (ControlMap::button()), all put back. Or
  // after a gesture changed or went: the gesture, its value before and now
  // (Count: none), and the type's gestures before (GestureMap::listLine()).
  struct Notice {
    ControlSlot slot;
    Job was = Job::None, now = Job::None;
    int before = 0;
    bool gesture = false;
    Gesture changed;
    GestureValue wasValue = GestureValue::Count, nowValue = GestureValue::Count;
    std::string gestures;
  };
  bool noticeShown = false;
  Notice notice;
  double noticeBox[4] = {0, 0, 0, 0};
  bool noticeCovers = false;  // it found no clear place: what it covers goes while it shows
  std::string noticeText;
  std::vector<std::string> noticeGlyphs;  // joined by +, then the twist's mark after a gesture's
  // Its text's lines, the width they wrap at and their height (one line: 0).
  int noticeLines = 1;
  double noticeWrapW = 0, noticeTextH = 0;
};
