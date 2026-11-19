#pragma once

#include "accent.h"

#include <cairo.h>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

// The menu's surfaces at rest, lit from above (the menu design's depths and
// states). A control is one of three heights: sunk (wells), flush (no
// surface: text, icons, bare buttons) or raised (buttons, the chosen piece
// in a well, knobs, lists); a state changes which finish it has. Pointed is
// drawn by the panel shader over the canvas instead (Menu::hoverLook()), so
// pointing never redraws the menu; a pressed button is drawn pushed in
// (Pressed, Menu::drawPressed()), redrawn as it is pressed and let go.
enum class Finish : uint8_t {
  Sunk,          // a well: darker, a shadow inside its top edge
  Raised,        // a button: one gradient, a lit top edge, a drop shadow
  Chosen,        // the chosen piece in a well, the chosen row: raised a step, lit
  Pressed,       // pushed in
  Latched,       // a mode that is on (mute, Repeat, an open list): pushed in, in the accent
  Primary,       // filled in the accent: Play, a confirming button, a switch that is on
  DangerFilled,  // a dialog's confirming button that destroys: filled red
  Group,         // a group of settings: a shallow darker box
  Pop,           // an open list or a dialog, over the page
  Tip,           // a tooltip
  SwitchOn,      // a switch's track while it is on
  Knob,          // a switch's knob
  SliderKnob,    // the volume's knob
  Track,         // a slider's thin track
  Filled,        // a well's fill to its value (the band, a value): in the accent, lit along its top
  Head,          // a value's head: a slim white bar with a dark edge
  kCount
};

// Rows of a panel (layout pixels) that are all one colour, its gradient's
// middle: where a list scrolls by moving its pixels, which would take a
// gradient along with them. Above and below, the gradient runs to its ends.
// None if bottom <= top.
struct PanelFlat {
  double top = 0, bottom = 0;
  bool operator==(const PanelFlat& o) const { return top == o.top && bottom == o.bottom; }
};

class MenuFinishes {
 public:
  MenuFinishes() = default;
  MenuFinishes(const MenuFinishes&) = delete;
  MenuFinishes& operator=(const MenuFinishes&) = delete;
  ~MenuFinishes();

  // The menu colour: the accent's finishes are made again in it.
  void setAccent(const AccentColours& accent);

  // Draws the box (layout pixels, corner radius r) in the finish, its
  // shadows around it. The box is snapped to the texture's pixels. Each
  // finish is drawn once per size and corner into a piece kept for drawing
  // again, its middle stretched for wider (or taller) boxes: no blur or
  // shadow is worked out while the menu is drawn.
  // `behind`: the box sits straight on a panel (panel(), fillPanel()), with
  // nothing else under the box: not in a well, not over another box's
  // shadow. The panel's drawn box is behind->top..bottom: the box's flat
  // insides are worked out over the panel and filled opaque, which costs a
  // third of blending them. behind->paint, if set, paints the panel within
  // the clip as it is on the canvas: a box that isn't opaque already is then
  // worked out over it once per place and copied (a tenth of the cost of
  // blending it).
  struct Behind {
    double top, bottom;
    std::function<void(cairo_t*)> paint;
  };
  void draw(cairo_t* cr, Finish finish, double x, double y, double w, double h, double r,
            const Behind* behind = nullptr);
  // How far beyond its box a finish draws (its shadows), in layout pixels.
  static double reach(Finish finish);

  using Flat = PanelFlat;
  // A panel's material within x, y, w, h (layout pixels; drawn `inset`
  // inside them, its corners r): a vertical gradient with a faint lit top
  // edge.
  static void panel(cairo_t* cr, double x, double y, double w, double h, double inset, double r,
                    Flat flat = {});
  // Fills x0..x1, y0..y1 (layout pixels, within what is being redrawn) as
  // the inside of a panel drawn from top to bottom: its gradient in flat rows.
  static void fillPanel(cairo_t* cr, double x0, double y0, double x1, double y1, double top,
                        double bottom, Flat flat = {});
  // Fills x0..x1, y0..y1 with a vertical gradient from `top` to `bottom`
  // (layout pixels; stops at 0..1 between them, straight alpha) as rows of
  // one 8-bit colour each, filled flat. x0 and x1 should be on the
  // texture's pixels.
  struct Stop {
    double at;
    double rgba[4];
  };
  static void fillRows(cairo_t* cr, double x0, double y0, double x1, double y1, const Stop* stops,
                       int count, double top, double bottom);

 private:
  // A finish drawn once around a box of w × h texture pixels. A box with
  // parts stretched has a middle kMiddle wide (or high) that is the same all
  // along, kept again repeated to kTile to be tiled: no scaling while the
  // menu is drawn, and each part of a box is one fill (a fill's own cost is
  // what matters most at the menu's sizes). An opaque tile is kept without
  // alpha: drawing it is a copy, not a blend.
  static constexpr int kMiddle = 64, kTile = 512;
  struct Piece {
    cairo_surface_t* surface = nullptr;
    cairo_pattern_t* pattern = nullptr;
    cairo_pattern_t* tiles[3] = {};  // the middle across, down, and both; null if not stretched so
    int pad = 0;       // texture pixels of shadow around the box
    int w = 0, h = 0;  // the box's, in texture pixels
    bool flatMiddle = false;  // the middle both ways is one colour: middleArgb, filled flat
    uint32_t middleArgb = 0;  // premultiplied
    // The middle across as rows of one colour each (premultiplied ARGB),
    // if it is so: a flat fill costs about half what blending a picture
    // does, but each fill costs as much as some 2000 pixels of that.
    struct Run {
      int y0, y1;
      uint32_t argb;
    };
    std::vector<Run> runs;
    bool opaqueAcross = false;  // the middle across is opaque: copying it is as quick as filling
    int corner = 0;             // texture pixels from the box's edge to its middle
    // What of a part is drawn: its pixels that aren't clear, and within them
    // a rectangle that is opaque, copied instead of blended (blending costs
    // ten times a copy). Offsets from where the part starts; along a tiled
    // axis 0..1 is all of it. Worked out when the part is first drawn.
    struct Cut {
      bool made = false;
      int keep[4] = {};   // x0, y0, x1, y1; empty if the part is all clear
      int solid[4] = {};  // empty if nothing is opaque
    };
    Cut cuts[4][4];  // by the part across and down: 0..2 sliced, 3 whole
    // The piece and its tiles as opaque images (the same pixels without
    // alpha), for their opaque rectangles; made when first needed.
    cairo_pattern_t* solid[3] = {};
  };
  // Along one axis, texture pixels of the piece from `from` drawn over
  // [at, end) of the canvas; the middle tiled.
  struct Part {
    int from, at, end;
    bool tiled;
  };
  // A box on a panel (Behind::paint) as it is drawn there, its pixels
  // within the box: kept per finish and place, a few at a time.
  struct Placed {
    Finish finish;
    int radius;
    int box[4];  // texture pixels, x0, y0, x1, y1
    const Behind* behind;
    double top, bottom;
    cairo_surface_t* surface = nullptr;
    cairo_pattern_t* pattern = nullptr;  // the same pixels without alpha; none if they aren't opaque
  };
  static constexpr size_t kPlacedMax = 32, kPlacedBytes = 8 << 20;
  static void release(Piece& p);
  static void release(Placed& p);
  // The box's parts, what of them is in `copy`'s box copied from it.
  void drawParts(cairo_t* cr, Finish finish, double x, double y, double w, double h, double r,
                 const Behind* behind, const Placed* copy = nullptr);
  const Placed& placed(Finish finish, double x, double y, double w, double h, double r,
                       const Behind& behind, int radius, const int box[4]);
  Piece& piece(Finish finish, int radius, int w, int h);
  void make(Piece& p, Finish finish, double radius) const;
  static void cut(const Piece& p, const Part& across, const Part& down, Piece::Cut& c);
  static cairo_pattern_t* solidView(cairo_pattern_t* pattern);

  AccentColours accent_ = accentColours(Accent{});
  std::unordered_map<uint64_t, Piece> pieces_;
  std::vector<Placed> placed_;
};
