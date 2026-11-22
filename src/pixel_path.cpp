#include "pixel_path.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

void describeEyeSampling(std::string& out, const VideoFormat& format, int frameWidth,
                         int frameHeight, float zoom, float screenWidthOverDistance,
                         float eyePixelsPerDegree, bool mipmaps) {
  const int eyeW = format.stereo == StereoLayout::SideBySide ? frameWidth / 2 : frameWidth;
  const int eyeH = format.stereo == StereoLayout::TopBottom ? frameHeight / 2 : frameHeight;
  // Source pixels across one degree of the picture, then of the view.
  float perDegree = 0;
  bool forcedLevel0 = false;
  switch (format.projection) {
    case Projection::Flat:
      // The same straight ahead for flat and curved screens.
      perDegree = screenWidthOverDistance > 0
                      ? eyeW / screenWidthOverDistance * 3.14159265f / 180.0f
                      : 0;
      zoom = 1;  // zoom only magnifies surround video
      break;
    case Projection::Equirect180: perDegree = eyeW / 180.0f; break;
    case Projection::Equirect360: perDegree = eyeW / 360.0f; break;
    case Projection::Fisheye180: perDegree = eyeW / 180.0f; break;
    case Projection::Fisheye190: perDegree = eyeW / 190.0f; break;
    case Projection::Fisheye200: perDegree = eyeW / 200.0f; break;
    case Projection::EquiAngularCubemap:
      perDegree = std::max(eyeW, eyeH) / 3 / 90.0f;  // three faces of 90° along the long side
      forcedLevel0 = true;
      break;
    default: break;
  }
  const float viewPerDegree = perDegree / std::max(zoom, 0.01f);
  char level[64];
  if (forcedLevel0)
    std::snprintf(level, sizeof(level), "mip level 0 (cube faces)");
  else if (eyePixelsPerDegree <= 0 || viewPerDegree <= 0)
    std::snprintf(level, sizeof(level), "mip level unknown");
  else if (viewPerDegree <= eyePixelsPerDegree)
    std::snprintf(level, sizeof(level), "mip level 0 (magnified %.2fx)",
                  eyePixelsPerDegree / viewPerDegree);
  else if (!mipmaps)
    std::snprintf(level, sizeof(level), "mip level 0 (mipmaps off, minified %.2fx)",
                  viewPerDegree / eyePixelsPerDegree);
  else
    std::snprintf(level, sizeof(level), "mip level ~%.2f",
                  std::log2(viewPerDegree / eyePixelsPerDegree));
  char where[32] = "on the screen";
  if (format.projection != Projection::Flat)
    std::snprintf(where, sizeof(where), "at zoom %.2f", zoom);
  char shape[48] = "";
  if (format.projection == Projection::Flat && frameHeight > 0)
    std::snprintf(shape, sizeof(shape), "%s, shown %.2f:1",
                  isHalfPacked(format) ? ", half packed" : "",
                  eyeDisplayAspect(format, static_cast<float>(frameWidth) / frameHeight));
  char line[320];
  std::snprintf(line, sizeof(line),
                "Per eye: %dx%d of %dx%d (%s, %s%s), %.1f px/° %s; eye image %.1f px/°: "
                "%s straight ahead",
                eyeW, eyeH, frameWidth, frameHeight, projectionName(format.projection),
                stereoName(format.stereo), shape, viewPerDegree, where, eyePixelsPerDegree, level);
  out.assign(line);
}

void SettledLine::update(double now) {
  if (line_ != pending_) {
    pending_ = line_;
    since_ = now;
  }
  if (pending_ != printed_ && now - since_ >= 1.0) {
    printed_ = pending_;
    std::printf("%s\n", printed_.c_str());
  }
}
