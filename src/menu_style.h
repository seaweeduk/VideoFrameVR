#pragma once

// What the menu's source files share: layout constants, the themes' colours
// and small drawing helpers.

#include "menu.h"

#include <cairo.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>

// Cairo colours, not premultiplied (see the palettes below).
struct Rgba {
  double r, g, b, a;
};
struct Palette {
  Rgba panel;       // panel fill
  Rgba edge;        // the panels' and group boxes' outline, in the accent
  Rgba button;      // buttons, keys, rows, tiles
  Rgba field;       // text and value fields, slider tracks, the timeline band
  Rgba accent;      // chosen buttons and tabs, slider and progress fill
  Rgba text, textDim;
  Rgba textAccent;  // chosen navigation row, the playing file's name
  Rgba buffered;    // the downloaded part of a stream on the timeline
};

namespace {

constexpr double kMargin = 40;
constexpr double kGap = 16;

// The typeface: Noto Sans Condensed Medium, narrow letters
// without thin strokes (fontconfig picks the nearest where it is missing);
// emphasis in its semibold.
constexpr const char* kFont = "Noto Sans Condensed Medium";
constexpr const char* kFontBold = "Noto Sans Condensed SemiBold";
// Keys and fingerprints, read character by character: l, I and 1 apart
// (the release bundles Noto Sans Mono; elsewhere the system's monospace).
constexpr const char* kFontMono = "Noto Sans Mono,Monospace Medium";

// The playback strip, in its texture's pixels: its body starts below the
// room for open lists and tooltips (Menu::kStripBodyY). Rows: the clock,
// tabs and info; the timeline band; tools, transport and volume.
constexpr double kTabX = 140;  // right of the clock
constexpr double kTabY = Menu::kStripBodyY + 16, kTabH = 50;
constexpr double kBandY = Menu::kStripBodyY + 78, kBandH = 62;
constexpr double kSeekBand = kBandH / 2;  // the band is the seek bar
constexpr double kRowY = Menu::kStripBodyY + 152;
constexpr double kPlayButton = 72;  // and the volume slider's height
constexpr double kToolSize = 60;    // the other round buttons of that row
constexpr double kTransportGap = 18;
constexpr double kVolumeX = 1205;   // the speaker, then the slider to the right margin
constexpr double kStripStarW = 44;  // a star of the rating among the tools

// The browser, in its texture's pixels: the navigation column on the left,
// then the central page from Menu::kPageX.
constexpr double kPagePad = 24;
constexpr double kNavRowH = 56, kPlaceRowH = 48;  // section rows, and places under them
constexpr int kNavPlaces = 11;                     // at most so many places
constexpr double kBrowserRowY = 20, kBrowserRowH = 56;      // up, star and the location
constexpr double kBrowserToolsY = 88, kBrowserToolsH = 56;  // views, order, search
constexpr double kListHeaderY = 156, kListHeaderH = 44;     // the list's column headings
constexpr double kListTop = 206, kGridTop = 156;            // where the list or grid starts
constexpr double kListBottom = Menu::kBrowserHeight - kPagePad;
// Dense list rows (80 reference layout units), a small thumbnail
// still in each; kRowGap of each is the gap to the next.
constexpr double kFileRowH = 50, kRowGap = 4;
constexpr int kFileRows = static_cast<int>((kListBottom - kListTop) / kFileRowH);  // whole ones
constexpr double kSizeColumnW = 100, kDateColumnW = 180;  // the list's columns
constexpr double kPlaysColumnW = 60;
constexpr double kStatusColumnW = 40;  // a video's offline download: its tick or button
// A rating control is five stars in equal cells, nothing beside them.
constexpr double kRowStarW = 26;  // a star's cell in a row's rating
constexpr double kRatingColumnW = 5 * kRowStarW;
// On a picture's strip (a tile's): cells and stars, the control's height and
// where it starts, in from the picture's left and up from its bottom.
constexpr double kCardStarW = 26, kCardStarSize = 24, kCardStarsH = 44;
constexpr double kCardStarsX = 14, kCardStarsUp = 46;
// The strip itself: a fade to dark over the picture's bottom, under the
// stars and the plays.
constexpr double kCoverStripH = 52;
// The space before the strip's and the Scene tab's stars (not theirs).
constexpr double kRatingLeadW = 30;
// The fields lists are sorted by (Strings::sortFields), each ascending and
// descending.
enum SortField { kByName, kByDate, kBySize, kByRating, kByPlays, kByPlayed, kByAdded, kSortFields };
struct FieldOrders {
  FolderListing::Order up, down;
};
const FieldOrders kFieldOrders[kSortFields] = {
    {FolderListing::Order::NameUp, FolderListing::Order::NameDown},
    {FolderListing::Order::Oldest, FolderListing::Order::Newest},
    {FolderListing::Order::Smallest, FolderListing::Order::Largest},
    {FolderListing::Order::RatingLow, FolderListing::Order::RatingHigh},
    {FolderListing::Order::PlaysFewest, FolderListing::Order::PlaysMost},
    {FolderListing::Order::PlayedOldest, FolderListing::Order::PlayedNewest},
    {FolderListing::Order::AddedOldest, FolderListing::Order::AddedNewest}};
// The fields the folder offers, and the library view.
constexpr int kFolderFields[] = {kByName, kByDate, kBySize, kByRating, kByPlays, kByPlayed};
constexpr int kLibraryFields[] = {kByAdded, kByRating, kByPlays, kByPlayed, kBySize, kByName};

inline int fieldOf(FolderListing::Order order) {
  for (int f = 0; f < kSortFields; ++f)
    if (kFieldOrders[f].up == order || kFieldOrders[f].down == order) return f;
  return kByName;
}

static_assert(sizeof(Strings::sortModes) / sizeof(char*) ==
                  static_cast<size_t>(FolderListing::Order::kCount),
              "a name for each order");

// What the library orders sort by; 0 = not known (unrated, never played),
// which comes last either way.
inline double recordKey(int field, const VideoRecord& r, int64_t addedAt) {
  switch (field) {
    case kByRating: return r.rating;
    case kByPlays: return r.playCount;
    case kByPlayed: return static_cast<double>(r.lastPlayedAt);
    case kByAdded: return static_cast<double>(addedAt);
    default: return 0;
  }
}

// Where a list row's columns start, for a row ending at x1: the offline
// status, rating, plays, size and date.
struct ListColumns {
  double status, rating, plays, size, date;
};
inline ListColumns listColumns(double x1) {
  ListColumns c;
  c.date = x1 - 16 - kDateColumnW;
  c.size = c.date - 24 - kSizeColumnW;
  c.plays = c.size - 16 - kPlaysColumnW;
  c.rating = c.plays - 16 - kRatingColumnW;
  c.status = c.rating - 8 - kStatusColumnW;
  return c;
}
// A row's byline (Stash's performers and studio) follows its name; a long
// name keeps at least this share of the name's column.
constexpr double kBylineShare = 0.65;
constexpr int kGalleryColumns = 4;
constexpr double kGalleryRowH = 356, kGalleryGap = 16;
constexpr int kGalleryRows = 2;  // whole; a third peeks in under them
// The pictures of the gallery's tiles and the list's rows as addGallery()
// and addList() lay them out on the page: what covers and thumbnails are
// made for.
constexpr double kCardPictureW =
    (Menu::kPageWidth - 2 * kPagePad + 6 - 30 - (kGalleryColumns - 1) * kGalleryGap) /
        kGalleryColumns - 16;
constexpr double kRowPictureW = (kFileRowH - kRowGap - 4) * 16 / 9;
// While typing into the folder search, the keyboard takes the bottom of the
// page and the results the rest.
constexpr double kKeyH = 58, kKeyStep = 66;
constexpr double kKeyboardPadY = 10;
// Keep the panel's bottom at the page inset, with equal padding above and
// below its five rows of keys.
constexpr double kKeyboardY = kListBottom - kKeyboardPadY - 4 * kKeyStep - kKeyH;
constexpr int kEditingFileRows = static_cast<int>((kKeyboardY - 16 - kListTop) / kFileRowH);
constexpr int kEditingGalleryRows = 1;

// Grouped settings pages: groups in columns on the page, with dense rows
// equivalent to controls 64 reference layout units high.
constexpr double kSettingRowH = 40, kSettingStep = 46;
constexpr double kGroupPad = 14;  // between a group's edge and its rows
constexpr double kGroupTitleH = 42;
constexpr double kColumnW = (Menu::kPageWidth - 2 * kPagePad - 2 * kGap) / 3;
inline double columnX(int column) { return Menu::kPageX + kPagePad + column * (kColumnW + kGap); }
// The first row's top in a group whose box starts at y.
inline double firstRow(double y) { return y + kGroupTitleH + 12; }
// The height a hint of kHintSize text takes, of so many lines.
constexpr int kHintSize = 22;
inline double hintHeight(int lines) { return lines * 30 + 4; }

constexpr double kButtonRadius = 18;
constexpr int kTooltipSize = 26;  // its text  // the panel shader's hover outline uses the same

constexpr double kIconButtonW = 120;

constexpr double kIconSize = 52;

constexpr int kSlideshowSeconds[] = {0, 3, 5, 10, 20, 30, 60};
constexpr int kPictureStep = 1;  // brightness etc., of mpv's -100..100
constexpr int kSharpenStep = 1;  // hundredths
constexpr int kZoomStepPercent = 1;
// The surround viewpoint, in percent of the sphere's radius toward the picture.
constexpr int kViewpointStepPercent = 1;
constexpr int kMaxViewpointPercent =
    static_cast<int>(VideoSettings::kMaxOriginForward * 100 + 0.5f);
// Match the sliders' centimetre and hundredth units for fine adjustment.
constexpr int kScreenStepCm = 1;
constexpr int kCurveStepPercent = 1;

inline double steadySeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// "1.2 GB", "850 MB", "12 KB".
inline std::string sizeText(uint64_t bytes) {
  if (bytes == 0) return "";
  char buf[32];
  const double b = static_cast<double>(bytes);
  if (b >= 1e9)
    std::snprintf(buf, sizeof(buf), "%.1f GB", b / 1e9);
  else if (b >= 1e6)
    std::snprintf(buf, sizeof(buf), "%.0f MB", b / 1e6);
  else
    std::snprintf(buf, sizeof(buf), "%.0f KB", std::max(1.0, b / 1e3));
  return buf;
}

// "2024-05-01 14:02", local time.
inline std::string dateText(int64_t seconds) {
  if (seconds <= 0) return "";
  const std::time_t t = static_cast<std::time_t>(seconds);
  std::tm local{};
  localtime_r(&t, &local);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &local);
  return buf;
}

inline std::string formatTime(double seconds) {
  if (!(seconds >= 0)) seconds = 0;
  const int total = static_cast<int>(seconds);
  char buf[32];
  if (total >= 3600)
    std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
  else
    std::snprintf(buf, sizeof(buf), "%d:%02d", total / 60, total % 60);
  return buf;
}

// A layout position on the texture's pixel grid (menu_geometry::kRasterScale):
// lines of text sit on it, so they are as sharp as they can be and the same
// wherever a list scrolls them.
inline double onPixel(double v) {
  return std::round(v * menu_geometry::kRasterScale) / menu_geometry::kRasterScale;
}

// An image of w × h layout pixels at the menu textures' resolution
// (menu_geometry::kRasterScale), for drawing parts once and pasting them.
inline cairo_surface_t* rasterSurface(int w, int h) {
  constexpr float s = menu_geometry::kRasterScale;
  cairo_surface_t* surface = cairo_image_surface_create(
      CAIRO_FORMAT_ARGB32, static_cast<int>(std::ceil(w * s)), static_cast<int>(std::ceil(h * s)));
  cairo_surface_set_device_scale(surface, s, s);
  return surface;
}

inline void roundedRect(cairo_t* cr, double x, double y, double w, double h, double r) {
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
  cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
  cairo_close_path(cr);
}

inline std::function<MenuCommand()> command(MenuCommand::Type type, double value = 0) {
  return [type, value] {
    MenuCommand c;
    c.type = type;
    c.value = value;
    return c;
  };
}

// "1.25× · 2700": the factor and the eye image's pixels across.
inline std::string resolutionText(double percent, int base) {
  char buf[48];
  if (base > 0)
    std::snprintf(buf, sizeof(buf), "%.2f× · %ld", percent / 100.0, std::lround(base * percent / 100.0));
  else
    std::snprintf(buf, sizeof(buf), "%.2f×", percent / 100.0);
  return buf;
}

// The look: charcoal panels with a thin bright edge in the accent (blue
// unless chosen otherwise, accent.h) and a soft glow, lighter rows and
// fields, white text, the accent for what is chosen (filled for buttons and
// tabs, its text colour for rows and file names). Both designs share it;
// Transparent lets the blurred scene show through the panels
// (Menu::kGlassAlpha), Solid does not.

const Palette kGlassPalette = {
    {0.118, 0.118, 0.125, Menu::kGlassAlpha}, {0.118, 0.471, 1.0, 1},
    {0.23, 0.23, 0.245, 0.88}, {0.08, 0.08, 0.085, 0.80}, {0.118, 0.471, 1.0, 1},
    {1, 1, 1, 1}, {0.72, 0.72, 0.75, 1}, {0.36, 0.64, 1.0, 1}, {1, 1, 1, 0.22}};

const Palette kMaterialPalette = {
    {0.118, 0.118, 0.125, 0.97}, {0.118, 0.471, 1.0, 1},
    {0.23, 0.23, 0.245, 1}, {0.08, 0.08, 0.085, 1}, {0.118, 0.471, 1.0, 1},
    {1, 1, 1, 1}, {0.72, 0.72, 0.75, 1}, {0.36, 0.64, 1.0, 1}, {1, 1, 1, 0.22}};

constexpr double kPanelRadius = 30;
constexpr double kGlow = 7;  // the glow around a panel's edge, inside its rectangle

inline Palette paletteFor(Theme theme, const AccentColours& accent) {
  Palette p = theme == Theme::Material ? kMaterialPalette : kGlassPalette;
  p.edge = p.accent = {accent.fill[0], accent.fill[1], accent.fill[2], 1};
  p.textAccent = {accent.text[0], accent.text[1], accent.text[2], 1};
  return p;
}


inline void setColor(cairo_t* cr, const Rgba& c, double alpha = 1.0) {
  cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a * alpha);
}

// A panel: the fill, a thin bright edge and a soft glow around it, all
// within x, y, w, h (the glow takes kGlow of it).
inline void panelFrame(cairo_t* cr, double x, double y, double w, double h, const Rgba& fill,
                       const Rgba& edge, double radius = kPanelRadius) {
  x += kGlow, y += kGlow, w -= 2 * kGlow, h -= 2 * kGlow;
  roundedRect(cr, x, y, w, h, radius);
  setColor(cr, fill);
  cairo_fill_preserve(cr);
  for (const auto& [width, alpha] : {std::pair{12.0, 0.10}, {6.0, 0.25}, {2.5, 1.0}}) {
    setColor(cr, edge, alpha);
    cairo_set_line_width(cr, width);
    cairo_stroke_preserve(cr);
  }
  cairo_new_path(cr);
}

// A group of settings: a thin outlined box with its title centred at the top
// over a dividing line (the title is drawn by the caller's text helper).
inline void groupFrame(cairo_t* cr, double x, double y, double w, double h, const Palette& pal) {
  roundedRect(cr, x, y, w, h, 18);
  setColor(cr, pal.field, 0.55);
  cairo_fill_preserve(cr);
  setColor(cr, pal.edge, 0.9);
  cairo_set_line_width(cr, 2.5);
  cairo_stroke(cr);
  cairo_rectangle(cr, x + 16, y + kGroupTitleH, w - 32, 1.5);
  setColor(cr, pal.edge, 0.6);
  cairo_fill(cr);
}

// The app's icon (packaging/videoframevr.svg): a tile, the two eyes' circles,
// their overlap lit, a play button in it. The intro draws its parts apart.
enum BrandPart {
  kBrandTile = 1, kBrandBlue = 2, kBrandGreen = 4, kBrandLens = 8, kBrandPlay = 16, kBrandWhole = 31
};

inline void drawBrand(cairo_t* cr, double x, double y, double size, int parts = kBrandWhole) {
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, size / 256, size / 256);
  if (parts & kBrandTile) {
    roundedRect(cr, 8, 8, 240, 240, 56);
    cairo_pattern_t* sky = cairo_pattern_create_linear(0, 8, 0, 248);
    cairo_pattern_add_color_stop_rgb(sky, 0, 0x0e / 255.0, 0x1a / 255.0, 0x36 / 255.0);
    cairo_pattern_add_color_stop_rgb(sky, 1, 0x1b / 255.0, 0x36 / 255.0, 0x56 / 255.0);
    cairo_set_source(cr, sky);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(sky);
    cairo_pattern_t* sheen = cairo_pattern_create_linear(0, 8, 0, 248);
    cairo_pattern_add_color_stop_rgba(sheen, 0, 1, 1, 1, 0.08);
    cairo_pattern_add_color_stop_rgba(sheen, 0.5, 1, 1, 1, 0);
    cairo_set_source(cr, sheen);
    cairo_fill(cr);
    cairo_pattern_destroy(sheen);
  }
  if (parts & kBrandBlue) {
    cairo_arc(cr, 106, 128, 72, 0, 2 * M_PI);
    cairo_set_source_rgb(cr, 0x3d / 255.0, 0x7f / 255.0, 0xe0 / 255.0);
    cairo_fill(cr);
  }
  if (parts & kBrandGreen) {
    cairo_arc(cr, 150, 128, 72, 0, 2 * M_PI);
    cairo_set_source_rgb(cr, 0x2f / 255.0, 0xc4 / 255.0, 0x8c / 255.0);
    cairo_fill(cr);
  }
  if (parts & kBrandLens) {
    cairo_save(cr);
    cairo_arc(cr, 106, 128, 72, 0, 2 * M_PI);
    cairo_clip(cr);
    cairo_arc(cr, 150, 128, 72, 0, 2 * M_PI);
    cairo_set_source_rgb(cr, 0xc9 / 255.0, 0xfb / 255.0, 0xe9 / 255.0);
    cairo_fill(cr);
    cairo_restore(cr);
  }
  if (parts & kBrandPlay) {
    cairo_move_to(cr, 118, 108);
    cairo_line_to(cr, 118, 148);
    cairo_line_to(cr, 150, 128);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 0x12 / 255.0, 0x24 / 255.0, 0x4a / 255.0);
    cairo_set_line_width(cr, 10);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_fill_preserve(cr);
    cairo_stroke(cr);
  }
  cairo_restore(cr);
}

constexpr double kSliderInset = 28;    // a slider's track starts this far into it
constexpr double kSliderValueW = 110;  // the value right of it, with room for the knob at 100
constexpr double kDialogW = 1160, kDialogH = 400;

}  // namespace
