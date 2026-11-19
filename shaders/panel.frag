#version 450
#extension GL_GOOGLE_include_directive : require

#include "panel.glsl"

layout(set = 0, binding = 0) uniform sampler2D tex;  // sRGB: sampling returns linear colour

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

// Which pipeline this is (Pipelines::kPanel...): 0 plain, the texture as it
// is, for every panel draw but the one with a pointed control; 1 pointed,
// for that one; 2 faded, the browser while a value is held; 3 pointed with
// stars, for an unrated list row pointed at. On the Frame, code a draw never
// runs still slows it (the subtitles and the strip are drawn every frame),
// so the plain pipeline has none of the rest, and the pointed one no stars.
layout(constant_id = 0) const int kLook = 0;

// Signed distance to a rounded rectangle (negative inside).
float roundedRectDistance(vec2 p, vec4 rect, float radius) {
  vec2 center = 0.5 * (rect.xy + rect.zw);
  vec2 q = abs(p - center) - 0.5 * (rect.zw - rect.xy) + radius;
  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

// How far beyond its box the pointed control's shadow reaches (layout pixels).
const float kReach = 24.0;

// A lit line inside the pointed control's edge, as soft as the finishes'
// (menu_finish.cpp, kLitLine: two layout pixels, 0.8 of its strength at
// most): kLitPeak of it, gone a layout pixel either side of kLitDepth inside
// the edge.
const float kLitPeak = 0.8, kLitDepth = 1.75;

// Signed distance to a five-pointed star with its top point up (in y up):
// outer radius r, inner radius r * inner.
float starDistance(vec2 p, float r, float inner) {
  const vec2 k1 = vec2(0.809016994375, -0.587785252292);
  const vec2 k2 = vec2(-k1.x, k1.y);
  p.x = abs(p.x);
  p -= 2.0 * max(dot(k1, p), 0.0) * k1;
  p -= 2.0 * max(dot(k2, p), 0.0) * k2;
  p.x = abs(p.x);
  p.y -= r;
  vec2 ba = inner * vec2(-k1.y, k1.x) - vec2(0.0, 1.0);
  float h = clamp(dot(p, ba) / dot(ba, ba), 0.0, r);
  return length(p - ba * h) * sign(p.y * ba.x - p.x * ba.y);
}

// An unrated list row's five stars, shown only while it is pointed at: the
// menu's empty star icon (Menu::drawRating(), kRowStarSize in cells of
// kRowStarW from x0) outlined; how much of it covers p.
float rowStars(vec2 p, float x0, float cy) {
  const float cell = 28.0, unit = 26.0 / 24.0;  // the icon is drawn in 24 units
  float i = floor((p.x - x0) / cell);
  if (i < 0.0 || i > 4.0 || abs(p.y - cy) > 14.0) return 0.0;
  vec2 q = p - vec2(x0 + cell * (i + 0.5), cy + 0.8 * unit);
  float d = starDistance(vec2(q.x, -q.y), 9.5 * unit, 4.2 / 9.5);
  return 1.0 - smoothstep(-0.5, 0.5, abs(d) - 0.9 * unit);
}

// The control pointed at (or pressed), over the canvas that shows it at rest:
// its inside lit or tinted (hoverFill), a lit line along its top edge, and
// a soft shadow under it, or when pressed one inside its top edge (the
// menu's finishes, menu_finish.h). Only near the control; elsewhere the
// canvas as it is. Blended in sRGB, as Cairo drew the canvas. p: the
// fragment in layout pixels; rect, radius: the control as shown.
vec4 pointed(vec4 c, uint flags, vec2 p, vec4 rect, float radius) {
  float d = roundedRectDistance(p, rect, radius);
  if (d > kReach) return c;
  // A row partly scrolled out of its list shows only where the row shows.
  vec4 clip = pc.draw.hoverClip;
  float outside = max(max(clip.x - p.x, p.x - clip.z), max(clip.y - p.y, p.y - clip.w));
  float shown = 1.0 - smoothstep(-1.0, 0.0, outside);
  float inside = (1.0 - smoothstep(-0.5, 0.5, d)) * shown;
  float strength = float((flags >> 16) & 255u) / 255.0;
  // Premultiplied sRGB.
  float a = c.a;
  vec3 s = a > 0.0 ? linearToSrgb(c.rgb / a) * a : vec3(0.0);
  float ink = 0.0, white = float(flags >> 24) / 255.0;
  if (white > 0.0 && a > 0.0)
    ink = smoothstep(0.55, 0.75, dot(s / a, vec3(0.2126, 0.7152, 0.0722))) * inside * white;
  if ((flags & kHoverPressed) != 0u) {
    // Sunk: a shadow inside the top edge, and a faint dark ring.
    float below = roundedRectDistance(p - vec2(0.0, 2.0), rect, radius);
    float sunk = 0.45 * smoothstep(-5.0, 5.0, below) + 0.12 * smoothstep(-1.5, -0.5, d);
    s *= 1.0 - min(sunk * strength, 1.0) * inside;
  } else if (strength > 0.0) {
    // Raised a step: a close dark shadow and a wide soft one, outside the box.
    float near = 0.32 * (1.0 - smoothstep(-3.0, 3.0, roundedRectDistance(p - vec2(0.0, 2.0), rect, radius)));
    float far = 0.30 * (1.0 - smoothstep(-16.0, 16.0, roundedRectDistance(p - vec2(0.0, 6.0), rect, radius)));
    float shade = (1.0 - (1.0 - near) * (1.0 - far)) * strength * (1.0 - inside) * shown;
    s *= 1.0 - shade;
    a += shade * (1.0 - a);
  }
  if (inside > 0.0) {
    uint packed = pc.draw.hoverFill;
    vec4 fill = vec4(uvec4(packed, packed >> 8, packed >> 16, packed >> 24) & 255u) / 255.0;
    // Lit from above: more of it at the top than at the bottom.
    float t = clamp((p.y - rect.y) / max(rect.w - rect.y, 1.0), 0.0, 1.0);
    float amount = min(fill.a * (1.2 - 0.4 * t), 1.0) * inside;
    if ((flags & kHoverRing) != 0u)  // as a lit line inside the edge
      amount *= kLitPeak * (1.0 - smoothstep(kLitDepth - 0.5, kLitDepth + 0.5, -d));
    amount *= 1.0 - ink;
    s = fill.rgb * amount + s * (1.0 - amount);
    a = amount + a * (1.0 - amount);
    // The lit top edge: what the box moved down kLitDepth doesn't cover of it.
    float highlight = float((flags >> 8) & 255u) / 255.0;
    if (highlight > 0.0) {
      float edge = highlight * kLitPeak * inside *
                   smoothstep(-0.5, 0.5, roundedRectDistance(p - vec2(0.0, kLitDepth), rect, radius));
      s = vec3(edge) + s * (1.0 - edge);
      a = edge + a * (1.0 - edge);
    }
    s = mix(s, vec3(a), ink);  // text and icons white
  }
  if (kLook == 3) {
    // Its empty stars, as faint as the menu's (kStarEmpty at 75 %), as far
    // as the hover has eased in.
    float eased = float((flags >> 3) & 31u) / 31.0;
    float star = 0.75 * eased * rowStars(p, pc.draw.shape.w, 0.5 * (rect.y + rect.w)) * shown;
    s = vec3(0.59, 0.59, 0.62) * star + s * (1.0 - star);
    a = star + a * (1.0 - star);
  }
  return a > 0.0 ? vec4(srgbToLinear(s / a) * a, a) : vec4(0.0);
}

// A value held: the panel fades away by `fade` but what stays, over a soft
// shadow: Magnify's scale (scale, its corners scaleRadius) while it is
// open, which carries the value's name and number, so the row under it
// fades with the rest; else (none: empty) the held row (rect, radius), on
// a surface of its own: a little lighter, lit along its top (the menu's
// Pop finish). Its outline against the video is smoothed across `aa` either
// side (layout pixels): at least half a layout pixel, else half of a
// fragment, which spans more of them in the Frame's coarser foveated
// fragments.
vec4 held(vec4 c, vec2 p, float fade, vec4 rect, float radius, vec4 scale, float scaleRadius, float aa) {
  bool hasScale = scale.z > scale.x;
  float d = hasScale ? roundedRectDistance(p, scale, scaleRadius) : roundedRectDistance(p, rect, radius);
  float keep = 1.0 - smoothstep(-aa, aa, d);
  if (!hasScale) {
    float row = keep * fade;
    // Pop's colour over the panel's (linear), and its lit top edge, as soft
    // as the finishes' (kLitPeak, kLitDepth).
    c.rgb += vec3(0.0065, 0.009, 0.013) * c.a * row;
    float top = kLitPeak *
                smoothstep(-aa, aa, roundedRectDistance(p - vec2(0.0, kLitDepth), rect, radius));
    c.rgb += vec3(0.02) * top * row;  // about white at 8 % over the dark row
  }
  c = c * mix(1.0 - fade, 1.0, keep);
  // The shadow, a little below what stays.
  vec2 below = p - vec2(0.0, 6.0);
  float ds = hasScale ? roundedRectDistance(below, scale, scaleRadius) : roundedRectDistance(below, rect, radius);
  float shade = 0.45 * (1.0 - smoothstep(-8.0, 24.0, ds)) * fade * (1.0 - keep);
  return vec4(c.rgb, c.a + shade * (1.0 - c.a));
}

void main() {
  if (kLook == 0) {
    fragColor = texture(tex, vUV, pc.draw.lodBias) * pc.draw.opacity;  // premultiplied alpha
    return;
  }
  if (kLook == 2) {
    vec2 at = vUV * vec2(textureSize(tex, 0)) / pc.draw.shape.y;  // layout pixels
    float aa = max(0.5, 0.5 * length(fwidth(at)));
    fragColor = held(texture(tex, vUV, pc.draw.lodBias), at, pc.draw.shape.w, pc.draw.hover,
                     pc.draw.shape.z, pc.draw.hoverClip, float((pc.draw.flags >> 8) & 255u), aa) *
                pc.draw.opacity;
    return;
  }
  // Pressed here: rows of lists and controls that draw themselves, which
  // only sink. Buttons are drawn pushed in by the menu (Menu::pressDrawn()).
  vec2 p = vUV * vec2(textureSize(tex, 0)) / pc.draw.shape.y;  // layout pixels
  fragColor = pointed(texture(tex, vUV, pc.draw.lodBias), pc.draw.flags, p, pc.draw.hover, pc.draw.shape.z) *
              pc.draw.opacity;
}
