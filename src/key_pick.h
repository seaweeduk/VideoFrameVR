#pragma once

#include "controller_state.h"
#include "gl_util.h"
#include "key_picker.h"
#include "menu_view.h"
#include "stereo_correction.h"
#include "xmath.h"

#include <functional>
#include <string>

struct EyeMatrices;
struct MenuCommand;
class RunningVideo;

// Picking the background key's colour (MenuCommand::PickKeyColour): the
// video it is for, a press of the trigger away from the menu where the
// laser dot is seen, the read of the colour there (KeyPicker), and what the
// menu shows meanwhile. Main thread only.
class KeyPick {
 public:
  // At startup, the GL read's program and buffers; false if they failed.
  bool init() { return picker_.init(); }

  // PickKeyColour and CancelKeyPick; false for any other command.
  bool handle(const MenuCommand& c, const RunningVideo& running);
  // Picking ends; a read on its way is forgotten.
  void end();

  // What the press needs this frame (handInput).
  struct Frame {
    bool menuOpen = false;
    int activeHand = 1;  // the hand whose pointer drives the menu
    bool onMenu[2] = {false, false};
    bool headValid = false;
    bool peeking = false;  // the room shows instead of the video
    const EyeMatrices* leftEye = nullptr;
    float laserLength = 0;  // where the laser's dot is drawn
    const EyeCorrection* correction = nullptr;  // the left eye's, as last drawn
    float frameAspect = 1;
  };
  // Each frame: ends picking where the key no longer applies, then takes a
  // press of the active hand's trigger away from the menu as the point to
  // read.
  void update(const ControllerState& input, const Frame& frame, const RunningVideo& running);

  bool picking() const { return !for_.empty(); }
  // A press waits to be read.
  bool wanted() const { return wanted_; }
  // A read is on its way.
  bool busy() const { return picker_.busy(); }
  // Reads the point pressed (KeyPicker::read); `unreadable`: a frame that
  // stays out of GL without a drawer for it.
  void read(bool unreadable, GLuint texture, bool tvGamma, const std::function<bool()>& prepare,
            DepthPatchDrawer* drawer);
  // The read, once it comes: a colour picked becomes the video's background
  // and picking ends; one that crossed an edge or failed lets it go on, for
  // another press.
  void takeResult(RunningVideo& running);

  void fill(MenuView& view) const;

 private:
  KeyPicker picker_;
  std::string for_;  // the video picking is for; empty: not picking
  // A press to read at uv_ within part_ (KeyPicker::pointOf()).
  bool wanted_ = false;
  float uv_[2] = {}, part_[4] = {};
  KeyPickNote note_ = KeyPickNote::None;  // why the last one gave none
};
