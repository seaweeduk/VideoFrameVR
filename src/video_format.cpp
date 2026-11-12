#include "video_format.h"

#include "bluray.h"
#include "file_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace {

bool containsAny(const std::string& s, std::initializer_list<const char*> tags) {
  for (const char* tag : tags)
    if (s.find(tag) != std::string::npos) return true;
  return false;
}

bool alnumAt(const std::string& s, size_t i) {
  return i < s.size() && std::isalnum(static_cast<unsigned char>(s[i]));
}

// A tag with no letter or digit right before it (`open` Before) and/or after
// it (`open` After): "_ou" but not "_out", "3dv" but not "r3dvr".
enum Open { Before = 1, After = 2 };
bool containsBounded(const std::string& s, std::initializer_list<const char*> tags, int open) {
  for (const char* tag : tags) {
    const size_t n = std::char_traits<char>::length(tag);
    for (size_t at = s.find(tag); at != std::string::npos; at = s.find(tag, at + 1)) {
      const bool before = !(open & Before) || at == 0 || !alnumAt(s, at - 1);
      const bool after = !(open & After) || !alnumAt(s, at + n);
      if (before && after) return true;
    }
  }
  return false;
}

// A tag standing on its own ("_F180_", not inside "baf180f3"), for short tags
// that also turn up in hashes and words.
bool containsToken(const std::string& s, std::initializer_list<const char*> tags) {
  return containsBounded(s, tags, Before | After);
}

// An angle as a number of its own: "_360_", "vr180", "180x180", but not
// inside a larger one ("3600p", "3360p", a scene number "18034") or a
// resolution ("360p").
bool containsAngle(const std::string& s, const char* angle) {
  const size_t n = std::char_traits<char>::length(angle);
  auto digit = [&](size_t i) { return i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])); };
  for (size_t at = s.find(angle); at != std::string::npos; at = s.find(angle, at + 1)) {
    if ((at > 0 && digit(at - 1)) || digit(at + n)) continue;
    if (at + n < s.size() && s[at + n] == 'p' && !alnumAt(s, at + n + 1)) continue;
    return true;
  }
  return false;
}

}  // namespace

const char* projectionName(Projection p) {
  switch (p) {
    case Projection::Flat: return "flat screen";
    case Projection::Equirect180: return "180";
    case Projection::Equirect360: return "360";
    case Projection::Fisheye180: return "fisheye 180";
    case Projection::Fisheye190: return "fisheye 190";
    case Projection::Fisheye200: return "fisheye 200";
    case Projection::EquiAngularCubemap: return "360 cubemap (EAC)";
    default: return "?";
  }
}

const char* stereoName(StereoLayout s) {
  switch (s) {
    case StereoLayout::Mono: return "mono";
    case StereoLayout::SideBySide: return "3D side-by-side";
    case StereoLayout::TopBottom: return "3D top-bottom";
    default: return "?";
  }
}

Projection nextProjection(Projection p) {
  constexpr int n = sizeof(kProjectionOrder) / sizeof(kProjectionOrder[0]);
  for (int i = 0; i < n; ++i)
    if (kProjectionOrder[i] == p) return kProjectionOrder[(i + 1) % n];
  return Projection::Flat;
}

const char* packingName(Packing p) {
  switch (p) {
    case Packing::Auto: return "auto";
    case Packing::Full: return "full";
    case Packing::Half: return "half";
    default: return "?";
  }
}

StereoLayout nextStereo(StereoLayout s) {
  return static_cast<StereoLayout>((static_cast<int>(s) + 1) %
                                   static_cast<int>(StereoLayout::kCount));
}

VideoFormat guessVideoFormat(const std::string& path, int width, int height, TwoViews views) {
  // Its name and size tell nothing then.
  if (views != TwoViews::No) {
    VideoFormat f;
    if (views == TwoViews::Both) {
      f.stereo = StereoLayout::SideBySide;
      f.packing = Packing::Full;
    }
    return f;
  }
  // A disc image is flat; a 3D title shows in 3D once mpv says it decodes
  // both views (above).
  if (bdmv::isDiscImage(path)) return {};
  std::string name = path.substr(path.find_last_of('/') + 1);
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  VideoFormat f;
  bool projectionTagged = true;
  if (containsAny(name, {"_eac", "-eac", ".eac"})) {
    f.projection = Projection::EquiAngularCubemap;
  } else if (containsAny(name, {"mkx200", "fisheye200", "fisheye_200"})) {
    f.projection = Projection::Fisheye200;
  } else if (containsAny(name, {"fisheye190", "fisheye_190", "fisheye-190", "fisheye 190"}) ||
             containsToken(name, {"rf52"})) {
    f.projection = Projection::Fisheye190;  // RF52: Canon's dual fisheye lens
  } else if (containsAny(name, {"fisheye180", "fisheye_180", "fisheye-180", "fisheye 180"}) ||
             containsToken(name, {"f180", "180f"})) {
    f.projection = Projection::Fisheye180;
  } else if (size_t at = name.find("fisheye"); at != std::string::npos) {
    // A bare "_fisheye" is 180° (as DeoVR reads it); other angles are not
    // known and keep being read as 190°.
    at += 7;
    if (at < name.size() && (name[at] == '_' || name[at] == '-' || name[at] == ' ')) ++at;
    const bool angle = at < name.size() && std::isdigit(static_cast<unsigned char>(name[at]));
    f.projection = angle ? Projection::Fisheye190 : Projection::Fisheye180;
  } else if (containsAngle(name, "360")) {
    f.projection = Projection::Equirect360;
  } else if (containsAngle(name, "180")) {
    f.projection = Projection::Equirect180;
  } else {
    projectionTagged = false;
  }

  // Short tags only where they are not the start of a word ("_out"), nor
  // its end ("r3dvr").
  bool stereoTagged = true;
  if (containsAny(name, {"overunder", "over_under", "topbottom", "top_bottom"}) ||
      containsBounded(name, {"_tb", "-tb", "_ou"}, After) || containsBounded(name, {"3dv"}, Before)) {
    f.stereo = StereoLayout::TopBottom;
  } else if (containsAny(name, {"sbs", "_lr", "-lr", "side_by_side", "sidebyside"}) ||
             containsBounded(name, {"3dh"}, Before) || containsToken(name, {"lr"})) {
    f.stereo = StereoLayout::SideBySide;
  } else {
    stereoTagged = false;
  }

  // Fisheye video is side by side unless tagged otherwise (SLR's
  // "_FISHEYE190" files carry no stereo tag).
  const bool fisheye = f.projection == Projection::Fisheye180 ||
                       f.projection == Projection::Fisheye190 ||
                       f.projection == Projection::Fisheye200;
  if (fisheye && !stereoTagged && width > 0 && height > 0 &&
      std::abs(static_cast<double>(width) / height - 2.0) < 0.02)
    f.stereo = StereoLayout::SideBySide;

  const double aspect = height > 0 ? static_cast<double>(width) / height : 0;
  const bool twoToOne = std::abs(aspect - 2.0) < 0.02, square = std::abs(aspect - 1.0) < 0.02;

  // A stereo tag alone, in a VR-sized frame: side by side about 2:1 has
  // square eyes (a little taller in SLR's 4320p files), as 180° video has;
  // SLR names its 180° files just "_LR". Top-bottom 1:1 has 2:1 eyes, as
  // 360° video has. And 180° video in a 2:1 frame is side by side (alone
  // it would be squeezed to half its width).
  if (!projectionTagged && width >= 1920) {
    if (f.stereo == StereoLayout::SideBySide && aspect >= 1.85 && aspect < 2.05)
      f.projection = Projection::Equirect180;
    else if (f.stereo == StereoLayout::TopBottom && square)
      f.projection = Projection::Equirect360;
  }
  if (f.projection == Projection::Equirect180 && !stereoTagged && twoToOne)
    f.stereo = StereoLayout::SideBySide;

  // Untagged files: big 2:1 videos are almost always VR180 side-by-side,
  // big 2:1 photos 360° panoramas (as 360° cameras and phones save them),
  // big 1:1 frames 360 top-bottom. Named for a headset, smaller ones too.
  const int big = containsToken(name, {"oculus", "oculusrift", "gearvr", "psvr"}) ? 1920 : 2560;
  if (!projectionTagged && !stereoTagged && width >= big) {
    if (twoToOne && isPhotoFile(path)) {
      f.projection = Projection::Equirect360;
    } else if (twoToOne) {
      f.projection = Projection::Equirect180;
      f.stereo = StereoLayout::SideBySide;
    } else if (square) {
      f.projection = Projection::Equirect360;
      f.stereo = StereoLayout::TopBottom;
    }
  }
  return f;
}

bool isVrFrame(int width, int height) {
  if (width <= 0 || height <= 0) return false;
  const double aspect = static_cast<double>(width) / height;
  return (aspect >= 1.95 && aspect <= 2.05 && width >= 1920) ||
         (aspect >= 0.98 && aspect <= 1.02 && width >= 2160);
}

bool hasVrNameTag(const std::string& path) {
  static const char* const kTags[] = {
      "180",       "190",        "200",        "360",        "180x180", "3dh",   "3dv",
      "lr",        "rl",         "sbs",        "tb",         "ou",      "oculus", "oculusrift",
      "gearvr",    "psvr",       "vr",         "mkx200",     "fisheye", "fisheye180",
      "fisheye190", "fisheye200", "rf52",      "eac"};
  const std::string name = lowerCase(path.substr(path.find_last_of('/') + 1));
  size_t start = 0;
  for (size_t i = 0; i <= name.size(); ++i) {
    const bool end = i == name.size() || std::strchr(" _-.[]()", name[i]) != nullptr;
    if (!end) continue;
    if (i > start)
      for (const char* tag : kTags)
        if (name.compare(start, i - start, tag) == 0) return true;
    start = i + 1;
  }
  return false;
}

bool isHalfPacked(const VideoFormat& format) {
  return format.projection == Projection::Flat && format.stereo != StereoLayout::Mono &&
         format.packing == Packing::Half;
}

float eyeDisplayAspect(const VideoFormat& format, float frameAspect) {
  // Half packing squeezed each eye to the frame's size: it has the frame's shape.
  if (isHalfPacked(format)) return frameAspect;
  if (format.stereo == StereoLayout::SideBySide) return frameAspect * 0.5f;
  if (format.stereo == StereoLayout::TopBottom) return frameAspect * 2.0f;
  return frameAspect;
}

bool hasPackedAlpha(const std::string& path) {
  std::string name = path.substr(path.find_last_of('/') + 1);
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (const char* tag : {"_alpha", "-alpha", " alpha"}) {
    const size_t at = name.find(tag);
    if (at == std::string::npos) continue;
    const char after = at + 6 < name.size() ? name[at + 6] : '.';
    if (!std::isalnum(static_cast<unsigned char>(after))) return true;  // not "_alphabet"
  }
  return false;
}

float SeeThrough::packedKeyColour() const {
  // 0.5: the colour's 24 bits take its mantissa and the exponent's lowest
  // bit, so it stays a normal float (0.5..2).
  uint32_t packed = 0x3f000000u;
  for (int c = 0; c < 3; ++c) {
    const float value = std::clamp(keyColour[c], 0.0f, 1.0f);
    packed |= static_cast<uint32_t>(std::lround(value * 255.0f)) << (8 * c);
  }
  float f;
  std::memcpy(&f, &packed, sizeof(f));
  return f;
}
