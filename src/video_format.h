#pragma once

#include <string>

// How the video maps onto the viewer's field of view.
// EquiAngularCubemap is the equi-angular cubemap (EAC) 360° format, as
// YouTube stores it: a 3x2 grid of cube faces (files tagged "_eac").
// The values are stored by number (the library's per-video settings): new
// ones go at the end.
enum class Projection { Flat, Equirect180, Equirect360, Fisheye190, Fisheye200, EquiAngularCubemap,
                        Fisheye180, kCount };

// The projections in the order the P key and the menu's tiles go through them.
constexpr Projection kProjectionOrder[] = {
    Projection::Flat,       Projection::Equirect180, Projection::Equirect360,
    Projection::Fisheye180, Projection::Fisheye190,  Projection::Fisheye200,
    Projection::EquiAngularCubemap};

// How the two eye images are packed into one video frame.
enum class StereoLayout { Mono, SideBySide, TopBottom, kCount };

// Flat 3D only: whether each eye was squeezed to half the width (side by
// side) or height (top-bottom) to fit a normal frame, as on 3D Blu-ray rips
// ("half SBS"), or keeps its full shape ("full SBS", as VR files tagged
// "_3dh"/"_LR" are). Half is only ever chosen by hand; Auto (never chosen)
// means full. 180°/360° video maps its eyes by angle and ignores this.
enum class Packing { Auto, Full, Half, kCount };

struct VideoFormat {
  Projection projection = Projection::Flat;
  StereoLayout stereo = StereoLayout::Mono;
  Packing packing = Packing::Auto;
};

const char* projectionName(Projection p);
const char* stereoName(StereoLayout s);
const char* packingName(Packing p);
Projection nextProjection(Projection p);
StereoLayout nextStereo(StereoLayout s);

// A two-view video (H.264 MVC: 3D Blu-ray discs and their MKV rips) as mpv
// decodes it: both views side by side in one frame, each at full size, or
// its base view alone (2D) where the views can't be decoded together.
enum class TwoViews { No, Both, BaseOnly };

// Guesses the format from common tags in the file name (e.g. "_180_SBS",
// "_360_TB", "FISHEYE180", "MKX200"), falling back to typical VR frame sizes.
// Two-view video is flat: full side by side, or 2D.
VideoFormat guessVideoFormat(const std::string& path, int width, int height,
                             TwoViews views = TwoViews::No);

// Whether a video is VR (or 360°) rather than flat 2D, without opening it,
// for lists that tell them apart (docs/stash.md). By its frame: 2:1 (aspect
// 1.95-2.05, at least 1920 wide) or square (0.98-1.02, at least 2160 wide);
// a size of 0 is not known.
bool isVrFrame(int width, int height);
// By its file name: one of the tags guessVideoFormat() reads ("180", "sbs",
// "lr", "fisheye190", "oculus", "vr", ...) as a whole word, the name split
// at spaces and _ - . [ ] ( ).
bool hasVrNameTag(const std::string& path);

// Whether a flat 3D video's eyes are squeezed to half size (chosen by hand).
bool isHalfPacked(const VideoFormat& format);

// The width/height one eye's picture is shown at on the flat screen.
float eyeDisplayAspect(const VideoFormat& format, float frameAspect);

// Whether the file name marks a passthrough video with a packed alpha mask
// ("_ALPHA", as DeoVR and SLR name them).
bool hasPackedAlpha(const std::string& path);

// Which of a packed mask's colours make it opaque: red (every mask), or red
// and blue (a mask whose blue second layer is shown).
enum class MaskColours { Red, RedBlue };

// Where passthrough shows the room through a video (seeThrough() in see_through.h).
struct SeeThrough {
  bool packedAlpha = false;  // DeoVR's packed alpha mask
  MaskColours maskColours = MaskColours::Red;  // ... read so
  float greenKey = 0;        // else a keyed green screen: the greenness cut away (0 = none)
};
