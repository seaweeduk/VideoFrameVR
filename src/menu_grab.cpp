#include "menu_grab.h"

#include "vr_space.h"

void MenuGrab::update(const ControllerState& input, const Frame& frame, MenuPanels& panel) {
  // Grip on the menu: it follows the hand and keeps facing the viewer.
  if (hand_ >= 0 && !input.hands[hand_].grip) hand_ = -1;
  for (int h = 0; h < 2 && hand_ < 0 && !frame.screenHeld && frame.headValid; ++h) {
    const HandState& g = input.hands[h];
    if (!g.gripPressed || !frame.onMenu[h]) continue;
    hand_ = h;
    startPoint_ = panel.front();
    startInverse_ = poseInverse(g.aimPosition, g.aimOrientation);
    buzz(h, 0.25f, 0.012);
  }
  if (hand_ >= 0 && input.hands[hand_].poseValid && frame.headValid) {
    const HandState& g = input.hands[hand_];
    panel.moveFront(transformPoint(poseMatrix(g.aimPosition, g.aimOrientation) * startInverse_,
                                   startPoint_),
                    frame.headPos, frame.up);
  }
}
