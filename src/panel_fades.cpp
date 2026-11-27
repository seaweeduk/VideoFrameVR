#include "panel_fades.h"

#include "menu.h"
#include "seek_preview.h"
#include "setup_intro.h"
#include "setup_placement.h"
#include "vr_input.h"
#include "vr_space.h"

namespace {

constexpr float kFreeBeamLength = 0.75f;  // meters
constexpr float kTargetBeamFraction = 0.85f;  // leave the last 15% clear before the dot
constexpr float kPickerStartDistance = 1.0f;  // until the first panel hit
constexpr float kDotRadiusPerMeter = 0.00698f;  // 0.8° across, however far from the eye

}  // namespace

PanelFades::PanelFades(Menu& menu, MenuPanels& panel, SeekPreview& seekPreview,
                       SetupPlacement& setupPlacement, SetupIntro& setupIntro)
    : menu(menu), panel(panel), seekPreview(seekPreview), setupPlacement(setupPlacement),
      setupIntro(setupIntro), laserLength_(kPickerStartDistance) {}

void PanelFades::pointerMoved() { laserLength_ = kPickerStartDistance; }

void PanelFades::update(bool seekStrip) {
  panelOn[Menu::kMain] = panel.open && menu.panelShown(Menu::kMain);
  panelOn[Menu::kBrowser] = panel.open && menu.panelShown(Menu::kBrowser);
  heldStrip = !panel.open && seekStrip;
  if (heldStrip) {
    menuFade.hide();
    for (PanelFade& f : panelFades) {
      f.fade.hide();
      f.shown = false;
    }
  }
}

bool PanelFades::stripFading() const {
  return !panelOn[Menu::kMain] && panelFades[Menu::kMain].fade.drawn();
}

void PanelFades::addPointer(std::vector<OverlayDraw>& overlays, const Frame& frame, float opacity) {
  const HandState& hand = *frame.hand;
  if (!frame.pointing && (panel.open || opacity <= 0 || !hand.poseValid || setupPlacement.fading()))
    return;

  LaserItem laser;
  laser.opacity = opacity;
  laser.from = frame.pointing ? frame.rayOrigin : hand.aimPosition;
  const Vec3 dir = frame.pointing ? frame.rayDir : aimDirection(hand);
  laser.to = laser.from + dir * kFreeBeamLength;
  const MenuFeel& feel = menuFeel();
  dotTight.to(hand.select ? 1.0f : 0.0f, feel.dotEase, frame.now);
  const float tight = dotTight.update(frame.easedAt);
  if (frame.pointing && (frame.hitPanel || menu.pickingColor())) {
    laser.dot = frame.hitPanel ? frame.hitPoint : laser.from + dir * laserLength_;
    laser.to = laser.from + (laser.dot - laser.from) * kTargetBeamFraction;
    laser.dotRadius = kDotRadiusPerMeter * (1 + (static_cast<float>(feel.dotPressed) - 1) * tight);
  }
  overlays.push_back(laser);
}

void PanelFades::addTo(std::vector<OverlayDraw>& overlays, const Frame& frame) {
  const double now = frame.now, easedAt = frame.easedAt;
  // The menu's panels: the strip and the browser each fade in as they
  // start being shown and out as they stop (menu_feel.h), whatever the
  // cause (the menu opening or closing, the browser coming or going in
  // it); the laser with the menu. Input is never held back: a panel
  // takes none from the frame it stops being shown, and is drawn as it
  // was last until it has faded, with nothing pointed. Setup's panel
  // fades as it is placed.
  const float menuOpacity = menuFade.update(panel.open, now, easedAt);
  if (panel.open || menuOpacity > 0 || panelFades[Menu::kMain].fade.drawn() ||
      panelFades[Menu::kBrowser].fade.drawn()) {
    // The textures first: a click may have changed the page and with it
    // what the panels show; then the pointed control eases on them.
    const Texture* textures[3] = {};
    if (panel.open) {
      for (const Menu::PanelId id : {Menu::kBrowser, Menu::kMain, Menu::kSetup})
        if (menu.panelShown(id)) textures[id] = menu.texture(id);
      menu.easeHover(now, easedAt);
    }
    float opacity[2] = {};
    for (const Menu::PanelId id : {Menu::kBrowser, Menu::kMain}) {
      PanelFade& f = panelFades[id];
      // The browser coming starts fading in once the page it opens on
      // can be drawn (it is drawn a band of rows a frame); a page
      // changed while it is shown doesn't fade.
      if (id == Menu::kBrowser && panelOn[id] && !f.shown) f.waiting = true;
      if (f.waiting) f.waiting = panelOn[id] && !menu.browserReady();
      f.shown = panelOn[id];
      opacity[id] = f.fade.update(panelOn[id] && !f.waiting, now, easedAt);
      if (panelOn[id]) {  // as it is now, kept for fading out
        f.count = 0;
        PanelItem& item = f.drawn[0];
        item.texture = textures[id];
        item.curved = panel.panel(menu, id);
        item.hover.reset();
        // A value held: the strip fades away (the browser in its shader).
        item.opacity = id == Menu::kMain ? static_cast<float>(1 - menu.heldFade()) : 1.0f;
        if (item.opacity <= 0) continue;
        PanelHover hover;
        if (menu.hoverLook(id, hover)) item.hover = hover;
        f.count = 1;
        // A list's faded edges, over it.
        for (int edge = 0; id == Menu::kBrowser && edge < 2; ++edge) {
          PanelItem& fade = f.drawn[f.count];
          CurvedPanel part;
          if (!item.texture || !menu.listFade(edge, *item.curved, part, fade.texture)) continue;
          fade.curved = part;
          ++f.count;
        }
      }
      for (int i = 0; i < f.count && opacity[id] > 0; ++i) {
        overlays.push_back(f.drawn[i]);
        PanelItem& item = std::get<PanelItem>(overlays.back());
        item.opacity *= opacity[id];
        // Fading out, nothing is pointed on it (but a held value's fade).
        if (!panelOn[id] && item.hover && !(item.hover->fade > 0)) item.hover.reset();
      }
    }
    if (panel.open && menu.panelShown(Menu::kSetup)) {
      PanelItem item;
      item.texture = textures[Menu::kSetup];
      item.curved = panel.panel(menu, Menu::kSetup);
      item.opacity = setupPlacement.opacity();
      if (setupIntro.playing(now)) {  // its layers under the page, which fades in last
        setupIntro.addUnder(overlays, menu, *item.curved, item.opacity, now);
        item.opacity *= setupIntro.pageOpacity(now);
      }
      PanelHover hover;
      if (menu.hoverLook(Menu::kSetup, hover)) item.hover = hover;
      overlays.push_back(item);
    }
    if (opacity[Menu::kMain] > 0) seekPreview.addTo(overlays, opacity[Menu::kMain]);  // with the strip
    addPointer(overlays, frame, menuOpacity);
  } else if (heldStrip) {
    // A held seek with the menu closed: the strip alone, no pointer.
    PanelItem item;
    item.texture = menu.texture(Menu::kMain);
    item.curved = panel.panel(menu, Menu::kMain);
    overlays.push_back(item);
  }
}
