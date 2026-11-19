#pragma once

// A content page's sizes beyond menu_style.h's: a view's icon piece in a
// tools well, a raised button's text from its ends, a count's size (dim,
// after its label), a round button in a row's right end (kit::kSpace in).
constexpr double kViewPieceW = 52, kButtonPadX = 20, kRowRoundSize = 40;
constexpr int kCountSize = 22;

// The parts of the browser's navigation column and of its content pages
// (the File explorer, the Library, Offline) that draw themselves:
// Menu::Button::listLook, drawn by Menu::drawListPart() (menu_list_parts.cpp).
enum class ListLook {
  None,
  NavSection,  // a navigation row (File explorer, Network …): its icon and label
  NavPlace,    // a place under File explorer: smaller, its label dim unless chosen
  PlacePin,    // a pinned place's pin at its row's end: faint, in the accent
  PagerDots,   // the places' pages: a dot each, the shown one (sliderValue of sliderMax) in the accent
  Heading,     // a list's column heading: semibold; the sorted one with its direction's arrow
  RowStars,    // a list row's stars (drawRating()): not shown while unrated, until pointed at
  Search,      // a search field in a tools row: its icon, text, and what it finds (`detail`)
  Section,     // a heading within a list (Offline's To go): semibold, dim, in line with the names
  Counted,     // a piece of a tools row's well: its label, then a count (`detail`), dim
  Badge,       // a label on a card's picture, from its top left corner ("VR · 8K"): not clickable
  // Drawn over the row or card before it by its own look (stashLook): its
  // columns, say; not clickable.
  Overlay,
};
