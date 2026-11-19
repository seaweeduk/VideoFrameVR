#include "menu_style.h"

#include "menu_list_parts.h"
#include "menu_stash.h"

// Drawing Stash's buttons (StashLook): a scene row's columns, the rail's and
// full lists' rows with their ⊘ targets, the filter chips, links, buttons
// with icons, and their icons.

namespace {

// Left out: red text on a faint red, as the strip's red label.
const Rgba kOutText = hexColour(0xff8a8d);

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
    // Drawn as the menu's own icons, at Stash's pages' stroke.
    case StashIcon::Stash:
      Menu::iconPath24(cr, Menu::Icon::Layers);
      cairo_stroke(cr);
      break;
    case StashIcon::Scenes:
      Menu::iconPath24(cr, Menu::Icon::Film);
      cairo_stroke(cr);
      break;
    case StashIcon::Performers:
      Menu::iconPath24(cr, Menu::Icon::People);
      cairo_stroke(cr);
      break;
    case StashIcon::Studios:
      Menu::iconPath24(cr, Menu::Icon::Building);
      cairo_stroke(cr);
      break;
    case StashIcon::Pin:
      Menu::iconPath24(cr, Menu::Icon::Pin);
      cairo_stroke(cr);
      break;
    case StashIcon::Shuffle:
      Menu::iconPath24(cr, Menu::Icon::Shuffle);
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

void StashPages::drawButton(const Menu& m, cairo_t* cr, const Button& b, const double ink[4]) {
  const Palette& pal = m.palette();
  const double dim[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, pal.textDim.a};
  switch (static_cast<StashLook>(b.stashLook)) {
    case StashLook::SceneColumns: {
      const SceneColumns c = sceneColumns(m, b.x + b.w);
      if (m.downloadedMark(b.offlinePath))
        m.drawOfflineTick(cr, c.status + kStatusColumnW / 2, b.y + b.h / 2, 22);
      constexpr int size = 24;
      const double playsW = m.textWidth(b.plays, size, false),
                   lengthW = m.textWidth(b.caption, size, false);
      m.drawText(cr, b.plays, c.plays + kPlaysColumnW - playsW, b.y, playsW + 2, b.h, size, false,
                 false, dim[0], dim[1], dim[2]);
      m.drawText(cr, b.date, c.released, b.y, c.length - kColumnGap - c.released, b.h, size, false,
                 false, dim[0], dim[1], dim[2]);
      m.drawText(cr, b.caption, c.length + kLengthColumnW - lengthW, b.y, lengthW + 2, b.h, size,
                 false, false, dim[0], dim[1], dim[2]);
      return;
    }
    case StashLook::Facet: {
      // Its name (left out: red after ⊘, on a faint red), its count dim at
      // the right end.
      const double out[4] = {kOutText.r, kOutText.g, kOutText.b, 1};
      const double* text = ink;
      double x = b.x + kit::kSpace;
      if (b.dim) {
        roundedRect(cr, b.x, b.y, b.w, b.h, b.radius);
        setColor(cr, pal.danger, 0.16);
        cairo_fill(cr);
        drawIcon(cr, StashIcon::Ban, x + 11, b.y + b.h / 2, 22, out);
        x += 22 + kit::kSpaceTight + 2;
        text = out;
      }
      const double countW = b.size.empty() ? 0 : m.textWidth(b.size, kCountSize, false);
      const double right = b.x + b.w - kit::kSpace;
      m.drawText(cr, b.label, x, b.y, right - (countW > 0 ? countW + kit::kSpace : 0) - x, b.h,
                 kit::kRowSize, false, false, text[0], text[1], text[2]);
      if (countW > 0) {
        const double* n = b.latched ? ink : dim;
        m.drawText(cr, b.size, right - countW, b.y, countW + 2, b.h, kCountSize, false, false, n[0],
                   n[1], n[2]);
      }
      return;
    }
    case StashLook::Exclude: {
      const double out[4] = {kOutText.r, kOutText.g, kOutText.b, 1};
      if (b.on) {
        roundedRect(cr, b.x, b.y, b.w, b.h, b.radius);
        setColor(cr, pal.danger, 0.16);
        cairo_fill(cr);
      }
      drawIcon(cr, StashIcon::Ban, b.x + b.w / 2, b.y + b.h / 2, 24, b.on ? out : dim);
      return;
    }
    case StashLook::Chip: {
      // Included: its label (latched, in the accent); left out: ⊘ and its
      // label in red on a faint red.
      if (!b.dim) {
        m.drawText(cr, b.label, b.x + kit::kSpace, b.y, b.w - 2 * kit::kSpace, b.h, b.fontSize,
                   false, true, ink[0], ink[1], ink[2]);
        return;
      }
      const double out[4] = {kOutText.r, kOutText.g, kOutText.b, 1};
      roundedRect(cr, b.x, b.y, b.w, b.h, b.radius);
      setColor(cr, pal.danger, 0.16);
      cairo_fill(cr);
      const double x = b.x + kit::kSpace;
      drawIcon(cr, StashIcon::Ban, x + 11, b.y + b.h / 2, 22, out);
      m.drawText(cr, b.label, x + 30, b.y, b.x + b.w - kit::kSpace - x - 30, b.h, b.fontSize, false,
                 false, out[0], out[1], out[2]);
      return;
    }
    case StashLook::Link:
      m.drawText(cr, b.label, b.x + kit::kSpace, b.y, b.w - 2 * kit::kSpace, b.h, kit::kRowSize,
                 false, false, pal.textAccent.r, pal.textAccent.g, pal.textAccent.b);
      return;
    case StashLook::Action: {
      const StashIcon icon = static_cast<StashIcon>(b.stashIcon);
      const int size = b.fontSize > 0 ? b.fontSize : kit::kRowSize;
      if (icon == StashIcon::None) {
        m.drawText(cr, b.label, b.x, b.y, b.w, b.h, size, false, true, ink[0], ink[1], ink[2]);
        return;
      }
      const ActionSpace a = actionSpace(b.h);
      drawIcon(cr, icon, b.x + a.pad + a.icon / 2, b.y + b.h / 2, a.icon, ink);
      const double textX = b.x + a.pad + a.icon + a.gap;
      m.drawText(cr, b.label, textX, b.y, b.x + b.w - a.pad - textX + 4, b.h, size, false, false,
                 ink[0], ink[1], ink[2]);
      return;
    }
    case StashLook::Warning: {
      Menu::drawIcon(cr, Menu::Icon::Warning, b.x + 20 + 14, b.y + b.h / 2, 28, 1.0, 0.45, 0.4);
      m.drawText(cr, b.label, b.x + 62, b.y, b.w - 62 - 20, b.h, b.fontSize > 0 ? b.fontSize : Menu::kTextSize,
                 false, false, ink[0], ink[1], ink[2]);
      return;
    }
    case StashLook::Field:
      // As the shared fields: an accent ring inside its edge while typed
      // into; a search's icon before its text, which shows even while
      // empty (its example then).
      if (b.on) {
        roundedRect(cr, b.x + 1, b.y + 1, b.w - 2, b.h - 2, std::max(0.0, b.radius - 1));
        setColor(cr, pal.accent);
        cairo_set_line_width(cr, 2);
        cairo_stroke(cr);
      }
      if (b.icon != Menu::Icon::None) {
        const double size = kIconSize * 0.75;
        Menu::drawIcon(cr, b.icon, b.x + 35 + size / 2, b.y + b.h / 2, size, dim[0], dim[1], dim[2]);
      }
      m.drawFieldText(cr, b, ink);
      return;
    case StashLook::Tab:
    case StashLook::TagChip:
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
