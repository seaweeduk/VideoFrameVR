#pragma once

// What the controllers' buttons do (docs/alpha/first-run.md): the app's
// bindings (controls.h), and SteamVR's own drawings, binding points and
// glyphs for them, read from the SteamVR installed (never shipped). First-run setup's Your controllers page shows them; so can the
// hints after it.

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

typedef struct _cairo_surface cairo_surface_t;
struct ControllerStrings;

// What Your controllers and the hints name: the trigger's and the grip's
// jobs, three buttons' and the stick's.
enum class ControlAction { Select, Grab, Menu, PlayPause, Recenter, Stick, Count };
constexpr int kControlActions = static_cast<int>(ControlAction::Count);

// Each hand's interaction profile and the controls that do each
// ControlAction (describeControls()).
struct ControllerBindings {
  std::string profiles[2];  // 0 left, 1 right; "" none
  struct Source {
    int hand = 0;
    std::string input;  // under the hand: "/input/b/click"
    bool operator==(const Source& o) const { return hand == o.hand && input == o.input; }
  };
  std::vector<Source> sources[kControlActions];
  std::vector<Source> gripSources[kControlActions];  // ... with that hand's grip held
  // The gestures: each hand's controls held (the grip first, the trigger
  // last) and the GestureValue a twist then changes.
  struct Twist {
    std::vector<Source> controls;
    int value = 0;
    bool operator==(const Twist& o) const { return controls == o.controls && value == o.value; }
  };
  std::vector<Twist> twists;
  bool operator==(const ControllerBindings& o) const;
};

// The one-line hints after setup (hints.h), each about a control: the menu
// button, the stick for scrolling, recenter, and the volume gesture.
enum class Hint { Menu, Scroll, Recenter, Volume, Count };
constexpr int kHints = static_cast<int>(Hint::Count);

// What the hands hold: a bit per hand and ControlAction (Stick: pushed
// sideways), and kStickUpDown.
constexpr int kStickUpDown = kControlActions;
constexpr uint32_t heldBit(int hand, int control) { return 1u << (hand * 8 + control); }

// A line starting with a control's name, as a sentence: "Trigger or bumper".
std::string capitalized(const std::string& line);
// A source's control: "/input/b/click" → "b"; SteamVR's names as OpenXR's
// ("/input/grip/value" → "squeeze").
std::string controlOf(const std::string& input);
// In the order controlNames() names them: the trigger first, Menu last.
void sortSources(std::vector<ControllerBindings::Source>& sources);
// How the page and the hints name sources: "B, X, Y or Menu", "D-pad up,
// left or right", in the language shown.
std::string controlNames(std::vector<ControllerBindings::Source> sources, const ControllerStrings& t);
// An action's sources on `hand` (0 left, 1 right), else on the other one,
// sorted; the first is the one to name in a hint.
std::vector<ControllerBindings::Source> handSources(const ControllerBindings& bindings,
                                                    ControlAction action, int hand);
// SteamVR's glyph for a source: a file in its bindingcallouts without
// ".svg", "" none. Bound to Stick, the stick's for moving it, or for
// scrolling with upDown.
std::string controlGlyph(const std::string& input, ControlAction action, bool upDown);

// SteamVR's pictures of the controllers in use, drawn for the panel.
struct ControllerArt {
  using Image = std::shared_ptr<cairo_surface_t>;  // ARGB32, `scale` pixels per unit
  struct Drawing {
    std::string file;
    Image image;
    double width = 0, height = 0;  // in units
    // Where a bound control is on it ("b", "squeeze"), in units from its
    // top left.
    std::map<std::string, std::array<double, 2>> points;
  };
  Drawing hands[2];  // without an image: no picture of that hand
  // The bindings as drawn: on the Steam Frame, sources of SteamVR's Touch
  // fallback are moved onto the Frame's controls, as SteamVR moves them.
  ControllerBindings bindings;
  std::map<std::string, Image> glyphs;  // by controlGlyph() name, `glyph` units square
};

// Reads and draws them for `bindings`, off the render thread: the drawings
// within w × h units, glyphs `glyph` units square, `scale` pixels per unit,
// and always the trigger's glyph (w ≤ 0: the glyphs only). What `reuse`
// has drawn already is kept.
// Missing or unreadable files, or another SteamVR's JSON without a field,
// leave out what they would give.
std::shared_ptr<const ControllerArt> loadControllerArt(const ControllerBindings& bindings, double w,
                                                       double h, double glyph, double scale,
                                                       std::shared_ptr<const ControllerArt> reuse);
