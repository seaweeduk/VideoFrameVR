#include "stick_seek.h"

#include "playback_state.h"
#include "video_player.h"
#include "vr_space.h"

#include <algorithm>
#include <cmath>

namespace {

// The seek bar a held seek shows with the menu closed stays this long after
// the stick lets go.
constexpr double kSeekBarLinger = 0.6;
// While a seek is held, the paused video shows the picture at the target at
// most this often, once the last one is shown and the target has moved on
// this far (each is a decoder restart on the Frame, about 250 ms).
constexpr double kHeldPictureEvery = 0.5;  // seconds
constexpr double kHeldPictureMove = 1.0;   // seconds of video

}  // namespace

bool StickSeek::update(const ControllerState& input, const Frame& frame) {
  const double now = frame.now;
  const PlaybackState& p = player.playback();
  // A stick counts sideways once it is mostly sideways; then all of its
  // sideways push, so a thumb drifting up or down keeps the hold. Of
  // the two, the one pushed further sideways seeks, until it lets go.
  if (!heldSeek.active()) {
    float most = 0.0f;
    for (int h = 0; h < 2; ++h) {
      const HandState& g = input.hands[h];
      if (std::abs(g.stickX) > std::abs(g.stickY) && std::abs(g.stickX) > most) {
        most = std::abs(g.stickX);
        heldSeekHand = h;
      }
    }
  }
  const HandState& seekStick = input.hands[heldSeekHand];
  const float x = frame.sticksTaken ? 0.0f : seekStick.stickX;
  const float sideways =
      heldSeek.active() || std::abs(x) > std::abs(seekStick.stickY) ? x : 0.0f;
  const float back = frame.seekable ? std::max(frame.backKey ? 1.0f : 0.0f, -sideways) : 0.0f;
  const float forward = frame.seekable ? std::max(frame.forwardKey ? 1.0f : 0.0f, sideways) : 0.0f;
  // Its limits from where it started: the video plays on until a hold.
  const double from = heldSeek.active() ? heldSeekFrom : p.position;
  // A tap goes Skip's length by the stick, the keyboard's arrows 10 s:
  // by the input that starts it.
  if (!heldSeek.active())
    heldSeek.setTap(std::abs(sideways) >= HeldSeek::kPress ? frame.skipSeconds : 10);
  const HeldSeek::Step step =
      heldSeek.update(back, forward, now, -from,
                      p.duration > 0 ? p.duration - kSeekEndMargin - from : 1e9, p.duration);
  if (step.pressed) {
    heldSeekFrom = p.position;
    heldSeekBar = false;  // a tap shows none
    heldSeekStick = std::abs(sideways) >= HeldSeek::kPress;
    heldSeekAtLimit = false;
    if (heldSeekStick) buzz(heldSeekHand, 0.15f, 0.010);
  }
  if (step.holdStarted) {
    // The picture waits, and is silent, until the stick lets go; with
    // the menu closed, the strip alone shows where it goes meanwhile.
    heldSeekPaused = !p.paused;
    if (heldSeekPaused) player.command("set pause yes");
    heldSeekBar = !panel.open && frame.headValid;
    if (heldSeekBar) panel.placeInFrontOf(frame.headPos, frame.menuAt);
    heldSeekPicture = heldSeekFrom;
    heldSeekPictureAt = now;
    heldSeekTens = std::floor(std::abs(heldSeek.distance()) / 10);
  }
  if (heldSeek.holding()) {
    heldSeekTarget = heldSeekFrom + heldSeek.distance();
    if (!player.seeking() && now - heldSeekPictureAt >= kHeldPictureEvery &&
        std::abs(heldSeekTarget - heldSeekPicture) >= kHeldPictureMove) {
      player.seekTo(heldSeekTarget, false);
      heldSeekPicture = heldSeekTarget;
      heldSeekPictureAt = now;
    }
    if (heldSeekStick) {
      const double end = p.duration > 0 ? p.duration - kSeekEndMargin : 1e9;
      const bool atLimit = heldSeekTarget <= 0.01 || heldSeekTarget >= end - 0.01;
      const double tens = std::floor(std::abs(heldSeek.distance()) / 10);
      if (atLimit && !heldSeekAtLimit) {
        buzz(heldSeekHand, 0.3f, 0.020);
      } else if (tens != heldSeekTens && now - heldSeekTickAt >= 0.125) {
        buzz(heldSeekHand, 0.1f, 0.006);
        heldSeekTickAt = now;
      }
      heldSeekAtLimit = atLimit;
      heldSeekTens = tens;
    }
  }
  if (step.released) {
    // A tap jumps on from where the video is now; a hold lands on the
    // keyframe before its target, never past it into the end, unless
    // its last picture is already there.
    if (step.held && heldSeekStick) buzz(heldSeekHand, 0.2f, 0.014);
    bool sent = step.distance != 0;
    if (step.held) {
      const double target = heldSeekFrom + step.distance;
      sent = std::abs(target - heldSeekPicture) > 0.01;
      if (sent) player.seekTo(target, false);
    } else if (sent) {
      player.seekBy(step.distance);
    }
    if (heldSeekPaused) player.command("set pause no");
    heldSeekPaused = false;
    heldSeekLanding = step.held && (sent || player.seeking());
    heldSeekBarUntil = now + kSeekBarLinger;
  }
  if (heldSeekLanding && !player.seeking()) heldSeekLanding = false;
  return step.pressed;
}

void StickSeek::cancel() {
  heldSeek.cancel();
  heldSeekPaused = heldSeekLanding = heldSeekBar = false;
}

void StickSeek::fill(PlaybackState& shown) const {
  if (heldSeek.holding() || heldSeekLanding) {
    shown.position = heldSeekTarget;
    if (heldSeekPaused) shown.paused = false;  // only waiting for the stick
  }
}

void StickSeek::beginScrub(double now) {
  scrub.on = true;
  scrub.sent = false;
  scrub.from = scrub.target = scrub.picture = player.playback().position;
  scrub.pictureAt = now;
}

void StickSeek::endScrub(bool released) {
  // Let go: exactly there; the video played on meanwhile.
  if (released && (scrub.sent || std::abs(scrub.target - scrub.from) > 0.01))
    player.seekTo(scrub.target, true);
  scrub.on = false;
}

double StickSeek::scrubValue() const { return scrub.on ? scrub.target : player.playback().position; }

void StickSeek::followScrub(double now) {
  // Scrub: the picture follows as the stick's held seek's does.
  if (scrub.on && !player.seeking() && now - scrub.pictureAt >= kHeldPictureEvery &&
      std::abs(scrub.target - scrub.picture) >= kHeldPictureMove) {
    player.seekTo(scrub.target, false);
    scrub.picture = scrub.target;
    scrub.pictureAt = now;
    scrub.sent = true;
  }
}
