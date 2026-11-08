#include "see_through.h"

#include "video_check.h"

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
                      const GreenScreen& key, MaskColours colours) {
  SeeThrough s;
  if (!passthrough) return s;
  s.packedAlpha = maskShown(alphaPacked, format);
  if (s.packedAlpha) s.maskColours = colours;
  if (!s.packedAlpha && key.on) s.greenKey = 0.55f - 0.45f * key.strength / 100.0f;
  return s;
}

AutoPassthrough autoPassthrough(const std::string& path, const VideoFormat& format,
                                const VideoSettings& video) {
  if (video.passthroughChosen) return AutoPassthrough::None;
  if (maskShown(packedMask(path, video), format)) return AutoPassthrough::Mask;
  if (video.greenCheckVersion > 0 && video.greenScreenFound) return AutoPassthrough::GreenScreen;
  return AutoPassthrough::None;
}

bool greenCheckWanted(const std::string& path, const VideoFormat& format,
                      const VideoSettings& video) {
  return !video.passthroughChosen && !maskShown(packedMask(path, video), format) &&
         video.greenCheckVersion < VideoCheck::kGreenVersion;
}

bool layerCheckWanted(const std::string& path, const VideoFormat& format,
                      const VideoSettings& video) {
  return maskShown(packedMask(path, video), format) && !video.maskLayersFound &&
         video.maskLayersVersion < VideoCheck::kLayersVersion;
}
