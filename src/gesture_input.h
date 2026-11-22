#pragma once

#include "controls.h"
#include "xmath.h"

#include <deque>
#include <functional>

// The gestures (controls.h) as the hands do them: a hand holds a gesture's
// buttons, the last one just pressed, and twists its wrist. One at a time.
//  - It doesn't start while that hand points at the menu if it has the grip
//    or the trigger (the grip moves the menu there, the trigger chooses);
//    buttons alone start anywhere. Of those a press completes, the one with
//    the most buttons: grip held and then the trigger is grip + trigger, also
//    while the grip alone is under way. A gesture with the trigger starts
//    only as the trigger goes down, one with the grip as its other button
//    does (the grip held first); two other buttons in either order.
//  - Where its value can't change now, the press says why instead, once.
//  - The first kDeadZone of twist does nothing; past it, the gesture is under
//    way (changing()) and its buttons' presses do no job (ControlInput). A
//    twist of one whose value can't change takes their jobs too, changing
//    nothing.
//  - A hand that loses its controller or the app's focus (JobInput::lost)
//    ends its gesture as it is: that isn't letting go.
//  - At either end of its range the value stays until the wrist turns back
//    as far as the dead zone again.
//  - Let go while under way, the value goes back to what it was
//    kReleaseGuard before: letting go of a button turns the wrist.
class GestureInput {
 public:
  static constexpr float kDeadZone = 20.0f;      // degrees
  static constexpr double kReleaseGuard = 0.15;  // seconds

  // What the app does for it (main.cpp).
  struct Host {
    // Why the value can't change now, said as a button's job says it; "" to
    // say nothing; null if it can.
    std::function<const char*(GestureValue)> why;
    // Its value now, in its units (GestureValueInfo), and one set.
    std::function<double(GestureValue)> read;
    std::function<void(GestureValue, double)> write;
    // Its range now: the table's but for Scrub's (the video's).
    std::function<void(GestureValue, double& least, double& most)> range;
    // A gesture got under way, and ended (`released`: let go, not cut short).
    std::function<void(GestureValue)> began;
    std::function<void(GestureValue, bool released)> ended;
    std::function<void(const char*)> say;
    std::function<void(int hand, float amplitude, double seconds)> buzz;
  };

  struct Frame {
    ControllerType types[2] = {ControllerType::Count, ControllerType::Count};
    bool onMenu[2] = {false, false};  // each hand's pointer is on the menu
    bool blocked = false;  // nothing starts (setup practises the controls); one under way ends
    uint8_t lost = 0;      // JobInput::lost
    double now = 0;        // seconds
  };

  // `buttons`: their jobs; a gesture with one that chooses doesn't run
  // (choosingButton()).
  void update(const ControllerState& input, const GestureMap& map, const ControlMap& buttons,
              const Frame& frame, ControlInput& taps, Host& host);
  // Ends it as it is (another video), until its buttons are let go.
  void cancel(Host& host);
  // Its value isn't shown any more.
  void hide() { shownAt_ = -1e9; }
  // A button's job changed this value (Zoom in and out): its ring shows as
  // if a gesture had changed it.
  void show(GestureValue value, double now) {
    shown_ = value;
    shownAt_ = now;
  }

  bool active() const { return hand_ >= 0; }
  int hand() const { return hand_; }  // -1 = none
  bool changing() const { return hand_ >= 0 && changing_; }
  const Gesture& gesture() const { return gesture_; }
  // A press of this button on that hand went to a gesture (one is under
  // way with it, or said why it couldn't), until it is let go: the trigger's
  // drag doesn't start meanwhile.
  bool claims(int hand, Control control) const {
    return (hand == hand_ && gesture_.has(control)) || (claimed_[hand] & controlBit(control));
  }
  // The value the last one under way changed, and when last (its ring shows
  // a while after).
  GestureValue shown() const { return shown_; }
  double shownAt() const { return shownAt_; }

 private:
  void start(int hand, const Gesture& g, const HandState& h, bool refused);
  void end(bool released, Host& host);

  double now_ = 0;
  int hand_ = -1;
  Gesture gesture_;
  bool refused_ = false;   // its value can't change: a twist only takes its buttons' jobs
  const char* refusal_ = nullptr;  // why, until said
  Quat last_;
  bool tracked_ = false;
  float twist_ = 0;        // degrees since it started, until past the dead zone
  bool changing_ = false;  // past it
  double value_ = 0;       // where the twist has taken the value, past an end by up to a dead zone
  double applied_ = 0;     // ... as last set
  double seen_ = 0;        // ... as last read
  std::deque<double> sent_;  // ... as set, not yet read back
  double began_ = 0;       // ... as it got under way
  // What it was set to, and when, as long as the release guard needs.
  std::deque<std::pair<double, double>> history_;
  uint32_t claimed_[2] = {0, 0};
  GestureValue shown_ = GestureValue::Count;
  double shownAt_ = -1e9;
};
