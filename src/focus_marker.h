#pragma once

#include "cairo_canvas.h"
#include "scene_renderer.h"
#include "xmath.h"

#include <string>
#include <vector>

// The focus target in the view: a ring where the focus measures, placed at
// the corrected distance (so it sits on what is lined up), with the corrected
// distance at its centre. The ring is the size of the head target's measured
// patch, larger on things nearer than a meter so the reading stays legible.
class FocusMarker {
 public:
  bool init(Uploader& uploader);
  // This frame's placement; hidden unless `show`. direction: unit, local space.
  void update(bool show, Vec3 headCenter, Quat head, Vec3 direction, float distance,
              const std::string& distanceText);
  // What is shown this frame, added to the scene's overlays.
  void addTo(std::vector<OverlayDraw>& overlays);

 private:
  void drawDistance();
  CairoCanvas ring_;
  std::string distanceText_;
  bool show_ = false;
  Mat4 ringModel_;
};
