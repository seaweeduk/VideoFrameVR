#pragma once

#include "cairo_canvas.h"
#include "scene_renderer.h"
#include "xmath.h"

#include <string>
#include <vector>

// Autofocus diagnostics in the view: a ring where the focus measures, placed
// at the corrected distance (so it sits on what is lined up), with the
// corrected distance at its centre. The rest of the focus state is in the
// menu: text under the ring moved with the gaze of whoever read it.
// The ring is the size of the head target's measured patch, larger on things
// nearer than a meter so the reading stays legible.
class FocusMarker {
 public:
  bool init(Uploader& uploader);
  // This frame's placement; hidden unless `show`. direction: unit, local space.
  void update(bool show, Vec3 headCenter, Quat head, Vec3 direction, float distance,
              const std::string& distanceText);
  // A small dot where the eyes look (the focus target's corrected, smoothed
  // gaze), for checking eye tracking. Hidden unless `show`. direction: unit,
  // local space. At the focus distance, so it fuses with the diagnostic marker.
  void updateGaze(bool show, Vec3 origin, Vec3 direction, float distance);
  // What is shown this frame, added to the scene's overlays: the dot and the
  // ring.
  void addTo(std::vector<OverlayDraw>& overlays);

 private:
  void drawDistance();
  CairoCanvas ring_, dot_;
  std::string distanceText_;
  bool show_ = false, showDot_ = false;
  Mat4 ringModel_, dotModel_;
};
