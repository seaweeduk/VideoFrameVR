#include "vr_space.h"

#include "menu.h"
#include "menu_geometry.h"

#include <cmath>

namespace mg = menu_geometry;

float wrapAngle(float a) {
  while (a > kPi) a -= 2 * kPi;
  while (a < -kPi) a += 2 * kPi;
  return a;
}

Vec3 forwardFromYaw(float yaw) { return {-std::sin(yaw), 0.0f, -std::cos(yaw)}; }

Mat4 headFrame(Quat headOrientation) { return rotationY(yawFromQuat(headOrientation)); }

Mat4 menuFrame(Quat headOrientation) {
  // Keep the head's pitch, take out its roll.
  const Vec3 back = transformDir(rotation(headOrientation), {0.0f, 0.0f, 1.0f});
  Mat4 frame;
  if (facingBasis(back, {0.0f, 1.0f, 0.0f}, frame)) return frame;
  return rotationY(yawFromQuat(headOrientation));  // looking straight up or down
}

Mat4 poseMatrix(Vec3 position, Quat orientation) {
  return translation(position) * rotation(orientation);
}

Mat4 poseInverse(Vec3 position, Quat orientation) {
  return transposedRotation(rotation(orientation)) * translation(position * -1.0f);
}

bool facingBasis(Vec3 z, Vec3 up, Mat4& out) {
  const Vec3 x = cross(up, z);
  if (length(x) < 1e-3f) return false;
  const Vec3 xn = normalize(x), y = cross(z, xn);
  out = Mat4();
  out.m[0] = xn.x, out.m[1] = xn.y, out.m[2] = xn.z;
  out.m[4] = y.x, out.m[5] = y.y, out.m[6] = y.z;
  out.m[8] = z.x, out.m[9] = z.y, out.m[10] = z.z;
  return true;
}

bool CurvedPanel::hit(Vec3 origin, Vec3 dir, float& u, float& v, float& distance) const {
  const Mat4 toLocal = transposedRotation(orientation);
  const Vec3 o = transformDir(toLocal, origin - axis);
  const Vec3 d = transformDir(toLocal, dir);
  // Where the ray meets the cylinder: |o + d t| = radius, ignoring y.
  const float a = d.x * d.x + d.z * d.z;
  const float b = 2 * (o.x * d.x + o.z * d.z);
  const float c = o.x * o.x + o.z * o.z - radius * radius;
  const float disc = b * b - 4 * a * c;
  if (a < 1e-8f || disc < 0) return false;
  for (const float t : {(-b - std::sqrt(disc)) / (2 * a), (-b + std::sqrt(disc)) / (2 * a)}) {
    if (t <= 0) continue;
    const Vec3 p = o + d * t;
    if (p.x * d.x + p.z * d.z <= 0) continue;  // the panel's back, seen from outside
    const float s = std::atan2(p.x, -p.z) * radius;
    const float pu = (s - left) / (right - left), pv = (top - p.y) / (top - bottom);
    if (pu < u0 || pu > u1 || pv < v0 || pv > v1) continue;
    u = pu;
    v = pv;
    distance = t;
    return true;
  }
  return false;
}

void MenuPanels::placeInFrontOf(Vec3 headPosition, const Mat4& frame) {
  anchor = headPosition + transformDir(frame, {0.0f, 0.0f, mg::kAxisBehind});
  orientation = frame;
}

Vec3 MenuPanels::front() const {
  return anchor + transformDir(orientation, {0.0f, 0.0f, -mg::kRadius});
}

void MenuPanels::moveFront(Vec3 point, Vec3 headPosition, Vec3 up) {
  const Vec3 toHead = headPosition - point;
  Mat4 basis;
  if (!facingBasis(normalize(toHead), up, basis)) return;
  orientation = basis;
  anchor = point + transformDir(basis, {0.0f, 0.0f, mg::kRadius});
}

CurvedPanel MenuPanels::panel(const Menu& menu, int id) const {
  constexpr float ppm = mg::kPixelsPerMeter;
  CurvedPanel p;
  p.axis = anchor;
  p.orientation = orientation;
  p.radius = mg::kRadius - mg::kLayerStep;  // the strip lies on the page
  p.pixelScale = mg::kRasterScale;
  // The strip's body (below its room for open lists) ends at kStripBottom.
  const float bodyTop = mg::kStripBottom + Menu::kStripBodyHeight / ppm;
  const float stripTop = bodyTop + Menu::kStripBodyY / ppm;
  float width = 0, height = 0;
  if (id == Menu::kMain) {
    width = Menu::kMainWidth / ppm;
    height = Menu::kMainHeight / ppm;
    p.left = -width / 2;
    p.top = stripTop;
  } else if (id == Menu::kSetup) {
    // Its top 38 cm above the eyes; 16 cm higher while the keyboard is up
    // (under it, shown only then).
    width = Menu::kSetupWidth / ppm;
    height = (Menu::kSetupHeight + Menu::kSetupKeyboardHeight) / ppm;
    p.radius = mg::kRadius;
    p.left = -width / 2;
    p.top = menu.setupKeyboardShown() ? 0.542f : 0.382f;
  } else {
    width = Menu::kBrowserWidth / ppm;
    height = Menu::kBrowserHeight / ppm;
    p.radius = mg::kRadius;
    p.left = -Menu::kPageCenterX / ppm;  // the central page is straight ahead
    p.top = bodyTop + mg::kPageGap + height;
  }
  p.right = p.left + width;
  p.bottom = p.top - height;
  int first = 0, end = 0;
  menu.shownRows(id, first, end);
  p.v0 = first / (height * ppm);
  p.v1 = end / (height * ppm);
  menu.shownColumns(id, first, end);
  p.u0 = first / (width * ppm);
  p.u1 = end / (width * ppm);
  return p;
}
