#pragma once

#include <cstdint>

// The menu control under the pointer, drawn pointed or pressed by the panel
// shader over the canvas, which shows it at rest; or the menu fading away
// while a value is held (Menu::hoverLook()).
struct PanelHover {
  float rect[4] = {0, 0, 0, 0};  // x0, y0, x1, y1 in layout pixels
  float radius = 14;             // corner radius; half the size for a round button
  float clip[4] = {-1e6f, -1e6f, 1e6f, 1e6f};  // cut off outside this (its list's edges)
  uint8_t fill[4] = {0, 0, 0, 0};  // laid over its inside: sRGB, straight alpha
  uint8_t highlight = 0;  // the lit line along its top edge, its alpha
  uint8_t shadow = 0;     // its drop shadow (or pressed, the shadow inside), 255: full
  uint8_t ink = 0;        // how far its text and icons turn white (danger, on red), 255: all
  bool pressed = false;   // sunk: the shadow is inside its top edge
  bool ring = false;      // the fill is only a ring inside its edge (wells)
  // A list row's stars while it is unrated, hidden at rest: the left edge
  // of its rating column (layout pixels), where five empty ones show; none
  // below 0.
  float stars = -1;
  float shown = 1;  // how far it has eased in (menu_feel.h), 0..1: its stars show as much
  // A value held (fade above 0): nothing is pointed; the panel fades away
  // by `fade` but the held row (rect, radius) and Magnify's scale (clip,
  // its corners scaleRadius; none if empty).
  float fade = 0;
  float scaleRadius = 0;
};
