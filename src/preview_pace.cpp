#include "preview_pace.h"

#include <algorithm>
#include <cmath>

namespace {
// How the pointer moves is taken over this long, so a hand's tremor (about
// ten shakes a second) evens out; after pointing anew, half of it is enough.
constexpr double kLook = 0.1;  // seconds
// All but stopped: slower than this many dead zones a second (50 of the
// bar's pixels). A picture asked for then is at most 15 pixels behind when
// it comes 0.3 s later.
constexpr double kSlow = 8;
// Sweeping across: faster than this many bar widths a second.
constexpr double kSweep = 0.3;
}  // namespace

bool PreviewPace::update(const Input& in) {
  const int size = static_cast<int>(samples_.size());
  // Where it was kLook ago, or as long ago as is known.
  const Sample* then = nullptr;
  for (int i = 1; i <= count_; ++i) {
    then = &samples_[(next_ - i + size) % size];
    if (in.now - then->at >= kLook) break;
  }
  samples_[next_] = {in.now, in.point};
  next_ = (next_ + 1) % size;
  count_ = std::min(count_ + 1, size);
  if (!then || in.now - then->at < kLook / 2) return false;
  const double speed = std::abs(in.point - then->point) / (in.now - then->at);
  // The bar already holds the point still under a hand's shake
  // (Menu::steadySeek()): wherever it has moved to is worth a picture.
  if (in.point == asked_) return false;
  return speed < kSlow * in.steady || (!in.busy && speed < kSweep);
}
