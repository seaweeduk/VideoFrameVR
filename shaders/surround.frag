#version 450
#extension GL_GOOGLE_include_directive : require
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
  vec4 toPicture0;  // columns of the rotation from local space into the picture's axes
  vec4 toPicture1;
  vec4 toPicture2;
  vec4 params;  // x fisheye field of view (radians), y zoom (> 1 magnifies); zw Kr, Kb (kYuv)
  vec4 adjust;  // brightness, contrast, saturation (-1..1), w sharpening strength (0 = off)
  // Green screen: x = how green is cut away, y = half its soft edge, z =
  // spill removal 0..2, w = shrink: how many texels the cut reaches past
  // what looks green.
  vec4 chroma;
  vec4 alpha;   // packed alpha: xy = each eye's mask size, in parts of the frame;
                // zw the chroma lookup's offset (kYuv)
  uvec4 mode;   // x projection: 0 = 180, 1 = 360, 2 = fisheye, 3 = equi-angular
                // cubemap, 4 = cubemap turned to a 2x3 grid; y flags (video.glsl);
                // z footprint taps per side at most (footprintSample)
  vec4 yuv;     // kYuv: the planes' range (videoTap)
};
DECLARE_PUSH(SurroundDraw)
uint drawFlags() { return pc.draw.mode.y; }

// The pipeline's mode (Pipelines::surround). A projection of -1 makes a
// general pipeline, which takes everything else from the draw too (with
// mipmaps, footprint taps, the tone look, sharpening and the rarer cases);
// otherwise a common mode, which samples the full-size level with one tap.
layout(constant_id = 0) const int kProjection = -1;  // 0: 180 or 360, 1: fisheye, 2: either cubemap
// What tex holds (video.glsl's kSource...); -1: an image or planes, from the
// draw (general pipelines only).
layout(constant_id = 1) const int kSource = -1;
// 0: no packed mask or green screen, 1: the packed mask (looked up first,
// with transparent output in a common mode), 2: a green screen without
// shrink (common modes only).
layout(constant_id = 2) const int kSeeThrough = -1;
layout(constant_id = 3) const bool kSharpen = false;  // general pipelines: sharpening, for a strength above 0
const bool kCommon = kProjection >= 0;
const bool kMaskFirst = kSeeThrough == 1;

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
vec4 yuvMatrix() { return vec4(pc.draw.params.zw, pc.draw.alpha.zw); }
// The picture at p (texture coordinates), full-size level, linear colour.
vec3 tap(vec2 p) { return videoTap(tex, chromaTex, p, source(), pc.draw.yuv, yuvMatrix()); }

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
// them), else sRGB's curve (mpv's image too).
vec3 adjustVideo(vec3 c, vec3 adjust, vec2 k, bool tv) {
  vec3 e = tv ? pow(c, vec3(1.0 / 2.4)) : linearToSrgb(c);
  float y = dot(e, vec3(k.x, 1.0 - k.x - k.y, k.y));
  e = clamp((1.0 + adjust.y) * mix(vec3(y), e, 1.0 + adjust.z) + adjust.x, 0.0, 1.0);
  return tv ? pow(e, vec3(2.4)) : srgbToLinear(e);
}

// The picture as this pixel (at fragCoord) shows it, from picture(): in a
// general pipeline with the picture settings (a photo's as adjustPicture
// applies them; a video's, which C++ passes only while a mask or key reads
// the frame, as mpv does) and the tone look. A common mode has neither.
vec3 shown(vec3 c, EyeData e, uint flags, vec2 fragCoord) {
  if (kCommon) return c;
  vec3 a = pc.draw.adjust.xyz;
  if ((flags & kPhoto) != 0u) c = adjustPicture(c, a);
  else if (a != vec3(0.0)) c = adjustVideo(c, a, pc.draw.params.zw, (flags & kTvGamma) != 0u);
  return clamp(applyLook(c, e, flags, fragCoord), 0.0, 1.0);
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
// next to the fisheye circles). Its red channel is opacity.
float packedAlpha(vec2 uv, EyeData e, vec2 dx, vec2 dy) {
  vec2 alphaScale = pc.draw.alpha.xy;
  vec2 alphaCenter = vec2(e.position.w < 0.5 ? 0.5 : 0.0, 0.0);
  vec2 m = uv - 0.5;
  if (length(m) > 0.5) return 0.0;
  vec2 maskUv = fract(alphaCenter + m * alphaScale);
  vec3 mask = isYuv() ? tap(maskUv) : textureGrad(tex, maskUv, dx * alphaScale, dy * alphaScale).rgb;
  float red = linearToSrgb(mask).r;
  // Compression leaves black a little above 0 and full red below 1.
  return smoothstep(0.08, 0.92, red);
}

// Where the view direction dir falls in this eye's picture (pictureUv).
vec2 surroundUv(vec3 dir, int projection, out bool inside) {
  inside = true;
  if (kProjection == 0) return equirectUv(dir, projection == 0 ? PI : 2.0 * PI, inside);
  if (kProjection == 1) return fisheyeUv(dir, pc.draw.params.x, inside);
  if (kProjection == 2) return cubemapUv(dir, projection == 4);
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
  vec3 c = inside ? picture(uv, lo, hi, dx, dy, e, flags) : vec3(0.0);

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
  bool packed = kSeeThrough < 0 ? (flags & kPackedAlpha) != 0u : kSeeThrough == 1;
  bool keyed = kSeeThrough < 0 ? !packed && (flags & kChromaKey) != 0u : kSeeThrough == 2;
  if (kSeeThrough == 0 || (kSeeThrough < 0 && !keyGreen && !alphaOut)) {
    // No packed mask or green screen: the picture where there is one, else
    // what the passthrough mode shows the room with.
    vec4 outside = alphaOut ? vec4(0.0) : keyGreen ? vec4(kKeyColor, 1.0) : vec4(0.0, 0.0, 0.0, 1.0);
    show(c, n, sharpened, e, flags);
    if (sharpened) c = sharpen(c, n[0], n[1], n[2], n[3], strength);
    fragColor = inside ? vec4(c, 1.0) : outside;
    return;
  }
  // Passthrough: how much of the picture stays here.
  packed = packed && inside;
  keyed = keyed && inside;
  float alpha = inside ? 1.0 : 0.0;
  vec4 chroma = pc.draw.chroma;
  if (packed) {
    alpha = kMaskFirst ? maskAlpha : packedAlpha(uv, e, dx, dy);
  } else if (keyed) {
    alpha = chromaAlpha(linearToSrgb(c), chroma);
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
  }
  // Near what is cut away nothing is sharpened, so the cut gets no fringes;
  // the coverage itself never is.
  if (sharpened && (packed || keyed)) {
    bool edge = alpha < 1.0;
    for (int i = 0; i < 4; ++i)
      edge = edge || (packed ? packedAlpha(nUv[i], e, dx, dy)
                             : chromaAlpha(linearToSrgb(n[i]), chroma)) < 1.0;
    sharpened = !edge;
  }
  // The mask and the key have read the picture as it came; sharpening, the
  // key colour's cap and spill removal act on what is shown.
  show(c, n, sharpened, e, flags);
  if (sharpened) c = sharpen(c, n[0], n[1], n[2], n[3], strength);
  if (packed) {
    if (!kCommon && keyGreen) {
      // The picture itself must never look like the key colour, or the
      // headset would cut holes into it (a green top, say): the key channel
      // is capped just above the larger other one, in display values, below
      // what the headset's keyer reacts to. (Key green with a packed mask
      // uses the general pipeline.)
      vec3 s = linearToSrgb(c);
      vec3 rest = s * (vec3(1.0) - kKeyColor);
      float keyed = dot(s, kKeyColor);
      s += kKeyColor * (min(keyed, max(max(rest.r, rest.g), rest.b) + 0.02) - keyed);
      c = srgbToLinear(s);
    }
  } else if (keyed) {
    c = srgbToLinear(removeSpill(linearToSrgb(c), chroma.z));
  }
  fragColor = alphaOut ? vec4(c * alpha, alpha) : vec4(mix(kKeyColor, c, alpha), 1.0);
}
