#include "panel_fades.h"

#include "menu.h"
#include "menu_geometry.h"
#include "seek_preview.h"
#include "setup_intro.h"
#include "setup_placement.h"
#include "vr_input.h"
#include "vr_space.h"

namespace {

// The pointer beam starts 1 m long; pointing at no panel, it
// keeps the length of its last hit. Its dot is 20 menu layout units wide.
constexpr float kLaserStartLength = 1.0f;  // meters
constexpr float kLaserDotRadius = 10 * menu_geometry::kUnit;

}  // namespace

PanelFades::PanelFades(Menu& menu, MenuPanels& panel, SeekPreview& seekPreview,
                       SetupPlacement& setupPlacement, SetupIntro& setupIntro)
    : menu(menu), panel(panel), seekPreview(seekPreview), setupPlacement(setupPlacement),
      setupIntro(setupIntro), laserLength_(kLaserStartLength) {}

void PanelFades::pointerMoved() { laserLength_ = kLaserStartLength; }

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

void PanelFades::addTo(std::vector<OverlayDraw>& overlays, const Frame& frame) {
  const double now = frame.now, easedAt = frame.easedAt;
  const HandState& hand = *frame.hand;
  // The dot at the laser's end: tighter while the trigger is held.
  auto dotLook = [&](LaserItem& laser) {
    const MenuFeel& feel = menuFeel();
    dotTight.to(hand.select ? 1.0f : 0.0f, feel.dotEase, now);
    const float tight = dotTight.update(easedAt);
    laser.dotRadius = kLaserDotRadius * (1 + (static_cast<float>(feel.dotPressed) - 1) * tight);
  };
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
    if (frame.pointing) {
      LaserItem laser;
      laser.from = frame.rayOrigin;
      laser.to = frame.hitPanel ? frame.hitPoint : frame.rayOrigin + frame.rayDir * laserLength_;
      dotLook(laser);
      laser.opacity = menuOpacity;
      overlays.push_back(laser);
    } else if (!panel.open && menuOpacity > 0 && hand.poseValid && !setupPlacement.fading()) {
      LaserItem laser;  // where the hand points now
      laser.from = hand.aimPosition;
      laser.to = laser.from + aimDirection(hand) * laserLength_;
      dotLook(laser);
      laser.opacity = menuOpacity;
      overlays.push_back(laser);
    }
  } else if (heldStrip) {
    // A held seek with the menu closed: the strip alone, no pointer.
    PanelItem item;
    item.texture = menu.texture(Menu::kMain);
    item.curved = panel.panel(menu, Menu::kMain);
    overlays.push_back(item);
  }
}
