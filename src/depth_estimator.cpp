#include "depth_estimator.h"

#include "stereo_correction.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {

// How the shaders read a packed mask (DepthPatch::alphaPacked).
int maskRule(const SeeThrough& s) {
  if (!s.packedAlpha) return 0;
  return s.maskColours == MaskColours::RedBlue ? 3 : 1;
}

// How near a flat background's colour the shaders' key leaves a pixel less
// than half there: clear in the patch (above 0).
float keyCut(const SeeThrough& s) { return s.keyCut + 0.5f * s.keySoft; }

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
uniform float uKeyCut;      // > 0: a flat background of the colour uKey, keyed within this
uniform vec3 uKey;          // (display values, as video.glsl's colourAlpha() compares them)
const float PI = 3.14159265359;
// The picture at p (frame coordinates) over the pixel's footprint dx, dy,
// through the mipmaps. Linear colour.
vec3 picture(vec2 p, vec2 dx, vec2 dy) {
  return textureGrad(uTex, p, dx, dy).rgb;
}
// Display values e as the colour key compares them (video.glsl).
vec3 keySpace(vec3 e) {
  return vec3(dot(e, vec3(0.299, 0.587, 0.114)) / 3.0, dot(e, vec3(-0.169, -0.331, 0.5)),
              dot(e, vec3(0.5, -0.419, -0.081)));
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
  } else if (uMode == 0 && uKeyCut > 0.0) {
    vec3 v = clamp(c, 0.0, 1.0);  // to display values, as the scene reads them
    vec3 e = mix(12.92 * v, 1.055 * pow(v, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, v));
    clear = distance(keySpace(e), keySpace(uKey)) <= uKeyCut;
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

// The patch reduced F times (F×F averages of what is not clear; outside if
// any is, clear if all are). A row of the result at a time, each of its
// pixels adding up its F×F values in the same order as one on its own would,
// which lets the compiler work on several pixels with one instruction.
template <int F>
void shrinkBy(const Image& im, std::vector<float>& out, std::vector<float>& sums,
              std::vector<int>& counts, std::vector<int>& outside) {
  const int w = im.width / F, h = im.height / F;
  out.resize(static_cast<size_t>(w) * h);
  sums.resize(w);
  counts.resize(w);
  outside.resize(w);
  float* sum = sums.data();
  int* count = counts.data();
  int* any = outside.data();
  for (int y = 0; y < h; ++y) {
    std::fill(sums.begin(), sums.end(), 0.0f);
    std::fill(counts.begin(), counts.end(), 0);
    std::fill(outside.begin(), outside.end(), 0);
    for (int j = 0; j < F; ++j)
      for (int i = 0; i < F; ++i) {
        const float* v = im.data + (F * y + j) * im.stride + i;
        for (int x = 0; x < w; ++x) {
          sum[x] += v[F * x] >= 0 ? v[F * x] : 0.0f;
          count[x] += v[F * x] >= 0;
          any[x] |= v[F * x] == kOutside;
        }
      }
    float* to = out.data() + static_cast<size_t>(y) * w;
    for (int x = 0; x < w; ++x) to[x] = any[x] ? kOutside : count[x] ? sum[x] / count[x] : kClear;
  }
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

// A pass of the match: every disparity dLow..dHigh at every row offset
// dyLow..dyHigh of the right window, as correlate() pairs them (the left
// window's top row is y0, both are moved across by sx).
struct Pass {
  int n, y0, sx, dLow, dHigh, dyLow, dyHigh;
};

// The matcher's buffers, kept from one measurement to the next (the worker's).
struct Scratch {
  std::vector<float> scores, fine, spreads;  // a pass's results, by disparity and row offset
  std::vector<int> summed, leftExcluded, rightExcluded;
  std::vector<double> leftMean, leftVariance, rightMean, rightVariance, products;
  std::vector<double> columns, columnSquares;  // slidingSums()' sums over a column of n pixels
  std::vector<float> leftRows, rightRows;  // the windows' rows laid out by disparity
  std::vector<float> left, right, rowSums;  // the patches reduced for the coarse pass
  std::vector<int> rowCounts, rowOutside;
};

// f is coarseFactor()'s.
Image shrink(const Image& im, int f, std::vector<float>& out, Scratch& s) {
  if (f == 4) shrinkBy<4>(im, out, s.rowSums, s.rowCounts, s.rowOutside);
  else shrinkBy<2>(im, out, s.rowSums, s.rowCounts, s.rowOutside);
  return {out.data(), im.width / f, im.width / f, im.height / f};
}

// How many windows' sums are added to for one pixel before the next one's
// turn: with only a few, each addition would wait for the last one to the
// same sum.
constexpr int kTogether = 256;

// How many pixels of each n×n window a match leaves out (clear ones), for the
// windows with top-left corners (x0 + k, y0 + j), k below `count` and j below
// `rows`, by summed counts. A pixel outside the picture counts as more than a
// whole window.
void excluded(const Image& im, int n, int x0, int count, int y0, int rows, std::vector<int>& sums,
              std::vector<int>& out) {
  const int width = count + n - 1, height = rows + n - 1, row = width + 1;
  out.assign(static_cast<size_t>(rows) * count, 0);
  // Most patches have nothing to leave out.
  bool any = false;
  for (int y = 0; y < height; ++y) {
    const float* v = im.data + (y0 + y) * im.stride + x0;
    for (int x = 0; x < width; ++x) any |= v[x] < 0;
  }
  if (!any) return;
  sums.assign(static_cast<size_t>(height + 1) * row, 0);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      const float v = im.at(x0 + x, y0 + y);
      const size_t i = static_cast<size_t>(y + 1) * row + x + 1;
      sums[i] = (v == kOutside ? n * n + 1 : v < 0) + sums[i - 1] + sums[i - row] -
                sums[i - row - 1];
    }
  for (int j = 0; j < rows; ++j)
    for (int k = 0; k < count; ++k)
      out[j * count + k] = sums[(j + n) * row + k + n] - sums[j * row + k + n] -
                           sums[(j + n) * row + k] + sums[j * row + k];
}

// Whether every sum of up to `terms` of the pixels x0 .. x0 + width - 1,
// y0 .. y0 + height - 1 of the image, or of their float squares, is exact in a
// double, and so the same in whatever order it is added up: addition and
// subtraction round only what a double cannot hold. With nonzero magnitudes in
// [2^lo, 2^(lo + spread)), between 2^-63 and 2^32 (where no square leaves the
// floats' normal range, and no value is infinite or NaN), their float squares
// are whole steps of 2^(2 lo - 23), up to 2^(2 lo + 2 spread). With b the bits
// `terms` needs, a sum of up to `terms` squares is at most
// 2^(b + 2 spread + 23) steps, and a double holds any whole number of steps up
// to 2^53: hence b + 2 spread <= 30. Plain sums need less. The exponents are
// read from the bits as integers, which the compiler compares several at a
// time.
bool sumsExact(const Image& im, int x0, int width, int y0, int height, int terms) {
  uint32_t most = 0, least = 0xffffffffu;
  for (int y = 0; y < height; ++y) {
    const float* v = im.data + (y0 + y) * im.stride + x0;
    for (int x = 0; x < width; ++x) {
      uint32_t u;
      std::memcpy(&u, v + x, sizeof(u));
      u &= 0x7fffffffu;
      most = std::max(most, u);
      least = std::min(least, u - 1);  // a zero wraps around, and so is never the least
    }
  }
  if (least == 0xffffffffu) return true;  // all zero
  const int maxE = static_cast<int>(most >> 23), minE = static_cast<int>((least + 1) >> 23);
  int bits = 0;
  while ((1 << bits) < terms) ++bits;
  return minE >= 64 && maxE <= 158 && bits + 2 * (maxE + 1 - minE) <= 30;
}

// The sums and sums of squares of those windows, into mean and variance, in
// the same order as correlate() adds them up (row by row), but a pixel at a
// time for every window, which lets the compiler add to several windows' sums
// with one instruction.
void orderedSums(const Image& im, int n, int x0, int count, int y0, int rows,
                 std::vector<double>& mean, std::vector<double>& variance) {
  const int together = std::max(1, kTogether / count);
  for (int first = 0; first < rows; first += together) {
    const int last = std::min(rows, first + together);
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x)
        for (int j = first; j < last; ++j) {
          const float* v = im.data + (y0 + j + y) * im.stride + x0 + x;
          double* sum = mean.data() + static_cast<size_t>(j) * count;
          double* squares = variance.data() + static_cast<size_t>(j) * count;
          for (int k = 0; k < count; ++k) sum[k] += v[k];
          for (int k = 0; k < count; ++k) squares[k] += v[k] * v[k];
        }
  }
}

// The same sums, slid along: sums over n pixels of each column gain the row
// that enters and lose the row that leaves, and each window's sum does the
// same with columns. Each is made of at most n * n + n values or squares on
// the way.
void slidingSums(const Image& im, int n, int x0, int count, int y0, int rows, Scratch& s,
                 std::vector<double>& mean, std::vector<double>& variance) {
  const int width = count + n - 1;
  s.columns.assign(width, 0.0);
  s.columnSquares.assign(width, 0.0);
  double* column = s.columns.data();
  double* columnSquares = s.columnSquares.data();
  for (int y = 0; y < n; ++y) {
    const float* v = im.data + (y0 + y) * im.stride + x0;
    for (int x = 0; x < width; ++x) column[x] += v[x];
    for (int x = 0; x < width; ++x) columnSquares[x] += v[x] * v[x];
  }
  for (int j = 0; j < rows; ++j) {
    if (j > 0) {  // each value on its own: in - out as floats would round
      const float* in = im.data + (y0 + j + n - 1) * im.stride + x0;
      const float* out = im.data + (y0 + j - 1) * im.stride + x0;
      for (int x = 0; x < width; ++x) column[x] = column[x] + in[x] - out[x];
      for (int x = 0; x < width; ++x)
        columnSquares[x] = columnSquares[x] + in[x] * in[x] - out[x] * out[x];
    }
    double* sum = mean.data() + static_cast<size_t>(j) * count;
    double* squares = variance.data() + static_cast<size_t>(j) * count;
    double window = 0, windowSquares = 0;
    for (int x = 0; x < n; ++x) {
      window += column[x];
      windowSquares += columnSquares[x];
    }
    sum[0] = window;
    squares[0] = windowSquares;
    for (int k = 1; k < count; ++k) {
      window = window + column[k + n - 1] - column[k - 1];
      windowSquares = windowSquares + columnSquares[k + n - 1] - columnSquares[k - 1];
      sum[k] = window;
      squares[k] = windowSquares;
    }
  }
}

// The mean and variance of each of those windows, as correlate() has them for
// a pair with nothing left out: from sliding sums when they cannot round,
// else from ordered ones.
void windowStats(const Image& im, int n, int x0, int count, int y0, int rows, Scratch& s,
                 std::vector<double>& mean, std::vector<double>& variance) {
  mean.assign(static_cast<size_t>(rows) * count, 0.0);      // first the sums
  variance.assign(static_cast<size_t>(rows) * count, 0.0);  // and the sums of squares
  if (sumsExact(im, x0, count + n - 1, y0, rows + n - 1, n * n + n))
    slidingSums(im, n, x0, count, y0, rows, s, mean, variance);
  else
    orderedSums(im, n, x0, count, y0, rows, mean, variance);
  // One copy for both, so both get the same instructions (on ARM64 the
  // compiler may fuse the multiply and subtract).
  const double pixels = static_cast<double>(n) * n;
  for (size_t i = 0; i < mean.size(); ++i) {
    mean[i] = mean[i] / pixels;
    variance[i] = std::max(0.0, variance[i] / pixels - mean[i] * mean[i]);
  }
}

// correlate() for every pair of a pass: its score in `scores` (-2: no valid
// window) and, if asked, its spread in `spreads` (-1: no valid window), at
// (d - dLow) * (dyHigh - dyLow + 1) + dy - dyLow. The same values, because
// each sum is either added up in the same order or cannot round. A pair with
// nothing left out, as nearly all are, needs only the sum of its products: its
// windows' means and variances are shared with the other pairs they are part
// of. One with clear pixels is left to correlate().
void correlateAll(const Image& l, const Image& r, const Pass& p, Scratch& s,
                  std::vector<float>& scores, std::vector<float>* spreads) {
  const int n = p.n, rows = p.dyHigh - p.dyLow + 1;
  scores.assign(static_cast<size_t>(std::max(0, p.dHigh - p.dLow + 1)) * std::max(0, rows), -2.0f);
  if (spreads) spreads->assign(scores.size(), -1.0f);
  // The pairs inside the patch: disparities dA..dB at row offsets dyA..dyB.
  auto inside = [&](int d) {
    const int lx = leftCorner(l.width, d, n, p.sx), rx = rightCorner(l.width, d, n, p.sx);
    return lx >= 0 && rx >= 0 && lx + n <= l.width && rx + n <= r.width;
  };
  int dA = p.dLow, dB = p.dHigh;
  while (dA <= dB && !inside(dA)) ++dA;
  while (dB >= dA && !inside(dB)) --dB;
  const int dyA = std::max(p.dyLow, -p.y0), dyB = std::min(p.dyHigh, r.height - n - p.y0);
  if (dA > dB || dyA > dyB || p.y0 < 0 || p.y0 + n > l.height) return;
  const int count = dB - dA + 1, ry0 = p.y0 + dyA, ryCount = dyB - dyA + 1;
  // The left windows move left as the disparity grows, the right ones right.
  const int lx0 = leftCorner(l.width, dB, n, p.sx),
            lxCount = leftCorner(l.width, dA, n, p.sx) - lx0 + 1;
  const int rx0 = rightCorner(l.width, dA, n, p.sx),
            rxCount = rightCorner(l.width, dB, n, p.sx) - rx0 + 1;
  excluded(l, n, lx0, lxCount, p.y0, 1, s.summed, s.leftExcluded);
  excluded(r, n, rx0, rxCount, ry0, ryCount, s.summed, s.rightExcluded);
  windowStats(l, n, lx0, lxCount, p.y0, 1, s, s.leftMean, s.leftVariance);
  windowStats(r, n, rx0, rxCount, ry0, ryCount, s, s.rightMean, s.rightVariance);

  // The sums of products. Every row is laid out by disparity first, each of
  // its pixels twice: pixel x of disparity (dA + i)'s window is element
  // i - 2x of the left window's row (from its end) and i + 2x of the right
  // one's, so one run over the elements adds that pixel's product to every
  // disparity's sum.
  const int centre = l.width / 2 - n / 2 + p.sx, back = 2 * (n - 1), length = count + back;
  s.leftRows.resize(static_cast<size_t>(n) * length);
  for (int y = 0; y < n; ++y)
    for (int i = 0; i < length; ++i)
      s.leftRows[y * length + i] = l.at(centre - ((dA + i - back) >> 1), p.y0 + y);
  s.rightRows.resize(static_cast<size_t>(ryCount + n - 1) * length);
  for (int y = 0; y < ryCount + n - 1; ++y)
    for (int i = 0; i < length; ++i)
      s.rightRows[y * length + i] = r.at(centre + ((dA + i + 1) >> 1), ry0 + y);
  s.products.assign(static_cast<size_t>(ryCount) * count, 0.0);
  const int together = std::max(1, kTogether / count);
  for (int first = 0; first < ryCount; first += together) {
    const int last = std::min(ryCount, first + together);
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) {
        const float* a = s.leftRows.data() + static_cast<size_t>(y) * length + back - 2 * x;
        for (int j = first; j < last; ++j) {
          const float* b = s.rightRows.data() + static_cast<size_t>(j + y) * length + 2 * x;
          double* sum = s.products.data() + static_cast<size_t>(j) * count;
          for (int i = 0; i < count; ++i) sum[i] += a[i] * b[i];
        }
      }
  }

  const double pixels = static_cast<double>(n) * n;
  for (int j = 0; j < ryCount; ++j)
    for (int i = 0; i < count; ++i) {
      const int d = dA + i;
      const int lx = leftCorner(l.width, d, n, p.sx), rx = rightCorner(l.width, d, n, p.sx);
      const size_t left = lx - lx0, right = static_cast<size_t>(j) * rxCount + rx - rx0;
      const size_t at = static_cast<size_t>(d - p.dLow) * rows + dyA + j - p.dyLow;
      const int out = std::max(s.leftExcluded[left], s.rightExcluded[right]);
      if (out == 0) {
        // As correlate() ends.
        const double vl = s.leftVariance[left], vr = s.rightVariance[right];
        const double denominator = std::sqrt(vl * vr);
        scores[at] = denominator > 1e-10
                         ? static_cast<float>((s.products[static_cast<size_t>(j) * count + i] / pixels -
                                               s.leftMean[left] * s.rightMean[right]) /
                                              denominator)
                         : 0.0f;
        if (spreads) (*spreads)[at] = static_cast<float>(std::sqrt(std::min(vl, vr)));
      } else if (2 * out <= n * n) {  // else one is outside the picture, or mostly clear
        const Correlation c = correlate(l, r, lx, p.y0, rx, ry0 + j, n);
        if (c.valid) {
          scores[at] = c.score;
          if (spreads) (*spreads)[at] = c.spread;
        }
      }
    }
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
                    VerticalOffset& vertical, Scratch& s) {
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
  const Image cl = shrink(l, f, s.left, s), cr = shrink(r, f, s.right, s);
  const int cn = window / f, cy = (cl.height - cn) / 2 + sy / f, csx = sx / f;
  const int slack = ((l.height - 2 * reach - window) / 2 - f) / f;
  const int cLow = low >= 0 ? low / f : -((-low + f - 1) / f), cHigh = (high + f - 1) / f;
  const int disparities = cHigh - cLow + 1, rows = 2 * slack + 1;
  const std::vector<float>& table = s.scores;
  correlateAll(cl, cr, {cn, cy, csx, cLow, cHigh, -slack, slack}, s, s.scores, nullptr);
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
  const int dLow = std::max(low, around - f - 1), dHigh = std::min(high, around + f + 1);
  const int dyLow = f * bestDy - f, dyHigh = f * bestDy + f, fineRows = dyHigh - dyLow + 1;
  correlateAll(l, r, {n, y0, sx, dLow, dHigh, dyLow, dyHigh}, s, s.fine, &s.spreads);
  for (int d = dLow; d <= dHigh; ++d)
    for (int dy = dyLow; dy <= dyHigh; ++dy) {
      const size_t at = static_cast<size_t>(d - dLow) * fineRows + dy - dyLow;
      if (s.spreads[at] >= 0 && s.fine[at] > fineBest) {
        fineBest = s.fine[at];
        fineD = d;
        fineDy = dy;
        spread = s.spreads[at];
      }
    }
  if (fineBest < -1) return e;  // Edge
  auto scoreAt = [&](int d) {
    if (d >= dLow && d <= dHigh) {
      const size_t at = static_cast<size_t>(d - dLow) * fineRows + fineDy - dyLow;
      return s.spreads[at] >= 0 ? s.fine[at] : std::numeric_limits<float>::quiet_NaN();
    }
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
  uKeyCut_ = glGetUniformLocation(program_, "uKeyCut");
  uKey_ = glGetUniformLocation(program_, "uKey");
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
  const bool masked =
      q.seeThrough.packedAlpha || q.seeThrough.greenKey > 0 || q.seeThrough.colourKey;
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
  // A cut is told from the thumbnail before it: after forgetVideo(), one at once.
  const auto now = std::chrono::steady_clock::now();
  job.thumbnail = job.forget || now - thumbnailAt_ >= std::chrono::duration<double>(kThumbEvery);
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
    patch.thumbnail = job.thumbnail;
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
    if (q.seeThrough.colourKey) {
      patch.keyColour = q.seeThrough.packedKeyColour();
      patch.keyCut = keyCut(q.seeThrough);
    }
    if (!q.drawer->draw(patch)) {
      forgetScene_ = job.forget;
      forgetVideo_ = job.forgetVideo;
      return false;
    }
    if (job.thumbnail) thumbnailAt_ = now;
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
  glUniform1f(uKeyCut_, q.seeThrough.colourKey ? keyCut(q.seeThrough) : 0.0f);
  glUniform3fv(uKey_, 1, q.seeThrough.keyColour);
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
  if (job.thumbnail) {
    glViewport(0, 2 * height, kThumbW, kThumbH);
    glUniform4f(uRect_, 0.0f, static_cast<float>(2 * height), kThumbW, kThumbH);
    glUniform2f(uEyeOffset_, 0.0f, 0.0f);
    glUniform1i(uMode_, 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    thumbnailAt_ = now;
  }
  glBindVertexArray(0);
  glBindSampler(0, 0);

  glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glReadPixels(0, 0, width, job.rows(), GL_RED, GL_FLOAT, nullptr);
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
  // The readback goes into the last measurement's pixels: sized already, they
  // are neither allocated nor cleared again.
  if (stage_ == Stage::Gpu && pending_.pixels.empty()) pending_.pixels.swap(job_.pixels);
  if (stage_ == Stage::Gpu && drawer_ && drawer_->ready()) {
    const size_t count = static_cast<size_t>(pending_.stride) * pending_.rows();
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
      const size_t count = static_cast<size_t>(pending_.stride) * pending_.rows();
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
  Scratch scratch;
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
                       job.beta, job.baseline, job.masked, job.reach, vertical_, scratch);
    job.result.tag = tag;
    if (job.result.status == DepthStatus::Ok) {
      job.result.angle = candidateAngle(job.result.distance, job.baseline, job.accuracy);
      job.result.distance = 0.5f * job.baseline / std::tan(job.result.angle);
    }
    // The whole eye against the thumbnail before: a scene cut changes most of it.
    if (job.forget) lastThumbnail_.clear();
    if (job.thumbnail) {
      std::vector<float> thumb(kThumbW * kThumbH);
      for (int y = 0; y < kThumbH; ++y)
        for (int x = 0; x < kThumbW; ++x)
          thumb[y * kThumbW + x] = job.pixels[(2 * job.height + y) * job.stride + x];
      // A readback that failed is all kOutside, which no thumbnail has: not
      // one to compare, now or with the next.
      const bool read = thumb.front() >= 0;
      if (read && lastThumbnail_.size() == thumb.size()) {
        double sum = 0;
        for (size_t i = 0; i < thumb.size(); ++i) sum += std::abs(thumb[i] - lastThumbnail_[i]);
        job.result.sceneChange = static_cast<float>(sum / thumb.size());
      }
      if (read) lastThumbnail_ = std::move(thumb);
    }

    lock.lock();
    job_ = std::move(job);
    stage_ = Stage::Ready;
  }
}
