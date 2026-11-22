#pragma once

// What the menu's source files share: layout constants, the design's
// tokens and colours, and small drawing helpers.

#include "menu.h"
#include "menu_finish.h"
#include "value_ranges.h"

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
  Rgba panel;       // the panel's material, flat (MenuFinishes::panel() draws its gradient)
  Rgba edge;        // an outline in the accent: a field typed into, the playing tile
  Rgba button;      // a flat surface: keys, rows, tiles drawn without their finish
  Rgba field;       // a well (MenuFinishes, Finish::Sunk), flat
  Rgba accent;      // chosen and filled things, slider and progress fill
  Rgba text, textDim;
  Rgba textAccent;  // the accent as text: a chosen row, the playing file's name
  Rgba buffered;    // the downloaded part of a stream on the timeline
  Rgba faint;       // notes and examples
  Rgba rule;        // a dividing line
  Rgba danger, gold, ok;  // what destroys or failed, stars and what waits, what works
  Rgba accentSoft;  // a mode that is on (latched): the accent at 16 %
};

namespace {

// The design's tokens (the menu kit): spacing, corners, sizes and type, in
// layout pixels. Pages being rebuilt use these; the constants after them are
// the layouts of pages that are not yet.
namespace kit {
// Spacing: between parts, between groups and sections, inside a well, between
// a well's pieces, a page's edge.
constexpr double kSpace = 16, kSpaceWide = 32, kSpaceTight = 8, kSpacePiece = 4, kSpaceEdge = 24;
// A group: its title row and its padding at the sides.
constexpr double kGroupTitle = 48, kGroupPadX = 8;
constexpr double kTextInset = 24;  // a row's text from its box's left
// Corners: a panel, a group or open list, a control, a piece inside a well.
constexpr double kPanelCorner = 30, kGroupCorner = 22, kCorner = 14, kPieceCorner = 10;
constexpr double kPanelInset = 7;  // the panel is drawn this far inside its rectangle
// Sizes.
constexpr double kRowH = 52, kRowStep = 60;
constexpr double kPageHeaderH = 56;
constexpr double kStripTabsH = 48, kStripBandH = 58, kStripRowH = 56;
constexpr double kStripButton = 56, kPlayW = 88;
constexpr double kNavRowH = 56;
constexpr double kTileMin = 84, kTileMax = 92;
constexpr double kSwitchW = 60, kSwitchH = 34, kSwitchKnob = 28;
constexpr double kSliderTrackH = 8, kSliderKnob = 28;
constexpr double kKeyH = 56;
constexpr double kPopPad = 8;  // an open list's edge around its items
// A dialog (Menu::DialogLayout): its title and lines from its edges, its
// buttons from its bottom-right corner; between its lines, between the last
// and its buttons; its buttons' height.
constexpr double kDialogPad = kSpaceWide, kDialogEdge = kSpace;
constexpr double kDialogLineGap = 4, kDialogButtonsGap = 24, kDialogButtonH = 56;
// Type: sizes in pixels, and the line height of notes and tooltips.
constexpr int kTitleSize = 32;       // a page's title, bold
constexpr int kRowSize = 25;         // a row's label
constexpr int kWellSize = 24;        // text in a well, a field
constexpr int kNavSize = 26, kPlaceSize = 24;
constexpr int kGroupTitleSize = 22;  // bold, dim
constexpr int kNoteSize = 21;        // faint
constexpr int kTipSize = 22;
constexpr double kNoteLine = 30, kTipLine = 30;
}  // namespace kit

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
// room for open lists and tooltips (Menu::kStripBodyY), its panel drawn
// kit::kPanelInset inside it. Every box is kit::kSpace from the panel, and
// its three rows are kit::kSpace apart: the tabs (the clock, the tabs, what
// is playing, Quit), the band (the timeline) and the controls (tools,
// transport, volume).
namespace strip {
constexpr double kLeft = kit::kPanelInset + kit::kSpace;
constexpr double kRight = Menu::kMainWidth - kLeft;
constexpr double kTabsY = Menu::kStripBodyY + kLeft, kTabsH = kit::kStripTabsH;
constexpr double kBandY = kTabsY + kTabsH + kit::kSpace, kBandH = kit::kStripBandH;
constexpr double kRowY = kBandY + kBandH + kit::kSpace, kRowH = kit::kStripRowH;
constexpr double kSeekBand = kBandH / 2;  // the band is the seek bar
constexpr double kButton = kit::kStripButton;  // a round button, its icon:
constexpr double kButtonIcon = 28, kPlayIcon = 30;
constexpr double kButtonGap = kit::kSpaceTight;  // between the buttons of a cluster
constexpr double kCluster = 24;            // between clusters, at least
// Text without a box of its own, from its row's box: the clock, the band's.
constexpr double kInset = kit::kSpace;
// A tab, and Quit: its icon and label (kit::kSpace in from its ends).
constexpr double kTabIcon = 26, kTabIconGap = 10;
constexpr int kTabSize = 24;
constexpr int kClockSize = 26, kInfoSize = 22, kBandSize = 26;
// A label on the top row (Software decoding, a found update): a chip.
constexpr double kChipH = 40, kChipPad = 14, kChipCorner = kit::kPieceCorner;
constexpr int kChipSize = 22;
// The rating: five stars in cells of 40.
constexpr double kStarCell = 40;
// The volume: the speaker, the slider, then the number, its right edge kInset
// in from the strip's last box.
constexpr double kVolumeW = 220, kNumberW = 48;
constexpr int kNumberSize = 26;
// A list opened above its button: items one under the other, columns of at
// most kListRows; kListAbove under the list's bottom is the row's top.
constexpr double kListItemH = 52, kListAbove = 12;
constexpr int kListRows = 6;
constexpr int kListSize = 25, kListSubSize = 22;
}  // namespace strip

// The browser, in its texture's pixels: the navigation column on the left,
// then the central page from Menu::kPageX.
constexpr double kPagePad = 24;
constexpr double kNavRowH = 56, kPlaceRowH = 48;  // section rows, and places under them
constexpr int kNavPlaces = 11;                     // at most so many places
// The navigation column's rows (the kit's): kit::kSpace inside its panel,
// places indented under File explorer, a round button at a row's end
// (Edit places, the pager's), the rule under the places.
constexpr double kNavLeft = kit::kPanelInset + kit::kSpace, kNavTop = kNavLeft;
constexpr double kNavWidth = menu_geometry::kSourcesWidthPx - 2 * kNavLeft;
constexpr double kNavIndent = 24, kNavButton = 48, kNavRuleH = 2;
// A content page's parts (the kit's): kit::kSpace inside its panel; the
// header row (up, where you are, its actions), the tools row (views, order,
// search, jumps), then the list's column headings and its rows, or the
// cards straight under the tools.
constexpr double kContentInset = kit::kPanelInset + kit::kSpace;
constexpr double kContentLeft = Menu::kPageX + kContentInset;
constexpr double kContentRight = Menu::kPageX + Menu::kPageWidth - kContentInset;
constexpr double kBrowserRowY = kContentInset, kBrowserRowH = 56;
constexpr double kBrowserToolsY = kBrowserRowY + kBrowserRowH + kit::kSpace, kBrowserToolsH = 56;
constexpr double kListHeaderY = kBrowserToolsY + kBrowserToolsH + kit::kSpace, kListHeaderH = 40;
constexpr double kListTop = kListHeaderY + kListHeaderH + kit::kSpacePiece, kGridTop = kListHeaderY;
constexpr double kListBottom = Menu::kBrowserHeight - kContentInset;
// The scroll bar's track: kScrollTrackW wide along the panel's inside edge;
// the rows end kit::kSpace left of it, 30 left of Menu::listRight_. Its
// target is kScrollbarW wide, its thumb at least kScrollThumbMin long.
constexpr double kScrollTrackW = 10, kScrollbarW = 40, kScrollThumbMin = 48;
constexpr double kListRightPad = 30 - kit::kSpace - kScrollTrackW;
// Where a list's content fades out where more of it is, above and below.
// Each fade also covers the panel kListFadePast past the list's edge,
// opaque: sampling the browser's texture carries the list's edge rows a few
// pixels beyond it, more where the eye image is coarser (foveation), which
// over a bright picture shows as a line there.
constexpr double kListFadeH = 28, kListFadePast = 8;
// What moves with a list across: its rows and cards with the playing one's
// shadow (kListShadowX out at the left), up to kListRightCut left of
// Menu::listRight_ (clear of the scroll bar's track).
constexpr double kListShadowX = 8, kListRightCut = 22;
// List rows 52 high, a small picture in each; kRowGap of each is the gap to
// the next.
constexpr double kFileRowH = 56, kRowGap = 4;
constexpr int kFileRows = static_cast<int>((kListBottom - kListTop) / kFileRowH);  // whole ones
constexpr double kRowPictureIn = 4;  // a row's picture from its edges
constexpr double kRowNameGap = 16;   // ... and from the name
constexpr double kSizeColumnW = 96, kDateColumnW = 176;  // Offline's columns
constexpr double kPlaysColumnW = 56;
constexpr double kStatusColumnW = 40;  // a video's offline download: its tick or button
constexpr double kColumnGap = 24, kStatusGap = 8;  // between them; the status column's
// A rating control is five stars in equal cells, nothing beside them.
constexpr double kRowStarW = 28;  // a star's cell in a row's rating
constexpr double kRowStarSize = 26;
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
// status, rating, plays, length, size and date, as wide and as far apart
// as `fit` has them (Menu::listFit()).
struct ListColumns {
  double status, rating, plays, length, size, date;
};
inline ListColumns listColumns(double x1, const ListFit& fit) {
  ListColumns c;
  c.date = x1 - kit::kSpace - fit.date;
  c.size = c.date - kColumnGap - fit.apart[3] - fit.size;
  c.length = c.size - kColumnGap - fit.apart[2] - fit.length;
  c.plays = c.length - kColumnGap - fit.apart[1] - kPlaysColumnW;
  c.rating = c.plays - kColumnGap - fit.apart[0] - kRatingColumnW;
  c.status = c.rating - kStatusGap - kStatusColumnW;
  return c;
}
// A row's byline (Stash's performers and studio) follows its name; a long
// name keeps at least this share of the name's column.
constexpr double kBylineShare = 0.65;
constexpr double kRowBylineGap = 18;  // between a row's name and its byline
// Cards: the picture 8 in (16:9), the name's two lines (30 each) 12 under it,
// the details' line, 16 at the bottom; kGalleryGap apart.
constexpr int kGalleryColumns = 4;
constexpr double kGalleryGap = kit::kSpace;
constexpr double kCardIn = 8, kCardNameGap = 12, kCardLine = 30, kCardCorner = 18;
// The playing video's mark before its name (a row's and a card's).
constexpr double kPlayMarkSize = 20, kPlayMarkW = kPlayMarkSize + 10;
constexpr double kListWidth = kContentRight - kScrollTrackW - kit::kSpace - kContentLeft;
constexpr double kFileCardW = (kListWidth - (kGalleryColumns - 1) * kGalleryGap) / kGalleryColumns;
// The pictures of the gallery's tiles and the list's rows as addGallery()
// and addList() lay them out on the page: what covers and thumbnails are
// made for.
constexpr double kCardPictureW = kFileCardW - 2 * kCardIn;
constexpr double kCardPictureH = static_cast<int>(kCardPictureW * 9 / 16 + 0.5);
constexpr double kFileCardH = kCardIn + kCardPictureH + kCardNameGap + 3 * kCardLine + kit::kSpace;
constexpr double kGalleryRowH = kFileCardH + kGalleryGap;
constexpr int kGalleryRows = 2;  // whole; a third peeks in under them
constexpr double kRowPictureW = static_cast<int>((kFileRowH - kRowGap - 2 * kRowPictureIn) * 16 / 9);
// While typing, the keyboard takes the bottom of the page and what it types
// into the rest: a well of five rows of keys (the kit's, 8 apart), kit::kSpace
// above the panel's edge. kKeyboardY is the well's top; what is above it
// ends kit::kSpace over it.
constexpr double kKeyH = kit::kKeyH, kKeyStep = kit::kKeyH + kit::kSpaceTight;
constexpr double kKeyboardPadY = kit::kSpaceTight;  // the keys from the well's edges
constexpr double kKeyboardY = kListBottom - 2 * kKeyboardPadY - 4 * kKeyStep - kKeyH;
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

constexpr double kButtonRadius = kit::kCorner;
constexpr int kTooltipSize = kit::kTipSize;

constexpr double kIconButtonW = 120;

constexpr double kIconSize = 52;

constexpr int kSlideshowSeconds[] = {0, 3, 5, 10, 20, 30, 60};

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

// "2024-05-01", local time: a day in a list's column or under a thumbnail.
inline std::string dayText(int64_t seconds) {
  if (seconds <= 0) return "";
  const std::time_t t = static_cast<std::time_t>(seconds);
  std::tm local{};
  localtime_r(&t, &local);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", &local);
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

inline std::string formatTime(double seconds) { return clockTime(seconds); }

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

// The left part, w wide, of a rounded box `full` wide: a value's fill in its
// well, without clipping to the well (a clip to a curve costs a mask).
inline void barPath(cairo_t* cr, double x, double y, double w, double h, double r, double full) {
  r = std::min(r, h / 2);
  const double left = std::min(r, w / 2), right = std::clamp(r - (full - w), 0.0, w / 2);
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - right, y + right, right, -M_PI / 2, 0);
  cairo_arc(cr, x + w - right, y + h - right, right, 0, M_PI / 2);
  cairo_arc(cr, x + left, y + h - left, left, M_PI / 2, M_PI);
  cairo_arc(cr, x + left, y + left, left, M_PI, 3 * M_PI / 2);
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

// The colours: white text on a dark matte panel, dimmer and fainter text for
// what matters less, the accent (blue unless chosen otherwise, accent.h) for
// what is chosen. Surfaces have finishes (menu_finish.h); these are their
// flat colours, and the text's.
inline Rgba hexColour(unsigned c, double a = 1) {
  return {((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0, a};
}

inline Palette paletteFor(const AccentColours& accent) {
  Palette p;
  p.panel = hexColour(0x1a1d24);
  p.button = {1, 1, 1, 0.09};
  p.field = {0, 0, 0, 0.30};
  p.text = hexColour(0xf2f4f8);
  p.textDim = hexColour(0xa3abb8);
  p.faint = hexColour(0x808895);  // 4.7:1 on the panel, 4.5:1 at its lightest
  p.buffered = {1, 1, 1, 0.22};
  p.rule = {1, 1, 1, 0.08};
  p.danger = hexColour(0xe5484d);
  p.gold = hexColour(0xffc22e);
  p.ok = hexColour(0x3ad070);
  p.edge = p.accent = {accent.fill[0], accent.fill[1], accent.fill[2], 1};
  p.accentSoft = {accent.fill[0], accent.fill[1], accent.fill[2], 0.16};
  p.textAccent = {accent.text[0], accent.text[1], accent.text[2], 1};
  return p;
}

inline void setColor(cairo_t* cr, const Rgba& c, double alpha = 1.0) {
  cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a * alpha);
}

// A group of settings: a shallow darker box (its title is drawn by the
// caller's text helper, at its top left).
inline void groupFrame(cairo_t* cr, double x, double y, double w, double h) {
  roundedRect(cr, x, y, w, h, kit::kGroupCorner);
  cairo_set_source_rgba(cr, 0, 0, 0, 0.18);
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

constexpr double kDialogW = 1160, kDialogH = 400;

}  // namespace
