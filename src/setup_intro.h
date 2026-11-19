#pragma once

#include "menu.h"
#include "scene_renderer.h"
#include "xmath.h"

#include <vector>

// Welcome's intro (docs/alpha/first-run.md, step 1), about 4 s, as the
// prototype times it: the sky fades in, the panel appears, the icon builds
// itself (the two circles slide together, their overlap lights, the play
// button pops in, then the tile), then the name, then the rest of the page.
// Its layers are drawn once (Menu::setupIntroPiece()), before it starts;
// each frame only places and fades them, on the GPU. Any button skips it.
class SetupIntro {
 public:
  void start(double now) { start_ = now; }
  void skip(double now);
  bool started() const { return start_ >= 0; }
  bool playing(double now) const;
  // The page (Menu::kSetup's texture), which fades in last.
  float pageOpacity(double now) const;
  // Under the page: the empty panel, the icon's parts and the name, on
  // `panel` (the setup panel's), `opacity` the panel's own.
  void addUnder(std::vector<OverlayDraw>& overlays, Menu& menu, const CurvedPanel& panel,
                float opacity, double now) const;
  // Over everything, last: black, fading as the sky comes in; all black
  // before it starts.
  void addCover(std::vector<OverlayDraw>& overlays, Menu& menu, Vec3 headPos, Quat headRot,
                double now) const;

 private:
  double start_ = -1;
};
