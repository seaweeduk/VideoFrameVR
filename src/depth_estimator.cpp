#include "depth_estimator.h"

#include "stereo_correction.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

// How the shaders read a packed mask (DepthPatch::alphaPacked).
int maskRule(const SeeThrough& s) {
  if (!s.packedAlpha) return 0;
  return s.maskColours == MaskColours::RedBlue ? 3 : 1;
}

constexpr float kDegree = 3.14159265f / 180.0f;
// Candidate depths: half-convergence angles (each camera's turn
// inward from parallel) from kAngleStart in steps of kAngleRange / accuracy,
// 64 × accuracy of them.
constexpr float kAngleStart = 0.0001f;
constexpr float kAngleRange = 0.008f;
// The patch's pixel pitch follows the candidates' disparity step (twice the
// angle step), kept between these; the match window is about 2.9° square.
constexpr float kMinStep = 0.04f * kDegree, kMaxStep = 0.16f * kDegree;
constexpr float kWindowAngle = 2.88f * kDegree;
constexpr int kMaxWindow = 72;
constexpr float kSearchMargin = 0.3f * kDegree;  // beyond infinity, for miscalibrated eyes
constexpr int kMaxWidth = 2048;
// Real stereo video is not perfectly aligned vertically: its eyes often
// differ by up to about 1.6° (one camera pitched or rolled against the
// other), and matches are searched that far up and down until the video's
// offset is known from a few clear matches; then within a band around it
// (it varies by about that much across the picture).
constexpr float kVerticalSlack = 2.0f * kDegree;
constexpr float kVerticalBand = 0.5f * kDegree;
constexpr float kLearnCorrelation = 0.8f, kLearnMargin = 0.15f;
constexpr int kLearnSamples = 3;
constexpr double kLearnDecay = 0.95;      // per sample: about the last 20 count
constexpr double kMinRollSpread = 0.04;   // variance of u needed to fit a roll
constexpr double kMaxRoll = 3.0 * kDegree;
// The coarse pass's reduction, by the window's pixels: at the usual window
// its pixels are 0.16° to 0.32° apart (a quarter for pitches below 0.06°).
int coarseFactor(int window) { return window > 48 ? 4 : 2; }
// Rows above and below the window: the slack, and the refinement's.
int marginRows(float step, int window) {
  return static_cast<int>(std::ceil(kVerticalSlack / step)) + coarseFactor(window);
}
constexpr int kMaxMarginRows = static_cast<int>(2.0f / 0.04f) + 1 + 4;
// Passthrough (DepthRequest::reach): how far a see-through target's window
// may move to find video, and how much of each eye's moved window may still
// be clear (both together no more than the half the match allows).
constexpr float kMaxReach = 3.0f * kDegree;
constexpr int kMaxReachRows = static_cast<int>(3.0f / 0.04f) + 1;
constexpr float kReachClear = 0.25f;
constexpr int kMaxHeight = 2 * (kMaxWindow + 2 * kMaxMarginRows + 2 * kMaxReachRows) + 32;
static_assert(kMaxWidth <= DepthEstimator::kMaxPatchWidth && kMaxHeight <= DepthEstimator::kMaxPatchRows,
              "DepthPatch images are smaller");
// A direction this close to the baseline sees (almost) no parallax.
constexpr float kMinSinAlpha = 0.34f;  // 20°

// Match quality limits (zero-mean normalized cross-correlation, -1..1, and
// the luminance spread of the patch, 0..1 perceptual).
constexpr float kMinCorrelation = 0.55f;
constexpr float kUniqueMargin = 0.08f;  // a second peak this close: ambiguous
constexpr float kMinSpread = 0.01f;
// Patch values other than luminance: outside the picture, and (passthrough)
// showing the room, which is left out of the match.
constexpr float kOutside = -1.0f, kClear = -0.5f;

const char* kVertex = R"(#version 330 core
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2) * 2.0 - 1.0;
  gl_Position = vec4(p, 0.0, 1.0);
}
)";

// Mode 0: one eye's patch, pixel (i, j) at epipolar angles uStart + (i, j) *
// uStep (pixel centres), looked up with the video's projection. Mode 1: the
// whole eye scaled down. Writes perceptual luminance, or -1 outside the
// picture; -0.5 where passthrough shows the room (as SceneRenderer keys it,
// whatever is less than half opaque).
const char* kFragment = R"(#version 330 core
out vec4 fragColor;
uniform sampler2D uTex;
uniform int uMode;
uniform vec4 uRect;       // viewport x, y, width, height
uniform vec2 uStart;      // alpha, beta at the first pixel's corner
uniform float uStep;
uniform int uProjection;  // 0 = 180, 2 = fisheye (as the renderer)
uniform float uFisheyeFov;
uniform bool uFlip;
uniform vec2 uEyeScale;
uniform vec2 uEyeOffset;
// DeoVR's packed mask, read as surround.frag's kSeeThrough (1: red, 3: red
// and blue; 0 = none): centre and scale in the frame
uniform int uAlphaPacked;
uniform vec2 uAlphaCenter;
uniform vec2 uAlphaScale;
uniform float uGreenKey;    // > 0: a green screen, keyed above this greenness
const float PI = 3.14159265359;
// The picture at p (frame coordinates) over the pixel's footprint dx, dy,
// through the mipmaps. Linear colour.
vec3 picture(vec2 p, vec2 dx, vec2 dy) {
  return textureGrad(uTex, p, dx, dy).rgb;
}
void main() {
  vec2 p = gl_FragCoord.xy - uRect.xy;
  vec2 uv;
  bool inside = true;
  if (uMode == 1) {
    uv = vec2(p.x / uRect.z, 1.0 - p.y / uRect.w);
  } else {
    float alpha = uStart.x + p.x * uStep, beta = uStart.y + p.y * uStep;
    vec3 dir = vec3(cos(alpha), sin(alpha) * sin(beta), -sin(alpha) * cos(beta));
    if (uProjection == 2) {
      float theta = acos(clamp(-dir.z, -1.0, 1.0));
      float r = theta / uFisheyeFov;
      vec2 d = dir.xy;
      float len = length(d);
      d = len > 1e-6 ? d / len : vec2(0.0);
      uv = vec2(0.5 + r * d.x, 0.5 - r * d.y);
      inside = r <= 0.49;
    } else {
      float lon = atan(dir.x, -dir.z);
      float lat = asin(clamp(dir.y, -1.0, 1.0));
      uv = vec2(0.5 + lon / PI, 0.5 - lat / PI);
      inside = abs(lon) <= 0.49 * PI;
    }
  }
  if (uFlip) uv.y = 1.0 - uv.y;
  vec2 dx = dFdx(uv), dy = dFdy(uv);
  vec3 c = picture(uEyeOffset + clamp(uv, 0.0005, 0.9995) * uEyeScale, dx * uEyeScale,
                   dy * uEyeScale);
  float l = pow(max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 0.0), 1.0 / 2.2);
  bool clear = false;
  if (uMode == 0 && uAlphaPacked != 0) {
    vec2 m = uv - 0.5;
    vec3 mask = picture(fract(uAlphaCenter + m * uAlphaScale), dx * uAlphaScale, dy * uAlphaScale);
    // Red below 0.5 in display values; or its colours, as the scene adds them up.
    if (uAlphaPacked == 1) {
      clear = mask.r < 0.214;
    } else {
      vec3 v = clamp(mask, 0.0, 1.0);  // to display values, as the scene reads them
      vec3 e = mix(12.92 * v, 1.055 * pow(v, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, v));
      clear = e.r + e.b + min(e.r, e.b) < 0.5;
    }
  } else if (uMode == 0 && uGreenKey > 0.0) {
    vec3 e = pow(max(c, 0.0), vec3(1.0 / 2.2));
    clear = (e.g - max(e.r, e.b)) / max(e.g, 0.04) > uGreenKey;
  }
  fragColor = vec4(!inside ? -1.0 : clear ? -0.5 : l);
}
)";

// A view of one eye's patch inside the read-back pixels.
struct Image {
  const float* data;
  int stride, width, height;
  float at(int x, int y) const { return data[y * stride + x]; }
};

// The patch reduced f times (f×f averages of what is not clear; outside if
// any is, clear if all are).
std::vector<float> shrink(const Image& im, int f, int& w, int& h) {
  w = im.width / f;
  h = im.height / f;
  std::vector<float> out(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      float sum = 0;
      int count = 0;
      bool outside = false;
      for (int j = 0; j < f; ++j)
        for (int i = 0; i < f; ++i) {
          const float v = im.at(f * x + i, f * y + j);
          outside = outside || v == kOutside;
          if (v >= 0) sum += v, ++count;
        }
      out[y * w + x] = outside ? kOutside : count ? sum / count : kClear;
    }
  return out;
}

struct Correlation {
  bool valid = false;
  float score = -1;   // zero-mean normalized cross-correlation
  float spread = 0;   // the smaller of both windows' standard deviations
};

// Windows of size n×n with top-left corners (lx, ly) and (rx, ry); pixel
// pairs with a clear one are left out, and at least half must remain.
Correlation correlate(const Image& l, const Image& r, int lx, int ly, int rx, int ry, int n) {
  Correlation c;
  if (lx < 0 || rx < 0 || ly < 0 || ry < 0 || lx + n > l.width || rx + n > r.width ||
      ly + n > l.height || ry + n > r.height)
    return c;
  double sl = 0, sr = 0, sll = 0, srr = 0, slr = 0;
  int clear = 0;
  for (int y = 0; y < n; ++y) {
    const float* a = l.data + (ly + y) * l.stride + lx;
    const float* b = r.data + (ry + y) * r.stride + rx;
    for (int x = 0; x < n; ++x) {
      if (a[x] < 0 || b[x] < 0) {
        if (a[x] == kOutside || b[x] == kOutside) return c;
        ++clear;
        continue;
      }
      sl += a[x];
      sr += b[x];
      sll += a[x] * a[x];
      srr += b[x] * b[x];
      slr += a[x] * b[x];
    }
  }
  const double count = static_cast<double>(n) * n - clear;
  if (2 * clear > n * n) return c;
  const double vl = std::max(0.0, sll / count - (sl / count) * (sl / count));
  const double vr = std::max(0.0, srr / count - (sr / count) * (sr / count));
  c.valid = true;
  c.spread = static_cast<float>(std::sqrt(std::min(vl, vr)));
  const double denominator = std::sqrt(vl * vr);
  c.score = denominator > 1e-10
                ? static_cast<float>((slr / count - (sl / count) * (sr / count)) / denominator)
                : 0.0f;
  return c;
}

// Disparity d (pixels) splits around the patch centre (moved by sx): the
// left window moves left by half, the right one right by the rest.
int leftCorner(int width, int d, int n, int sx = 0) { return width / 2 - (d >> 1) - n / 2 + sx; }
int rightCorner(int width, int d, int n, int sx = 0) {
  return width / 2 + (d - (d >> 1)) - n / 2 + sx;
}

// Passthrough: where the match sees video. A near object is seen half its
// disparity to either side, so a spot shows video if at some disparity both
// eyes' n×n windows are inside the picture and mostly not clear: the centre
// (at most half clear), or else the nearest offset (sx, sy) within `reach`
// pixels, in steps of f (at most kReachClear clear). Summed counts over the
// rows within reach make each window four lookups; rows without a window of
// video in either eye are skipped.
enum class Spot { Centre, Moved, None };
Spot findVideo(const Image& l, const Image& r, int n, int low, int high, int reach, int f,
               int& sx, int& sy) {
  const int rows = n + 2 * reach, y0 = (l.height - n) / 2 - reach, row = l.width + 1;
  const int outside = n * n + 1;  // one outside pixel is too many
  auto sums = [&](const Image& im) {
    std::vector<int> s(static_cast<size_t>(rows + 1) * row, 0);
    for (int y = 0; y < rows; ++y)
      for (int x = 0; x < im.width; ++x) {
        const float v = im.at(x, y0 + y);
        const size_t i = static_cast<size_t>(y + 1) * row + x + 1;
        s[i] = (v == kClear) + (v == kOutside) * outside + s[i - 1] + s[i - row] - s[i - row - 1];
      }
    return s;
  };
  const std::vector<int> ls = sums(l), rs = sums(r);
  auto count = [&](const std::vector<int>& s, int x, int y) {
    return s[(y + n) * row + x + n] - s[y * row + x + n] - s[(y + n) * row + x] + s[y * row + x];
  };
  // Which windows of the row offset dy are video in each eye, by left corner.
  std::vector<char> lok(l.width), rok(l.width);
  auto rowHasVideo = [&](int dy, int limit) {
    bool left = false, right = false;
    for (int x = 0; x + n <= l.width; ++x) {
      left |= lok[x] = count(ls, x, reach + dy) <= limit;
      right |= rok[x] = count(rs, x, reach + dy) <= limit;
    }
    return left && right;
  };
  auto video = [&](int dx, int every) {
    for (int d = low; d <= high; d += every) {
      const int lx = leftCorner(l.width, d, n, dx), rx = rightCorner(l.width, d, n, dx);
      if (lx >= 0 && rx >= 0 && lx + n <= l.width && rx + n <= l.width && lok[lx] && rok[rx])
        return true;
    }
    return false;
  };
  if (rowHasVideo(0, n * n / 2) && video(0, 1)) return Spot::Centre;
  const int limit = static_cast<int>(kReachClear * n * n), start = reach / f * f;
  int best = reach * reach + 1;
  for (int dy = -start; dy <= start; dy += f) {
    if (dy * dy >= best || !rowHasVideo(dy, limit)) continue;
    for (int dx = -start; dx <= start; dx += f) {
      const int d2 = dx * dx + dy * dy;
      if (d2 < best && video(dx, f)) best = d2, sx = dx, sy = dy;
    }
  }
  return reach > 0 && best <= reach * reach ? Spot::Moved : Spot::None;
}

// The best score per disparity over a range of vertical offsets, its peak and
// the best other peak, clearly apart from it.
struct Curve {
  std::vector<float> score;  // -2: no valid window
  std::vector<int> dy;
  int best = -1;
  float second = -1;
};

Curve bestPerDisparity(const std::vector<float>& table, int disparities, int rows, int dyLow,
                       int dyHigh, int slack) {
  Curve c;
  c.score.assign(disparities, -2.0f);
  c.dy.assign(disparities, 0);
  for (int i = 0; i < disparities; ++i) {
    for (int dy = std::max(dyLow, -slack); dy <= std::min(dyHigh, slack); ++dy) {
      const float s = table[i * rows + dy + slack];
      if (s > c.score[i]) c.score[i] = s, c.dy[i] = dy;
    }
    if (c.score[i] > -2.0f && (c.best < 0 || c.score[i] > c.score[c.best])) c.best = i;
  }
  for (int i = 1; i + 1 < disparities; ++i)
    if (std::abs(i - c.best) > 2 && c.score[i] >= c.score[i - 1] && c.score[i] >= c.score[i + 1])
      c.second = std::max(c.second, c.score[i]);
  return c;
}

DepthEstimate match(const Image& l, const Image& r, int window, int low, int high, float step,
                    float alpha, float beta, float baseline, bool masked, int reach,
                    VerticalOffset& vertical) {
  DepthEstimate e;
  const int f = coarseFactor(window);
  // Passthrough shows the room through most of the target's window: the
  // nearest spot within reach that shows the video, or nothing to focus on.
  int sx = 0, sy = 0;
  if (masked) {
    const Spot spot = findVideo(l, r, window, low, high, reach, f, sx, sy);
    if (spot == Spot::None) {
      e.status = DepthStatus::Clear;
      return e;
    }
    e.reachAlpha = sx * step;
    e.reachBeta = sy * step;
    alpha += e.reachAlpha;
    beta += e.reachBeta;
  }
  // Coarse: every disparity and vertical offset, at half resolution or, for
  // fine patches, a quarter (the vertical search would cost too much there).
  int cw = 0, ch = 0;
  const std::vector<float> lc = shrink(l, f, cw, ch), rc = shrink(r, f, cw, ch);
  const Image cl{lc.data(), cw, cw, ch}, cr{rc.data(), cw, cw, ch};
  const int cn = window / f, cy = (ch - cn) / 2 + sy / f, csx = sx / f;
  const int slack = ((l.height - 2 * reach - window) / 2 - f) / f;
  const int cLow = low >= 0 ? low / f : -((-low + f - 1) / f), cHigh = (high + f - 1) / f;
  const int disparities = cHigh - cLow + 1, rows = 2 * slack + 1;
  std::vector<float> table(static_cast<size_t>(disparities) * rows, -2.0f);
  for (int d = cLow; d <= cHigh; ++d)
    for (int dy = -slack; dy <= slack; ++dy) {
      const Correlation c = correlate(cl, cr, leftCorner(cw, d, cn, csx), cy,
                                      rightCorner(cw, d, cn, csx), cy + dy, cn);
      if (c.valid) table[(d - cLow) * rows + dy + slack] = c.score;
    }
  // The whole vertical range finds the video's offset; once it is known, only
  // a band around it is searched (along a straight edge, a free vertical
  // offset matches at many disparities).
  const Curve wide = bestPerDisparity(table, disparities, rows, -slack, slack, slack);
  if (wide.best < 0) return e;  // Edge
  const float u = std::cos(alpha) / std::sin(alpha) * std::cos(beta);
  if (wide.score[wide.best] >= kLearnCorrelation &&
      wide.score[wide.best] - wide.second >= kLearnMargin)
    vertical.add(u, f * wide.dy[wide.best] * step);
  Curve narrow;
  if (vertical.samples >= kLearnSamples) {
    const int centre = static_cast<int>(std::lround(vertical.predict(u) / (f * step)));
    const int band = static_cast<int>(std::ceil(kVerticalBand / (f * step)));
    narrow = bestPerDisparity(table, disparities, rows, centre - band, centre + band, slack);
  }
  const Curve& use = narrow.best >= 0 ? narrow : wide;
  const std::vector<float>& curve = use.score;
  const int best = use.best, bestDy = use.dy[best];
  const float second = use.second;

  // Fine: full resolution around the coarse peak.
  const int n = window, y0 = (l.height - n) / 2 + sy;
  float fineBest = -2;
  int fineD = 0, fineDy = 0;
  float spread = 0;
  const int around = f * (best + cLow);
  for (int d = std::max(low, around - f - 1); d <= std::min(high, around + f + 1); ++d)
    for (int dy = f * bestDy - f; dy <= f * bestDy + f; ++dy) {
      const Correlation c = correlate(l, r, leftCorner(l.width, d, n, sx), y0,
                                      rightCorner(l.width, d, n, sx), y0 + dy, n);
      if (c.valid && c.score > fineBest) {
        fineBest = c.score;
        fineD = d;
        fineDy = dy;
        spread = c.spread;
      }
    }
  if (fineBest < -1) return e;  // Edge
  auto scoreAt = [&](int d) {
    const Correlation c = correlate(l, r, leftCorner(l.width, d, n, sx), y0,
                                    rightCorner(l.width, d, n, sx), y0 + fineDy, n);
    return c.valid ? c.score : std::numeric_limits<float>::quiet_NaN();
  };
  float sub = 0;
  const float before = scoreAt(fineD - 1), after = scoreAt(fineD + 1);
  if (!std::isnan(before) && !std::isnan(after)) {
    const float curvature = before - 2 * fineBest + after;
    if (curvature < -1e-6f) sub = std::clamp(0.5f * (before - after) / curvature, -0.5f, 0.5f);
  }
  e.distance = distanceFromDisparity(alpha, (fineD + sub) * step, baseline);

  if (spread < kMinSpread) {
    e.status = DepthStatus::Blank;
    return e;
  }
  if (fineBest < kMinCorrelation) {
    e.status = DepthStatus::Mismatch;
    return e;
  }
  if (second > curve[best] - kUniqueMargin) {
    e.status = DepthStatus::Ambiguous;
    return e;
  }
  e.confidence = std::clamp((fineBest - kMinCorrelation) / 0.4f, 0.0f, 1.0f) *
                 std::clamp((curve[best] - second) / 0.3f, 0.0f, 1.0f) *
                 std::clamp(spread / 0.04f, 0.0f, 1.0f);
  e.status = DepthStatus::Ok;
  return e;
}

}  // namespace

void VerticalOffset::add(float su, float sv) {
  w = w * kLearnDecay + 1;
  u = u * kLearnDecay + su;
  v = v * kLearnDecay + sv;
  uu = uu * kLearnDecay + su * su;
  uv = uv * kLearnDecay + su * sv;
  ++samples;
}

float VerticalOffset::predict(float at) const {
  if (w <= 0) return 0;
  const double mu = u / w, mv = v / w, var = uu / w - mu * mu;
  const double roll =
      var < kMinRollSpread ? 0.0 : std::clamp((uv / w - mu * mv) / var, -kMaxRoll, kMaxRoll);
  return static_cast<float>(mv + roll * (at - mu));
}

float candidateAngle(float distance, float baseline, int accuracy) {
  const float step = kAngleRange / accuracy;
  const float angle = std::isfinite(distance) && distance > 0
                          ? std::atan(0.5f * baseline / distance)
                          : 0.0f;
  const int bin =
      std::clamp(static_cast<int>(std::lround((angle - kAngleStart) / step)), 0, 64 * accuracy - 1);
  return kAngleStart + bin * step;
}

DepthEstimator::~DepthEstimator() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    quit_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
  if (fence_) glDeleteSync(fence_);
  if (pbo_) glDeleteBuffers(1, &pbo_);
  if (fbo_) glDeleteFramebuffers(1, &fbo_);
  if (target_) glDeleteTextures(1, &target_);
  if (vao_) glDeleteVertexArrays(1, &vao_);
  if (sampler_) glDeleteSamplers(1, &sampler_);
  if (program_) glDeleteProgram(program_);
}

bool DepthEstimator::init() {
  program_ = compileProgram(kVertex, kFragment);
  if (!program_) return false;
  uTex_ = glGetUniformLocation(program_, "uTex");
  uMode_ = glGetUniformLocation(program_, "uMode");
  uRect_ = glGetUniformLocation(program_, "uRect");
  uStart_ = glGetUniformLocation(program_, "uStart");
  uStep_ = glGetUniformLocation(program_, "uStep");
  uProjection_ = glGetUniformLocation(program_, "uProjection");
  uFisheyeFov_ = glGetUniformLocation(program_, "uFisheyeFov");
  uFlip_ = glGetUniformLocation(program_, "uFlip");
  uEyeScale_ = glGetUniformLocation(program_, "uEyeScale");
  uEyeOffset_ = glGetUniformLocation(program_, "uEyeOffset");
  uAlphaPacked_ = glGetUniformLocation(program_, "uAlphaPacked");
  uAlphaCenter_ = glGetUniformLocation(program_, "uAlphaCenter");
  uAlphaScale_ = glGetUniformLocation(program_, "uAlphaScale");
  uGreenKey_ = glGetUniformLocation(program_, "uGreenKey");
  glGenVertexArrays(1, &vao_);
  // The patch is coarser than the video: it is read through the mipmaps even
  // while the video itself is shown from its full-size level (Video mipmaps off).
  glGenSamplers(1, &sampler_);
  glSamplerParameteri(sampler_, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glSamplerParameteri(sampler_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glSamplerParameteri(sampler_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glSamplerParameteri(sampler_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenTextures(1, &target_);
  glBindTexture(GL_TEXTURE_2D, target_);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32F, kMaxWidth, kMaxHeight);
  glBindTexture(GL_TEXTURE_2D, 0);
  glGenFramebuffers(1, &fbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_, 0);
  const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (!complete) return false;
  glGenBuffers(1, &pbo_);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_);
  glBufferData(GL_PIXEL_PACK_BUFFER, sizeof(float) * kMaxWidth * kMaxHeight, nullptr,
               GL_STREAM_READ);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  thread_ = std::thread([this] { work(); });
  return true;
}

void DepthEstimator::forgetScene() { forgetScene_ = true; }

void DepthEstimator::forgetVideo() {
  forgetScene_ = true;
  forgetVideo_ = true;
}

bool DepthEstimator::request(const DepthRequest& q) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stage_ != Stage::Idle) return false;
  }
  Job job;
  job.result.tag = q.tag;
  job.forget = forgetScene_;
  job.forgetVideo = forgetVideo_;
  forgetScene_ = forgetVideo_ = false;
  float alpha = 0, beta = 0;
  epipolarAngles(normalize(q.direction), alpha, beta);
  if (std::sin(alpha) < kMinSinAlpha || (!q.texture && !q.drawer)) {
    forgetScene_ = job.forget;  // for the next real measurement
    forgetVideo_ = job.forgetVideo;
    std::lock_guard<std::mutex> lock(mutex_);
    job_ = job;  // status Edge
    stage_ = Stage::Ready;
    return true;
  }
  const int accuracy = std::clamp(q.accuracy, 1, 128);
  const float angleStep = kAngleRange / accuracy;
  // A wider window keeps the pixel count, and so the cost: a coarser pitch.
  const float window = q.window > 0 ? q.window : kWindowAngle;
  const float step = std::clamp(2 * angleStep, kMinStep, kMaxStep) * window / kWindowAngle;
  const int n = std::min(kMaxWindow, 2 * static_cast<int>(std::lround(0.5f * window / step)));
  // All candidate depths: from infinity to the nearest one (6 cm at
  // a 65 mm baseline), so that depths outside the accepted range are told
  // apart from no match.
  const float nearest =
      0.5f * q.baseline / std::tan(kAngleStart + (64 * accuracy - 1) * angleStep);
  int low = static_cast<int>(std::floor(-kSearchMargin / step));
  int high = static_cast<int>(
      std::ceil((disparityAt(alpha, nearest, q.baseline) + kSearchMargin) / step));
  // Room on every side for the window to move off a see-through target, in
  // whole coarse pixels so that they stay aligned with the window.
  const bool masked = q.seeThrough.packedAlpha || q.seeThrough.greenKey > 0;
  const int f = coarseFactor(n);
  const int reach =
      masked && q.reach > 0
          ? f * static_cast<int>(std::ceil(std::min(q.reach, kMaxReach) / step / f))
          : 0;
  const int widest = kMaxWidth - n - 8 - 2 * reach;
  low = std::max(low, -widest);
  high = std::min(high, widest);
  int width = std::max(std::abs(low), std::abs(high)) + n + 8 + 2 * reach;
  width = std::max(width + (width & 1), kThumbW);
  const int height = n + 2 * marginRows(step, n) + 2 * reach;
  job.stride = width;
  job.width = width;
  job.height = height;
  job.window = n;
  job.masked = masked;
  job.reach = reach;
  job.low = low;
  job.high = high;
  job.step = step;
  job.alpha = alpha;
  job.beta = beta;
  job.baseline = q.baseline;
  job.accuracy = accuracy;

  int projection = 0;
  float fisheyeFov = 0;
  if (q.format.projection == Projection::Fisheye180) projection = 2, fisheyeFov = 180 * kDegree;
  if (q.format.projection == Projection::Fisheye190) projection = 2, fisheyeFov = 190 * kDegree;
  if (q.format.projection == Projection::Fisheye200) projection = 2, fisheyeFov = 200 * kDegree;
  const bool sbs = q.format.stereo == StereoLayout::SideBySide;

  if (q.drawer) {
    DepthPatch patch;
    patch.width = width;
    patch.height = height;
    patch.start[0] = alpha - 0.5f * width * step;
    patch.start[1] = beta - 0.5f * height * step;
    patch.step = step;
    patch.projection = projection;
    patch.fisheyeFov = fisheyeFov;
    patch.flip = q.flip;
    patch.sbs = sbs;
    patch.alphaPacked = maskRule(q.seeThrough);
    patch.alphaScale[0] = 0.4f * (sbs ? 0.5f : 1.0f);
    patch.alphaScale[1] = 0.4f * (sbs ? 1.0f : 0.5f);
    patch.greenKey = q.seeThrough.greenKey;
    if (!q.drawer->draw(patch)) {
      forgetScene_ = job.forget;
      forgetVideo_ = job.forgetVideo;
      return false;
    }
    drawer_ = q.drawer;
    pending_ = std::move(job);
    std::lock_guard<std::mutex> lock(mutex_);
    stage_ = Stage::Gpu;
    return true;
  }
  if (q.prepareRead && !q.prepareRead()) {
    forgetScene_ = job.forget;
    forgetVideo_ = job.forgetVideo;
    return false;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glDisable(GL_FRAMEBUFFER_SRGB);
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glUseProgram(program_);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, q.texture);
  glBindSampler(0, sampler_);
  glBindVertexArray(vao_);
  glUniform1i(uTex_, 0);
  glUniform1f(uStep_, step);
  glUniform1i(uProjection_, projection);
  glUniform1f(uFisheyeFov_, fisheyeFov);
  glUniform1i(uFlip_, q.flip);
  glUniform2f(uEyeScale_, sbs ? 0.5f : 1.0f, sbs ? 1.0f : 0.5f);
  glUniform1i(uAlphaPacked_, maskRule(q.seeThrough));
  glUniform2f(uAlphaScale_, 0.4f * (sbs ? 0.5f : 1.0f), 0.4f * (sbs ? 1.0f : 0.5f));
  glUniform1f(uGreenKey_, q.seeThrough.greenKey);
  // Both eyes over the same angles; rows 0.. left, then right, then the thumbnail.
  glUniform2f(uStart_, alpha - 0.5f * width * step, beta - 0.5f * height * step);
  for (int eye = 0; eye < 2; ++eye) {
    glViewport(0, eye * height, width, height);
    glUniform4f(uRect_, 0.0f, static_cast<float>(eye * height), static_cast<float>(width),
                static_cast<float>(height));
    glUniform2f(uEyeOffset_, sbs ? 0.5f * eye : 0.0f, sbs ? 0.0f : 0.5f * eye);
    glUniform2f(uAlphaCenter_, eye == 0 ? 0.5f : 0.0f, 0.0f);  // as SceneRenderer
    glUniform1i(uMode_, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }
  glViewport(0, 2 * height, kThumbW, kThumbH);
  glUniform4f(uRect_, 0.0f, static_cast<float>(2 * height), kThumbW, kThumbH);
  glUniform2f(uEyeOffset_, 0.0f, 0.0f);
  glUniform1i(uMode_, 1);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindVertexArray(0);
  glBindSampler(0, 0);

  glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glReadPixels(0, 0, width, 2 * height + kThumbH, GL_RED, GL_FLOAT, nullptr);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  fence_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);  // flushed with the frame
  pending_ = std::move(job);
  std::lock_guard<std::mutex> lock(mutex_);
  stage_ = Stage::Gpu;
  return true;
}

bool DepthEstimator::poll(DepthEstimate& out) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (stage_ == Stage::Gpu && drawer_ && drawer_->ready()) {
    const size_t count = static_cast<size_t>(pending_.stride) * (2 * pending_.height + kThumbH);
    pending_.pixels.resize(count);
    if (const float* data = drawer_->values()) std::copy_n(data, count, pending_.pixels.begin());
    else pending_.pixels.assign(count, -1.0f);
    drawer_ = nullptr;
    job_ = std::move(pending_);
    stage_ = Stage::Working;
    lock.unlock();
    wake_.notify_one();
    return false;
  }
  if (stage_ == Stage::Gpu && fence_) {
    const GLenum state = glClientWaitSync(fence_, 0, 0);
    if (state == GL_ALREADY_SIGNALED || state == GL_CONDITION_SATISFIED) {
      glDeleteSync(fence_);
      fence_ = nullptr;
      const size_t count = static_cast<size_t>(pending_.stride) * (2 * pending_.height + kThumbH);
      pending_.pixels.resize(count);
      glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_);
      if (const void* data = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, count * sizeof(float),
                                              GL_MAP_READ_BIT)) {
        std::copy_n(static_cast<const float*>(data), count, pending_.pixels.begin());
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
      } else {
        pending_.pixels.assign(count, -1.0f);
      }
      glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
      job_ = std::move(pending_);
      stage_ = Stage::Working;
      lock.unlock();
      wake_.notify_one();
      return false;
    }
  }
  if (stage_ != Stage::Ready) return false;
  out = job_.result;
  stage_ = Stage::Idle;
  return true;
}

void DepthEstimator::work() {
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    wake_.wait(lock, [this] { return quit_ || stage_ == Stage::Working; });
    if (quit_) return;
    Job job = std::move(job_);
    lock.unlock();

    const Image left{job.pixels.data(), job.stride, job.width, job.height};
    const Image right{job.pixels.data() + job.stride * job.height, job.stride, job.width,
                      job.height};
    const uint64_t tag = job.result.tag;
    if (job.forgetVideo) vertical_ = VerticalOffset{};
    job.result = match(left, right, job.window, job.low, job.high, job.step, job.alpha,
                       job.beta, job.baseline, job.masked, job.reach, vertical_);
    job.result.tag = tag;
    if (job.result.status == DepthStatus::Ok) {
      job.result.angle = candidateAngle(job.result.distance, job.baseline, job.accuracy);
      job.result.distance = 0.5f * job.baseline / std::tan(job.result.angle);
    }
    // The whole eye against the last measurement's: a scene cut changes most of it.
    std::vector<float> thumb(kThumbW * kThumbH);
    for (int y = 0; y < kThumbH; ++y)
      for (int x = 0; x < kThumbW; ++x)
        thumb[y * kThumbW + x] = job.pixels[(2 * job.height + y) * job.stride + x];
    if (job.forget) lastThumbnail_.clear();
    if (lastThumbnail_.size() == thumb.size()) {
      double sum = 0;
      for (size_t i = 0; i < thumb.size(); ++i) sum += std::abs(thumb[i] - lastThumbnail_[i]);
      job.result.sceneChange = static_cast<float>(sum / thumb.size());
    }
    lastThumbnail_ = std::move(thumb);

    lock.lock();
    job_ = std::move(job);
    stage_ = Stage::Ready;
  }
}
