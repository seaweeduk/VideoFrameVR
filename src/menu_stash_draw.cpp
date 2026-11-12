#include "menu_style.h"

#include "menu_stash.h"

// Drawing Stash's buttons (StashLook): scene tiles and rows, the rail's and
// full lists' rows with their ⊘ targets, the filter chips, links, header
// buttons and the navigation rows, and their icons.

namespace {

// Left out: Stash's red, as the delete dialog's.
constexpr Rgba kOutFill = {0.72, 0.18, 0.16, 1};
constexpr Rgba kOutText = {1.0, 0.50, 0.45, 1};

void stroke24(cairo_t* cr, double cx, double cy, double s, const double rgba[4]) {
  cairo_translate(cr, cx - s / 2, cy - s / 2);
  cairo_scale(cr, s / 24, s / 24);
  cairo_set_line_width(cr, 2);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_set_source_rgba(cr, rgba[0], rgba[1], rgba[2], rgba[3]);
}

}  // namespace

void StashPages::drawIcon(cairo_t* cr, StashIcon icon, double cx, double cy, double s,
                          const double rgba[4]) {
  if (icon == StashIcon::None) return;
  cairo_save(cr);
  cairo_new_path(cr);
  stroke24(cr, cx, cy, s, rgba);
  switch (icon) {
    case StashIcon::Stash:  // three layers, as Stash's own mark
      for (const double y : {7.0, 12.0, 17.0}) {
        cairo_move_to(cr, 3, y);
        cairo_line_to(cr, 12, y - 4);
        cairo_line_to(cr, 21, y);
        cairo_line_to(cr, 12, y + 4);
        cairo_close_path(cr);
      }
      cairo_stroke(cr);
      break;
    case StashIcon::Home:  // a house
      cairo_move_to(cr, 3, 11);
      cairo_line_to(cr, 12, 3);
      cairo_line_to(cr, 21, 11);
      cairo_move_to(cr, 5, 9);
      cairo_line_to(cr, 5, 21);
      cairo_line_to(cr, 10, 21);
      cairo_line_to(cr, 10, 14);
      cairo_line_to(cr, 14, 14);
      cairo_line_to(cr, 14, 21);
      cairo_line_to(cr, 19, 21);
      cairo_line_to(cr, 19, 9);
      cairo_stroke(cr);
      break;
    case StashIcon::Scenes:  // a frame of film
      roundedRect(cr, 3, 5, 18, 14, 2);
      cairo_move_to(cr, 7.5, 5);
      cairo_line_to(cr, 7.5, 19);
      cairo_move_to(cr, 16.5, 5);
      cairo_line_to(cr, 16.5, 19);
      cairo_move_to(cr, 3, 12);
      cairo_line_to(cr, 7.5, 12);
      cairo_move_to(cr, 16.5, 12);
      cairo_line_to(cr, 21, 12);
      cairo_stroke(cr);
      break;
    case StashIcon::Performers:  // two people
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
      cairo_stroke(cr);
      break;
    case StashIcon::Studios:  // a building
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
      cairo_stroke(cr);
      break;
    case StashIcon::Pin:  // a pushpin
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
      cairo_stroke(cr);
      break;
    case StashIcon::Shuffle:  // two arrows crossing
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
      cairo_stroke(cr);
      break;
    case StashIcon::Filters:  // three sliders
      for (const auto& [y, knob] : {std::pair{6.0, 15.0}, {12.0, 8.0}, {18.0, 13.0}}) {
        cairo_move_to(cr, 3, y);
        cairo_line_to(cr, 21, y);
        cairo_stroke(cr);
        cairo_arc(cr, knob, y, 2.6, 0, 2 * M_PI);
        cairo_fill(cr);
      }
      break;
    case StashIcon::Ban:  // ⊘
      cairo_arc(cr, 12, 12, 8.5, 0, 2 * M_PI);
      cairo_move_to(cr, 6, 18);
      cairo_line_to(cr, 18, 6);
      cairo_stroke(cr);
      break;
    case StashIcon::Undo:  // an arrow turning back
      cairo_move_to(cr, 8, 4);
      cairo_line_to(cr, 4, 8);
      cairo_line_to(cr, 8, 12);
      cairo_move_to(cr, 4, 8);
      cairo_line_to(cr, 14, 8);
      cairo_curve_to(cr, 23, 8, 23, 20, 14, 20);
      cairo_line_to(cr, 8, 20);
      cairo_stroke(cr);
      break;
    case StashIcon::Back:
      cairo_move_to(cr, 15, 5);
      cairo_line_to(cr, 8, 12);
      cairo_line_to(cr, 15, 19);
      cairo_stroke(cr);
      break;
    case StashIcon::None:
      break;
  }
  cairo_restore(cr);
}

// A scene's picture, as the file browser's (Menu::pictureOf()), and on a
// tile the studio's logo over its top right corner.
void StashPages::drawPicture(const Menu& m, cairo_t* cr, const Button& b, double x, double y,
                             double w, double h, double radius) {
  if (!b.thumbPath.empty()) {
    m.drawThumbnail(cr, b.thumbPath, x, y, w, h, radius);
    if (b.stashLook == static_cast<int>(StashLook::Tile))
      m.drawStudioMark(cr, b.thumbPath, x, y, w, h);
    return;
  }
  roundedRect(cr, x, y, w, h, radius);
  cairo_set_source_rgba(cr, 1, 1, 1, 0.06);
  cairo_fill(cr);
  if (b.icon != Menu::Icon::None)
    Menu::drawIcon(cr, b.icon, x + w / 2, y + h / 2, std::min(96.0, h * 0.6), 0.85, 0.85, 0.88);
}

void StashPages::drawCaption(const Menu& m, cairo_t* cr, const std::string& caption, double x,
                             double y) {
  if (caption.empty()) return;
  const double w = m.textWidth(caption, 20, true) + 24, h = 34;
  roundedRect(cr, x, y, w, h, 12);
  cairo_set_source_rgba(cr, 0, 0, 0, 0.62);
  cairo_fill(cr);
  m.drawText(cr, caption, x, y, w, h, 20, true, true, 1, 1, 1);
}

void StashPages::drawPlays(const Menu& m, cairo_t* cr, const std::string& plays, double right,
                           double bottom) {
  if (plays.empty()) return;
  const double w = m.textWidth(plays, 24, false) + 58, h = 46;
  const double x = right - w, y = bottom - h;
  roundedRect(cr, x, y, w, h, 16);
  cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
  cairo_fill(cr);
  Menu::drawIcon(cr, Menu::Icon::Play, x + 22, y + h / 2, 22, 1, 1, 1);
  m.drawText(cr, plays, x + 40, y, w - 46, h, 24, false, false, 1, 1, 1);
}

void StashPages::drawButton(const Menu& m, cairo_t* cr, const Button& b, const double ink[4]) {
  const Palette& pal = m.palette();
  const double dim[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a};
  const double white[4] = {1, 1, 1, 1};
  switch (static_cast<StashLook>(b.stashLook)) {
    case StashLook::Tile: {
      const double tw = b.w - 16, th = tw * 9 / 16, tx = b.x + 8, ty = b.y + 8;
      drawPicture(m, cr, b, tx, ty, tw, th, 10);
      drawCaption(m, cr, b.caption, tx + 8, ty + 8);  // "VR · 8K"
      m.drawCoverStrip(cr, b, tx, ty, tw, th);  // under the stars, and the plays
      m.drawOfflineState(cr, b, tx, ty, tw, th);
      const double textX = b.x + 14, textW = b.w - 28;
      m.drawTextLines(cr, b.label, textX, ty + th + 4, textW, 26, 1, ink, false);
      const double light[4] = {0.88, 0.88, 0.9, 1};
      m.drawTextLines(cr, b.detail, textX, ty + th + 40, textW, 22, 1, light, false);
      // A downloaded scene's tick starts the last line.
      const double tick = m.downloadedMark(b.offlinePath) ? 30 : 0;
      if (tick > 0) m.drawOfflineTick(cr, textX + 11, ty + th + 85, 22);
      m.drawTextLines(cr, b.date, textX + tick, ty + th + 70, textW - tick, 22, 1, dim, false);
      if (b.selected) {  // the playing file: a blue outline
        roundedRect(cr, b.x + 1.5, b.y + 1.5, b.w - 3, b.h - 3, kButtonRadius);
        setColor(cr, pal.edge);
        cairo_set_line_width(cr, 3);
        cairo_stroke(cr);
      }
      return;
    }
    case StashLook::Row: {
      const double th = b.h - 4, tw = m.listThumbW(b), tx = b.x + 10, ty = b.y + 2;
      drawPicture(m, cr, b, tx, ty, tw, th, 5);
      m.drawOfflineState(cr, b, tx, ty, tw, th);
      const SceneColumns c = sceneColumns(b.x, b.x + b.w);
      if (m.downloadedMark(b.offlinePath))
        m.drawOfflineTick(cr, c.status + kStatusColumnW / 2, b.y + b.h / 2, 22);
      const double nameX = tx + tw + 16, nameW = c.status - 16 - nameX;
      double titleW = m.textWidth(b.label, Menu::kTextSize, false) + 4;
      if (!b.detail.empty()) titleW = std::min(titleW, nameW * 0.65);
      titleW = std::min(titleW, nameW);
      m.drawText(cr, b.label, nameX, b.y, titleW, b.h, Menu::kTextSize, false, false, ink[0],
                 ink[1], ink[2]);
      if (!b.detail.empty() && nameW - titleW > 60)
        m.drawText(cr, b.detail, nameX + titleW + 16, b.y, nameW - titleW - 16, b.h, 24, false,
                   false, dim[0], dim[1], dim[2]);
      const double playsW = m.textWidth(b.plays, 24, false),
                   lengthW = m.textWidth(b.caption, 24, false);
      m.drawText(cr, b.plays, c.plays + kPlaysColumnW - playsW, b.y, playsW + 2, b.h, 24, false,
                 false, dim[0], dim[1], dim[2]);
      if (c.studio < c.released)
        m.drawText(cr, b.size, c.studio, b.y, c.released - 20 - c.studio, b.h, 24, false, false,
                   dim[0], dim[1], dim[2]);
      m.drawText(cr, b.date, c.released, b.y, c.length - 20 - c.released, b.h, 24, false, false,
                 dim[0], dim[1], dim[2]);
      m.drawText(cr, b.caption, b.x + b.w - 16 - lengthW, b.y, lengthW + 2, b.h, 24, false, false,
                 dim[0], dim[1], dim[2]);
      return;
    }
    case StashLook::Facet: {
      // Included: filled blue; left out: outlined red with ⊘ before the name.
      roundedRect(cr, b.x, b.y, b.w, b.h, b.radius);
      if (b.on) {
        setColor(cr, pal.accent);
        cairo_fill(cr);
      } else if (b.dim) {
        cairo_set_source_rgba(cr, 0, 0, 0, 0.25);
        cairo_fill_preserve(cr);
        cairo_new_path(cr);
        roundedRect(cr, b.x + 1.25, b.y + 1.25, b.w - 2.5, b.h - 2.5, b.radius);
        setColor(cr, kOutFill);
        cairo_set_line_width(cr, 2.5);
        cairo_stroke(cr);
      } else {
        setColor(cr, pal.button);
        cairo_fill(cr);
      }
      double x = b.x + 16;
      const double* text = b.dim ? nullptr : white;
      const double out[4] = {kOutText.r, kOutText.g, kOutText.b, 1};
      if (b.dim) {
        drawIcon(cr, StashIcon::Ban, x + 12, b.y + b.h / 2, 24, out);
        x += 34;
        text = out;
      }
      const double countW = b.size.empty() ? 0 : m.textWidth(b.size, 22, false) + 8;
      m.drawText(cr, b.label, x, b.y, b.x + b.w - 14 - countW - x, b.h, Menu::kTextSize, false,
                 false, text[0], text[1], text[2]);
      if (!b.size.empty()) {
        const double* n = b.on ? white : dim;
        m.drawText(cr, b.size, b.x + b.w - 14 - countW + 8, b.y, countW, b.h, 22, false, false,
                   n[0], n[1], n[2], b.on ? 0.9 : 1);
      }
      return;
    }
    case StashLook::Exclude: {
      roundedRect(cr, b.x, b.y, b.w, b.h, b.radius);
      if (b.on)
        setColor(cr, kOutFill);
      else
        setColor(cr, pal.button, 0.7);
      cairo_fill(cr);
      drawIcon(cr, StashIcon::Ban, b.x + b.w / 2, b.y + b.h / 2, 28, b.on ? white : dim);
      return;
    }
    case StashLook::Chip: {
      roundedRect(cr, b.x, b.y, b.w, b.h, b.radius);
      if (!b.dim) {
        setColor(cr, pal.accent);
        cairo_fill(cr);
        m.drawText(cr, b.label, b.x + 20, b.y, b.w - 40, b.h, 24, false, true, 1, 1, 1);
        return;
      }
      cairo_set_source_rgba(cr, 0, 0, 0, 0.25);
      cairo_fill(cr);
      roundedRect(cr, b.x + 1.25, b.y + 1.25, b.w - 2.5, b.h - 2.5, b.radius);
      setColor(cr, kOutFill);
      cairo_set_line_width(cr, 2.5);
      cairo_stroke(cr);
      const double out[4] = {kOutText.r, kOutText.g, kOutText.b, 1};
      drawIcon(cr, StashIcon::Ban, b.x + 30, b.y + b.h / 2, 24, out);
      m.drawText(cr, b.label, b.x + 50, b.y, b.w - 70, b.h, 24, false, false, out[0], out[1],
                 out[2]);
      return;
    }
    case StashLook::Link:
      m.drawText(cr, b.label, b.x + 16, b.y, b.w - 32, b.h, Menu::kTextSize, false, false,
                 pal.textAccent.r, pal.textAccent.g, pal.textAccent.b);
      return;
    case StashLook::Action: {
      const StashIcon icon = static_cast<StashIcon>(b.stashIcon);
      if (icon == StashIcon::None) {
        m.drawText(cr, b.label, b.x + 8, b.y, b.w - 16, b.h, Menu::kTextSize, false, true, ink[0],
                   ink[1], ink[2]);
        return;
      }
      drawIcon(cr, icon, b.x + 22 + 14, b.y + b.h / 2, 28, ink);
      m.drawText(cr, b.label, b.x + 66, b.y, b.w - 66 - 22, b.h, Menu::kTextSize, false, false,
                 ink[0], ink[1], ink[2]);
      return;
    }
    case StashLook::Warning: {
      Menu::drawIcon(cr, Menu::Icon::Warning, b.x + 20 + 14, b.y + b.h / 2, 28, 1.0, 0.45, 0.4);
      m.drawText(cr, b.label, b.x + 62, b.y, b.w - 62 - 20, b.h, b.fontSize > 0 ? b.fontSize : Menu::kTextSize,
                 false, false, ink[0], ink[1], ink[2]);
      return;
    }
    case StashLook::Nav: {  // as the navigation's own rows
      const double iconSize = kIconSize * 0.75;
      drawIcon(cr, static_cast<StashIcon>(b.stashIcon), b.x + 35 + iconSize / 2, b.y + b.h / 2,
               iconSize * 0.9, ink);
      const double textX = b.x + 35 + iconSize + 20;
      m.drawText(cr, b.label, textX, b.y, b.w - (textX - b.x) - 20, b.h,
                 b.fontSize > 0 ? b.fontSize : Menu::kTextSize, false, false, ink[0], ink[1],
                 ink[2]);
      return;
    }
    case StashLook::Field:
      if (b.on) {
        roundedRect(cr, b.x + 1.5, b.y + 1.5, b.w - 3, b.h - 3, b.radius);
        setColor(cr, pal.edge);
        cairo_set_line_width(cr, 3);
        cairo_stroke(cr);
      }
      m.drawFieldText(cr, b, ink);
      return;
    case StashLook::Tab:
    case StashLook::TagChip:
    case StashLook::Portrait:
    case StashLook::Go:
      drawSceneButton(m, cr, b, ink);  // menu_stash_scene.cpp
      return;
    case StashLook::PerformerCard:
    case StashLook::CoStar:
      drawPerformerButton(m, cr, b, ink);
      return;
    case StashLook::Studio:
      drawStudioCard(m, cr, b, ink);
      return;
    case StashLook::Shelf:
      drawShelf(m, cr, b, ink);
      return;
    case StashLook::None:
      return;
  }
}
