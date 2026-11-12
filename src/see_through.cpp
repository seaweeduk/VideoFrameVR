#include "see_through.h"

#include "video_check.h"

#include <cmath>

bool packedMask(const std::string& path, const VideoSettings& video) {
  return video.packedMask.value_or(hasPackedAlpha(path));
}

bool maskFormat(const VideoFormat& format) {
  return format.stereo == StereoLayout::SideBySide &&
         (format.projection == Projection::Fisheye180 ||
          format.projection == Projection::Fisheye190 ||
          format.projection == Projection::Fisheye200);
}

namespace {

bool maskShown(bool alphaPacked, const VideoFormat& format) {
  return alphaPacked && maskFormat(format);
}

}  // namespace

MaskColours maskColours(const VideoSettings& video) {
  return video.maskLayersFound && !video.secondLayerHidden ? MaskColours::RedBlue
                                                           : MaskColours::Red;
}

SeeThrough seeThrough(bool passthrough, bool alphaPacked, const VideoFormat& format,
                      const VideoSettings& video, bool tvGamma, bool picking) {
  SeeThrough s;
  if (!passthrough) return s;
  s.packedAlpha = maskShown(alphaPacked, format);
  if (s.packedAlpha) s.maskColours = maskColours(video);
  if (s.packedAlpha || !video.greenScreen.on) return s;
  if (picking) {
    s.picking = true;
    return s;
  }
  if (!video.keyRemoves()) return s;
  const KeySliders& key = video.keySliders();
  if (!video.colourKeyed()) {
    s.greenKey = 0.55f - 0.45f * key.strength / 100.0f;
    return s;
  }
  // Fine near 0, where exact backgrounds (black) need a cut of a level or two.
  const float strength = key.strength / 100.0f;
  s.colourKey = true;
  s.keyCut = 0.5f * strength * strength;
  s.keySoft = 0.002f + 0.3f * key.softness / 100.0f;
  const KeyColour c = video.keyColour();
  s.keyColour[0] = c.r / 255.0f;
  s.keyColour[1] = c.g / 255.0f;
  s.keyColour[2] = c.b / 255.0f;
  // As mpv converts the picture, BT.1886's 2.4 power to sRGB.
  if (tvGamma)
    for (float& e : s.keyColour) {
      const float l = std::pow(e, 2.4f);
      e = l <= 0.0031308f ? 12.92f * l : 1.055f * std::pow(l, 1 / 2.4f) - 0.055f;
    }
  return s;
}

AutoPassthrough autoPassthrough(const std::string& path, const VideoFormat& format,
                                const VideoSettings& video) {
  if (video.passthroughChosen) return AutoPassthrough::None;
  if (maskShown(packedMask(path, video), format)) return AutoPassthrough::Mask;
  if (video.fillCheckVersion > 0 && video.fillFound) return AutoPassthrough::Fill;
  if (video.greenCheckVersion > 0 && video.greenScreenFound) return AutoPassthrough::GreenScreen;
  return AutoPassthrough::None;
}

bool greenCheckWanted(const std::string& path, const VideoFormat& format,
                      const VideoSettings& video) {
  return !video.passthroughChosen && !maskShown(packedMask(path, video), format) &&
         video.greenCheckVersion < VideoCheck::kGreenVersion;
}

bool fillCheckWanted(const std::string& path, const VideoFormat& format,
                     const VideoSettings& video) {
  return (video.passthrough || !video.passthroughChosen) &&
         !maskShown(packedMask(path, video), format) &&
         video.fillCheckVersion < VideoCheck::kFillVersion;
}

bool layerCheckWanted(const std::string& path, const VideoFormat& format,
                      const VideoSettings& video) {
  return maskShown(packedMask(path, video), format) && !video.maskLayersFound &&
         video.maskLayersVersion < VideoCheck::kLayersVersion;
}
