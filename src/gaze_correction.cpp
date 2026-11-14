#include "gaze_correction.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

// Bump when a format or learner change makes previously saved fits unsuitable.
// Independent of the app version: ordinary updates keep compatible learning.
constexpr unsigned kLearningVersion = 1;

constexpr double kBefore = 0.15;    // seconds before the press that the eyes and the dot rest
constexpr float kGazeRest = 1.0f;   // degrees the gaze may be from its mean in that time
constexpr float kDotRest = 1.5f;    // ... and the dot: pulling the trigger moves the hand
constexpr double kQuick = 0.35;     // a click lasts at most this; longer is a drag or a hold
// Clicks count within 35° of 5° below straight ahead (SteamVR's tracker takes
// them within 27°; the ones furthest out say most about the tilt).
constexpr float kView = 35.0f, kViewPitch = -5.0f;
constexpr float kMaxStep = 6.0f;    // further than this from the correction in use: not looked at
constexpr float kMaxOffset = 6.0f;  // the correction is never larger
constexpr size_t kKept = 16;        // clicks of a sitting
// A sitting's offset straight ahead is remembered from this many clicks, its
// stretch if they were also this far apart along its axis; the last kVotes
// sittings' are kept.
constexpr size_t kEnough = 4;
constexpr float kSpread = 25.0f;
constexpr size_t kVotes = 5, kTrusted = 3;
constexpr float kMaxStretch = 0.25f;  // degrees per degree
// The fit: how strongly the plane is held to the remembered offset straight
// ahead (as this many clicks saying so) and to the remembered stretch in its
// tilt (per squared degree of spread the clicks have: two clicks 25° apart
// count half for a tilt).
constexpr float kHoldValue = 1.0f, kHoldTilt = 300.0f;
constexpr float kClip = 1.5f;       // degrees a click counts beyond the fit so far
constexpr float kEase = 0.3f;       // seconds a change takes to show
constexpr float kPi = 3.14159265f;

float median(std::vector<float> v) {
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

float apart(GazeOffset a, GazeOffset b) { return std::hypot(a.yaw - b.yaw, a.pitch - b.pitch); }

// Solves m x = v for a symmetric positive definite m.
void solve(double m[3][3], double v[3], float x[3]) {
  for (int a = 0; a < 3; ++a)
    for (int r = a + 1; r < 3; ++r) {
      const double f = m[r][a] / m[a][a];
      for (int k = a; k < 3; ++k) m[r][k] -= f * m[a][k];
      v[r] -= f * v[a];
    }
  for (int a = 2; a >= 0; --a) {
    double sum = v[a];
    for (int k = a + 1; k < 3; ++k) sum -= m[a][k] * x[k];
    x[a] = static_cast<float>(sum / m[a][a]);
  }
}

}  // namespace

GazeOffset gazeAngles(Vec3 d) {
  return {std::atan2(d.x, -d.z) * 180 / kPi, std::asin(std::clamp(d.y, -1.0f, 1.0f)) * 180 / kPi};
}

Vec3 applyGazeFit(Vec3 d, const GazeFit& f) {
  const GazeOffset a = gazeAngles(d);
  auto added = [&](const float c[3]) {
    return std::clamp(c[0] + c[1] * a.yaw + c[2] * a.pitch, -kMaxOffset, kMaxOffset);
  };
  const float y = (a.yaw + added(f.yaw)) * kPi / 180, p = (a.pitch + added(f.pitch)) * kPi / 180;
  return {std::sin(y) * std::cos(p), std::sin(p), -std::cos(y) * std::cos(p)};
}

float GazeCorrection::Part::at(float yaw, float pitch) const {
  return std::clamp(base[0] + plane[0] + (base[1] + plane[1]) * yaw + (base[2] + plane[2]) * pitch,
                    -kMaxOffset, kMaxOffset);
}

GazeFit GazeCorrection::fit() const {
  GazeFit f;
  std::copy(parts_[0].shown, parts_[0].shown + 3, f.yaw);
  std::copy(parts_[1].shown, parts_[1].shown + 3, f.pitch);
  return f;
}

void GazeCorrection::update(const Input& in) {
  const float dt = last_ < 0 ? 0.0f : static_cast<float>(std::clamp(in.now - last_, 0.0, 0.1));
  last_ = in.now;
  const float eased = 1 - std::exp(-dt / kEase);
  for (Part& p : parts_)
    for (int k = 0; k < 3; ++k)
      p.shown[k] += (p.base[k] + p.plane[k] - p.shown[k]) * eased;

  Rest& r = history_[next_++ % history_.size()];
  r.at = in.now;
  r.ok = in.gazeGood && in.pointing;
  if (r.ok) {
    r.gaze = gazeAngles(in.gaze);
    r.dot = gazeAngles(in.dot);
  }

  if (in.pressed) {
    pressedAt_ = in.now;
    pending_ = rested(in.now);
  } else if (in.dragged) {
    pending_ = false;
  } else if (pending_ && !in.held) {
    pending_ = false;
    // Let go at once, where it was pressed (the click may have closed the menu).
    const bool moved = in.pointing && apart(gazeAngles(in.dot), pressDot_) > kDotRest;
    if (in.now - pressedAt_ <= kQuick && !moved) learn();
  }
}

// Whether the eyes and the dot were at rest up to the press, and where.
bool GazeCorrection::rested(double now) {
  GazeOffset gaze, dot;
  int n = 0;
  double oldest = now;
  for (const Rest& r : history_) {
    if (r.at < 0 || now - r.at > kBefore) continue;
    if (!r.ok) return false;
    gaze.yaw += r.gaze.yaw;
    gaze.pitch += r.gaze.pitch;
    dot.yaw += r.dot.yaw;
    dot.pitch += r.dot.pitch;
    oldest = std::min(oldest, r.at);
    ++n;
  }
  if (n < 3 || now - oldest < 0.6 * kBefore) return false;
  gaze = {gaze.yaw / n, gaze.pitch / n};
  dot = {dot.yaw / n, dot.pitch / n};
  if (apart(dot, {0, kViewPitch}) > kView) return false;
  for (const Rest& r : history_) {
    if (r.at < 0 || now - r.at > kBefore) continue;
    if (apart(r.gaze, gaze) > kGazeRest || apart(r.dot, dot) > kDotRest) return false;
  }
  pressGaze_ = gaze;
  pressDot_ = dot;
  return true;
}

void GazeCorrection::learn() {
  // Where the eyes were, the dot, less the gaze; not if the corrected gaze is far from it.
  const float click[2] = {pressDot_.yaw - pressGaze_.yaw, pressDot_.pitch - pressGaze_.pitch};
  for (int i = 0; i < 2; ++i)
    if (std::abs(parts_[i].at(pressGaze_.yaw, pressGaze_.pitch) - click[i]) > kMaxStep) return;
  for (int i = 0; i < 2; ++i) {
    Part& p = parts_[i];
    p.clicks.push_back({pressGaze_.yaw, pressGaze_.pitch, click[i]});
    if (p.clicks.size() > kKept) p.clicks.erase(p.clicks.begin());
    settle(p);
  }
  std::printf("Gaze correction: %+.1f° right, %+.1f° up straight ahead (%zu and %zu clicks)\n",
              parts_[0].at(0, 0), parts_[1].at(0, 0), parts_[0].clicks.size(),
              parts_[1].clicks.size());
}

// The plane through the sitting's clicks by least squares, as what it adds
// to what is remembered: held to nothing, each click counting for at most
// kClip beyond what the plane so far says there.
void GazeCorrection::settle(Part& p) {
  if (p.clicks.empty()) {
    std::fill(p.plane, p.plane + 3, 0.0f);
    return;
  }
  double m[3][3] = {{kHoldValue, 0, 0}, {0, kHoldTilt, 0}, {0, 0, kHoldTilt}}, v[3] = {};
  for (const Click& c : p.clicks) {
    const double x[3] = {1, c.yaw, c.pitch};
    const float remembered = p.base[0] + p.base[1] * c.yaw + p.base[2] * c.pitch;
    const float so = p.plane[0] + p.plane[1] * c.yaw + p.plane[2] * c.pitch;
    const double off = std::clamp(c.off - remembered, so - kClip, so + kClip);
    for (int a = 0; a < 3; ++a) {
      for (int b = 0; b < 3; ++b) m[a][b] += x[a] * x[b];
      v[a] += x[a] * off;
    }
  }
  solve(m, v, p.plane);
}

// What a sitting starts from: the medians of the sittings' stretch and
// offset, each counted in part until there are kTrusted of them.
void GazeCorrection::remember(Part& p, int own) {
  auto trusted = [](const std::vector<float>& v) {
    return v.empty() ? 0.0f
                     : median(v) * std::min(1.0f, static_cast<float>(v.size()) /
                                                      static_cast<float>(kTrusted));
  };
  p.base[own] = trusted(p.votes);
  p.base[0] = trusted(p.ahead);
}

void GazeCorrection::endSitting() {
  pending_ = false;
  for (int i = 0; i < 2; ++i) {
    Part& p = parts_[i];
    const int own = i + 1;  // the tilt along its own axis
    float low = 0, high = 0;
    for (const Click& c : p.clicks) {
      const float along = i ? c.pitch : c.yaw;
      low = &c == &p.clicks.front() ? along : std::min(low, along);
      high = &c == &p.clicks.front() ? along : std::max(high, along);
    }
    if (p.clicks.size() >= kEnough) {
      p.ahead.push_back(std::clamp(p.base[0] + p.plane[0], -kMaxOffset, kMaxOffset));
      if (p.ahead.size() > kVotes) p.ahead.erase(p.ahead.begin());
      if (high - low >= kSpread) {
        p.votes.push_back(std::clamp(p.base[own] + p.plane[own], -kMaxStretch, kMaxStretch));
        if (p.votes.size() > kVotes) p.votes.erase(p.votes.begin());
      }
      remember(p, own);
      changed_ = true;
    }
    p.clicks.clear();
    settle(p);
  }
}

// "<learning version>;<runtime>;<yaw>;<pitch>", each axis
// "<stretches>/<offsets>": stretch in ten-thousandths of a degree per degree
// and offset in hundredths of a degree, with spaces between sittings.
// Unversioned data and other learning/runtime versions start fresh.
void GazeCorrection::load(const std::string& saved, uint64_t runtime) {
  runtime_ = runtime;
  const std::string prefix = std::to_string(kLearningVersion) + ';' + std::to_string(runtime) + ';';
  if (saved.compare(0, prefix.size(), prefix) != 0) return;
  const size_t split = saved.find(';', prefix.size());
  if (split == std::string::npos) return;
  const std::string lists[2] = {saved.substr(prefix.size(), split - prefix.size()),
                               saved.substr(split + 1)};
  auto read = [](const char*& at, std::vector<float>& into, float unit, float limit) {
    while (into.size() < kVotes) {
      char* end = nullptr;
      const long v = std::strtol(at, &end, 10);
      if (end == at) break;
      at = end;
      into.push_back(std::clamp(static_cast<float>(v) / unit, -limit, limit));
    }
  };
  for (int i = 0; i < 2; ++i) {
    Part& p = parts_[i];
    const char* at = lists[i].c_str();
    read(at, p.votes, 10000, kMaxStretch);
    if (*at == '/') read(++at, p.ahead, 100, kMaxOffset);
    remember(p, i + 1);
    std::copy(p.base, p.base + 3, p.shown);
  }
}

std::string GazeCorrection::saved() const {
  std::string out = std::to_string(kLearningVersion) + ';' + std::to_string(runtime_);
  for (const Part& p : parts_) {
    out += ';';
    for (size_t i = 0; i < p.votes.size(); ++i)
      out += (i ? " " : "") + std::to_string(std::lround(p.votes[i] * 10000));
    out += '/';
    for (size_t i = 0; i < p.ahead.size(); ++i)
      out += (i ? " " : "") + std::to_string(std::lround(p.ahead[i] * 100));
  }
  return out;
}

bool GazeCorrection::takeChanged() {
  const bool changed = changed_;
  changed_ = false;
  return changed;
}
