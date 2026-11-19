#pragma once

#include "controller_guide.h"
#include "scene_renderer.h"
#include "xmath.h"

#include <string>
#include <utility>
#include <vector>

// The one-line hints after first-run setup (docs/alpha/first-run.md): fixed
// to the head, low in view, for 5 s each, the first three times their
// situation arises or until their control is used, one at a time. Only a
// profile that went through setup has any (settings.conf's hints= line);
// under --first-run they last for the run only.
class Hints {
 public:
  // settings.conf's line ("menu:0 scroll:2"): those still to show, each
  // with the times it was shown. persist: changes are for it (takeChanged()).
  void load(const std::string& line, bool persist);
  // Setup has ended: all of them, none shown yet.
  void begin(bool persist);
  std::string line() const;
  bool any() const;

  // What happened this frame.
  struct Frame {
    double now = 0;
    bool allowed = false;          // setup isn't running and the app has input focus
    bool menuOpen = false;
    bool menuBroughtBack = false;  // the menu button opened it
    bool longListPointed = false;  // a list longer than its page, pointed at
    int listPage = -1;             // ... the page it is on
    bool videoAway = false;        // a video playing, its centre over 70° off the view
    bool volumePointed = false;    // the pointer on the strip's volume
    // The controls the hints are about, used.
    bool scrolled = false, recentered = false, volumeTwisted = false;
  };
  // The hint that starts showing this frame, else Hint::Count.
  Hint update(const Frame& f);
  // Once after a change to persist: settings.conf's line is line() now.
  bool takeChanged() { return std::exchange(changed_, false); }

  // A hint shows (or fades in or out).
  bool showing() const { return showing_ != Hint::Count; }
  // The one shown, `texture` (width × height layout pixels) in front of the
  // eyes at the messages' place (notices.h), fading in and out.
  void addTo(std::vector<OverlayDraw>& overlays, const Texture* texture, int width, int height,
             Vec3 headPos, Quat headRot, double now) const;

 private:
  void use(Hint hint, double now);

  int shown_[kHints] = {-1, -1, -1, -1};  // times shown; -1: not to show again
  bool persist_ = false;
  bool changed_ = false;
  Hint showing_ = Hint::Count;
  double shownAt_ = 0, hideAt_ = 0;
  // The situations: the menu was open; a list to point at since the menu
  // opened or its page changed; since when the video is away, and the pointer
  // on the volume (-1 not), each shown once until it ends.
  bool menuWasOpen_ = false;
  bool listArmed_ = false;
  int listPage_ = -1;
  double awaySince_ = -1, volumeSince_ = -1;
  bool awayDone_ = false, volumeDone_ = false;
};
