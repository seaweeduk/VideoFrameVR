#pragma once

#include "controller_state.h"

#include <algorithm>
#include <cmath>

// Where a hand's pointer points, a unit vector.
inline Vec3 aimDirection(const HandState& hand) {
  return normalize(transformDir(rotation(hand.aimOrientation), {0.0f, 0.0f, -1.0f}));
}

// Fires once when an input goes down, then repeatedly while it stays down.
class HoldRepeat {
 public:
  HoldRepeat(double delay, double interval) : delay_(delay), interval_(interval) {}

  bool update(bool held, double now) {
    if (!held) {
      held_ = false;
      return false;
    }
    if (!held_) {
      held_ = true;
      next_ = now + delay_;
      return true;
    }
    if (now >= next_) {
      next_ = std::max(next_ + interval_, now);
      return true;
    }
    return false;
  }

 private:
  double delay_, interval_;
  bool held_ = false;
  double next_ = 0;
};

// Which way a thumbstick is pushed, counting only its dominant axis.
struct StickDirections {
  bool left = false, right = false, up = false, down = false;
};

inline StickDirections stickDirections(const HandState& hand) {
  constexpr float kThreshold = 0.6f;
  StickDirections d;
  if (std::abs(hand.stickX) > std::abs(hand.stickY)) {
    d.left = hand.stickX < -kThreshold;
    d.right = hand.stickX > kThreshold;
  } else {
    d.up = hand.stickY > kThreshold;
    d.down = hand.stickY < -kThreshold;
  }
  return d;
}

// A thumbstick held forward or back, to move the surround viewpoint: only
// once it has been held there a moment, and not after it went sideways (a
// seek) until it is let go, so that a thumb sweeping round to seek
// or brushing past doesn't move the picture.
class ForwardStick {
 public:
  static constexpr double kHoldSeconds = 0.35;
  void update(const HandState& hand, double now) {
    const float x = std::abs(hand.stickX), y = std::abs(hand.stickY);
    if (x < 0.3f && y < 0.3f)
      clean_ = true;
    else if (x > y && x > 0.5f)
      clean_ = false;
    if (clean_ && y > x && y > 0.75f) {
      if (since_ < 0) since_ = now;
    } else {
      since_ = -1;
    }
  }
  bool held(double now) const { return since_ >= 0 && now - since_ >= kHoldSeconds; }

 private:
  bool clean_ = true;   // back in the middle since it last went sideways
  double since_ = -1;   // held forward or back from then, else -1
};

// How far a hand turned about its pointer from orientation `from` to its
// current one, in degrees, clockwise as the user sees it positive.
inline float wristTwist(const Quat& from, const HandState& hand) {
  // The turn about the pointer (away from the user: positive about it is
  // clockwise as seen from behind).
  const Quat turn = hand.aimOrientation * conjugate(from);
  const Vec3 pointer = aimDirection(hand);
  const float along = turn.x * pointer.x + turn.y * pointer.y + turn.z * pointer.z;
  float degrees = 2.0f * std::atan2(along, turn.w) * 180.0f / 3.14159265f;
  if (degrees > 180.0f) degrees -= 360.0f;  // the short way round
  if (degrees < -180.0f) degrees += 360.0f;
  return degrees;
}

// Seeking with a stick sideways (or an arrow key): the video jumps once,
// when the input is let go. A tap jumps tap() (Skip's length). Held past
// kDelay, the caller pauses the video and shows the seek bar while the
// target glides on, as far as the video goes, and the video jumps there on
// letting go. How far
// the stick is pushed sets the speed: a little creeps (kCreep), all the way
// goes a fifth of the top speed and, kept there, speeds up to the top, at
// which the whole video passes in kCross. A hold starts at kPress and lasts
// until the stick is back under kKeep, so easing off slows it rather than
// letting go. Seeking at every step restarted the Frame's decoder 5 times a
// second and flashed each new picture.
class HeldSeek {
 public:
  static constexpr double kDelay = 0.4;     // seconds down before a hold
  static constexpr float kPress = 0.6f;     // stick deflection that starts it
  static constexpr float kKeep = 0.25f;     // ... and keeps it going
  static constexpr double kCreep = 5;       // seconds of video a second, least push
  static constexpr double kCross = 12;      // seconds the whole video takes at the top speed
  static constexpr double kTopLeast = 30;   // top speed at least (seconds a second)

  struct Step {
    bool pressed = false;      // the input went down
    bool holdStarted = false;  // held past the delay: pause, show the bar
    bool released = false;     // let go: seek by distance (none if 0), restore
    bool held = false;         // ... after holdStarted
    double distance = 0;       // seconds
  };

  void setTap(double seconds) { tap_ = seconds; }
  double tap() const { return tap_; }

  // back and forward: how far the input is pushed each way (0..1; a key 1).
  // least and most: how far the video goes back and on from where it is;
  // duration: its length (0 if unknown).
  Step update(float back, float forward, double now, double least, double most, double duration) {
    Step s;
    if (ignore_) {
      ignore_ = std::max(back, forward) >= kKeep;
      return s;
    }
    least = std::min(least, 0.0);
    most = std::max(most, 0.0);
    if (direction_ == 0) {
      if ((back >= kPress) == (forward >= kPress)) return s;
      direction_ = forward >= kPress ? 1 : -1;
      s.pressed = true;
      since_ = last_ = now;
      distance_ = std::clamp(direction_ * tap_, least, most);
      return s;
    }
    const float push = direction_ > 0 ? forward : back;
    const double dt = std::clamp(now - last_, 0.0, 0.1);
    last_ = now;
    if (push < kKeep) {
      s.released = true;
      s.held = holding_;
      s.distance = distance_;
      reset();
      return s;
    }
    if (!holding_ && now - since_ >= kDelay) holding_ = s.holdStarted = true;
    if (holding_) {
      const double amount = std::clamp((push - kKeep) / (1.0f - kKeep), 0.0f, 1.0f);
      if (amount > 0.9)
        fullFor_ += dt;
      else
        fullFor_ = std::max(0.0, fullFor_ - 4 * dt);
      // Kept all the way over for a second, it speeds up over the next two.
      const double ramp = std::clamp((fullFor_ - 1.0) / 2.0, 0.0, 1.0);
      const double top = std::max(kTopLeast, (duration > 0 ? duration : 3600) / kCross);
      const double full = std::max(kCreep, top / 5);
      const double speed = kCreep + (full - kCreep) * amount * amount +
                           (top - full) * ramp * ramp * (3 - 2 * ramp);
      distance_ = std::clamp(distance_ + direction_ * speed * dt, least, most);
    }
    return s;
  }
  // The input is down; holding() past the delay.
  bool active() const { return direction_ != 0; }
  bool holding() const { return holding_; }
  double distance() const { return distance_; }
  // Forgets the input (another video): nothing more until it is let go.
  void cancel() {
    ignore_ = direction_ != 0;
    reset();
  }

 private:
  void reset() {
    direction_ = 0;
    holding_ = false;
    distance_ = fullFor_ = 0;
  }

  double tap_ = 10;    // seconds
  int direction_ = 0;  // -1 back, 1 forward, 0 not down
  bool holding_ = false;
  bool ignore_ = false;
  double since_ = 0, last_ = 0;  // when it went down, the last update
  double distance_ = 0;
  double fullFor_ = 0;  // seconds pushed all the way over (decays when eased)
};
