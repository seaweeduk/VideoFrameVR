#pragma once

#include <array>

// When the seek preview asks for a new picture while it decodes in software.
// One takes a while to come (on the Steam Frame about 0.3 s of an 8K video)
// and the next waits for it. Where the pointer has all but stopped, a
// picture is asked for at once. While it moves along the bar, one at a time:
// each shows a place it has since left, which is still something to go by,
// but none waits behind another to hold up the one where it stops. While it
// sweeps across, none: it would show a place long gone.
class PreviewPace {
 public:
  struct Input {
    double now = 0;     // seconds
    double point = 0;   // where on the bar (0..1) the preview is
    double steady = 0;  // the bar's dead zone, which speeds are counted in
    bool busy = false;  // a picture asked for hasn't come yet
  };
  // Pointed at anew: how the pointer moves isn't known yet.
  void reset() { count_ = 0; }
  // Every frame the preview shows. True while a picture of `point` is worth
  // asking for.
  bool update(const Input& in);
  // A picture of `point` was asked for.
  void asked(double point) { asked_ = point; }

 private:
  struct Sample {
    double at = 0, point = 0;
  };
  std::array<Sample, 32> samples_;  // the last frames', a ring
  int next_ = 0, count_ = 0;
  double asked_ = -1;  // where the last picture asked for is
};
