#pragma once

#include "controller_state.h"
#include "menu_feel.h"
#include "scene_renderer.h"
#include "xmath.h"

#include <array>
#include <vector>

class Menu;
struct MenuPanels;
class SeekPreview;
class SetupIntro;
class SetupPlacement;

// The menu's panels as drawn: the strip and the browser each fading in and
// out, setup's panel, the seek preview with the strip and the pointer's
// laser; or, with the menu closed, a held seek's strip alone.
class PanelFades {
 public:
  // What else is going on this frame.
  struct Frame {
    double now = 0, easedAt = 0;
    const HandState* hand = nullptr;  // the pointer's
    bool pointing = false;            // at the panels (the menu open), along the ray
    Vec3 rayOrigin, rayDir;
    bool hitPanel = false;  // ... which hits one here
    Vec3 hitPoint;
  };

  PanelFades(Menu& menu, MenuPanels& panel, SeekPreview& seekPreview, SetupPlacement& setupPlacement,
             SetupIntro& setupIntro);

  // The other hand points now: its laser starts at its first length.
  void pointerMoved();
  // The ray hit a panel this far along; pointing at none, the laser keeps it.
  void hit(float distance) { laserLength_ = distance; }
  float laserLength() const { return laserLength_; }
  // The menu opened: the dot starts loose.
  void menuOpened() { dotTight.set(0); }

  // Each frame before drawing: the panels shown now; `seekStrip`: a held
  // seek's strip shows (StickSeek::stripShown()).
  void update(bool seekStrip);
  // The strip has stopped being shown and fades out (the seek preview with it).
  bool stripFading() const;
  // Drawn over the video, after the readout and before the focus markers.
  void addTo(std::vector<OverlayDraw>& overlays, const Frame& frame);

 private:
  // The strip or the browser fading in as it starts being shown and out as it
  // stops (menu_feel.h): drawn as it was last shown until it has faded.
  struct PanelFade {
    ShownFade fade;
    std::array<PanelItem, 3> drawn;  // the panel, then its list's faded edges
    int count = 0;
    bool shown = false;    // last frame
    bool waiting = false;  // shown, not yet fading in: its page isn't drawn yet
  };

  Menu& menu;
  MenuPanels& panel;
  SeekPreview& seekPreview;
  SetupPlacement& setupPlacement;
  SetupIntro& setupIntro;

  float laserLength_;
  ShownFade menuFade;  // the laser's, as the menu opens and closes
  PanelFade panelFades[2];  // the strip's and the browser's (Menu::kMain, Menu::kBrowser)
  Ease dotTight;  // how far the pointer's dot has tightened
  // This frame: the strip and the browser shown; a held seek's strip with the
  // menu closed, in place of the menu's (which goes at once).
  bool panelOn[2] = {false, false};
  bool heldStrip = false;
};
