#pragma once

#include "scene_renderer.h"
#include "xr_context.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <optional>

// Showing the last drawn frame again instead of drawing one that would look
// the same (docs/vulkan-renderer.md, 12). A headset frame is due at every
// refresh, but the picture only changes with the video's frames: 60 fps
// video at 120 Hz changes at every other refresh. For the frames between,
// the runtime is handed the eye images it already has, with the poses they
// were drawn for, and reprojects them to where the head is now, as it does
// for every frame anyway. Nothing is acquired, recorded or submitted for them.
//
// A frame is shown again only while all of this holds, so that it cannot be
// told from a drawn one:
//  - what it shows is what the last drawn frame showed: the same picture,
//    drawn the same way, and nothing over it (menus, the pointer, subtitles,
//    messages and the rest are drawn every frame);
//  - the head has turned so little since that frame was drawn that
//    reprojection covers it (the view's edge moves by a few pixels at most),
//    and what reprojection does not follow (a flat screen as the eyes move,
//    the stereo correction with its motion and focus distance, a zoomed
//    view as the head turns) moved by less than a pixel;
//  - the gaze is still well inside the sharp area of that frame's density map;
//  - the frame was drawn no longer ago than kMaxAge, whatever else holds.
//
// On by default on the Steam Frame, where the setting Skip repeated frames
// (Settings::frameReuse) turns it off; elsewhere off unless --frame-reuse=on
// (main.cpp).
class FrameReuse {
 public:
  // Longer ago than this, a frame is drawn again: 30 drawn frames a second
  // at least. It bounds how late a change this comparison does not see can
  // show. Drawing less often costs more than it saves on the Steam Frame:
  // at 18 frames a second (a paused video with 50 ms here) its GPU's clock
  // falls and rises between draws and one drawn frame in ten came late.
  static constexpr double kMaxAge = 0.030;

  // What a frame's eye images depend on.
  struct Frame {
    const FrameScene* scene = nullptr;
    uint64_t picture = 0;      // VideoPlayer::pictureVersion()
    bool alphaBlend = false;   // XrContext::setAlphaBlend
    double displayTime = 0;    // XrContext::displayTime()
    int eyeCount = 0;
    const EyeMatrices* eyes = nullptr;
    uint64_t targets = 0;      // XrContext::eyeTargetsVersion()
    // The density maps: Foveation::mapsVersion(), and per eye offset() and
    // sharpSize(). known: the maps are the app's, or there are none.
    bool mapsKnown = false;
    uint64_t maps = 0;
    VkOffset2D mapOffset[2] = {};
    VkExtent2D sharp[2] = {};
  };

  // Why a frame is drawn, or Same: the last drawn frame does as well.
  enum Reason {
    Same,
    Off,       // not enabled, or no drawn frame to show again (none yet, or one with overlays)
    Overlays,  // something is drawn over the picture (or it is a photo, or the foveation view)
    Picture,   // another video frame
    Scene,     // the picture is drawn another way, or onto other eye images
    Focus,     // the stereo correction moved the picture (the focus distance, the eyes' places)
    View,      // the eyes' fields of view changed
    Turn,      // the head turned too far
    Move,      // an eye moved too far for a flat screen
    Gaze,      // the gaze moved too far from where the sharp area was drawn
    Age,       // drawn longer ago than kMaxAge
    kReasons
  };
  static const char* reasonName(Reason reason);

  void setEnabled(bool enabled) { enabled_ = enabled; }
  bool enabled() const { return enabled_; }
  // Same if showing the last drawn frame again cannot be told from drawing
  // `frame`, else the first reason found to draw it.
  Reason compare(const Frame& frame) const;
  // `frame` was drawn and submitted. Whether the runtime still has its eye
  // images is XrContext::canShowAgain()'s to say.
  void drawn(const Frame& frame);
  // The frame drawn last was not what its scene describes: none to show again.
  void forget() { valid_ = false; }

 private:
  bool enabled_ = false;
  bool valid_ = false;
  ClearMode clear_ = ClearMode::Dark;
  bool sky_ = false;
  std::optional<VideoDraw> video_;
  uint64_t picture_ = 0;
  bool alphaBlend_ = false;
  double displayTime_ = 0;
  int eyeCount_ = 0;
  EyeMatrices eyes_[2];
  uint64_t targets_ = 0;
  uint64_t maps_ = 0;
  VkOffset2D mapOffset_[2] = {};
};
