#include "menu_style.h"

#include <pango/pangocairo.h>

namespace {

void triangle(cairo_t* cr, double x0, double y0, double x1, double y1, double x2, double y2) {
  cairo_move_to(cr, x0, y0);
  cairo_line_to(cr, x1, y1);
  cairo_line_to(cr, x2, y2);
  cairo_close_path(cr);
}

// Speaker body pointing right, within the left 55% of an s-sized box.
void speaker(cairo_t* cr, double x, double y, double s) {
  cairo_rectangle(cr, x, y + s * 0.35, s * 0.2, s * 0.3);
  cairo_move_to(cr, x + s * 0.2, y + s * 0.35);
  cairo_line_to(cr, x + s * 0.5, y + s * 0.1);
  cairo_line_to(cr, x + s * 0.5, y + s * 0.9);
  cairo_line_to(cr, x + s * 0.2, y + s * 0.65);
  cairo_close_path(cr);
  cairo_fill(cr);
}

// Strokes a path drawn in a 24-unit icon box (like an SVG icon) at x, y with size s.
void beginIcon24(cairo_t* cr, double x, double y, double s) {
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, s / 24, s / 24);
  cairo_set_line_width(cr, 1.8);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
}

void ellipse(cairo_t* cr, double cx, double cy, double rx, double ry, double from = 0,
             double to = 2 * M_PI) {
  cairo_save(cr);
  cairo_translate(cr, cx, cy);
  cairo_scale(cr, rx, ry);
  cairo_new_sub_path(cr);
  cairo_arc(cr, 0, 0, 1, from, to);
  cairo_restore(cr);
}

}  // namespace

// Draws an icon in colour r, g, b centered at (cx, cy) in an s-sized box;
// one drawn in one paint (onePaint()) can be drawn at alpha a.
void Menu::drawIcon(cairo_t* cr, Icon icon, double cx, double cy, double s, double r, double g,
                    double b, double a) {
  const double x = cx - s / 2, y = cy - s / 2;
  cairo_save(cr);
  cairo_new_path(cr);  // no line from where text left the current point
  cairo_set_source_rgba(cr, r, g, b, a);
  cairo_set_line_width(cr, s * 0.08);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  switch (icon) {
    case Icon::Pin:
    case Icon::Shuffle:
    case Icon::Layers:
    case Icon::Film:
    case Icon::People:
    case Icon::Building:
      beginIcon24(cr, x, y, s);
      iconPath24(cr, icon);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Play:
      triangle(cr, x + s * 0.2, y, x + s * 0.2, y + s, x + s * 0.95, y + s / 2);
      cairo_fill(cr);
      break;
    case Icon::Pause:
      cairo_rectangle(cr, x + s * 0.15, y, s * 0.25, s);
      cairo_rectangle(cr, x + s * 0.6, y, s * 0.25, s);
      cairo_fill(cr);
      break;
    case Icon::Stop:
      cairo_rectangle(cr, x + s * 0.12, y + s * 0.12, s * 0.76, s * 0.76);
      cairo_fill(cr);
      break;
    case Icon::Previous:
      cairo_rectangle(cr, x + s * 0.05, y + s * 0.1, s * 0.12, s * 0.8);
      triangle(cr, x + s * 0.9, y + s * 0.1, x + s * 0.9, y + s * 0.9, x + s * 0.2, y + s / 2);
      cairo_fill(cr);
      break;
    case Icon::Next:
      cairo_rectangle(cr, x + s * 0.83, y + s * 0.1, s * 0.12, s * 0.8);
      triangle(cr, x + s * 0.1, y + s * 0.1, x + s * 0.1, y + s * 0.9, x + s * 0.8, y + s / 2);
      cairo_fill(cr);
      break;
    case Icon::Back10:
    case Icon::Forward10: {
      // A circular arrow around a small "10".
      const bool back = icon == Icon::Back10;
      beginIcon24(cr, x, y, s);
      // Most of a circle, from the side to the upper corner, then the arrow head.
      if (back) {
        cairo_arc_negative(cr, 12, 12, 8.5, M_PI, -0.75 * M_PI);
        cairo_line_to(cr, 3.5, 8.5);
        cairo_move_to(cr, 3.5, 3.5);
        cairo_line_to(cr, 3.5, 8.5);
        cairo_line_to(cr, 8.5, 8.5);
      } else {
        cairo_arc(cr, 12, 12, 8.5, 0, 1.75 * M_PI);
        cairo_line_to(cr, 20.5, 8.5);
        cairo_move_to(cr, 20.5, 3.5);
        cairo_line_to(cr, 20.5, 8.5);
        cairo_line_to(cr, 15.5, 8.5);
      }
      cairo_stroke(cr);
      cairo_restore(cr);
      PangoLayout* layout = pango_cairo_create_layout(cr);
      PangoFontDescription* font = pango_font_description_from_string("Sans Bold");
      pango_font_description_set_absolute_size(font, s * 0.3 * PANGO_SCALE);
      pango_layout_set_font_description(layout, font);
      pango_layout_set_text(layout, "10", -1);
      int tw = 0, th = 0;
      pango_layout_get_pixel_size(layout, &tw, &th);
      cairo_move_to(cr, cx - tw / 2.0, cy - th / 2.0);
      pango_cairo_show_layout(cr, layout);
      pango_font_description_free(font);
      g_object_unref(layout);
      break;
    }
    case Icon::VolumeUp:
    case Icon::Mute:
      speaker(cr, x, y, s);
      if (icon == Icon::Mute) {
        cairo_move_to(cr, x + s * 0.65, y + s * 0.32);
        cairo_line_to(cr, x + s * 0.95, y + s * 0.68);
        cairo_move_to(cr, x + s * 0.95, y + s * 0.32);
        cairo_line_to(cr, x + s * 0.65, y + s * 0.68);
        cairo_stroke(cr);
      } else {
        for (int i = 0; i < 2; ++i) {
          cairo_new_sub_path(cr);
          cairo_arc(cr, x + s * 0.5, y + s / 2, s * (0.2 + 0.22 * i), -M_PI / 4, M_PI / 4);
          cairo_stroke(cr);
        }
      }
      break;
    case Icon::Folder:
    case Icon::FolderUp:
      cairo_move_to(cr, x, y + s * 0.15);
      cairo_line_to(cr, x + s * 0.38, y + s * 0.15);
      cairo_line_to(cr, x + s * 0.48, y + s * 0.27);
      cairo_line_to(cr, x + s, y + s * 0.27);
      cairo_line_to(cr, x + s, y + s * 0.88);
      cairo_line_to(cr, x, y + s * 0.88);
      cairo_close_path(cr);
      cairo_fill(cr);
      if (icon == Icon::FolderUp) {
        // Arrow pointing up, cut out of the folder in the button colour.
        cairo_set_source_rgba(cr, 0.17, 0.17, 0.21, 1);
        triangle(cr, cx, y + s * 0.34, cx - s * 0.2, y + s * 0.56, cx + s * 0.2, y + s * 0.56);
        cairo_fill(cr);
        cairo_rectangle(cr, cx - s * 0.07, y + s * 0.55, s * 0.14, s * 0.24);
        cairo_fill(cr);
      }
      break;
    case Icon::Settings: {
      // Gear: eight teeth around a ring.
      for (int i = 0; i < 8; ++i) {
        cairo_save(cr);
        cairo_translate(cr, cx, cy);
        cairo_rotate(cr, i * M_PI / 4);
        cairo_rectangle(cr, -s * 0.09, -s * 0.5, s * 0.18, s * 0.25);
        cairo_restore(cr);
      }
      cairo_fill(cr);
      cairo_set_line_width(cr, s * 0.16);
      cairo_arc(cr, cx, cy, s * 0.28, 0, 2 * M_PI);
      cairo_stroke(cr);
      break;
    }
    case Icon::Recenter:
      cairo_arc(cr, cx, cy, s * 0.32, 0, 2 * M_PI);
      cairo_stroke(cr);
      cairo_arc(cr, cx, cy, s * 0.08, 0, 2 * M_PI);
      cairo_fill(cr);
      for (int i = 0; i < 4; ++i) {
        const double a = i * M_PI / 2;
        cairo_move_to(cr, cx + std::cos(a) * s * 0.36, cy + std::sin(a) * s * 0.36);
        cairo_line_to(cr, cx + std::cos(a) * s * 0.5, cy + std::sin(a) * s * 0.5);
      }
      cairo_stroke(cr);
      break;
    case Icon::Trash:
      // Lid with handle, then a tapered bin with two grooves.
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 4, 7);
      cairo_line_to(cr, 20, 7);
      cairo_move_to(cr, 9, 7);
      cairo_line_to(cr, 9, 4.5);
      cairo_line_to(cr, 15, 4.5);
      cairo_line_to(cr, 15, 7);
      cairo_move_to(cr, 6, 7);
      cairo_line_to(cr, 7, 20);
      cairo_line_to(cr, 17, 20);
      cairo_line_to(cr, 18, 7);
      cairo_move_to(cr, 10, 11);
      cairo_line_to(cr, 10, 17);
      cairo_move_to(cr, 14, 11);
      cairo_line_to(cr, 14, 17);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Globe:
      beginIcon24(cr, x, y, s);
      cairo_arc(cr, 12, 12, 9, 0, 2 * M_PI);
      cairo_move_to(cr, 3, 12);
      cairo_line_to(cr, 21, 12);
      ellipse(cr, 12, 12, 4, 9);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::FormatDome:
      // A dome over a floor with the viewer in the middle: how the video surrounds you.
      beginIcon24(cr, x, y, s);
      cairo_arc(cr, 12, 15.5, 9, M_PI, 2 * M_PI);
      ellipse(cr, 12, 15.5, 9, 3);
      cairo_stroke(cr);
      cairo_arc(cr, 12, 15.5, 1.4, 0, 2 * M_PI);
      cairo_fill(cr);
      cairo_restore(cr);
      break;
    case Icon::ProjFlat:
      beginIcon24(cr, x, y, s);
      cairo_rectangle(cr, 3, 6, 18, 12);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Proj180:
      beginIcon24(cr, x, y, s);
      cairo_arc(cr, 12, 16, 9, M_PI, 2 * M_PI);
      cairo_close_path(cr);
      ellipse(cr, 12, 16, 3, 9, M_PI, 2 * M_PI);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Proj360:
      beginIcon24(cr, x, y, s);
      cairo_arc(cr, 12, 12, 9, 0, 2 * M_PI);
      ellipse(cr, 12, 12, 9, 3.2);
      ellipse(cr, 12, 12, 3.2, 9);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Fisheye180:
      beginIcon24(cr, x, y, s);
      cairo_arc(cr, 12, 12, 8.5, 0, 2 * M_PI);
      cairo_stroke(cr);
      cairo_arc(cr, 10.2, 10.2, 1.3, 0, 2 * M_PI);
      cairo_fill(cr);
      cairo_restore(cr);
      break;
    case Icon::Fisheye190:
      beginIcon24(cr, x, y, s);
      cairo_arc(cr, 12, 12, 9, 0, 2 * M_PI);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 12, 5, 0, 2 * M_PI);
      cairo_stroke(cr);
      cairo_arc(cr, 10.2, 10.2, 1.3, 0, 2 * M_PI);
      cairo_fill(cr);
      cairo_restore(cr);
      break;
    case Icon::Fisheye200:
      beginIcon24(cr, x, y, s);
      cairo_arc(cr, 12, 12, 9.5, 0, 2 * M_PI);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 12, 6, 0, 2 * M_PI);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 12, 2.4, 0, 2 * M_PI);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Cube:
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 12, 3);
      cairo_line_to(cr, 20, 7.5);
      cairo_line_to(cr, 20, 16.5);
      cairo_line_to(cr, 12, 21);
      cairo_line_to(cr, 4, 16.5);
      cairo_line_to(cr, 4, 7.5);
      cairo_close_path(cr);
      cairo_move_to(cr, 4, 7.5);
      cairo_line_to(cr, 12, 12);
      cairo_line_to(cr, 20, 7.5);
      cairo_move_to(cr, 12, 12);
      cairo_line_to(cr, 12, 21);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::StereoMono:
      beginIcon24(cr, x, y, s);
      cairo_rectangle(cr, 6, 6, 12, 12);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::StereoSbs:
      beginIcon24(cr, x, y, s);
      cairo_rectangle(cr, 2.5, 7, 9, 10);
      cairo_rectangle(cr, 12.5, 7, 9, 10);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::StereoTb:
      beginIcon24(cr, x, y, s);
      cairo_rectangle(cr, 6, 2.5, 12, 9);
      cairo_rectangle(cr, 6, 12.5, 12, 9);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::JumpTop:
    case Icon::JumpBottom: {
      // An arrow to a bar: to the top (or bottom) of the list.
      const bool top = icon == Icon::JumpTop;
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 5, top ? 4.5 : 19.5);
      cairo_line_to(cr, 19, top ? 4.5 : 19.5);
      cairo_move_to(cr, 12, top ? 20 : 4);
      cairo_line_to(cr, 12, top ? 9 : 15);
      cairo_move_to(cr, 7, top ? 13.5 : 10.5);
      cairo_line_to(cr, 12, top ? 8.5 : 15.5);
      cairo_line_to(cr, 17, top ? 13.5 : 10.5);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    }
    case Icon::MoveUp:
    case Icon::MoveDown: {
      // A chevron: one place up (or down) in a list.
      const double tip = icon == Icon::MoveUp ? 8.5 : 15.5, arms = icon == Icon::MoveUp ? 15.5 : 8.5;
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 6, arms);
      cairo_line_to(cr, 12, tip);
      cairo_line_to(cr, 18, arms);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    }
    case Icon::MenuLines:
      // Three lines: the main menu.
      beginIcon24(cr, x, y, s);
      for (double row : {6.0, 12.0, 18.0}) {
        cairo_move_to(cr, 4, row);
        cairo_line_to(cr, 20, row);
      }
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Focus:
      // Two eyes' lines of sight meeting at a point: stereo focus.
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 3, 20);
      cairo_line_to(cr, 12, 6);
      cairo_line_to(cr, 21, 20);
      cairo_stroke(cr);
      cairo_arc(cr, 12, 6, 2.6, 0, 2 * M_PI);
      cairo_fill(cr);
      cairo_restore(cr);
      break;
    case Icon::Audio:
      // A note.
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 10, 18);
      cairo_line_to(cr, 10, 4.5);
      cairo_line_to(cr, 19, 3);
      cairo_line_to(cr, 19, 16);
      cairo_stroke(cr);
      ellipse(cr, 7.5, 18, 3, 2.4);
      ellipse(cr, 16.5, 16, 3, 2.4);
      cairo_fill(cr);
      cairo_restore(cr);
      break;
    case Icon::Subtitles:
      // A screen with two lines of text at its bottom.
      beginIcon24(cr, x, y, s);
      cairo_rectangle(cr, 2.5, 5, 19, 14);
      cairo_move_to(cr, 6, 12.5);
      cairo_line_to(cr, 14, 12.5);
      cairo_move_to(cr, 6, 15.5);
      cairo_line_to(cr, 18, 15.5);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Chapters:
      // A bookmark.
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 6, 3);
      cairo_line_to(cr, 18, 3);
      cairo_line_to(cr, 18, 21);
      cairo_line_to(cr, 12, 16);
      cairo_line_to(cr, 6, 21);
      cairo_close_path(cr);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Star:
    case Icon::StarFilled: {
      // A five-pointed star, outlined or filled (a favourite).
      beginIcon24(cr, x, y, s);
      for (int i = 0; i < 10; ++i) {
        const double a = -M_PI / 2 + i * M_PI / 5, r = i % 2 ? 4.2 : 9.5;
        const double px = 12 + r * std::cos(a), py = 12.8 + r * std::sin(a);
        if (i == 0)
          cairo_move_to(cr, px, py);
        else
          cairo_line_to(cr, px, py);
      }
      cairo_close_path(cr);
      if (icon == Icon::StarFilled)
        cairo_fill(cr);
      else
        cairo_stroke(cr);
      cairo_restore(cr);
      break;
    }
    case Icon::Library: {
      // A pile of videos: one in front with its play mark, the edges of
      // another behind it.
      beginIcon24(cr, x, y, s);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 4.5, 9.5, 1.5, M_PI, 1.5 * M_PI);
      cairo_arc(cr, 16.5, 9.5, 1.5, 1.5 * M_PI, 2 * M_PI);
      cairo_arc(cr, 16.5, 19.5, 1.5, 0, 0.5 * M_PI);
      cairo_arc(cr, 4.5, 19.5, 1.5, 0.5 * M_PI, M_PI);
      cairo_close_path(cr);
      cairo_move_to(cr, 6.5, 5);
      cairo_line_to(cr, 19.5, 5);
      cairo_arc(cr, 19.5, 6.5, 1.5, 1.5 * M_PI, 2 * M_PI);
      cairo_line_to(cr, 21, 16);
      cairo_stroke(cr);
      triangle(cr, 9, 11.5, 9, 17.5, 14, 14.5);
      cairo_fill(cr);
      cairo_restore(cr);
      break;
    }
    case Icon::Search:
      // A magnifying glass.
      beginIcon24(cr, x, y, s);
      cairo_arc(cr, 10.5, 10.5, 6.5, 0, 2 * M_PI);
      cairo_move_to(cr, 15.3, 15.3);
      cairo_line_to(cr, 20.5, 20.5);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Refresh:
      // Two arrows turning round a circle.
      beginIcon24(cr, x, y, s);
      cairo_arc_negative(cr, 12, 12, 8, std::atan2(-1, 8), std::atan2(-5.6, -6.4));
      cairo_line_to(cr, 4, 8);
      cairo_new_sub_path(cr);
      cairo_arc_negative(cr, 12, 12, 8, std::atan2(1, -8), std::atan2(4.6, 6.4));
      cairo_line_to(cr, 20, 16);
      cairo_move_to(cr, 4, 4);
      cairo_line_to(cr, 4, 8);
      cairo_line_to(cr, 8, 8);
      cairo_move_to(cr, 20, 20);
      cairo_line_to(cr, 20, 16);
      cairo_line_to(cr, 16, 16);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Arrow:
      // Pointing left: back.
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 19, 12);
      cairo_line_to(cr, 5.5, 12);
      cairo_move_to(cr, 11, 6);
      cairo_line_to(cr, 5, 12);
      cairo_line_to(cr, 11, 18);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::ArrowRight:
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 5, 12);
      cairo_line_to(cr, 18.5, 12);
      cairo_move_to(cr, 13, 6);
      cairo_line_to(cr, 19, 12);
      cairo_line_to(cr, 13, 18);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Chevron:
      // Pointing right.
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 9, 6);
      cairo_line_to(cr, 15, 12);
      cairo_line_to(cr, 9, 18);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Caret:
      // Pointing down: a list opens under it.
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 7, 10);
      cairo_line_to(cr, 12, 15);
      cairo_line_to(cr, 17, 10);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Trigger:
      // A trigger's face as SteamVR's glyphs draw it: wide at the top, to a
      // rounded point, lightly filled, thinly outlined.
      beginIcon24(cr, x, y, s);
      cairo_set_line_width(cr, 0.96);
      cairo_move_to(cr, 4.2, 9.8);
      cairo_curve_to(cr, 3.2, 6.7, 4.8, 5.1, 7.0, 4.8);
      cairo_curve_to(cr, 9.6, 4.3, 14.4, 4.3, 17.0, 4.8);
      cairo_curve_to(cr, 19.2, 5.1, 20.8, 6.7, 19.8, 9.8);
      cairo_curve_to(cr, 18.9, 13.4, 16.6, 17.0, 15.0, 18.6);
      cairo_curve_to(cr, 13.4, 20.0, 10.6, 20.0, 9.0, 18.6);
      cairo_curve_to(cr, 7.4, 17.0, 5.1, 13.4, 4.2, 9.8);
      cairo_close_path(cr);
      cairo_set_source_rgba(cr, r, g, b, 0.2);
      cairo_fill_preserve(cr);
      cairo_set_source_rgba(cr, r, g, b, 1);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Controller:
      // A gamepad: its body, a cross on the left, two buttons on the right.
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 7, 8);
      cairo_line_to(cr, 17, 8);
      cairo_curve_to(cr, 19.5, 8, 21, 10.2, 21, 12.5);
      cairo_line_to(cr, 20.4, 16.5);
      cairo_curve_to(cr, 20.1, 18.6, 17.5, 19.2, 16, 17.7);
      cairo_line_to(cr, 14.5, 16);
      cairo_line_to(cr, 9.5, 16);
      cairo_line_to(cr, 8, 17.7);
      cairo_curve_to(cr, 6.5, 19.2, 3.9, 18.6, 3.6, 16.5);
      cairo_line_to(cr, 3, 12.5);
      cairo_curve_to(cr, 3, 10.2, 4.5, 8, 7, 8);
      cairo_close_path(cr);
      cairo_move_to(cr, 7.5, 11);
      cairo_line_to(cr, 7.5, 14);
      cairo_move_to(cr, 6, 12.5);
      cairo_line_to(cr, 9, 12.5);
      cairo_stroke(cr);
      cairo_arc(cr, 16, 11.5, 0.9, 0, 2 * M_PI);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 17.5, 13.8, 0.9, 0, 2 * M_PI);
      cairo_fill(cr);
      cairo_restore(cr);
      break;
    case Icon::Grid:
      beginIcon24(cr, x, y, s);
      cairo_rectangle(cr, 4, 4, 7, 7);
      cairo_rectangle(cr, 13, 4, 7, 7);
      cairo_rectangle(cr, 4, 13, 7, 7);
      cairo_rectangle(cr, 13, 13, 7, 7);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::List:
      beginIcon24(cr, x, y, s);
      for (double row : {6.5, 12.0, 17.5}) {
        cairo_move_to(cr, 9, row);
        cairo_line_to(cr, 20, row);
      }
      cairo_stroke(cr);
      for (double row : {6.5, 12.0, 17.5}) {
        cairo_new_sub_path(cr);
        cairo_arc(cr, 5, row, 1.3, 0, 2 * M_PI);
      }
      cairo_fill(cr);
      cairo_restore(cr);
      break;
    case Icon::Video:
      // A film frame: perforated strips above
      // and below, a play mark between them.
      beginIcon24(cr, x, y, s);
      cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
      for (double top : {4.0, 16.5}) {
        cairo_rectangle(cr, 3, top, 18, 3.5);
        for (double hole = 5; hole < 20; hole += 4) cairo_rectangle(cr, hole, top + 1.1, 2, 1.3);
      }
      cairo_fill(cr);
      cairo_move_to(cr, 3.9, 7.5);
      cairo_line_to(cr, 3.9, 16.5);
      cairo_move_to(cr, 20.1, 7.5);
      cairo_line_to(cr, 20.1, 16.5);
      cairo_stroke(cr);
      triangle(cr, 10, 9.5, 10, 14.5, 14.5, 12);
      cairo_fill(cr);
      cairo_restore(cr);
      break;
    case Icon::Slideshow:
      // A picture frame with a play triangle.
      beginIcon24(cr, x, y, s);
      cairo_rectangle(cr, 3, 5, 18, 14);
      cairo_stroke(cr);
      cairo_move_to(cr, 10, 9);
      cairo_line_to(cr, 15.5, 12);
      cairo_line_to(cr, 10, 15);
      cairo_close_path(cr);
      cairo_fill(cr);
      cairo_restore(cr);
      break;
    case Icon::Power:
      // Ring open at the top with a bar through the gap.
      cairo_set_line_width(cr, s * 0.1);
      cairo_arc(cr, cx, cy + s * 0.05, s * 0.38, -M_PI / 2 + 0.6, 3 * M_PI / 2 - 0.6);
      cairo_stroke(cr);
      cairo_move_to(cr, cx, y + s * 0.05);
      cairo_line_to(cr, cx, cy + s * 0.05);
      cairo_stroke(cr);
      break;
    case Icon::Download:
      // An arrow down into a tray: kept on this device.
      beginIcon24(cr, x, y, s);
      cairo_set_line_width(cr, 2);
      cairo_move_to(cr, 12, 3.5);
      cairo_line_to(cr, 12, 15);
      cairo_move_to(cr, 7, 10.5);
      cairo_line_to(cr, 12, 15.5);
      cairo_line_to(cr, 17, 10.5);
      cairo_move_to(cr, 4, 15);
      cairo_line_to(cr, 4, 20);
      cairo_line_to(cr, 20, 20);
      cairo_line_to(cr, 20, 15);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Check:
      beginIcon24(cr, x, y, s);
      cairo_set_line_width(cr, 2.2);
      cairo_move_to(cr, 4.5, 12.5);
      cairo_line_to(cr, 9.5, 17.5);
      cairo_line_to(cr, 19.5, 6.5);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Card:
      // A memory card, its corner cut.
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 10, 2.5);
      cairo_line_to(cr, 18, 2.5);
      cairo_line_to(cr, 18, 21.5);
      cairo_line_to(cr, 6, 21.5);
      cairo_line_to(cr, 6, 6.5);
      cairo_close_path(cr);
      cairo_move_to(cr, 10.5, 5.5);
      cairo_line_to(cr, 10.5, 8.5);
      cairo_move_to(cr, 14, 5.5);
      cairo_line_to(cr, 14, 8.5);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Storage:
      // A drive: a box with its light.
      beginIcon24(cr, x, y, s);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 5, 9, 2, M_PI, 1.5 * M_PI);
      cairo_arc(cr, 19, 9, 2, 1.5 * M_PI, 2 * M_PI);
      cairo_arc(cr, 19, 15, 2, 0, 0.5 * M_PI);
      cairo_arc(cr, 5, 15, 2, 0.5 * M_PI, M_PI);
      cairo_close_path(cr);
      cairo_move_to(cr, 7, 12);
      cairo_line_to(cr, 12, 12);
      cairo_stroke(cr);
      cairo_arc(cr, 16.5, 12, 1.3, 0, 2 * M_PI);
      cairo_fill(cr);
      cairo_restore(cr);
      break;
    case Icon::Close:
      beginIcon24(cr, x, y, s);
      cairo_set_line_width(cr, 2);
      cairo_move_to(cr, 6, 6);
      cairo_line_to(cr, 18, 18);
      cairo_move_to(cr, 18, 6);
      cairo_line_to(cr, 6, 18);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Pencil:
      // Renaming: a pencil, its point down at the left.
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 4.5, 19.5);
      cairo_line_to(cr, 8.5, 19.5);
      cairo_line_to(cr, 19, 9);
      cairo_line_to(cr, 15, 5);
      cairo_line_to(cr, 4.5, 15.5);
      cairo_close_path(cr);
      cairo_move_to(cr, 13, 7);
      cairo_line_to(cr, 17, 11);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Plus:
      beginIcon24(cr, x, y, s);
      cairo_set_line_width(cr, 2);
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
      cairo_move_to(cr, 12, 5);
      cairo_line_to(cr, 12, 19);
      cairo_move_to(cr, 5, 12);
      cairo_line_to(cr, 19, 12);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Warning:
      // A triangle with "!" in it.
      beginIcon24(cr, x, y, s);
      cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
      cairo_move_to(cr, 12, 3.5);
      cairo_line_to(cr, 21.5, 20);
      cairo_line_to(cr, 2.5, 20);
      cairo_close_path(cr);
      cairo_move_to(cr, 12, 9.5);
      cairo_line_to(cr, 12, 13.5);
      cairo_stroke(cr);
      cairo_arc(cr, 12, 16.8, 1.1, 0, 2 * M_PI);
      cairo_fill(cr);
      cairo_restore(cr);
      break;
    case Icon::Headset:
      // VR goggles from the front: two lenses, the nose cut out under them,
      // the strap's tabs at the sides.
      beginIcon24(cr, x, y, s);
      cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 5.5, 8.5, 2.5, M_PI, 1.5 * M_PI);
      cairo_arc(cr, 18.5, 8.5, 2.5, 1.5 * M_PI, 2 * M_PI);
      cairo_arc(cr, 18.5, 15.5, 2.5, 0, 0.5 * M_PI);
      cairo_line_to(cr, 14.9, 18);
      cairo_line_to(cr, 13.6, 15.4);
      cairo_arc_negative(cr, 12, 16.2, 1.8, -0.46, M_PI + 0.46);
      cairo_line_to(cr, 9.1, 18);
      cairo_arc(cr, 5.5, 15.5, 2.5, 0.5 * M_PI, M_PI);
      cairo_close_path(cr);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 8, 11, 1.8, 0, 2 * M_PI);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 16, 11, 1.8, 0, 2 * M_PI);
      cairo_move_to(cr, 3, 10);
      cairo_line_to(cr, 1.5, 10);
      cairo_line_to(cr, 1.5, 14);
      cairo_line_to(cr, 3, 14);
      cairo_move_to(cr, 21, 10);
      cairo_line_to(cr, 22.5, 10);
      cairo_line_to(cr, 22.5, 14);
      cairo_line_to(cr, 21, 14);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Computer:
      // A monitor on its stand.
      beginIcon24(cr, x, y, s);
      roundedRect(cr, 3, 4.5, 18, 12, 1.5);
      cairo_move_to(cr, 8.5, 20);
      cairo_line_to(cr, 15.5, 20);
      cairo_move_to(cr, 12, 16.5);
      cairo_line_to(cr, 12, 20);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::StopAtEnd:
      // An arrow running up to a bar.
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 3, 12);
      cairo_line_to(cr, 15, 12);
      cairo_move_to(cr, 10.5, 7.5);
      cairo_line_to(cr, 15, 12);
      cairo_line_to(cr, 10.5, 16.5);
      cairo_move_to(cr, 20, 5.5);
      cairo_line_to(cr, 20, 18.5);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Repeat:
      // Two arrows chasing each other.
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 17, 2.5);
      cairo_line_to(cr, 20.5, 6);
      cairo_line_to(cr, 17, 9.5);
      cairo_move_to(cr, 3.5, 11.5);
      cairo_line_to(cr, 3.5, 10);
      cairo_arc(cr, 7.5, 10, 4, M_PI, 1.5 * M_PI);
      cairo_line_to(cr, 20.5, 6);
      cairo_move_to(cr, 7, 21.5);
      cairo_line_to(cr, 3.5, 18);
      cairo_line_to(cr, 7, 14.5);
      cairo_move_to(cr, 20.5, 12.5);
      cairo_line_to(cr, 20.5, 14);
      cairo_arc(cr, 16.5, 14, 4, 0, 0.5 * M_PI);
      cairo_line_to(cr, 3.5, 18);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::PlayNext:
      // A list's lines with a play triangle after them.
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 3, 6);
      cairo_line_to(cr, 16, 6);
      cairo_move_to(cr, 3, 11.5);
      cairo_line_to(cr, 16, 11.5);
      cairo_move_to(cr, 3, 17);
      cairo_line_to(cr, 10, 17);
      cairo_stroke(cr);
      triangle(cr, 14, 13.5, 14, 21, 21, 17.25);
      cairo_fill(cr);
      cairo_restore(cr);
      break;
    case Icon::Home:
      // A house with its door.
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 4, 11);
      cairo_line_to(cr, 12, 4);
      cairo_line_to(cr, 20, 11);
      cairo_line_to(cr, 20, 20);
      cairo_line_to(cr, 14.5, 20);
      cairo_line_to(cr, 14.5, 14);
      cairo_line_to(cr, 9.5, 14);
      cairo_line_to(cr, 9.5, 20);
      cairo_line_to(cr, 4, 20);
      cairo_close_path(cr);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::Usb:
      // A USB stick, its plug up.
      beginIcon24(cr, x, y, s);
      cairo_new_sub_path(cr);
      cairo_move_to(cr, 8, 10);
      cairo_line_to(cr, 16, 10);
      cairo_line_to(cr, 16, 20.5);
      cairo_arc(cr, 15, 20.5, 1, 0, 0.5 * M_PI);
      cairo_arc(cr, 9, 20.5, 1, 0.5 * M_PI, M_PI);
      cairo_close_path(cr);
      cairo_move_to(cr, 9.5, 10);
      cairo_line_to(cr, 9.5, 3.5);
      cairo_line_to(cr, 14.5, 3.5);
      cairo_line_to(cr, 14.5, 10);
      cairo_stroke(cr);
      cairo_arc(cr, 11, 6, 0.9, 0, 2 * M_PI);
      cairo_fill(cr);
      cairo_arc(cr, 13, 6, 0.9, 0, 2 * M_PI);
      cairo_fill(cr);
      cairo_restore(cr);
      break;
    case Icon::ChevronLeft:
      beginIcon24(cr, x, y, s);
      cairo_move_to(cr, 15, 6);
      cairo_line_to(cr, 9, 12);
      cairo_line_to(cr, 15, 18);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    case Icon::ArrowUp:
    case Icon::ArrowDown: {
      // An order's direction: ascending up, descending down.
      const bool up = icon == Icon::ArrowUp;
      beginIcon24(cr, x, y, s);
      if (s <= 22) cairo_set_line_width(cr, 2.4);
      cairo_move_to(cr, 12, up ? 19 : 5);
      cairo_line_to(cr, 12, up ? 5 : 19);
      cairo_move_to(cr, 6, up ? 11 : 13);
      cairo_line_to(cr, 12, up ? 5 : 19);
      cairo_line_to(cr, 18, up ? 11 : 13);
      cairo_stroke(cr);
      cairo_restore(cr);
      break;
    }
    case Icon::None:
      break;
  }
  cairo_restore(cr);
}

// Faded at alpha in place, an icon whose parts overlap would show the
// overlap: those are faded through a temporary surface (drawButtons()).
bool Menu::onePaint(Icon icon) {
  switch (icon) {
    case Icon::FolderUp:  // its arrow cut out of the folder
    case Icon::Settings:
    case Icon::Recenter:
    case Icon::Fisheye190:
    case Icon::Focus:
    case Icon::Audio:
    case Icon::Video:
    case Icon::Trigger:  // filled lightly under its outline
    case Icon::Usb:
      return false;
    case Icon::None:
    case Icon::Play:
    case Icon::Pause:
    case Icon::Stop:
    case Icon::Previous:
    case Icon::Next:
    case Icon::Back10:
    case Icon::Forward10:
    case Icon::VolumeUp:
    case Icon::Mute:
    case Icon::Folder:
    case Icon::Trash:
    case Icon::Power:
    case Icon::Globe:
    case Icon::FormatDome:
    case Icon::ProjFlat:
    case Icon::Proj180:
    case Icon::Proj360:
    case Icon::Fisheye180:
    case Icon::Fisheye200:
    case Icon::Cube:
    case Icon::StereoMono:
    case Icon::StereoSbs:
    case Icon::StereoTb:
    case Icon::Slideshow:
    case Icon::JumpTop:
    case Icon::JumpBottom:
    case Icon::Grid:
    case Icon::MenuLines:
    case Icon::Subtitles:
    case Icon::Chapters:
    case Icon::List:
    case Icon::Star:
    case Icon::StarFilled:
    case Icon::Search:
    case Icon::Library:
    case Icon::Pin:
    case Icon::MoveUp:
    case Icon::MoveDown:
    case Icon::Download:
    case Icon::Check:
    case Icon::Card:
    case Icon::Storage:
    case Icon::Close:
    case Icon::Pencil:
    case Icon::Plus:
    case Icon::Refresh:
    case Icon::Arrow:
    case Icon::ArrowRight:
    case Icon::Chevron:
    case Icon::Caret:
    case Icon::Controller:
    case Icon::Warning:
    case Icon::Headset:
    case Icon::Computer:
    case Icon::StopAtEnd:
    case Icon::Repeat:
    case Icon::PlayNext:
    case Icon::Shuffle:
    case Icon::Home:
    case Icon::ChevronLeft:
    case Icon::ArrowUp:
    case Icon::ArrowDown:
    case Icon::Layers:
    case Icon::Film:
    case Icon::People:
    case Icon::Building:
      return true;
  }
  return false;
}

void Menu::iconPath24(cairo_t* cr, Icon icon) {
  switch (icon) {
    case Icon::Pin:  // a pushpin
      cairo_move_to(cr, 8, 3);
      cairo_line_to(cr, 16, 3);
      cairo_move_to(cr, 9.5, 3);
      cairo_line_to(cr, 9.5, 9);
      cairo_line_to(cr, 6, 14);
      cairo_line_to(cr, 18, 14);
      cairo_line_to(cr, 14.5, 9);
      cairo_line_to(cr, 14.5, 3);
      cairo_move_to(cr, 12, 14);
      cairo_line_to(cr, 12, 21);
      break;
    case Icon::Shuffle:  // two arrows crossing
      cairo_move_to(cr, 3, 7);
      cairo_curve_to(cr, 11, 7, 12, 17, 20, 17);
      cairo_move_to(cr, 3, 17);
      cairo_curve_to(cr, 11, 17, 12, 7, 20, 7);
      cairo_move_to(cr, 17, 4);
      cairo_line_to(cr, 20, 7);
      cairo_line_to(cr, 17, 10);
      cairo_move_to(cr, 17, 14);
      cairo_line_to(cr, 20, 17);
      cairo_line_to(cr, 17, 20);
      break;
    case Icon::Layers:  // three layers, as Stash's own mark
      for (const double y : {7.0, 12.0, 17.0}) {
        cairo_move_to(cr, 3, y);
        cairo_line_to(cr, 12, y - 4);
        cairo_line_to(cr, 21, y);
        cairo_line_to(cr, 12, y + 4);
        cairo_close_path(cr);
      }
      break;
    case Icon::Film:  // a frame of film
      roundedRect(cr, 3, 5, 18, 14, 2);
      cairo_move_to(cr, 7.5, 5);
      cairo_line_to(cr, 7.5, 19);
      cairo_move_to(cr, 16.5, 5);
      cairo_line_to(cr, 16.5, 19);
      cairo_move_to(cr, 3, 12);
      cairo_line_to(cr, 7.5, 12);
      cairo_move_to(cr, 16.5, 12);
      cairo_line_to(cr, 21, 12);
      break;
    case Icon::People:  // two people
      cairo_arc(cr, 9, 7, 3, 0, 2 * M_PI);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 18, 8, 2.5, 0, 2 * M_PI);
      cairo_move_to(cr, 3, 21);
      cairo_line_to(cr, 3, 18);
      cairo_curve_to(cr, 3, 12, 15, 12, 15, 18);
      cairo_line_to(cr, 15, 21);
      cairo_move_to(cr, 17, 14);
      cairo_curve_to(cr, 21, 14, 22, 16, 22, 19);
      cairo_line_to(cr, 22, 21);
      break;
    case Icon::Building:  // its windows, its door
      roundedRect(cr, 5, 3, 14, 18, 1);
      for (const double x : {9.0, 15.0})
        for (const double y : {7.0, 11.0}) {
          cairo_move_to(cr, x, y);
          cairo_line_to(cr, x, y + 1);
        }
      cairo_move_to(cr, 10, 21);
      cairo_line_to(cr, 10, 16);
      cairo_line_to(cr, 14, 16);
      cairo_line_to(cr, 14, 21);
      break;
    default:
      break;
  }
}
