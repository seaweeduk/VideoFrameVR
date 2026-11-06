// Shared by the video's shaders (surround.frag, screen.vert/frag).

#include "common.glsl"

// Full precision unless a variable says otherwise (yuvToRgb's mediump).
precision highp float;
precision highp int;

// Draw flags (VideoFlag in scene_renderer.h).
const uint kFlip = 1u;          // the picture is shown upside down
const uint kBaseLevel = 2u;     // the texture is sampled from its full-size level: plain bilinear
const uint kLook = 4u;          // tone curve and vignette
const uint kKeyGreen = 8u;      // passthrough: what shows the room is drawn in key green
const uint kAlphaOut = 16u;     // passthrough: ... is transparent (premultiplied alpha)
const uint kPackedAlpha = 32u;  // DeoVR's packed alpha mask
const uint kChromaKey = 64u;    // a green screen keyed here
const uint kCorrect = 128u;     // stereo correction (EyeData.viewer) applies
const uint kTexture = 256u;     // screen: a picture (else its placeholder colour)
const uint kYuv = 512u;         // a decoded frame: the texture is its luma, binding 1 its chroma
const uint kTvGamma = 1024u;    // kYuv: TV gamma, BT.1886's plain 2.4 power instead of sRGB's curve

// The draw's flags (above), from its push constants: each shader defines it.
uint drawFlags();

// Passthrough key colour: pure green background, which the Frame's
// camera keyer replaces with the room.
const vec3 kKeyColor = vec3(0.0, 1.0, 0.0);

// Up to this many texels per pixel, one bilinear tap (as 1.60x Resolution on
// the Frame, where it looked right); the spread grows from there over the
// next quarter texel, so the blur changes smoothly across the picture.
const float kFootprintStart = 1.15;
// A pixel spanning more texels than this straddles a jump in the picture
// (a cubemap's face edge): one tap.
const float kFootprintJump = 64.0;

// Taps along one side of a pixel that spans `texels` (footprintSample), and
// how far apart the first and last are, in texels: spread as a box filter
// over the pixel, n taps at the midpoints of n equal parts.
int footprintTaps(float texels, int maxTaps, out float span) {
  int n = texels > kFootprintStart ? clamp(int(ceil(texels)), 1, maxTaps) : 1;
  span = n > 1 ? texels * float(n - 1) / float(n) *
                     min((texels - kFootprintStart) * 4.0, 1.0)
               : 0.0;
  return n;
}

// What mpv draws from a decoded frame's planes (DirectVideo::computeColor),
// display values: y, c as sampled; range: luma scale and offset, chroma scale
// and offset; k: Kr, Kb. In half precision (the planes hold 10 bits at most):
// on the Frame full precision takes fewer instructions but runs the NV12 eye
// passes about 2% slower.
mediump vec3 yuvToRgb(mediump float y, mediump vec2 c, mediump vec4 range, mediump vec2 k) {
  y = y * range.x + range.y;
  c = c * range.z + range.w;
  mediump float kg = 1.0 - k.x - k.y;
  return vec3(y + 2.0 * (1.0 - k.x) * c.y,
              y - (2.0 * k.y * (1.0 - k.y) * c.x + 2.0 * k.x * (1.0 - k.x) * c.y) / kg,
              y + 2.0 * (1.0 - k.y) * c.x);
}

#ifdef TP10
// A Q10C decoder frame (DirectVideo, docs/vulkan-renderer.md 11), in the
// pipelines for it: its luma rows in overlapping bands, each a TP10 texture
// of at most 2^24 samples; its chroma whole at binding 1. A full band has
// tp10Bands[0]'s rows, band k starts at k times half of that (in 64-row
// tiles), and the last ends at the picture's bottom. A lookup reads the band
// whose middle is nearest, which reaches at least a quarter band beyond it.
layout(set = 0, binding = 0) uniform sampler2D tp10Bands[8];

float tp10Luma(sampler2D chroma, vec2 p) {
  float h = float(2 * textureSize(chroma, 0).y), full = float(textureSize(tp10Bands[0], 0).y);
  float step = floor(full / 128.0) * 64.0;
  int last = full < h ? int(ceil((h - full) / step)) : 0;
  float row = p.y * h;
  int k = clamp(int(floor(row / step - 0.5)), 0, last);
  float row0 = float(k) * step;
  return textureLod(tp10Bands[nonuniformEXT(k)], vec2(p.x, (row - row0) / min(full, h - row0)), 0.0).r;
}
#endif

// The size in texels of the picture whose luma (or image) tex is, and
// whether it is sampled from its full-size level (kBaseLevel): always for a
// Q10C frame. Macros: the other pipelines' code stays as it was.
#ifdef TP10
#define videoSize(tex, chroma) vec2(textureSize(tp10Bands[0], 0).x, 2 * textureSize(chroma, 0).y)
#define baseLevel(flags) true
#else
#define videoSize(tex, chroma) vec2(textureSize(tex, 0))
#define baseLevel(flags) (((flags) & kBaseLevel) != 0u)
#endif

// What tex holds (videoTap): an RGBA image (mpv's), a decoded frame's luma
// (its chroma at binding 1), or a decoded frame through a Y'CbCr sampler with
// an identity conversion, whose one lookup returns Cr, Y, Cb. A Q10C frame's
// TP10 bands are built apart (TP10).
const int kSourceImage = 0;
const int kSourcePlanes = 1;
const int kSourceYcbcr = 2;

// The picture at p (texture coordinates) from its full-size level, bilinear,
// linear colour: the image, or a decoded frame as mpv would have drawn it
// into the sRGB image the other path samples. range as yuvToRgb's; matrix:
// Kr, Kb, then the chroma lookup's offset (planes).
vec3 videoTap(sampler2D tex, sampler2D chroma, vec2 p, int source, vec4 range, vec4 matrix) {
  // Lookups at full precision: half precision ones move the taps.
  float y;
  vec2 c;
#ifdef TP10
  y = tp10Luma(chroma, p);
  c = textureLod(chroma, p + matrix.zw, 0.0).rg;
#else
  if (source == kSourceImage) return textureLod(tex, p, 0.0).rgb;
  if (source == kSourceYcbcr) {
    vec3 s = textureLod(tex, p, 0.0).rgb;
    y = s.g;
    c = s.br;
  } else {
    y = textureLod(tex, p, 0.0).r;
    c = textureLod(chroma, p + matrix.zw, 0.0).rg;
  }
#endif
  mediump vec3 e = clamp(yuvToRgb(y, c, range, matrix.xy), 0.0, 1.0);
  // To linear light as mpv's image would be sampled: sRGB's curve, or with TV
  // gamma, the 2.4 power mpv converts BT.1886 video with (the same work).
  bool tv = (drawFlags() & kTvGamma) != 0u;
  float a = tv ? 0.0 : 0.055, toe = tv ? 0.0 : 0.04045;
  return mix(e / 12.92, pow((e + a) / (1.0 + a), vec3(2.4)), step(vec3(toe), e));
}

// The picture at uv (0..1 in this eye's part, `stereo` as EyeData's) from
// its full-size level, averaged over what this pixel covers: uv moves by dx
// and dy to the next pixel. Bilinear alone shimmers once a pixel spans more
// than a texel (8K video in a 2160 eye image): the pixel takes a grid of
// bilinear taps over its footprint instead, at most `maxTaps` per side, so
// fine lines blend to their average instead of flickering. maxTaps is the
// Anti-shimmer setting (anti_shimmer.h), from the draw's push constants: 1
// is one bilinear tap whatever the footprint. Magnified or about
// 1:1 it is one bilinear tap as before. No mipmaps, so nothing extra per video
// frame. Taps stay within lo..hi. Linear colour. chroma, source, range,
// matrix: videoTap's.
vec3 footprintSample(sampler2D tex, sampler2D chroma, int source, vec4 range, vec4 matrix, vec2 uv,
                     vec2 dx, vec2 dy, vec4 stereo, vec2 lo, vec2 hi, int maxTaps) {
  if (maxTaps <= 1)
    return videoTap(tex, chroma, stereo.zw + clamp(uv, lo, hi) * stereo.xy, source, range, matrix);
  vec2 size = videoSize(tex, chroma) * stereo.xy;
  float lx = length(dx * size), ly = length(dy * size);
  if (max(lx, ly) > kFootprintJump) lx = ly = 1.0;
  float spanX, spanY;
  int nx = footprintTaps(lx, maxTaps, spanX), ny = footprintTaps(ly, maxTaps, spanY);
  if (nx * ny == 1)
    return videoTap(tex, chroma, stereo.zw + clamp(uv, lo, hi) * stereo.xy, source, range, matrix);
  // From one end to the other, in steps along dx and dy.
  vec2 stepX = nx > 1 ? dx * (spanX / lx / float(nx - 1)) : vec2(0.0);
  vec2 stepY = ny > 1 ? dy * (spanY / ly / float(ny - 1)) : vec2(0.0);
  vec2 first = uv - 0.5 * (stepX * float(nx - 1) + stepY * float(ny - 1));
  vec3 sum = vec3(0.0);
  for (int j = 0; j < ny; ++j)
    for (int i = 0; i < nx; ++i) {
      vec2 p = clamp(first + stepX * float(i) + stepY * float(j), lo, hi);
      sum += videoTap(tex, chroma, stereo.zw + p * stereo.xy, source, range, matrix);
    }
  return sum / float(nx * ny);
}

// Brightness, contrast and saturation (each -1..1) for pictures mpv does not
// adjust (photos).
vec3 adjustPicture(vec3 c, vec3 adjust) {
  float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
  c = mix(vec3(l), c, 1.0 + adjust.z);
  c = (c - 0.18) * (1.0 + adjust.y) + 0.18;
  return max(c * exp2(adjust.x * 1.5), 0.0);
}

// Tone look (docs/video-sharpness.md):
// linear light brightened by a third of a stop with a little more contrast,
// falling off softly towards the edges of the eye image.
// fragCoord: gl_FragCoord.xy.
vec3 applyLook(vec3 c, EyeData e, uint flags, vec2 fragCoord) {
  if ((flags & kLook) == 0u) return c;
  vec2 p = (fragCoord - e.area.xy) / e.area.zw * 2.0 - 1.0;
  float v = 1.0 / (1.0 + 0.16 * dot(p, p));
  return 1.26 * pow(max(c, 0.0), vec3(1.08)) * (v * v);
}

// Sharpening (docs/video-sharpness.md), as the old pass over the finished eye
// image did it, from the picture at this pixel (c) and one eye pixel left,
// right, up and down: the pixel's brightness moves away from the average of
// its four neighbours by `strength` times the difference, in display values,
// limited to ±0.1 so hard edges get no strong halos. The change is the same
// for R, G and B, so hue stays. Linear colours (0..1) in and out.
vec3 sharpen(vec3 c, vec3 l, vec3 r, vec3 u, vec3 d, float strength) {
  const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);
  vec3 e = linearToSrgb(c);
  float around = 0.25 * (dot(linearToSrgb(l), kLuma) + dot(linearToSrgb(r), kLuma) +
                         dot(linearToSrgb(u), kLuma) + dot(linearToSrgb(d), kLuma));
  float boost = clamp(strength * (dot(e, kLuma) - around), -0.1, 0.1);
  return srgbToLinear(clamp(e + boost, 0.0, 1.0));
}

// How much of a pixel stays (1) or is cut (0) by the green screen key
// (x = how green is cut away, y = half the width of the soft edge around
// that). Its greenness is measured whatever its brightness (display values):
// 0 for grey, skin or hair, about 0.6 to 0.75 for a green screen, lit or in
// shadow.
float chromaAlpha(vec3 e, vec4 chroma) {
  float green = (e.g - max(e.r, e.b)) / max(e.g, 0.04);
  return 1.0 - smoothstep(chroma.x - chroma.y, chroma.x + chroma.y, green);
}

// What stays of a keyed pixel loses its green tint (hair edges lit by the
// screen): green no higher than red or blue, with more spill removal (0..2)
// their average, and beyond that pushed below it by as much as it was above,
// so only tinted pixels change.
vec3 removeSpill(vec3 e, float spill) {
  float limit = mix(max(e.r, e.b) + 0.02, 0.5 * (e.r + e.b), min(spill, 1.0));
  if (e.g > limit) e.g = max(limit - max(spill - 1.0, 0.0) * (e.g - limit), 0.0);
  return e;
}
