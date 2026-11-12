#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_control_flow_attributes : require
#ifdef TP10
#extension GL_EXT_nonuniform_qualifier : require
#endif

// The video around the viewer (180°/360°, fisheye, equi-angular cubemap) or
// the room-fixed sky, computed per pixel from the view direction.
//
// Performance note (Steam Frame, Adreno 750): this shader runs as 128-wide
// waves and is limited by texture fetch. A general pipeline (9 registers,
// 10 waves per SP, about 2 ms per 3456² eye from an 8K frame) also runs
// what only its rarer modes need, even for one tap: six coarse derivatives
// with their seam choice, and the choices of source, filter, look and cut.
// The common modes get pipelines of their own (the constants below) with
// none of that: 6 registers, 16 waves, about 1.3 ms. Their picture can
// differ from the general one's by a level on a few pixels. Two more things
// stop waves that instruction counts and desktop timings don't show: where
// the per-pixel path sits inside the binary (instruction cache misses: the
// same code 512 bytes further in ran up to 2.3x slower; moving the whole
// binary changes nothing), and executed branches on draw values, which some
// schedules make cost up to 0.2 ms each. So keep other modes' code and
// draw-value branches out of the common path. Time every edit of this file
// on the Frame against master (8K RGBA, P010 and NV12; with and without a
// packed mask): misses above 0 or many more end-of-wave cycles mean it
// landed in a slow layout.

#include "video.glsl"
#include "projection.glsl"

struct SurroundDraw {
  // xyz: columns of the rotation from local space into the picture's axes;
  // w: the colour balance's red, green and blue gains minus 1 (balanced()).
  vec4 toPicture0;
  vec4 toPicture1;
  vec4 toPicture2;
  vec4 params;  // x fisheye field of view (radians), y zoom (> 1 magnifies); zw Kr, Kb (kYuv)
  vec4 adjust;  // brightness, contrast, saturation (-1..1), w sharpening strength (0 = off)
  // Green screen: x = how green is cut away, y = half its soft edge, z =
  // spill removal 0..2, w = shrink: how many texels the cut reaches past
  // what looks green. A flat background's colour key: x, y its cut and
  // soft edge (colourAlpha()), zw the part of its mask's image the frame's
  // mask fills.
  vec4 chroma;
  vec4 alpha;   // packed alpha: xy = each eye's mask size, in parts of the frame;
                // zw the chroma lookup's offset (kYuv)
  uvec4 mode;   // x projection: 0 = 180, 1 = 360, 2 = fisheye, 3 = equi-angular
                // cubemap, 4 = cubemap turned to a 2x3 grid; y flags (video.glsl);
                // z footprint taps per side at most (footprintSample); w opacity
                // percent
  vec4 yuv;     // kYuv: the planes' range (videoTap)
};
DECLARE_PUSH(SurroundDraw)
uint drawFlags() { return pc.draw.mode.y; }

// The pipeline's mode (Pipelines::surround). A projection of -1 makes a
// general pipeline, which takes everything else from the draw too (with
// mipmaps, footprint taps, the tone look, sharpening and the rarer cases);
// otherwise a common mode, which samples the full-size level with one tap.
// 0: 180 or 360, 1: fisheye, 2: either cubemap; 3 (the COLOUR_KEY build): 180,
// 360 or fisheye.
layout(constant_id = 0) const int kProjection = -1;
// What tex holds (video.glsl's kSource...); -1: an image or planes, from the
// draw (general pipelines only).
layout(constant_id = 1) const int kSource = -1;
// 0: no packed mask or green screen, 1: the packed mask (looked up first,
// with transparent output in a common mode), 2: a green screen without
// shrink (common modes only), 3: as 1 with its blue second layer shown, 4: a
// flat background's colour key (the COLOUR_KEY build alone: no other
// pipeline's shader has its code), 5: as 2 with shrink (common modes only).
layout(constant_id = 2) const int kSeeThrough = -1;
layout(constant_id = 3) const bool kSharpen = false;  // general pipelines: sharpening, for a strength above 0
// Common modes: the colour balance (general pipelines always apply it).
layout(constant_id = 4) const bool kBalance = false;
// Common modes: the tone look, always with kBalance (general pipelines take
// the look from the draw).
layout(constant_id = 5) const bool kToneLook = false;
const bool kCommon = kProjection >= 0;
const bool kMaskRedBlue = kSeeThrough == 3;
const bool kMaskFirst = kSeeThrough == 1 || kMaskRedBlue;
const bool kShrink = kSeeThrough == 5;
// The colour key's common modes, whose key reads a decoded frame's display
// values as they came (main()). The green screen's key does not: its spill
// removal converts the same picture with linearToSrgb() anyway, and the key
// shares that.
#ifdef COLOUR_KEY
const bool kKeyDisplay = kCommon;
#else
const bool kKeyDisplay = false;
#endif

#ifdef TP10
#define tex tp10Bands[0]  // a Q10C frame's luma (video.glsl), always kYuv
#else
layout(set = 0, binding = 0) uniform sampler2D tex;
#endif
layout(set = 0, binding = 1) uniform sampler2D chromaTex;  // kYuv

#ifdef TP10
int source() { return kSourcePlanes; }
#else
int source() {
  if (kSource >= 0) return kSource;
  return (pc.draw.mode.y & kYuv) != 0u ? kSourcePlanes : kSourceImage;
}
#endif
bool isYuv() { return source() != kSourceImage; }
float opacity() { return float(pc.draw.mode.w) * 0.01; }
vec4 yuvMatrix() { return vec4(pc.draw.params.zw, pc.draw.alpha.zw); }
// The picture at p (texture coordinates), full-size level, linear colour.
vec3 tap(vec2 p) { return videoTap(tex, chromaTex, p, source(), pc.draw.yuv, yuvMatrix()); }
// A decoded frame's display values at p (videoDisplay), and those values in
// linear colour, as videoTap makes them. The curve's constants come from the
// TV gamma bit, not a test of it: in a branch on that test the compiler would
// fold them and round mix() other than videoTap does.
mediump vec3 display(vec2 p) {
  return videoDisplay(tex, chromaTex, p, source(), pc.draw.yuv, yuvMatrix());
}
vec3 displayLinear(mediump vec3 e) {
  float srgb = float(1u - (drawFlags() / kTvGamma & 1u));
  float a = 0.055 * srgb, toe = 0.04045 * srgb;
  return mix(e / 12.92, pow((e + a) / (1.0 + a), vec3(2.4)), step(vec3(toe), e));
}

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 fragColor;

// The picture at uv (0..1 in this eye's part, kept within lo..hi) as it
// came, sampled with the gradients dx, dy: through the mipmaps, or without
// them over the pixel's footprint. A mask or key reads it so; shown()
// finishes it.
vec3 picture(vec2 uv, vec2 lo, vec2 hi, vec2 dx, vec2 dy, EyeData e, uint flags) {
  vec3 c;
  if (kCommon || kSource == kSourceYcbcr || baseLevel(flags)) {
    c = footprintSample(tex, chromaTex, source(), pc.draw.yuv, yuvMatrix(), uv, dx, dy, e.stereo, lo,
                        hi, kCommon ? 1 : int(pc.draw.mode.z));
  } else {
    vec2 eyeUv = e.stereo.zw + clamp(uv, lo, hi) * e.stereo.xy;
    c = textureGrad(tex, eyeUv, dx * e.stereo.xy, dy * e.stereo.xy).rgb;
  }
  return clamp(c, 0.0, 1.0);
}

// Brightness, contrast and saturation (each -1..1) as mpv applies them to a
// video (DirectVideo::computeColor), in display values: contrast scales luma
// and chroma, saturation chroma, brightness adds to all. k: Kr, Kb. tv: the
// display values are BT.1886's 2.4 power (kTvGamma, as videoTap decoded
// them or mpv converted them), else sRGB's curve.
vec3 adjustVideo(vec3 c, vec3 adjust, vec2 k, bool tv) {
  vec3 e = tv ? pow(c, vec3(1.0 / 2.4)) : linearToSrgb(c);
  float y = dot(e, vec3(k.x, 1.0 - k.x - k.y, k.y));
  e = clamp((1.0 + adjust.y) * mix(vec3(y), e, 1.0 + adjust.z) + adjust.x, 0.0, 1.0);
  return tv ? pow(e, vec3(2.4)) : srgbToLinear(e);
}

// The picture as this pixel (at fragCoord) shows it, from picture(): in a
// general pipeline with the picture settings (a photo's as adjustPicture
// applies them; a video's, which C++ passes only while a mask or key reads
// the frame, as mpv does) and the tone look. A common mode has no picture
// settings, and the look only in its pipeline for it (kToneLook).
vec3 shown(vec3 c, EyeData e, uint flags, vec2 fragCoord) {
  if (kCommon) return kToneLook ? clamp(toneLook(c, e, fragCoord), 0.0, 1.0) : c;
  vec3 a = pc.draw.adjust.xyz;
  if ((flags & kPhoto) != 0u) c = adjustPicture(c, a);
  else if (a != vec3(0.0)) c = adjustVideo(c, a, pc.draw.params.zw, (flags & kTvGamma) != 0u);
  return clamp(applyLook(c, e, flags, fragCoord), 0.0, 1.0);
}

// The colour balance (warmth, tint and exposure as gains on the light), the
// last of the picture settings, after sharpening and spill removal; before
// the key colour's cap, which must come last. Gains of 1 (pushed as 0) leave
// the picture as it is, so a general pipeline always applies it; a common
// mode only in its pipelines for it and for the look (kBalanced).
const bool kBalanced = !kCommon || kBalance;
vec3 balanced(vec3 c) {
  vec3 g = vec3(pc.draw.toPicture0.w, pc.draw.toPicture1.w, pc.draw.toPicture2.w);
  return clamp(c + c * g, 0.0, 1.0);
}

// shown() for the picture c and, if sharpened, its neighbours n.
void show(inout vec3 c, inout vec3 n[4], bool sharpened, EyeData e, uint flags) {
  c = shown(c, e, flags, gl_FragCoord.xy);
  if (!kCommon && sharpened) {
    vec2 pixels[4] = vec2[](vec2(-1.0, 0.0), vec2(1.0, 0.0), vec2(0.0, -1.0), vec2(0.0, 1.0));
    for (int i = 0; i < 4; ++i) n[i] = shown(n[i], e, flags, gl_FragCoord.xy + pixels[i]);
  }
}

// DeoVR's packed alpha at uv: each eye's mask, at alpha.xy of the frame,
// sits centred on the middle of the top edge (left eye) or on the corners
// (right eye) and wraps around the frame's edges (into the corners and gaps
// next to the fisheye circles). Its red channel is opacity. A second layer
// in blue (SLR's layered masks) adds to it where shown: where the layers
// meet, the decoder's half-size chroma leaves both a little low for a
// pixel, which their overlap makes up, so no line shows between them.
float packedAlpha(vec2 uv, EyeData e, vec2 dx, vec2 dy) {
  vec2 alphaScale = pc.draw.alpha.xy;
  vec2 alphaCenter = vec2(e.position.w < 0.5 ? 0.5 : 0.0, 0.0);
  vec2 m = uv - 0.5;
  if (length(m) > 0.5) return 0.0;
  vec2 maskUv = fract(alphaCenter + m * alphaScale);
  // Compression leaves black a little above 0 and full red below 1.
  if (kMaskRedBlue) {
    // Display values: a decoded frame's straight from its planes.
    vec3 s = isYuv() ? videoDisplay(tex, chromaTex, maskUv, source(), pc.draw.yuv, yuvMatrix())
                     : linearToSrgb(textureGrad(tex, maskUv, dx * alphaScale, dy * alphaScale).rgb);
    return smoothstep(0.08, 0.92, s.r + s.b + min(s.r, s.b));
  }
  vec3 mask = isYuv() ? tap(maskUv) : textureGrad(tex, maskUv, dx * alphaScale, dy * alphaScale).rgb;
  float red = linearToSrgb(mask).r;
  return smoothstep(0.08, 0.92, red);
}

#ifdef COLOUR_KEY
// The colour key's mask (KeyMask), made from each new frame before the eye
// passes, over the frame at chroma.zw of the image: x the square root of
// the key's distance (as colourAlpha() takes it) the shrink reaches here, y
// how much of the background is near (spill removal's edge).
layout(set = 1, binding = 0) uniform sampler2D keyMask;
#endif

// Where the view direction dir falls in this eye's picture (pictureUv).
vec2 surroundUv(vec3 dir, int projection, out bool inside) {
  inside = true;
  if (kProjection == 0) return equirectUv(dir, projection == 0 ? PI : 2.0 * PI, inside);
  if (kProjection == 1) return fisheyeUv(dir, pc.draw.params.x, inside);
  if (kProjection == 2) return cubemapUv(dir, projection == 4);
#ifdef COLOUR_KEY
  if (kProjection == 3) {
    // 180, 360 or fisheye, both worked out and one taken, without a branch.
    bool fisheyeInside;
    vec2 fisheye = fisheyeUv(dir, pc.draw.params.x, fisheyeInside);
    vec2 equirect = equirectUv(dir, projection == 0 ? PI : 2.0 * PI, inside);
    bool isFisheye = projection == 2;
    inside = isFisheye ? fisheyeInside : inside;
    return isFisheye ? fisheye : equirect;
  }
#endif
  return pictureUv(dir, projection, pc.draw.params.x, inside);
}

void main() {
  EyeData e = currentEye();
  uint flags = pc.draw.mode.y;
  int projection = int(pc.draw.mode.x);
  float zoom = pc.draw.params.y;
  vec2 eyeScale = e.stereo.xy, eyeOffset = e.stereo.zw;

  // The view ray through this pixel; the top row (t.y = 0) looks along the
  // upper edge of the field of view.
  vec2 t = vNdc * 0.5 + 0.5;
  vec3 ray = vec3(mix(e.tangents.x, e.tangents.y, t.x) / zoom,
                  mix(e.tangents.w, e.tangents.z, t.y) / zoom, -1.0);
  mat3 toPicture = mat3(pc.draw.toPicture0.xyz, pc.draw.toPicture1.xyz, pc.draw.toPicture2.xyz);
  vec3 dir = normalize(toPicture * rotateByQuat(e.orientation, ray));
  // Stereo correction (stereo_correction.h): intersect a sphere of the focus
  // distance's radius around this eye's source camera. Both the ray origin
  // and the resulting lookup direction are relative to that camera. A
  // common mode computes it always and selects it rather than branching.
  bool correct = (flags & kCorrect) != 0u && e.viewer.w > 0.0;
  if (kCommon || correct) {
    vec3 viewer = e.viewer.xyz - e.camera.xyz;
    float along = dot(viewer, dir);
    float reach = along * along - dot(viewer, viewer) + e.viewer.w * e.viewer.w;
    vec3 corrected = normalize(viewer + dir * (sqrt(max(reach, 0.0)) - along));
    dir = correct ? corrected : dir;
  }
  bool inside;
  vec2 uv = surroundUv(dir, projection, inside);
  if ((flags & kFlip) != 0u) uv.y = 1.0 - uv.y;

  // Mip selection from derivatives; for 360 pick the version without the
  // jump at the back seam so no line appears there.
  vec2 dx = dFdx(uv), dy = dFdy(uv);
  vec2 shifted = vec2(fract(uv.x + 0.5), uv.y);
  vec2 dxs = dFdx(shifted), dys = dFdy(shifted);
  bool acrossSeam = abs(dxs.x) + abs(dys.x) < abs(dx.x) + abs(dy.x);
  if (acrossSeam) {
    dx = dxs;
    dy = dys;
  }
  // Sharpening's neighbours: one eye pixel away along the same gradients,
  // per pixel rather than per 2x2 block.
  vec2 fineX = dFdxFine(uv), fineY = dFdyFine(uv);
  vec2 fineXs = dFdxFine(shifted), fineYs = dFdyFine(shifted);
  vec2 stepX = acrossSeam ? fineXs : fineX, stepY = acrossSeam ? fineYs : fineY;
  // Cube face edges jump across the frame: through mipmaps the full-size
  // level avoids seams there and roughly matches the headset's resolution
  // for 8K video; without them, taps stay on the face.
  bool cube = kProjection < 0 ? projection >= 3 : kProjection == 2;
  if (cube && !kCommon && (flags & kBaseLevel) == 0u) {
    dx = vec2(0.0);
    dy = vec2(0.0);
  }

  // Bilinear alone reaches half a texel out: keep it inside this eye's part,
  // and a cubemap's face.
  vec2 margin = vec2(0.0005);
  if (kCommon || baseLevel(flags))
    margin = max(margin, 0.5 / (videoSize(tex, chromaTex) * eyeScale));
  vec2 eyeUv = eyeOffset + clamp(uv, margin, 1.0 - margin) * eyeScale;
  vec2 lo = margin, hi = 1.0 - margin;
  if (cube) {
    vec2 grid = projection == 3 ? vec2(3.0, 2.0) : vec2(2.0, 3.0);
    vec2 cell = min(floor(uv * grid), grid - 1.0);
    vec2 texel = 0.5 / (videoSize(tex, chromaTex) * eyeScale);
    lo = cell / grid + texel;
    hi = (cell + 1.0) / grid - texel;
  }
  // Passthrough with a packed mask: the mask first, as it doesn't depend on
  // the picture. Where it cuts everything away the output is fixed, and the
  // picture (most of the cost: its taps and conversion) is never sampled.
  float maskAlpha = kMaskFirst && inside ? packedAlpha(uv, e, dx, dy) : 0.0;
  if (kMaskFirst && maskAlpha == 0.0) {
    fragColor = (flags & kAlphaOut) != 0u ? vec4(0.0) : vec4(kKeyColor, 1.0);
    return;
  }
  // The keys compare display values. The colour key's common modes keep a
  // decoded frame's (d) beside its linear colour, from one lookup, instead
  // of bringing them back with sRGB's curve: the same values but for
  // rounding. Not with TV gamma, whose linear colour sRGB's curve does not
  // bring back to them; the key's input there stays what it was.
  vec3 c;
  vec3 d = vec3(0.0);
  if (kKeyDisplay && isYuv()) {
    d = inside ? display(eyeOffset + clamp(uv, lo, hi) * eyeScale) : vec3(0.0);
    c = inside ? clamp(displayLinear(d), 0.0, 1.0) : vec3(0.0);
    if ((flags & kTvGamma) != 0u) d = linearToSrgb(c);
  } else {
    c = inside ? picture(uv, lo, hi, dx, dy, e, flags) : vec3(0.0);
  }

  // Sharpening: the picture one eye pixel to each side, as those pixels
  // would sample it. Taps stay on this eye's part, a cubemap's face, or wrap
  // around 360 video.
  float strength = kSharpen ? pc.draw.adjust.w : 0.0;
  bool sharpened = strength > 0.0 && inside;
  vec2 nUv[4];
  vec3 n[4];
  if (sharpened) {
    vec2 steps[4] = vec2[](-stepX, stepX, -stepY, stepY);
    for (int i = 0; i < 4; ++i) {
      vec2 p = uv + steps[i];
      if (projection == 1) p.x = fract(p.x);
      p = clamp(p, lo, hi);
      nUv[i] = p;
      n[i] = picture(p, lo, hi, dx, dy, e, flags);
    }
  }

  bool keyGreen = (flags & kKeyGreen) != 0u, alphaOut = (flags & kAlphaOut) != 0u;
  bool packed = kSeeThrough < 0 ? (flags & kPackedAlpha) != 0u : kMaskFirst;
#ifdef COLOUR_KEY
  bool keyed = true;  // kSeeThrough 4: no packed mask
#else
  bool keyed =
      kSeeThrough < 0 ? !packed && (flags & kChromaKey) != 0u : kSeeThrough == 2 || kShrink;
#endif
  if (kSeeThrough == 0 || (kSeeThrough < 0 && !keyGreen && !alphaOut)) {
    // No packed mask or green screen: the picture where there is one, else
    // what the passthrough mode shows the room with.
    vec4 outside = alphaOut ? vec4(0.0) : keyGreen ? vec4(kKeyColor, 1.0) : vec4(0.0, 0.0, 0.0, 1.0);
    show(c, n, sharpened, e, flags);
    if (sharpened) c = sharpen(c, n[0], n[1], n[2], n[3], strength);
    if (kBalanced) c = balanced(c);
    fragColor = inside ? vec4(c, 1.0) * opacity() : outside;
    return;
  }
  // Passthrough: how much of the picture stays here.
  packed = packed && inside;
  keyed = keyed && inside;
  float alpha = inside ? 1.0 : 0.0;
  vec4 chroma = pc.draw.chroma;
#ifdef COLOUR_KEY
  // The colour key: its colour (display values), and its mask here (kept on
  // the part of the image the frame's mask fills).
  vec3 keyRgb = keyColour(pc.draw.alpha.x);
  vec2 faceLo = vec2(0.0), faceHi = vec2(0.0);
  if (cube) {
    // A cubemap's on this face, half a mask texel in (the next face's mask
    // is elsewhere in the frame).
    vec2 grid = projection == 3 ? vec2(3.0, 2.0) : vec2(2.0, 3.0);
    vec2 cell = min(floor(uv * grid), grid - 1.0);
    faceLo = (eyeOffset + cell / grid * eyeScale) * chroma.zw + 0.5 / kKeyMaskSize;
    faceHi = (eyeOffset + (cell + 1.0) / grid * eyeScale) * chroma.zw - 0.5 / kKeyMaskSize;
  }
  vec2 keyAt = textureLod(keyMask,
                          cube ? clamp(eyeUv * chroma.zw, faceLo, faceHi)
                               : min(eyeUv * chroma.zw, chroma.zw - 0.5 / kKeyMaskSize),
                          0.0).rg;
#endif
  if (packed) {
    alpha = kMaskFirst ? maskAlpha : packedAlpha(uv, e, dx, dy);
  } else if (keyed) {
#ifdef COLOUR_KEY
    // This pixel's own distance, or the shrink's from the mask.
    vec3 s = kKeyDisplay && isYuv() ? d : linearToSrgb(c);
    alpha = colourAlpha(min(sqrt(keyDistance2(s, keyRgb)), keyAt.x * keyAt.x), chroma);
#else
    alpha = chromaAlpha(linearToSrgb(c), chroma);
    if (kShrink && alpha > 0.0) {
      // Shrink in its common mode: as below, a tap at a time (a loop: four
      // taps live at once took twice the registers). A decoded frame's taps
      // are its display values as they came, not taken to linear colour and
      // back (the same but for rounding), except with TV gamma (main()).
      vec2 texel = chroma.w / videoSize(tex, chromaTex);
      [[dont_unroll]] for (int i = 0; i < 4; ++i) {
        vec2 o = texel * vec2(i == 0 ? 1.0 : i == 1 ? -1.0 : 0.0, i == 2 ? 1.0 : i == 3 ? -1.0 : 0.0);
        vec3 s;
        if (isYuv()) {
          s = display(eyeUv + o);
          if ((flags & kTvGamma) != 0u) s = linearToSrgb(clamp(displayLinear(s), 0.0, 1.0));
        } else {
          s = linearToSrgb(clamp(tap(eyeUv + o), 0.0, 1.0));
        }
        alpha = min(alpha, chromaAlpha(s, chroma));
      }
    }
    if (!kCommon && chroma.w > 0.0 && alpha > 0.0) {
      // Shrink (general pipelines): cut where any neighbour that far away is
      // cut.
      vec2 texel = chroma.w / videoSize(tex, chromaTex);
      vec2 dxs = dx * eyeScale, dys = dy * eyeScale;
      for (int i = 0; i < 4; ++i) {
        vec2 o = texel * vec2(i == 0 ? 1.0 : i == 1 ? -1.0 : 0.0, i == 2 ? 1.0 : i == 3 ? -1.0 : 0.0);
        vec3 t = clamp(isYuv() ? tap(eyeUv + o) : textureGrad(tex, eyeUv + o, dxs, dys).rgb, 0.0, 1.0);
        alpha = min(alpha, chromaAlpha(linearToSrgb(t), chroma));
      }
    }
#endif
  }
  // Near what is cut away nothing is sharpened, so the cut gets no fringes;
  // the coverage itself never is.
  if (sharpened && (packed || keyed)) {
    bool edge = alpha < 1.0;
    for (int i = 0; i < 4; ++i)
#ifdef COLOUR_KEY
      edge = edge || colourAlpha(sqrt(keyDistance2(linearToSrgb(n[i]), keyRgb)), chroma) < 1.0;
#else
      edge = edge || (packed ? packedAlpha(nUv[i], e, dx, dy)
                             : chromaAlpha(linearToSrgb(n[i]), chroma)) < 1.0;
#endif
    sharpened = !edge;
  }
  // The mask and the key have read the picture as it came; sharpening,
  // spill removal, the colour balance and the key colour's cap act on what
  // is shown.
  show(c, n, sharpened, e, flags);
  if (sharpened) c = sharpen(c, n[0], n[1], n[2], n[3], strength);
#ifdef COLOUR_KEY
  if (keyed && keyAt.y > 0.0 && alpha > 0.0) {
    // The colour key's spill removal, on a pixel tinted towards the key, acts
    // as much as the background is near (the mask's y).
    vec3 s = linearToSrgb(c);
    vec3 axis;
    float excess = keySpill(s, keyRgb, axis);
    if (excess > 0.0) c = srgbToLinear(mix(s, clamp(s - excess * axis, 0.0, 1.0), keyAt.y));
  }
#else
  if (keyed) c = srgbToLinear(removeSpill(linearToSrgb(c), chroma.z));
#endif
  if (kBalanced) c = balanced(c);
  if (!kCommon && keyGreen && (packed || keyed)) {
    // The picture itself must never look like the key colour, or the
    // headset would cut holes into it (a green top, say, or green the
    // colour balance brings back after spill removal): the key channel is
    // capped just above the larger other one, in display values, below what
    // the headset's keyer reacts to. (Key green with a packed mask, or with
    // a green screen and the colour balance, uses the general pipeline.)
    vec3 s = linearToSrgb(c);
    vec3 rest = s * (vec3(1.0) - kKeyColor);
    float keyed = dot(s, kKeyColor);
    s += kKeyColor * (min(keyed, max(max(rest.r, rest.g), rest.b) + 0.02) - keyed);
    c = srgbToLinear(s);
  }
  // The Opacity setting: below 100 only with transparent output.
  alpha *= opacity();
  fragColor = alphaOut ? vec4(c * alpha, alpha) : vec4(mix(kKeyColor, c, alpha), 1.0);
}
