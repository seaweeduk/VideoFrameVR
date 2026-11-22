#include "gesture_input.h"

#include "vr_input.h"

#include <algorithm>
#include <cmath>

namespace {

// The grip as ControlInput leaves it: a consumed one isn't held.
bool held(const HandState& h, Control c) {
  return c == Control::Grip ? h.grip : (h.controls & controlBit(c)) != 0;
}
bool pressed(const HandState& h, Control c) {
  return c == Control::Grip ? h.gripPressed : (h.pressed & controlBit(c)) != 0;
}
bool allHeld(const HandState& h, const Gesture& g) {
  return held(h, g.first) && (g.second == Control::Count || held(h, g.second));
}
uint32_t heldBits(const HandState& h) {
  const uint32_t grip = controlBit(Control::Grip);
  return (h.controls & ~grip) | (h.grip ? grip : 0u);
}

}  // namespace

void GestureInput::update(const ControllerState& input, const GestureMap& map, const ControlMap& buttons,
                          const Frame& frame, ControlInput& taps, Host& host) {
  now_ = frame.now;
  if (hand_ >= 0 && (frame.lost >> hand_ & 1)) cancel(host);
  for (int h = 0; h < 2; ++h) claimed_[h] &= heldBits(input.hands[h]);

  if (hand_ >= 0) {
    const HandState& h = input.hands[hand_];
    bool holding = false;  // its press does its button's held job: the gesture can't be
    for (const Control c : {gesture_.first, gesture_.second})
      holding = holding || (c != Control::Count && taps.holdingJob(hand_, c));
    if (!allHeld(h, gesture_))
      end(true, host);
    else if (frame.blocked || (!refused_ && host.why(gesture_.value)) || (!changing_ && holding))
      end(false, host);
  }

  // A press that completes a gesture's buttons; the one with the most.
  bool started = false;
  for (int i = 0; i < 2 && !frame.blocked; ++i) {
    const HandState& h = input.hands[i];
    if (!h.poseValid || frame.types[i] == ControllerType::Count) continue;
    const Gesture* best = nullptr;
    for (const Gesture& g : map.list(frame.types[i])) {
      if (!(g.hands >> i & 1) || g.movement != Movement::Twist || !allHeld(h, g)) continue;
      if (!pressed(h, g.first) && !(g.second != Control::Count && pressed(h, g.second))) continue;
      if (g.second == Control::Trigger && !pressed(h, Control::Trigger)) continue;
      // The grip first, as a button's job with grip held (ControlInput defers it so).
      if (g.has(Control::Grip) && g.controls() == 2 &&
          !pressed(h, g.first == Control::Grip ? g.second : g.first))
        continue;
      if ((g.has(Control::Grip) || g.has(Control::Trigger)) && frame.onMenu[i]) continue;
      if (taps.holdingJob(i, g.first) || (g.second != Control::Count && taps.holdingJob(i, g.second)))
        continue;
      if (choosingButton(buttons, frame.types[i], i, g) != Control::Count) continue;
      if (!best || g.controls() > best->controls()) best = &g;
    }
    if (!best) continue;
    // One under way gives way only to one of its hand with more buttons, its own among them.
    if (hand_ >= 0) {
      if (hand_ != i || best->controls() <= gesture_.controls() ||
          (best->bits() & gesture_.bits()) != gesture_.bits())
        continue;
      end(false, host);
    }
    claimed_[i] |= best->bits();
    const char* why = host.why(best->value);
    if (why && *why) host.say(why);
    start(i, *best, h, why != nullptr);
    started = true;
  }
  if (hand_ < 0 || started) return;

  const HandState& h = input.hands[hand_];
  if (!h.poseValid) {
    tracked_ = false;
    return;
  }
  if (!tracked_) {  // tracked again: it goes on from here, not with the jump
    tracked_ = true;
    last_ = h.aimOrientation;
    return;
  }
  float degrees = wristTwist(last_, h);
  last_ = h.aimOrientation;
  const GestureValue v = gesture_.value;
  if (!changing_) {
    twist_ += degrees;
    if (std::abs(twist_) <= kDeadZone) return;
    for (const Control c : {gesture_.first, gesture_.second})
      if (c != Control::Count) taps.cancelTap(hand_, c);
    if (refused_) return;
    changing_ = true;
    degrees = twist_ - std::copysign(kDeadZone, twist_);
    host.began(v);
    value_ = applied_ = seen_ = began_ = host.read(v);
    history_.clear();
    sent_.clear();
    host.buzz(hand_, 0.25f, 0.012);
    shown_ = v;
    shownAt_ = now_;
  }

  const GestureValueInfo& info = gestureValueInfo(v);
  double least = info.range.least, most = info.range.most;
  host.range(v, least, most);
  const double step = info.range.step;
  const auto snapped = [&](double x) {
    return std::clamp(least + std::round((x - least) / step) * step, least, most);
  };
  // Changed meanwhile: by one of its own sets read back late (the player's
  // volume), those before it too; else by something else (a late library
  // restore), and it goes on from there.
  const double read = host.read(v);
  if (snapped(read) != snapped(seen_)) {
    const auto own = std::find(sent_.begin(), sent_.end(), snapped(read));
    if (own != sent_.end()) {
      sent_.erase(sent_.begin(), own + 1);
    } else if (snapped(read) != applied_) {
      value_ = applied_ = read;
      sent_.clear();
    }
  }
  seen_ = read;
  const double hold = kDeadZone * info.perDegree;
  double next = value_ + degrees * info.perDegree;
  if (next >= most) next = value_ > most ? std::min(next, most + hold) : most + hold;
  if (next <= least) next = value_ < least ? std::max(next, least - hold) : least - hold;
  const double to = snapped(next);
  if (to != applied_) {
    host.write(v, to);
    sent_.push_back(to);
    if (sent_.size() > 64) sent_.pop_front();
    // A detent at each multiple it reaches, a stronger one at an end.
    const double origin = info.centred ? 0.0 : info.range.least;
    const double from = (applied_ - origin) / info.detent, at = (to - origin) / info.detent;
    if (to <= least || to >= most)
      host.buzz(hand_, 0.3f, 0.020);
    else if (to > applied_ ? std::floor(at) > std::floor(from) : std::ceil(at) < std::ceil(from))
      host.buzz(hand_, 0.1f, 0.006);
    applied_ = to;
    shownAt_ = now_;  // the ring shows a while after each change, not while the wrist stays
  }
  value_ = next;
  history_.emplace_back(now_, applied_);
  while (history_.size() > 1 && history_[1].first <= now_ - kReleaseGuard) history_.pop_front();
}

void GestureInput::cancel(Host& host) {
  if (hand_ < 0) return;
  claimed_[hand_] |= gesture_.bits();
  end(false, host);
}

void GestureInput::start(int hand, const Gesture& g, const HandState& h, bool refused) {
  hand_ = hand;
  gesture_ = g;
  refused_ = refused;
  last_ = h.aimOrientation;
  tracked_ = true;
  twist_ = 0;
  changing_ = false;
}

void GestureInput::end(bool released, Host& host) {
  if (hand_ < 0) return;
  const GestureValue v = gesture_.value;
  if (changing_) {
    if (released) {
      // What it was kReleaseGuard ago: the newest kept that old (also from
      // before a frame gap or lost tracking), else as it began.
      double back = began_;
      for (auto at = history_.rbegin(); at != history_.rend(); ++at)
        if (at->first <= now_ - kReleaseGuard) {
          back = at->second;
          break;
        }
      if (back != applied_) {
        host.write(v, back);
        shownAt_ = now_;
      }
      applied_ = back;
    }
    host.ended(v, released);
  }
  hand_ = -1;
  changing_ = false;
  history_.clear();
}
