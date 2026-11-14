#include "focus_marker.h"

#include "vr_space.h"

#include <cairo.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr float kRingRadius = 0.0253f;  // per meter of distance: 1.45°, the measured patch
// Nearer than kFullSize meters the marker keeps the size it has there, up to
// kMaxGrowth times its usual angle: at a constant angle a marker on a closeup
// is a ring the size of a coin, with a reading too small to make out.
constexpr float kFullSize = 1.0f;
constexpr float kMaxGrowth = 3.0f;
constexpr float kGazeDotDistance = 2.0f;
// Transparent canvas size per meter of distance; the centre dot is about 0.35° across.
constexpr float kGazeDotSize = 2 * 0.0524f * 64 / 60;

}  // namespace

bool FocusMarker::init(Uploader& uploader) {
  if (!ring_.init(uploader, 128, 128)) return false;
  drawDistance();

  if (!dot_.init(uploader, 128, 128)) return false;
  cairo_t* cr = dot_.cr();
  dot_.clear();
  cairo_arc(cr, 64, 64, 5, 0, 2 * 3.14159265);
  cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
  cairo_fill(cr);
  cairo_arc(cr, 64, 64, 3.5, 0, 2 * 3.14159265);
  cairo_set_source_rgba(cr, 0.2, 0.9, 1.0, 1.0);
  cairo_fill(cr);
  dot_.upload();
  return true;
}

void FocusMarker::drawDistance() {
  cairo_t* cr = ring_.cr();
  ring_.clear();
  cairo_arc(cr, 64, 64, 56, 0, 2 * 3.14159265);
  cairo_set_line_width(cr, 10);
  cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
  cairo_stroke_preserve(cr);
  cairo_set_line_width(cr, 5);
  cairo_set_source_rgba(cr, 1.0, 0.85, 0.2, 1.0);
  cairo_stroke(cr);
  if (!distanceText_.empty()) {
    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* font = pango_font_description_from_string("Sans Bold");
    pango_font_description_set_absolute_size(font, 22 * PANGO_SCALE);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, distanceText_.c_str(), -1);
    PangoRectangle ink;
    pango_layout_get_pixel_extents(layout, &ink, nullptr);
    if (ink.width > 108) {
      pango_font_description_set_absolute_size(font, 22.0 * 108 / ink.width * PANGO_SCALE);
      pango_layout_set_font_description(layout, font);
      pango_layout_get_pixel_extents(layout, &ink, nullptr);
    }
    // Centre the visible glyphs, not the subtitle canvas's bottom baseline.
    cairo_move_to(cr, 64 - ink.width * 0.5 - ink.x, 64 - ink.height * 0.5 - ink.y);
    pango_cairo_layout_path(cr, layout);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cr, 3);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.85);
    cairo_stroke_preserve(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    cairo_fill(cr);
    pango_font_description_free(font);
    g_object_unref(layout);
  }
  ring_.upload();
}

void FocusMarker::updateGaze(bool show, Vec3 origin, Vec3 direction, float distance) {
  showDot_ = show;
  if (!show) return;
  distance = distance > 0 ? std::min(distance, 50.0f) : kGazeDotDistance;
  const Vec3 at = origin + direction * distance;
  Mat4 basis;
  if (!facingBasis(direction * -1.0f, {0, 1, 0}, basis)) basis = Mat4{};
  const float d = kGazeDotSize * distance;
  dotModel_ = translation(at) * basis * scaling(d, d, 1);
}

void FocusMarker::update(bool show, Vec3 headCenter, Quat head, Vec3 direction, float distance,
                         const std::string& distanceText) {
  show_ = show && distance > 0;
  if (!show_) return;
  if (distanceText != distanceText_) {
    distanceText_ = distanceText;
    drawDistance();
  }
  distance = std::min(distance, 50.0f);
  const Mat4 facing = rotation(head);
  const Vec3 up = transformDir(facing, {0, 1, 0});
  const Vec3 at = headCenter + direction * distance;
  // Facing the head, upright with it.
  Mat4 basis;
  if (!facingBasis(normalize(headCenter - at), up, basis)) basis = facing;
  const float size = std::max(distance, std::min(kFullSize, kMaxGrowth * distance));
  const float r = kRingRadius * size;
  ringModel_ = translation(at) * basis * scaling(2 * r, 2 * r, 1);
}

void FocusMarker::addTo(std::vector<OverlayDraw>& overlays) {
  auto add = [&](const Texture* texture, const Mat4& model) {
    PanelItem item;
    item.texture = texture;
    item.model = model;
    overlays.push_back(item);
  };
  if (showDot_) add(dot_.texture(), dotModel_);
  if (!show_) return;
  add(ring_.texture(), ringModel_);
}
