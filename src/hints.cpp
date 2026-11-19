#include "hints.h"

#include "notices.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace {

const char* const kNames[kHints] = {"menu", "scroll", "recenter", "volume"};
constexpr int kTimes = 3;
constexpr double kShown = 5, kFade = 0.2;
constexpr double kAwayFor = 3, kVolumeFor = 1;  // seconds the situation lasts first

}  // namespace

void Hints::load(const std::string& line, bool persist) {
  persist_ = persist;
  std::fill(std::begin(shown_), std::end(shown_), -1);
  std::istringstream in(line);
  std::string entry;
  while (in >> entry) {
    const size_t colon = entry.find(':');
    for (int i = 0; i < kHints; ++i)
      if (entry.compare(0, colon, kNames[i]) == 0)
        shown_[i] = colon == std::string::npos ? 0 : std::clamp(std::atoi(entry.c_str() + colon + 1), 0, kTimes - 1);
  }
}

void Hints::begin(bool persist) {
  persist_ = persist;
  std::fill(std::begin(shown_), std::end(shown_), 0);
  changed_ = persist;
}

std::string Hints::line() const {
  std::string out;
  for (int i = 0; i < kHints; ++i)
    if (shown_[i] >= 0) out += (out.empty() ? "" : " ") + std::string(kNames[i]) + ":" + std::to_string(shown_[i]);
  return out;
}

bool Hints::any() const {
  return showing_ != Hint::Count ||
         std::any_of(std::begin(shown_), std::end(shown_), [](int n) { return n >= 0; });
}

void Hints::use(Hint hint, double now) {
  int& shown = shown_[static_cast<int>(hint)];
  if (shown >= 0) {
    shown = -1;
    changed_ = persist_;
    std::printf("Hints: %s used\n", kNames[static_cast<int>(hint)]);
  }
  if (showing_ == hint) hideAt_ = std::min(hideAt_, now);
}

Hint Hints::update(const Frame& f) {
  if (f.menuBroughtBack) use(Hint::Menu, f.now);
  if (f.scrolled) use(Hint::Scroll, f.now);
  if (f.recentered) use(Hint::Recenter, f.now);
  if (f.volumeTwisted) use(Hint::Volume, f.now);
  if (showing_ != Hint::Count && (!f.allowed || f.now >= hideAt_ + kFade)) showing_ = Hint::Count;

  // The situations, each arising once until it ends.
  bool arose[kHints] = {};
  arose[static_cast<int>(Hint::Menu)] = menuWasOpen_ && !f.menuOpen;
  menuWasOpen_ = f.menuOpen;
  if (f.menuOpen && f.listPage != listPage_) listArmed_ = true;
  listPage_ = f.menuOpen ? f.listPage : -1;
  arose[static_cast<int>(Hint::Scroll)] = listArmed_ && f.longListPointed;
  auto lasting = [&](bool on, double& since, bool& done, double seconds) {
    if (!on) {
      since = -1;
      done = false;
      return false;
    }
    if (since < 0) since = f.now;
    return !done && f.now - since >= seconds;
  };
  arose[static_cast<int>(Hint::Recenter)] = lasting(f.videoAway, awaySince_, awayDone_, kAwayFor);
  arose[static_cast<int>(Hint::Volume)] = lasting(f.volumePointed, volumeSince_, volumeDone_, kVolumeFor);

  for (int i = 0; i < kHints; ++i) {
    if (!arose[i] || shown_[i] < 0 || !f.allowed || showing_ != Hint::Count) continue;
    const Hint hint = static_cast<Hint>(i);
    if (hint == Hint::Scroll) listArmed_ = false;
    if (hint == Hint::Recenter) awayDone_ = true;
    if (hint == Hint::Volume) volumeDone_ = true;
    if (++shown_[i] >= kTimes) shown_[i] = -1;
    changed_ = persist_;
    showing_ = hint;
    shownAt_ = f.now;
    hideAt_ = f.now + kShown;
    std::printf("Hints: %s shown\n", kNames[i]);
    return hint;
  }
  return Hint::Count;
}

void Hints::addTo(std::vector<OverlayDraw>& overlays, const Texture* texture, int width, int height,
                  Vec3 headPos, Quat headRot, double now) const {
  if (showing_ == Hint::Count || !texture) return;
  const float in = static_cast<float>((now - shownAt_) / kFade);
  const float out = static_cast<float>((hideAt_ + kFade - now) / kFade);
  using namespace notice_place;
  const Mat4 gaze = rotation(headRot);
  PanelItem item;
  item.texture = texture;
  item.model = translation(headPos + transformDir(gaze, {0.0f, kLow, -kNear})) * gaze *
               scaling(width * kScale, height * kScale, 1);
  item.opacity = std::clamp(std::min(in, out), 0.0f, 1.0f);
  overlays.push_back(item);
}
