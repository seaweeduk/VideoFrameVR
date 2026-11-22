// Settings' Controls: the controllers in use as SteamVR
// draws them, a label for each control with a line to it. A button's label
// is raised: pressed, a chooser for its job opens. The controls that can't
// be changed (the trigger, the grip, the stick's directions) are flush and
// dim, their lines dashed. Without SteamVR's drawings the labels are a list,
// as first-run's Your controllers has them. The well under them shows the
// buttons' jobs with grip held instead, or the gestures as such a list: each
// a row, pointed at its editor opens, the ones that can't be changed flush.
// Under the controllers, how far Skip and Big skip go. The page never
// scrolls.

#include "menu_controls.h"

#include "menu_parts.h"
#include "menu_setup.h"

#include <pango/pangocairo.h>

#include <cmath>
#include <cstdio>

namespace {

using Part = ControlsPage::Part;
using Lead = ControlsPage::Lead;

constexpr double kPageL = Menu::kPageX + kit::kPanelInset + kit::kSpace;
constexpr double kPageR = Menu::kPageX + Menu::kPageWidth - kit::kPanelInset - kit::kSpace;
// Settings' pages end a margin above the panel's bottom, or they scroll.
constexpr double kTop = kit::kPanelInset + kit::kSpace + kit::kPageHeaderH + kit::kSpace;
constexpr double kBottom = Menu::kBrowserHeight - kPagePad;
constexpr double kRow = kit::kRowH, kPitch = kit::kRowStep;
// Under the controllers, the skips: a row.
constexpr double kUnderH = kit::kGroupTitle + kPitch, kUnderY = kBottom - kUnderH;
// The controllers' group: a column of labels outside each drawing, one
// between them for what faces the other hand and what both hands do, each
// wide enough for the longest job in English beside a glyph and a gesture's
// mark.
constexpr double kInX = kPageL + kit::kGroupPadX, kInW = kPageR - kPageL - 2 * kit::kGroupPadX;
constexpr double kOuterW = 304, kMidW = 312;
constexpr double kDrawW = (kInW - 2 * kOuterW - kMidW - 4 * kit::kSpace) / 2;
constexpr double kMidX = kInX + kOuterW + kit::kSpace + kDrawW + kit::kSpace;
constexpr double kDrawX[2] = {kInX + kOuterW + kit::kSpace, kMidX + kMidW + kit::kSpace};
constexpr double kLabelX[3] = {kInX, kMidX, kInX + kInW - kOuterW};  // left, middle, right
constexpr double kLabelW[3] = {kOuterW, kMidW, kOuterW};
constexpr double kAreaTop = kTop + kit::kGroupTitle;
constexpr double kAreaH = kUnderY - kit::kSpace - kit::kSpaceTight - kAreaTop;
// The drawings fade out over their bottom, and their top where it is cut off.
constexpr double kFadeBottom = 72, kFadeTop = 96;
// Glyphs are drawn kGlyph square and shown at these sizes.
constexpr double kGlyph = 44, kLabelGlyph = 36, kChipGlyph = 28;
constexpr double kNoticeGlyph = 40, kHeadGlyph = 44;
constexpr int kLabelSize = 24, kHowSize = 21;
// The notice: the hints' pill, at most kNoticeW wide (taller with more
// lines, never cut); Undo inside its end.
constexpr double kNoticeW = 1040, kUndoH = 56, kNoticePadY = 20;
constexpr int kNoticeLines = 8;
constexpr int kUndoSize = 25;
// The chooser: its header, then the section's jobs in two columns.
constexpr double kHeadH = 56;
constexpr double kJobH = 52, kJobPitch = 56;
constexpr int kJobSize = 25, kSectionSize = 24;
constexpr double kListHeadingH = 32;
// The gestures' list: as many rows as fit in a column above the well.
constexpr int kListRows =
    static_cast<int>((kAreaH - kListHeadingH - kRow - kit::kSpace + kPitch - kRow) / kPitch);
static_assert(kListRows == kGestureRows, "the gestures' columns hold what GestureMap allows");
// A gesture's row: its controls' glyphs (right-aligned to at least
// kGestureGlyphsW), a + between them, the movement, the twist's mark.
constexpr double kGestureGlyphsW = 104, kRowGlyph = 36, kGlyphGap = 6, kTurn = 26, kLabelTurn = 26;
constexpr int kMoveSize = 22;
// The editor: wider than the chooser for its drawings; the first step's
// drawings and what is held between them, the second's hands well.
constexpr double kEditorW = 1100, kEditorDrawW = 300, kEditorDrawH = 400;
constexpr double kHeldGlyph = 60, kHeldBoxH = 120, kHandsH = 48;
constexpr int kHeldNameSize = 28;
// What is held, smaller where it doesn't fit the well in one line.
constexpr double kHeldGlyphSmall = 44;
constexpr int kHeldNameSmall = 22;

// The chooser's rows: the longest section's, whichever shows, so it keeps its
// height from section to section.
int jobRows() {
  int most = 0;
  for (int s = 1; s <= static_cast<int>(JobSection::Picture); ++s) {
    int jobs = 0;
    for (int j = 1; j < kJobs; ++j) jobs += jobInfo(static_cast<Job>(j)).section == static_cast<JobSection>(s);
    most = std::max(most, (jobs + 1) / 2);
  }
  return most;
}

// What a label is for: a button, or a control that can't be changed.
struct Item {
  enum What : uint8_t { Button, Trigger, Grip, Seek, Closer } what;
  int hand;  // -1: both, one label
  Control control;
};

// Each type's labels in the left column, the middle one and the right one,
// top to bottom; each is placed as near its control's height as it goes.
std::vector<Item> columnItems(ControllerType type, int column) {
  using C = Control;
  auto b = [](int hand, Control c) { return Item{Item::Button, hand, c}; };
  auto seek = [](int hand) { return Item{Item::Seek, hand, C::Count}; };
  auto closer = [](int hand) { return Item{Item::Closer, hand, C::Count}; };
  const Item trigger{Item::Trigger, -1, C::Trigger}, grip{Item::Grip, -1, C::Grip};
  switch (type) {
    case ControllerType::Frame:
      if (column == 0)
        return {b(0, C::DpadUp), b(0, C::DpadLeft), b(0, C::DpadRight), b(0, C::DpadDown),
                b(0, C::StickClick), seek(0), closer(0)};
      if (column == 1) return {b(0, C::View), b(1, C::Menu), b(0, C::Bumper), b(1, C::Bumper), trigger, grip};
      return {b(1, C::Y), b(1, C::B), b(1, C::X), b(1, C::A), b(1, C::StickClick), seek(1), closer(1)};
    case ControllerType::Touch:
      if (column == 0) return {b(0, C::StickClick), seek(0), closer(0), b(0, C::Menu), b(0, C::X), b(0, C::Y)};
      if (column == 1) return {trigger, grip};
      return {b(1, C::StickClick), seek(1), closer(1), b(1, C::A), b(1, C::B)};
    case ControllerType::Index:
      if (column == 1) return {trigger, grip};
      return {b(column / 2, C::StickClick), seek(column / 2), closer(column / 2), b(column / 2, C::B),
              b(column / 2, C::A)};
    default:
      if (column == 1) return {trigger};
      return {b(column / 2, C::Menu)};
  }
}

const LayoutControl* layoutControl(ControllerType type, int hand, Control control) {
  for (const LayoutControl& c : controllerLayout(type).controls)
    if (c.hand == hand && c.control == control) return &c;
  return nullptr;
}

std::string glyphOf(const LayoutControl& c) { return controlGlyph(c.input, ControlAction::Select, false); }

// Labels in a column, each as near as it goes to `want` (its top), kPitch
// apart in the order given: those that would overlap become a block centred
// on theirs, within top..bottom.
std::vector<double> placeRows(const std::vector<double>& wants, double top, double bottom) {
  std::vector<std::vector<double>> blocks;
  for (double want : wants) blocks.push_back({want});
  auto at = [&](const std::vector<double>& b) {
    double sum = 0;
    for (size_t k = 0; k < b.size(); ++k) sum += b[k] - k * kPitch;
    return std::max(top, std::min(bottom - b.size() * kPitch + kit::kSpaceTight, sum / b.size()));
  };
  for (bool again = true; again;) {
    again = false;
    for (size_t i = 1; i < blocks.size(); ++i)
      if (at(blocks[i - 1]) + blocks[i - 1].size() * kPitch > at(blocks[i]) + 0.5) {
        blocks[i - 1].insert(blocks[i - 1].end(), blocks[i].begin(), blocks[i].end());
        blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(i));
        again = true;
        break;
      }
  }
  std::vector<double> tops;
  for (const auto& b : blocks)
    for (size_t k = 0; k < b.size(); ++k) tops.push_back(at(b) + k * kPitch);
  return tops;
}

bool paintGlyph(cairo_t* cr, const ControllerArt* art, const std::string& name, double cx, double cy,
                double size, double alpha = 1) {
  if (!art) return false;
  const auto it = art->glyphs.find(name);
  if (it == art->glyphs.end() || !it->second) return false;
  const double s = size / (kGlyph * menu_geometry::kRasterScale);
  cairo_save(cr);
  cairo_translate(cr, cx - size / 2, cy - size / 2);
  cairo_scale(cr, s, s);
  cairo_set_source_surface(cr, it->second.get(), 0, 0);
  cairo_paint_with_alpha(cr, alpha);
  cairo_restore(cr);
  return true;
}

// A button of the type can take a job with grip held.
bool gripCombos(ControllerType type) {
  for (const LayoutControl& lc : controllerLayout(type).controls)
    if (!lc.fixed && lc.control != Control::StickClick) return true;
  return false;
}

const char* printArg(const std::string& s) { return s.c_str(); }
const char* printArg(const char* s) { return s; }
int printArg(int n) { return n; }
template <typename... Args>
std::string printed(const char* format, const Args&... args) {
  char line[512];
  std::snprintf(line, sizeof(line), format, printArg(args)...);
  return line;
}

// The notice's glyphs: joined by a +, then after a gesture's the twist's mark.
constexpr double kPlusW = 22, kNoticeTurn = 34;
double noticeGlyphsW(size_t glyphs, bool turn) {
  return static_cast<double>(glyphs) * kNoticeGlyph + static_cast<double>(glyphs - 1) * kPlusW +
         (turn ? kit::kSpaceTight + kNoticeTurn : 0);
}

// The twist's mark: an arc over a dot, its arrow turning clockwise.
void drawTurn(cairo_t* cr, double cx, double cy, double size, const Rgba& ink, double alpha = 1) {
  const double s = size / 24;
  cairo_save(cr);
  cairo_translate(cr, cx - size / 2, cy - size / 2);
  cairo_scale(cr, s, s);
  cairo_set_line_width(cr, 2.2);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_new_path(cr);
  cairo_arc(cr, 12, 15, 7.5, M_PI, 2 * M_PI);
  cairo_move_to(cr, 16.6, 12.6);
  cairo_line_to(cr, 19.5, 15.4);
  cairo_line_to(cr, 22.3, 12.5);
  setColor(cr, ink, alpha);
  cairo_stroke(cr);
  cairo_arc(cr, 12, 15, 2.2, 0, 2 * M_PI);
  cairo_fill(cr);
  cairo_restore(cr);
}

// A control's ring, glowing in the accent: the one being changed, or held.
void drawRing(cairo_t* cr, double x, double y, const Rgba& accent) {
  constexpr double r = 16;
  for (const auto& [width, alpha] : {std::pair{10.0, 0.15}, {5.0, 0.3}}) {
    cairo_arc(cr, x, y, r + width / 2, 0, 2 * M_PI);
    setColor(cr, accent, alpha);
    cairo_set_line_width(cr, width);
    cairo_stroke(cr);
  }
  cairo_arc(cr, x, y, r, 0, 2 * M_PI);
  setColor(cr, accent, 0.35);
  cairo_fill_preserve(cr);
  setColor(cr, accent);
  cairo_set_line_width(cr, 3);
  cairo_stroke(cr);
}
constexpr double kRingReach = 16 + 10 + 2;

const char* typeTitle(ControllerType type, const ControlsPageStrings& t) {
  switch (type) {
    case ControllerType::Frame: return t.frame;
    case ControllerType::Index: return t.index;
    case ControllerType::Touch: return t.touch;
    default: return t.simple;
  }
}

// A button's name as a label (the chooser's header, tooltips): "B",
// "Bumper", "Stick click".
std::string buttonName(const LayoutControl& c, const Strings& t) {
  if (c.control == Control::Trigger) return t.controlsPage.triggerName;
  if (c.control == Control::Grip) return t.controlsPage.gripName;
  if (c.control == Control::Bumper) return t.controlsPage.bumperName;
  if (c.control == Control::DpadUp) return t.controlsPage.dpadUpName;
  if (c.control == Control::DpadDown) return t.controlsPage.dpadDownName;
  if (c.control == Control::DpadLeft) return t.controlsPage.dpadLeftName;
  if (c.control == Control::DpadRight) return t.controlsPage.dpadRightName;
  if (c.control == Control::StickClick) return t.controlsPage.stickClickName;
  return capitalized(controlNames({{c.hand, c.input}}, t.controls));
}

// A fixed control's or a gesture's glyph's name, where the glyph is missing.
const char* glyphName(const std::string& glyph, const ControlsPageStrings& cp) {
  if (glyph == "button_grip") return cp.gripName;
  if (glyph == "button_trigger") return cp.triggerName;
  if (glyph == "button_thumbstick_click") return cp.stickClickName;
  if (glyph.rfind("button_thumbstick", 0) == 0) return cp.stickName;
  return "";
}

// A gesture row's control's name, where its glyph is missing.
std::string gestureControlName(const Part& p, size_t i, const ControlsPageStrings& cp) {
  return p.names[i].empty() ? std::string(glyphName(p.glyphs[i], cp)) : p.names[i];
}

bool hasGlyph(const ControllerArt* art, const std::string& name) {
  if (!art) return false;
  const auto it = art->glyphs.find(name);
  return it != art->glyphs.end() && it->second;
}

// A gesture's movement word, unless its last control's name (shown where
// its glyph isn't) already says it.
std::string shownMove(const Part& p, bool lastGlyph) {
  return p.moveNamed && !lastGlyph ? std::string() : p.move;
}

// A gesture's row: the space between its parts, and where what it does
// ends (a user's just before its remove ×, a button inside its end).
constexpr double kRemoveW = 44, kRemoveInset = 4;
double gestureGap(const Part& p) { return p.fixed ? 12 : 10; }
double gestureEnd(const Part& p) {
  return p.x + p.w - (p.fixed ? kit::kSpace : kRemoveInset + kRemoveW + kit::kSpacePiece);
}

// A slot's job as the page shows it: with grip held and none of its own,
// kInherit.
int shownJob(const ControlMap& map, const ControlSlot& s) {
  if (!s.grip) return static_cast<int>(map.job(s.type, s.hand, s.control));
  return map.hasGrip(s.type, s.hand, s.control) ? static_cast<int>(map.gripJob(s.type, s.hand, s.control))
                                                : ControlsPage::kInherit;
}

// A change of a slot's job (kInherit: none with grip held).
MenuCommand setJob(const ControlSlot& slot, int job) {
  MenuCommand cmd;
  cmd.type = MenuCommand::SetButtonJob;
  cmd.option = slot.pack();
  cmd.value = job;
  return cmd;
}

// ... and in the notice's sentence: "the left bumper", "the Menu button".
std::string buttonNoun(ControllerType type, const LayoutControl& c, const Strings& t) {
  const ControlsPageStrings& p = t.controlsPage;
  std::string noun = c.control == Control::Menu       ? p.menuButton
                     : c.control == Control::View     ? p.viewButton
                     : c.control == Control::StickClick ? p.stickClick
                                                      : controlNames({{c.hand, c.input}}, t.controls);
  if (c.control == Control::Menu || c.control == Control::View || !layoutControl(type, 1 - c.hand, c.control))
    return noun;
  char line[256];
  std::snprintf(line, sizeof(line), c.hand ? p.rightOne : p.leftOne, noun.c_str());
  return line;
}

// ---------------------------------------------------------------- gestures

// Its controls, in the order said and shown: the grip first, the trigger last.
std::vector<Control> gestureControls(const Gesture& g) {
  std::vector<Control> out{g.first};
  if (g.second != Control::Count) out.push_back(g.second);
  return out;
}

// A hand that has the gesture's controls (the right one where both have them).
int gestureHand(const Gesture& g) { return g.hands & 2 ? 1 : 0; }

// Both controllers have its controls: one hand's says which.
bool bothHave(ControllerType type, const Gesture& g) {
  for (const Control c : gestureControls(g))
    if (!layoutControl(type, 0, c) || !layoutControl(type, 1, c)) return false;
  return true;
}

std::string controlGlyphName(ControllerType type, int hand, Control control) {
  if (control == Control::Grip) return "button_grip";
  if (control == Control::Trigger) return "button_trigger";
  const LayoutControl* lc = layoutControl(type, hand, control);
  if (!lc) lc = layoutControl(type, 1 - hand, control);
  return lc ? glyphOf(*lc) : std::string();
}

// A control's name in a gesture's: as a label ("Grip", "Y"), or in a sentence
// and after another ("grip", "trigger", "Y").
std::string controlName(ControllerType type, int hand, Control control, bool sentence, const Strings& t) {
  if (sentence) {
    if (control == Control::Grip) return t.controls.grip;
    if (control == Control::Trigger) return t.controls.trigger;
    if (control == Control::Bumper) return t.controls.bumper;
    if (control == Control::StickClick) return t.controlsPage.stickClick;
  }
  const LayoutControl* lc = layoutControl(type, hand, control);
  if (!lc) lc = layoutControl(type, 1 - hand, control);
  return lc ? buttonName(*lc, t) : std::string();
}

// Its controls as said: "Y", "Grip + trigger", "Right A"; in a sentence
// "grip + trigger", "the right A". In some languages it starts with an
// article ("la poignée"): capitalized() where it starts a title or a line.
std::string gestureName(ControllerType type, const Gesture& g, const Strings& t, bool sentence = false) {
  const int hand = gestureHand(g);
  const bool side = g.hands != 3 && bothHave(type, g);
  std::string name;
  for (const Control c : gestureControls(g)) {
    if (!name.empty()) name += " + ";
    name += controlName(type, hand, c, sentence || side || !name.empty(), t);
  }
  if (!side) return name;
  const GesturePageStrings& gp = t.gesturePage;
  const ControlsPageStrings& cp = t.controlsPage;
  return printed(sentence ? (hand ? cp.rightOne : cp.leftOne) : (hand ? gp.rightName : gp.leftName), name);
}

// Where a value works, beside its name: none where it works anywhere, or
// where its name says it (Zoom (180°/360°)).
const char* gestureWhere(GestureValue value, const Strings& t) {
  if (value == GestureValue::Zoom) return "";
  switch (gestureValueInfo(value).where) {
    case GestureWhere::Surround: return t.gesturePage.surround;
    case GestureWhere::Flat: return t.controlsPage.heightHow;
    default: return "";
  }
}

// What a value needs to change, as the value chooser's tooltip says it (as
// main.cpp's gestureHost.why checks it); "" where a file open is enough.
const char* gestureNeeds(GestureValue value, const GesturePageStrings& gp) {
  using V = GestureValue;
  switch (value) {
    case V::Opacity: return gp.onlyRoom;
    case V::MatchBrightness:
    case V::MatchColour: return gp.onlyMatchRoom;
    case V::KeyStrength:
    case V::KeyEdges:
    case V::KeySpill:
    case V::KeyShrink: return gp.onlyRemoveBackground;
    case V::Scrub: return gp.onlyVideo;
    case V::Rating: return gp.onlyRated;
    default: break;
  }
  switch (gestureValueInfo(value).where) {
    case GestureWhere::Surround: return gp.onlySurround;
    case GestureWhere::Flat: return gp.onlyFlat;
    default: return "";
  }
}

// The gestures that can't be changed, as the list heads Either's column.
struct FixedGesture {
  const char* name;
  std::vector<std::string> glyphs;
  bool plus;  // a + before its movement's word
  const char* move;
  bool turn;  // ... and the twist after it
  const char* where;
  const char* tip;
  bool clicks = false;  // its movement is a click of its last control (the stick's)
};
std::vector<FixedGesture> fixedGestures(ControllerType type, const Strings& t) {
  const ControlsPageStrings& cp = t.controlsPage;
  const GesturePageStrings& gp = t.gesturePage;
  const bool grip = layoutControl(type, 0, Control::Grip) != nullptr;
  const bool stick = controllerLayout(type).stick;
  std::vector<FixedGesture> out;
  out.push_back({cp.horizon, {"button_trigger"}, false, gp.drag, true, gp.surround, cp.horizonTip});
  if (grip && stick)
    out.push_back({cp.height, {"button_grip", "button_thumbstick_scroll"}, false, cp.closerHow, false,
                   cp.heightHow, cp.heightTip});
  out.push_back({gp.resize, {"button_trigger"}, true, gp.move, false, cp.sizeHow, cp.sizeTip});
  if (grip) out.push_back({cp.move, {"button_grip"}, true, gp.move, false, gp.onMenu, gp.moveMenuTip});
  if (grip && stick)
    out.push_back({cp.clearRating, {"button_grip", "button_thumbstick_click"}, false, gp.click, false,
                   cp.clearRatingHow, cp.clearRatingTip, true});
#ifndef NDEBUG
  if (static_cast<int>(out.size()) != kGestureRows - 1 - gestureColumnRoom(type, 1))
    std::fprintf(stderr, "[fit] Controls: %zu fixed gestures, gestureColumnRoom() counts otherwise\n", out.size());
#endif
  return out;
}

// A type can have gestures: a grip, or a button that can be changed.
bool gesturesFor(ControllerType type) {
  for (const LayoutControl& lc : controllerLayout(type).controls)
    if (!lc.fixed || lc.control == Control::Grip) return true;
  return false;
}

// The controls of a hand that can be a gesture's.
uint32_t gestureBits(ControllerType type, int hand) {
  uint32_t bits = 0;
  for (const LayoutControl& lc : controllerLayout(type).controls)
    if (lc.hand == hand && (!lc.fixed || lc.control == Control::Grip || lc.control == Control::Trigger))
      bits |= controlBit(lc.control);
  return bits;
}

void startRecording(ControlsPage::Editor& e, const uint32_t held[2]);

}  // namespace

bool Menu::recordingGesture() const {
  return dialog_ == Dialog::EditGesture && controls_ && controls_->editor.step == 1 && controlsShown();
}

void Menu::stopRecordingGesture() {
  if (dialog_ != Dialog::EditGesture || !controls_ || controls_->editor.step != 1) return;
  dialog_ = Dialog::None;
  dirty_ = true;
}

void Menu::setControlsHeld(const uint32_t held[2], uint8_t lost) {
  if (!controls_) return;
  ControlsPage& c = *controls_;
  if (dialog_ == Dialog::EditGesture && c.editor.step == 1 &&
      ((lost & 1 && c.editor.seen[0]) || (lost & 2 && c.editor.seen[1]))) {
    startRecording(c.editor, held);
    dirty_ = true;
  }
  if (c.held[0] == held[0] && c.held[1] == held[1]) return;
  const uint32_t was[2] = {c.held[0], c.held[1]};
  c.held[0] = held[0], c.held[1] = held[1];
  if (recordingGesture()) recordGesture();
  if (!controlsShown() || dirty_) return;
  // Only what changes is drawn again: a label, and the ring on its control.
  auto changed = [&](int hand, Control control) {
    return control != Control::Count && ((held[hand] ^ was[hand]) & controlBit(control));
  };
  for (const Part& p : c.parts)
    for (int h = 0; h < 2; ++h)
      if ((p.hands >> h & 1) && changed(h, p.control)) browserChanged(p.y - 2, p.y + p.h + 2, p.x - 2, p.x + p.w + 2);
  for (const Lead& l : c.leads)
    if (changed(l.hand, l.control))
      browserChanged(l.y - kRingReach, l.y + kRingReach, l.x - kRingReach, l.x + kRingReach);
}

void Menu::updateControls(const MenuView& view) {
  if (!controls_) return;
  ControlsPage& c = *controls_;
  std::shared_ptr<const ControllerArt> art;
  if (c.loading.take(art)) {
    c.art = art;
    if (controlsShown()) dirty_ = true;
  }
  if (view.controls == view_.controls) return;
  if (view.controls.type != view_.controls.type) {
    c.noticeShown = false;
    if (dialog_ == Dialog::ChooseJob || dialog_ == Dialog::ResetButtons || dialog_ == Dialog::EditGesture)
      dialog_ = Dialog::None;
  }
  if (controlsShown()) dirty_ = true;
}

void Menu::layoutControls(PageLayout& page) {
  if (!controls_) controls_ = std::make_unique<ControlsPage>();
  ControlsPage& c = *controls_;
  const Strings& t = text();
  const ControlsPageStrings& cp = t.controlsPage;
  const ControlsView& v = view_.controls;
  const ControllerType type = v.type;
  if (c.type != type) {
    c.type = type;
    c.loading.start([type, reuse = std::move(c.art)] {
      return loadControllerArt(layoutSources(type), kDrawW, 4 * kDrawW, kGlyph, menu_geometry::kRasterScale,
                               reuse);
    });
    c.art = nullptr;
  }
  c.parts.clear();
  c.leads.clear();
  c.headings.clear();
  c.headingX.clear();

  // Grip combinations, where a button can take one, and gestures: the well
  // under the drawings switches the labels to them.
  const bool combos = gripCombos(type), gestures = gesturesFor(type);
  if (!combos) c.gripView = false;
  if (!gestures) c.gestureView = false;
  GroupLayout group = page.groupAt(0, kPageL, kTop, kPageR - kPageL, typeTitle(type, cp),
                                   {t.reset, combos ? cp.resetGripTip : cp.resetTip, [this] {
                                      controls_->noticeShown = false;
                                      openDialog(Dialog::ResetButtons, "");
                                      return MenuCommand{};
                                    }});
  group.room(kAreaH);
  const size_t firstButton = buttons_.size();  // the labels' and rows'
  const double y0 = kAreaTop, areaBottom = kAreaTop + kAreaH;
  const double bottom = areaBottom - (combos || gestures ? kRow + kit::kSpace : 0);  // the labels'
  c.artTop = y0, c.artBottom = areaBottom;
  const ControllerArt* art = c.art.get();
  c.picture = art && art->hands[0].image && art->hands[1].image && !c.gestureView;

  // An invisible part with a tooltip (flush labels, gestures).
  auto tip = [this](double x, double y, double w, double h, const std::string& text) {
    if (text.empty()) return;
    Button b;
    b.panel = kBrowser;
    b.x = x, b.y = y, b.w = w, b.h = h;
    b.staticText = b.disabled = true;
    b.action = [] { return MenuCommand{}; };
    b.tooltip = text;
    buttons_.push_back(b);
  };
  auto howWidth = [this](const Part& p) {
    double w = kLabelGlyph + kit::kSpaceTight + 4 + textWidth(p.text, kLabelSize, false);
    if (!p.how.empty()) w += kit::kSpaceTight + textWidth("· " + p.how, kHowSize, false);
    return w;
  };

  // While the drawings load, no labels yet; without them, a list. The
  // gestures are a list either way.
  const bool laid = art || !c.loading.running();
  if (c.gestureView && laid) layoutGestureList(y0);
  if (!c.gestureView && laid) {
    // Where each label wants to be: beside its control's point.
    auto point = [&](int hand, const std::string& input, double& x, double& y) {
      if (!c.picture) return false;
      const auto& points = art->hands[hand].points;
      const auto it = points.find(controlOf(input));
      if (it == points.end()) return false;
      x = c.drawX[hand] + it->second[0];
      y = c.drawY + it->second[1];
      return true;
    };
    struct Placed {
      Item item;
      std::vector<std::pair<int, std::array<double, 2>>> points;  // by hand
    };
    std::vector<Placed> columns[3];
    std::vector<double> tops[3];
    for (int h = 0; h < 2 && c.picture; ++h) c.drawX[h] = kDrawX[h] + (kDrawW - art->hands[h].width) / 2;
    auto inputOf = [&](const Item& item, int hand) -> std::string {
      if (item.what == Item::Seek || item.what == Item::Closer) return "/input/thumbstick";
      const LayoutControl* lc = layoutControl(type, hand, item.control);
      return lc ? lc->input : std::string();
    };
    auto plan = [&](double drop) {
      c.drawY = y0 + drop;
      for (int col = 0; col < 3; ++col) {
        columns[col].clear();
        std::vector<double> wants;
        for (const Item& item : columnItems(type, col)) {
          Placed placed{item, {}};
          for (int h = 0; h < 2; ++h) {
            if (item.hand >= 0 && item.hand != h) continue;
            const std::string input = inputOf(item, h);
            if (input.empty()) continue;
            double x = 0, y = 0;
            if (point(h, input, x, y)) placed.points.push_back({h, {x, y}});
          }
          if (item.what == Item::Button && !layoutControl(type, item.hand, item.control)) continue;
          if ((item.what == Item::Seek || item.what == Item::Closer) && !controllerLayout(type).stick) continue;
          if (item.what == Item::Grip && !layoutControl(type, 0, Control::Grip)) continue;
          double want = wants.empty() ? y0 : wants.back() + kPitch;
          if (!placed.points.empty()) {
            want = 1e9;
            for (const auto& p : placed.points) want = std::min(want, p.second[1] - kRow / 2);
          }
          if (!c.picture) want = 0;
          wants.push_back(want);
          columns[col].push_back(placed);
        }
        if (c.picture) {
          tops[col] = placeRows(wants, y0, bottom);
        } else {
          tops[col].clear();
          for (size_t i = 0; i < wants.size(); ++i) tops[col].push_back(y0 + kListHeadingH + i * kPitch);
        }
      }
    };
    // The drawings move up or down until the highest label starts the group's rows.
    double drop = 0;
    plan(drop);
    for (int n = 0; n < 2 && c.picture; ++n) {
      double highest = 1e9;
      for (const auto& column : tops)
        for (double top : column) highest = std::min(highest, top);
      if (highest > 1e8) break;
      drop -= highest - y0;
      plan(drop);
    }
    if (!c.picture) {
      // As a list: the left controller's, what both do, the right one's.
      std::vector<Placed> lists[3];
      for (int col = 0; col < 3; ++col)
        for (const Placed& p : columns[col]) lists[p.item.hand < 0 ? 1 : 2 * p.item.hand].push_back(p);
      const char* headings[3] = {t.controls.left, t.controls.either, t.controls.right};
      for (int col = 0; col < 3; ++col) {
        columns[col] = lists[col];
        tops[col].clear();
        for (size_t i = 0; i < lists[col].size(); ++i) tops[col].push_back(y0 + kListHeadingH + i * kPitch);
        if (!lists[col].empty()) {
          c.headings.push_back(headings[col]);
          c.headingX.push_back(PageLayout::columnX(col) + kit::kGroupPadX + kit::kSpace);
        }
      }
    }

    for (int col = 0; col < 3; ++col) {
      const double x = c.picture ? kLabelX[col] : PageLayout::columnX(col) + kit::kGroupPadX;
      const double w = c.picture ? kLabelW[col] : PageLayout::kColumnW - 2 * kit::kGroupPadX;
      for (size_t i = 0; i < columns[col].size(); ++i) {
        const Placed& p = columns[col][i];
        const Item& item = p.item;
        const double y = tops[col][i], cy = y + kRow / 2;
        // Which edge faces the drawing: outside the left one its right edge;
        // between them, by hand.
        const int side = col == 0 ? 1 : col == 2 ? -1 : item.hand > 0 ? 1 : -1;
        const LayoutControl* lc =
            item.what == Item::Button ? layoutControl(type, item.hand, item.control) : nullptr;
        // With grip held, a stick's click and a button that chooses keep a
        // job of their own: shown as fixed.
        if (lc && !lc->fixed && (!c.gripView || v.map.takesGrip(type, item.hand, item.control))) {
          const ControlSlot slot{type, item.hand, item.control, c.gripView};
          // With grip held and no grip job: its own, dim.
          const int shown = shownJob(v.map, slot);
          const Job job = shown == ControlsPage::kInherit ? v.map.job(type, item.hand, item.control)
                                                         : static_cast<Job>(shown);
          Part part;
          part.kind = Part::Label;
          part.x = x, part.y = y, part.w = w, part.h = kRow;
          part.glyphs = {glyphOf(*lc)};
          part.name = buttonName(*lc, t);
          part.text = jobLabel(job, t, v.skipSeconds, v.bigSkipSeconds);
          part.inherited = shown == ControlsPage::kInherit;
          part.faint = job == Job::None && !c.gripView;
          part.hands = 1 << item.hand;
          part.control = item.control;
          // The gestures it is in, alone (with grip held: with the grip): a
          // mark, and its tooltip names them.
          std::string twists;
          for (const Gesture& g : v.gestures.list(type))
            if ((g.hands >> item.hand & 1) && g.has(item.control) && g.has(Control::Grip) == c.gripView)
              twists += (twists.empty() ? "" : "; ") +
                        capitalized(printed(t.gesturePage.labelTip, gestureName(type, g, t),
                                            gestureValueLabel(g.value, t)));
          part.twist = !twists.empty();
          const bool open = dialog_ == Dialog::ChooseJob && c.open == slot;
          Button b;
          b.panel = kBrowser;
          b.x = x, b.y = y, b.w = w, b.h = kRow;
          b.radius = kit::kCorner;
          b.latched = open;
          b.tooltip = buttonName(*lc, t);
          if (c.gripView) b.tooltip = printed(part.inherited ? cp.gripInheritsTip : cp.gripPlus, b.tooltip);
          if (part.twist) b.tooltip = twists;
          b.controlPart = static_cast<int>(c.parts.size());
          b.action = [this, slot, job] {
            ControlsPage& page = *controls_;
            page.open = slot;
            page.section = jobInfo(job).section == JobSection::None ? JobSection::General
                                                                    : jobInfo(job).section;
            page.noticeShown = false;
            openDialog(Dialog::ChooseJob, "");
            return MenuCommand{};
          };
          buttons_.push_back(b);
          c.parts.push_back(part);
          if (!p.points.empty())
            c.leads.push_back({side < 0 ? x - 6 : x + w + 6, cy, p.points[0].second[0], p.points[0].second[1],
                               false, item.hand, item.control});
          continue;
        }
        Part part;
        part.kind = Part::Fixed;
        part.x = x + kit::kSpace, part.y = y, part.w = w - 2 * kit::kSpace, part.h = kRow;
        std::string tooltip;
        switch (item.what) {
          case Item::Trigger:
            part.glyphs = {"button_trigger"};
            if (!c.gripView) {
              part.text = t.jobs.choose, part.how = cp.chooseHow, tooltip = cp.chooseTip;
            } else {  // with grip held, each hand's gesture's; one for both where they agree
              const Gesture* g[2] = {v.gestures.find(type, 0, Control::Grip, Control::Trigger),
                                     v.gestures.find(type, 1, Control::Grip, Control::Trigger)};
              std::string value[2];
              for (int h = 0; h < 2; ++h) value[h] = g[h] ? gestureValueLabel(g[h]->value, t) : t.jobs.nothing;
              if (!g[0] && !g[1]) {
                part.text = t.jobs.nothing, tooltip = t.gesturePage.gripTriggerNothingTip;
              } else if (value[0] == value[1]) {
                part.text = value[0], part.how = cp.twist;
                tooltip = printed(t.gesturePage.gripTriggerTip, part.text);
              } else {
                part.text = printed("%s %s · %s %s", cp.leftTag, value[0], cp.rightTag, value[1]);
                tooltip = printed(t.gesturePage.gripTriggerHandsTip, value[0], value[1]);
                // Too long: a line for each hand, its value without its group
                // where that is still too long.
                for (int h = 0; h < 2; ++h) {
                  const char* tag = h ? cp.rightTag : cp.leftTag;
                  part.lines.push_back(printed("%s %s", tag, value[h]));
                  part.shorter.push_back(
                      printed("%s %s", tag, g[h] ? gestureValueInfo(g[h]->value).label(t) : t.jobs.nothing));
                }
              }
            }
            part.hands = 3, part.control = Control::Trigger;
            break;
          case Item::Grip:
            part.glyphs = {"button_grip"};
            part.text = cp.move, part.how = cp.moveHow, tooltip = cp.moveTip;
            part.hands = 3, part.control = Control::Grip;
            break;
          case Item::Seek:
            part.glyphs = {"button_thumbstick_move"};
            part.text = cp.seek, part.how = cp.seekHow, tooltip = cp.seekTip;
            break;
          case Item::Closer:
            part.glyphs = {"button_thumbstick_scroll"};
            if (c.gripView) part.text = cp.height, part.how = cp.closerHow, tooltip = cp.heightTip;
            else part.text = cp.closer, part.how = cp.closerHow, tooltip = cp.closerTip;
            break;
          case Item::Button:
            if (!lc) continue;
            part.hands = 1 << item.hand, part.control = item.control;
            part.name = buttonName(*lc, t);
            if (lc->fixed) {  // the simple controller's
              part.glyphs = {glyphOf(*lc)};
              part.text = jobLabel(lc->job, t, v.skipSeconds, v.bigSkipSeconds);
            } else if (item.control == Control::StickClick) {  // with grip held, on the stars
              part.glyphs = {glyphOf(*lc)};
              part.text = cp.clearRating, tooltip = cp.clearRatingTip;
            } else {  // it chooses, with grip held too
              part.glyphs = {glyphOf(*lc)};
              part.text = t.jobs.choose, tooltip = cp.chooserVolumeTip;
            }
            break;
        }
        const double contentW = howWidth(part);
        part.align = !c.picture ? 'l' : item.hand < 0 ? 'c' : side > 0 ? 'r' : 'l';
        tip(x, y, w, kRow, tooltip);
        c.parts.push_back(part);
        if (item.hand >= 0) {
          if (!p.points.empty())
            c.leads.push_back({side < 0 ? x + 6 : x + w - 6, cy, p.points[0].second[0],
                               p.points[0].second[1], true, item.hand, part.control});
        } else {
          const double cx = x + w / 2;
          for (const auto& [hand, at] : p.points)
            c.leads.push_back({hand ? cx + contentW / 2 + 10 : cx - contentW / 2 - 10, cy, at[0], at[1], true,
                               hand, part.control});
        }
      }
    }
  }

#ifndef NDEBUG
  // The labels and rows stay in the group: its height is fixed, for the page never scrolls.
  for (const Part& part : c.parts)
    if (part.y + part.h > bottom + 0.5)
      std::fprintf(stderr, "[fit] Controls: '%s' ends %.0f px below its group%s\n", part.text.c_str(),
                   part.y + part.h - bottom, fitCase_ ? fitCase_ : "");
#endif
  // The notice after a change that replaced a job or a gesture's value, or
  // took a gesture away: as low as it goes across the middle without
  // covering a label or a row (between the drawings' handles; under a list,
  // else in its middle column).
  const ControlsPage::Notice& n = c.notice;
  const LayoutControl* changed = c.noticeShown && !n.gesture && n.slot.type == type
                                     ? layoutControl(type, n.slot.hand, n.slot.control)
                                     : nullptr;
  if (laid && (changed || (c.noticeShown && n.gesture))) {
    std::string line;
    if (n.gesture) {
      const std::string who = gestureName(type, n.changed, t);
      const std::string was = gestureValueLabel(n.wasValue, t);
      line = capitalized(n.nowValue == GestureValue::Count
                             ? printed(t.gesturePage.noLonger, who, was)
                             : printed(t.gesturePage.nowDoes, who, gestureValueLabel(n.nowValue, t), was));
      c.noticeGlyphs.clear();
      for (const Control control : gestureControls(n.changed))
        c.noticeGlyphs.push_back(controlGlyphName(type, gestureHand(n.changed), control));
    } else {
      const std::string who = n.slot.grip ? printed(cp.withGrip, buttonNoun(type, *changed, t))
                                          : capitalized(buttonNoun(type, *changed, t));
      const std::string was = jobLabel(n.was, t, v.skipSeconds, v.bigSkipSeconds);
      const std::string now = jobLabel(n.now, t, v.skipSeconds, v.bigSkipSeconds);
      line = n.now == Job::None ? printed(cp.noLonger, who, was) : printed(cp.nowDoes, who, now, was);
      c.noticeGlyphs = {glyphOf(*changed)};
      if (n.slot.grip) c.noticeGlyphs.insert(c.noticeGlyphs.begin(), "button_grip");
    }
    c.noticeText = line;
    const double undoW = textWidth(t.stash.undo, kUndoSize, false) + 2 * 20;
    const double fixed = 28 + noticeGlyphsW(c.noticeGlyphs.size(), n.gesture) + kit::kSpace + kit::kSpace +
                         kit::kSpaceTight + undoW + kit::kSpace;
    // Its size at most maxW wide: one line, two as even as they go, or more.
    struct Shape {
      double w, h, wrapW, textH;
      int lines;
    };
    auto shape = [&](double maxW) {
      Shape s{};
      const double room = maxW - fixed;
      s.lines = wrappedLines(line, room, kPillSize, kNoticeLines);
      double textW = 0;
      if (s.lines <= 1) {
        textW = s.wrapW = textWidth(line, kPillSize, false);
      } else if (s.lines == 2) {
        twoLines(line, room, kPillSize, s.wrapW, s.textH);
        textW = s.wrapW;
      } else {
        int pw = 0, ph = 0;
        pango_layout_get_pixel_size(textLayout(canvases_[kMain].cr(), line, kPillSize, false, room, s.lines, false),
                                    &pw, &ph);
        s.wrapW = room, textW = pw, s.textH = ph;
      }
      s.w = std::min(maxW, std::ceil(fixed + textW));
      s.h = std::max<double>(kPillH, std::ceil(s.textH + 2 * kNoticePadY));
      return s;
    };
    // Covering no label nor row (nor the list's headings).
    const double lowest = y0 + (c.picture ? 0 : kListHeadingH);
    auto clear = [&](double x, double y, double w, double h) {
      for (const Part& p : c.parts)
        if (p.kind != Part::Job && x < p.x + p.w + kit::kSpaceTight && p.x - kit::kSpaceTight < x + w &&
            y < p.y + p.h + kit::kSpaceTight && p.y - kit::kSpaceTight < y + h)
          return false;
      for (const Button& b : buttons_)  // Add gesture
        if (b.panel == kBrowser && b.icon == Icon::Plus && x < b.x + b.w && b.x < x + w && y < b.y + b.h &&
            b.y < y + h)
          return false;
      return true;
    };
    const double middle = c.picture ? kMidW : PageLayout::kColumnW - 2 * kit::kGroupPadX;
    Shape s{};
    double x = 0, y = -1;
    for (const double maxW : {kNoticeW, std::max(middle, fixed + 120)}) {
      s = shape(maxW);
      x = std::round(kPageCenterX - s.w / 2);
      for (double at = bottom - s.h; at >= lowest && y < 0; at -= 4)
        if (clear(x, at, s.w, s.h)) y = at;
      if (y >= 0) break;
    }
    c.noticeCovers = y < 0;
    if (c.noticeCovers) {
      // Nowhere clear (a full column of gestures): at the bottom, as wide
      // as it goes, and the labels and rows it covers go while it shows, so
      // none is drawn over it nor pointed at through it.
      s = shape(kNoticeW);
      x = std::round(kPageCenterX - s.w / 2);
      y = bottom - s.h;
      buttons_.erase(std::remove_if(buttons_.begin() + static_cast<std::ptrdiff_t>(firstButton), buttons_.end(),
                                    [&](const Button& b) {
                                      return b.panel == kBrowser && x < b.x + b.w && b.x < x + s.w &&
                                             y < b.y + b.h && b.y < y + s.h;
                                    }),
                     buttons_.end());
    }
    const double w = s.w;
    c.noticeLines = s.lines, c.noticeWrapW = s.wrapW, c.noticeTextH = s.textH;
    c.noticeBox[0] = x, c.noticeBox[1] = y, c.noticeBox[2] = w, c.noticeBox[3] = s.h;
    Button undo = textButton(t.stash.undo, Icon::None, [this, n] {
      controls_->noticeShown = false;
      MenuCommand cmd;
      if (n.gesture) {
        cmd.type = MenuCommand::RestoreGestures;
        cmd.option = static_cast<int>(controls_->type);
        cmd.path = n.gestures;
        return cmd;
      }
      cmd.type = MenuCommand::RestoreButton;
      cmd.option = ControlSlot{n.slot.type, n.slot.hand, n.slot.control, false}.pack();
      cmd.value = n.before;
      return cmd;
    });
    undo.panel = kBrowser;
    undo.fontSize = kUndoSize;
    undo.w = undoW, undo.h = kUndoH;
    undo.x = x + w - kit::kSpace - undoW;
    undo.y = y + (s.h - kUndoH) / 2;
    undo.radius = kit::kCorner;
    undo.tooltip = cp.undoTip;
    buttons_.push_back(undo);
  } else {
    c.noticeBox[2] = 0;
    c.noticeCovers = false;
  }

  // Under the drawings: the buttons' jobs alone, with grip held, or the gestures.
  if (combos || gestures) {
    constexpr double in = kit::kSpacePiece;
    constexpr int size = 23;
    struct View {
      const char* name;
      bool grip, gesture;
    };
    std::vector<View> views{{cp.buttons, false, false}};
    if (combos) views.push_back({cp.withGripHeld, true, false});
    if (gestures) views.push_back({cp.gestures, false, true});
    std::vector<double> widths;
    double total = in;
    for (const View& view : views) total += widths.emplace_back(textWidth(view.name, size, false) + 2 * 22) + in;
    Button well;
    well.panel = kBrowser;
    well.well = true;
    well.x = std::round(kPageCenterX - total / 2), well.y = areaBottom - kRow;
    well.w = total, well.h = kRow;
    well.radius = kit::kCorner;
    buttons_.push_back(well);
    double x = well.x + in;
    for (size_t i = 0; i < views.size(); ++i) {
      const View view = views[i];
      Button b = textButton(view.name, Icon::None,
                            [this, view] {
                              controls_->gripView = view.grip;
                              controls_->gestureView = view.gesture;
                              controls_->noticeShown = false;
                              dirty_ = true;
                              return MenuCommand{};
                            },
                            c.gripView == view.grip && c.gestureView == view.gesture);
      b.panel = kBrowser;
      b.piece = true;
      b.radius = kit::kPieceCorner;
      b.fontSize = size;
      b.x = x, b.y = well.y + in, b.w = widths[i], b.h = kRow - 2 * in;
      buttons_.push_back(b);
      x += widths[i] + in;
    }
  }

  // How far Skip and Big skip go: side by side across the page.
  {
    GroupLayout g = page.groupAt(0, kPageL, kUnderY, kPageR - kPageL, t.groupPlayback);
    const GroupLayout::Box row = g.room(kRow);
    const double half = (row.w - kit::kSpace) / 2;
    double labelW = 0;
    for (const char* label : {cp.skip, cp.bigSkip}) labelW = std::max(labelW, textWidth(label, kit::kRowSize, false));
    labelW = std::clamp(std::ceil(labelW) + 2 * kit::kSpace + 2, 150.0, 300.0);
    auto format = [&t](int seconds, bool tick) {
      return tick ? std::to_string(seconds) : skipLength(seconds, t);
    };
    static const Settings defaults;
    struct Skip {
      const char* label;
      MenuCommand::Type type;
      int now;
      SkipRange range;
      int mark;
      const char* tip;
    };
    const Skip skips[2] = {
        {cp.skip, MenuCommand::SetSkipSeconds, v.skipSeconds, kSkipRange, defaults.skipSeconds, cp.skipTip},
        {cp.bigSkip, MenuCommand::SetBigSkipSeconds, v.bigSkipSeconds, kBigSkipRange, defaults.bigSkipSeconds,
         cp.bigSkipTip}};
    for (int i = 0; i < 2; ++i) {
      const Skip& s = skips[i];
      const size_t first = buttons_.size();
      const double x = row.x + i * (half + kit::kSpace);
      addValue(x, row.y, half, labelW, kRow, s.label, s.type, s.now, s.range.least, s.range.most,
               skipLength(s.now, t), s.range.step, command(s.type, std::max(s.range.least, s.now - s.range.step)),
               command(s.type, std::min(s.range.most, s.now + s.range.step)));
      Row(*this, first, x + half).format(format).mark(s.mark).tip(s.tip);
    }
  }
}

// The Gestures view: the page's list layout, under the Left, Either and
// Right controller headings. Either's column starts with the gestures that
// can't be changed (flush, dim), then the user's for either controller, and
// ends with Add gesture; the side columns have the user's for that hand.
// Each user's row is raised, an × at its end removes it.
void Menu::layoutGestureList(double y0) {
  ControlsPage& c = *controls_;
  const Strings& t = text();
  const GesturePageStrings& gp = t.gesturePage;
  const ControlsView& v = view_.controls;
  const ControllerType type = v.type;
  const std::vector<Gesture>& list = v.gestures.list(type);
  const char* headings[3] = {t.controls.left, t.controls.either, t.controls.right};
  const std::vector<FixedGesture> fixed = fixedGestures(type, t);
  const size_t first = c.parts.size();
  for (int col = 0; col < 3; ++col) {
    const double x = PageLayout::columnX(col) + kit::kGroupPadX, w = PageLayout::kColumnW - 2 * kit::kGroupPadX;
    c.headings.push_back(headings[col]);
    c.headingX.push_back(x + kit::kSpace);
    double y = y0 + kListHeadingH;
    if (col == 1)
      for (const FixedGesture& f : fixed) {
        Part part;
        part.kind = Part::Gesture;
        part.fixed = true;
        part.x = x, part.y = y, part.w = w, part.h = kRow;
        part.glyphs = f.glyphs;
        part.names.assign(f.glyphs.size(), "");
        if (f.turn) part.word = f.move;  // Horizon: the trigger's drag, then + twist
        else part.move = f.move;
        part.plus = f.plus || f.turn;
        part.turn = f.turn;
        part.moveNamed = f.clicks;  // the stick click's name says the click
        part.text = f.name, part.how = f.where;
        c.parts.push_back(part);
        Button b;
        b.panel = kBrowser;
        b.x = x, b.y = y, b.w = w, b.h = kRow;
        b.staticText = b.disabled = true;
        b.action = [] { return MenuCommand{}; };
        b.tooltip = printed(gp.fixedTip, f.tip);
        buttons_.push_back(b);
        y += kPitch;
      }
    for (size_t i = 0; i < list.size(); ++i) {
      const Gesture& g = list[i];
      if (gestureColumn(g) != col) continue;
      const int hand = gestureHand(g);
      Part part;
      part.kind = Part::Gesture;
      part.x = x, part.y = y, part.w = w, part.h = kRow;
      for (const Control control : gestureControls(g)) {
        part.glyphs.push_back(controlGlyphName(type, hand, control));
        part.names.push_back(controlName(type, hand, control, !part.names.empty(), t));
      }
      part.plus = part.turn = true;
      part.text = gestureValueLabel(g.value, t), part.how = gestureWhere(g.value, t);
      part.hands = g.hands;
      // A button of it that chooses: it does nothing (faint), and says why.
      Control chooser = Control::Count;
      int chooserHand = hand;
      for (int h = 0; h < 2 && chooser == Control::Count; ++h)
        if (g.hands >> h & 1) chooser = choosingButton(v.map, type, chooserHand = h, g);
      part.faint = chooser != Control::Count;
      // A tap of its one button still does that button's job.
      const Job tap = g.controls() == 1 && g.first != Control::Grip ? v.map.job(type, hand, g.first) : Job::None;
      const std::string name = gestureName(type, g, t, true);
      Button b;
      b.panel = kBrowser;
      b.x = x, b.y = y, b.w = w, b.h = kRow;
      b.radius = kit::kCorner;
      const std::string tapJob = jobLabel(tap, t, v.skipSeconds, v.bigSkipSeconds);
      b.tooltip = capitalized(tap == Job::None || part.faint ? printed(gp.rowTip, name, part.text)
                                                             : printed(gp.rowTapTip, name, part.text, tapJob));
      if (part.faint) b.tooltip += " " + printed(gp.choosesTip, controlName(type, chooserHand, chooser, true, t));
      b.controlPart = static_cast<int>(c.parts.size());
      b.action = [this, i] {
        openGestureEditor(static_cast<int>(i));
        return MenuCommand{};
      };
      buttons_.push_back(b);
      c.parts.push_back(part);
      Button remove = iconButton(Icon::Close, [this, i, g, type, before = v.gestures.listLine(type)] {
        ControlsPage& page = *controls_;
        page.noticeShown = true;
        page.notice = {};
        page.notice.gesture = true;
        page.notice.changed = g;
        page.notice.wasValue = g.value;
        page.notice.gestures = before;
        MenuCommand cmd;
        cmd.type = MenuCommand::RemoveGesture;
        cmd.option = static_cast<int>(type);
        cmd.value = static_cast<double>(i);
        return cmd;
      });
      remove.panel = kBrowser;
      remove.bare = remove.dimInk = true;
      remove.w = remove.h = kRemoveW;
      remove.x = x + w - kRemoveInset - remove.w, remove.y = y + (kRow - remove.h) / 2;
      remove.iconSize = 24;
      remove.radius = 10;
      remove.tooltip = gp.removeTip;
      buttons_.push_back(remove);
      y += kPitch;
    }
    if (col == 1) {
      const bool full = list.size() >= kMaxGestures;
      Part part;
      part.kind = Part::Add;
      part.x = x, part.y = y, part.w = w, part.h = kRow;
      part.text = gp.add;
      Button b;
      b.panel = kBrowser;
      b.x = x, b.y = y, b.w = w, b.h = kRow;
      b.radius = kit::kCorner;
      b.disabled = full;
      b.tooltip = full ? printed(gp.fullTip, kMaxGestures) : gp.addTip;
      b.controlPart = static_cast<int>(c.parts.size());
      b.action = [this] {
        openGestureEditor(-1);
        return MenuCommand{};
      };
      buttons_.push_back(b);
      c.parts.push_back(part);
    }
  }
#ifndef NDEBUG
  // Every row's name whole with SteamVR's glyphs, and the fixed ones' also
  // without them (the controls' names instead, wider); where it works may
  // give way. A user's own without glyphs may be shortened: two long names
  // and a long value are wider than a column.
  for (const int glyphs : {0, 1}) {
    placeGestureRows(first, glyphs);
    for (size_t i = first; i < c.parts.size(); ++i) {
      const Part& p = c.parts[i];
      if (p.kind != Part::Gesture || (!glyphs && !p.fixed)) continue;
      const double over = p.valueX + gestureWidths(static_cast<int>(i), glyphs).name - gestureEnd(p);
      if (over > 0.5)
        std::fprintf(stderr, "[fit] Controls: gesture '%s' is %.0f px too wide %s glyphs%s\n", p.text.c_str(), over,
                     glyphs ? "with" : "without", fitCase_ ? fitCase_ : "");
    }
  }
#endif
  placeGestureRows(first, -1);
}

Menu::GestureWidths Menu::gestureWidths(int index, int glyphs) const {
  const ControlsPage& c = *controls_;
  const Part& p = c.parts[static_cast<size_t>(index)];
  const ControlsPageStrings& cp = text().controlsPage;
  const ControllerArt* art = c.art.get();
  GestureWidths w;
  bool shown = false;  // the last one's glyph
  for (size_t i = 0; i < p.glyphs.size(); ++i) {
    if (i) w.controls += 2 * kGlyphGap + textWidth("+", kHowSize, false);
    shown = glyphs < 0 ? hasGlyph(art, p.glyphs[i]) : glyphs > 0;
    w.controls += shown ? kRowGlyph : textWidth(gestureControlName(p, i, cp), kHowSize, true);
  }
  if (!p.word.empty()) w.controls += kGlyphGap + textWidth(p.word, kHowSize, false);
  const std::string move = shownMove(p, shown);
  if (p.plus) w.move += textWidth("+", kHowSize, false) + kGlyphGap;
  if (!move.empty()) w.move += textWidth(move, kMoveSize, false);
  if (p.turn) w.move += (move.empty() ? 0 : kGlyphGap) + kTurn;
  w.name = textWidth(p.text, kLabelSize, false);
  w.where = p.how.empty() ? 0 : textWidth(p.how, kHowSize, false);
  return w;
}

// Each column's rows line up, the fixed ones apart from the user's: their
// controls end at one place, the fixed ones' arrows at another, the user's
// at theirs. The controls take at least kGestureGlyphsW where every row's
// name and where still fit. Where a name doesn't fit otherwise, the arrows
// go each after its own movement, and then the rows don't line up at all,
// if that fits every name.
void Menu::placeGestureRows(size_t first, int glyphs) {
  std::vector<Part>& parts = controls_->parts;
  const double arrowW = textWidth("→", kMoveSize, false);
  std::vector<std::pair<double, bool>> groups;  // a column's, fixed or not
  for (size_t i = first; i < parts.size(); ++i) {
    const std::pair<double, bool> group{parts[i].x, parts[i].fixed};
    if (parts[i].kind == Part::Gesture && std::find(groups.begin(), groups.end(), group) == groups.end())
      groups.push_back(group);
  }
  for (const auto& [column, fixed] : groups) {
    std::vector<std::pair<size_t, GestureWidths>> rows;
    double controlsW = 0, moveW = 0;
    for (size_t i = first; i < parts.size(); ++i)
      if (parts[i].kind == Part::Gesture && parts[i].x == column && parts[i].fixed == fixed) {
        const GestureWidths w = gestureWidths(static_cast<int>(i), glyphs);
        controlsW = std::max(controlsW, w.controls);
        moveW = std::max(moveW, w.move);
        rows.emplace_back(i, w);
      }
    // Every row whole (where too), or every row's name; `controls` < 0:
    // each row's own.
    auto place = [&](double controls, bool lineUp, bool where) {
      bool fits = true;
      for (const auto& [i, w] : rows) {
        Part& p = parts[i];
        const double gap = gestureGap(p);
        p.controlsEnd = p.x + kit::kSpace + (controls < 0 ? w.controls : controls);
        p.arrowX = p.controlsEnd + gap - 4 + (lineUp ? moveW : w.move) + gap - 2;
        p.valueX = p.arrowX + arrowW - 2 + gap;
        fits = fits && p.valueX + w.name + (where && w.where > 0 ? gap + w.where : 0) <= gestureEnd(p);
      }
      return fits;
    };
    if (!place(std::max(controlsW, kGestureGlyphsW), true, true) && !place(controlsW, true, true) &&
        !place(controlsW, true, false) && !place(controlsW, false, false) && !place(-1, false, false))
      place(controlsW, true, false);  // some are shortened anyway: lined up
  }
}

// A gesture's row: its controls' glyphs right-aligned to its column's, a
// word after them, the movement, → what it does and where (faint). Without
// SteamVR's glyphs, the controls' names. Where it works gives way to what it
// does, and that is shortened at its end only if it still doesn't fit.
void Menu::gestureRow(cairo_t* cr, int index, const double ink[4]) const {
  const ControlsPage& c = *controls_;
  const Part& p = c.parts[static_cast<size_t>(index)];
  const Palette pal = palette();
  const ControlsPageStrings& cp = text().controlsPage;
  const ControllerArt* art = c.art.get();
  const double gap = gestureGap(p), cy = p.y + p.h / 2;
  const double alpha = ink[3] * (p.faint ? 0.5 : 1);  // one that does nothing (its button chooses)
  const GestureWidths widths = gestureWidths(index, -1);

  const Rgba& faint = pal.faint;
  const Rgba& dim = pal.textDim;
  double x = p.controlsEnd - widths.controls;
  for (size_t i = 0; i < p.glyphs.size(); ++i) {
    if (i) {
      x += kGlyphGap;
      const double plusW = textWidth("+", kHowSize, false);
      drawText(cr, "+", x, p.y, plusW + 2, p.h, kHowSize, false, false, faint.r, faint.g, faint.b, alpha);
      x += plusW + kGlyphGap;
    }
    if (paintGlyph(cr, art, p.glyphs[i], x + kRowGlyph / 2, cy, kRowGlyph, (p.fixed ? 0.75 : 1) * alpha)) {
      x += kRowGlyph;
    } else {
      const std::string n = gestureControlName(p, i, cp);
      const double w = textWidth(n, kHowSize, true);
      drawText(cr, n, x, p.y, w + 2, p.h, kHowSize, true, false, dim.r, dim.g, dim.b, alpha);
      x += w;
    }
  }
  if (!p.word.empty()) {
    x += kGlyphGap;
    drawText(cr, p.word, x, p.y, textWidth(p.word, kHowSize, false) + 2, p.h, kHowSize, false, false, faint.r,
             faint.g, faint.b, alpha);
  }
  x = p.controlsEnd + gap - 4;
  if (p.plus) {
    const double plusW = textWidth("+", kHowSize, false);
    drawText(cr, "+", x, p.y, plusW + 2, p.h, kHowSize, false, false, faint.r, faint.g, faint.b, alpha);
    x += plusW + kGlyphGap;
  }
  const std::string move = shownMove(p, !p.glyphs.empty() && hasGlyph(art, p.glyphs.back()));
  if (!move.empty()) {
    const double w = textWidth(move, kMoveSize, false);
    drawText(cr, move, x, p.y, w + 2, p.h, kMoveSize, false, false, dim.r, dim.g, dim.b, alpha);
    x += w + kGlyphGap;
  }
  if (p.turn) drawTurn(cr, x + kTurn / 2, cy, kTurn, dim, alpha);
  drawText(cr, "→", p.arrowX, p.y, textWidth("→", kMoveSize, false) + 2, p.h, kMoveSize, false, false, faint.r,
           faint.g, faint.b, alpha);
  x = p.valueX;
  const double end = gestureEnd(p);
  const Rgba k = p.fixed ? dim : Rgba{ink[0], ink[1], ink[2], 1};
  drawTextEnd(cr, p.text, x, p.y, std::min(widths.name + 2, end - x), p.h, kLabelSize, false, k.r, k.g, k.b, alpha);
  x += widths.name + gap;
  if (widths.where > 0 && x + widths.where <= end)
    drawText(cr, p.how, x, p.y, widths.where + 2, p.h, kHowSize, false, false, faint.r, faint.g, faint.b, alpha);
}

// The drawings, the lines to their controls, the flush labels, the
// gestures and the notice's pill; the buttons go over them.
void Menu::drawControls(cairo_t* cr) {
  if (!controls_) return;
  const ControlsPage& c = *controls_;
  const Palette pal = palette();
  const ControllerArt* art = c.art.get();
  if (c.picture && art) {
    const double h = c.artBottom - c.artTop;
    const bool cut = c.drawY < c.artTop;
    for (int hand = 0; hand < 2; ++hand) {
      const ControllerArt::Drawing& d = art->hands[hand];
      cairo_save(cr);
      cairo_rectangle(cr, kDrawX[hand] - kDrawW, c.artTop, 3 * kDrawW, h);
      cairo_clip(cr);
      cairo_pattern_t* image = cairo_pattern_create_for_surface(d.image.get());
      cairo_matrix_t m;
      cairo_matrix_init_scale(&m, menu_geometry::kRasterScale, menu_geometry::kRasterScale);
      cairo_matrix_translate(&m, -c.drawX[hand], -c.drawY);
      cairo_pattern_set_matrix(image, &m);
      cairo_set_source(cr, image);
      cairo_pattern_t* fade = cairo_pattern_create_linear(0, c.artTop, 0, c.artBottom);
      cairo_pattern_add_color_stop_rgba(fade, 0, 0, 0, 0, cut ? 0 : 1);
      if (cut) cairo_pattern_add_color_stop_rgba(fade, kFadeTop / h, 0, 0, 0, 1);
      cairo_pattern_add_color_stop_rgba(fade, 1 - kFadeBottom / h, 0, 0, 0, 1);
      cairo_pattern_add_color_stop_rgba(fade, 1, 0, 0, 0, 0);
      cairo_mask(cr, fade);
      cairo_pattern_destroy(fade);
      cairo_pattern_destroy(image);
      cairo_restore(cr);
    }
    // The lines: a button's in the accent, a fixed control's dashed.
    cairo_set_line_width(cr, 2.5);
    for (const Lead& l : c.leads)
      if (!l.fixed) {
        cairo_move_to(cr, l.ax, l.ay);
        cairo_line_to(cr, l.x, l.y);
      }
    setColor(cr, pal.accent, 0.75);
    cairo_stroke(cr);
    const double dash[] = {5, 5};
    cairo_set_dash(cr, dash, 2, 0);
    for (const Lead& l : c.leads)
      if (l.fixed) {
        cairo_move_to(cr, l.ax, l.ay);
        cairo_line_to(cr, l.x, l.y);
      }
    cairo_set_source_rgba(cr, 1, 1, 1, 0.34);
    cairo_stroke(cr);
    cairo_set_dash(cr, nullptr, 0, 0);
    // A ring round the control being changed, and round those held.
    for (const Lead& l : c.leads) {
      const bool open = dialog_ == Dialog::ChooseJob && !l.fixed && c.open.hand == l.hand &&
                        c.open.control == l.control;
      const bool held = l.control != Control::Count && (c.held[l.hand] & controlBit(l.control));
      if (open || held) drawRing(cr, l.x, l.y, pal.accent);
    }
    for (const Lead& l : c.leads) {
      cairo_arc(cr, l.x, l.y, 10, 0, 2 * M_PI);
      cairo_set_source_rgba(cr, 6 / 255.0, 8 / 255.0, 12 / 255.0, 0.9);
      cairo_fill(cr);
      cairo_arc(cr, l.x, l.y, l.fixed ? 6 : 7, 0, 2 * M_PI);
      if (l.fixed) cairo_set_source_rgba(cr, 1, 1, 1, 0.5);
      else setColor(cr, pal.accent);
      cairo_fill(cr);
    }
  }
  for (size_t i = 0; i < c.headings.size(); ++i)
    drawText(cr, c.headings[i], c.headingX[i], c.artTop, 400, kListHeadingH, kit::kGroupTitleSize, true, false,
             pal.textDim.r, pal.textDim.g, pal.textDim.b);

  for (size_t i = 0; i < c.parts.size(); ++i) {
    const Part& p = c.parts[i];
    const double cy = p.y + p.h / 2;
    const double* box = c.noticeBox;
    if (c.noticeCovers && box[0] < p.x + p.w && p.x < box[0] + box[2] && box[1] < p.y + p.h && p.y < box[1] + box[3])
      continue;
    if (p.kind == Part::Fixed) {
      // Its glyph, what it does (dim) and how (faint), to one side or centred.
      // Without SteamVR's glyph, the control's name in its place. Too long,
      // it goes without how, then what it does is shortened at its end.
      const bool glyph = art && art->glyphs.count(p.glyphs[0]);
      const std::string name = p.name.empty() ? glyphName(p.glyphs[0], text().controlsPage) : p.name;
      const bool named = !glyph && !name.empty();
      const double nameW = named ? std::min(textWidth(name, kLabelSize, true), p.w / 2) : kLabelGlyph;
      const double textW = textWidth(p.text, kLabelSize, false);
      std::string how = p.how.empty() ? std::string() : "· " + p.how;
      double w = nameW + kit::kSpaceTight + 4 + textW;
      if (!how.empty() && w + kit::kSpaceTight + textWidth(how, kHowSize, false) > p.w) how.clear();
      if (!how.empty()) w += kit::kSpaceTight + textWidth(how, kHowSize, false);
      // Still too long: its lines, where it has them.
      std::vector<std::string> lines;
      if (w > p.w && !p.lines.empty()) {
        const double room = p.w - nameW - kit::kSpaceTight - 4;
        double widest = 0;
        for (size_t l = 0; l < p.lines.size(); ++l) {
          const bool fits = textWidth(p.lines[l], kHowSize, false) <= room;
          lines.push_back(fits ? p.lines[l] : p.shorter[l]);
          widest = std::max(widest, textWidth(lines.back(), kHowSize, false));
        }
        w = p.w - room + std::min(widest, room);
      }
      double x = p.align == 'r' ? p.x + p.w - w : p.align == 'c' ? p.x + (p.w - w) / 2 : p.x;
      x = std::max(x, p.x);
      if (named)
        drawTextEnd(cr, name, x, p.y, nameW + 2, p.h, kLabelSize, true, pal.textDim.r, pal.textDim.g,
                    pal.textDim.b);
      else
        paintGlyph(cr, art, p.glyphs[0], x + kLabelGlyph / 2, cy, kLabelGlyph, 0.75);
      x += nameW + kit::kSpaceTight + 4;
      bool lit = false;
      for (int h = 0; h < 2; ++h)
        lit = lit || ((p.hands >> h & 1) && p.control != Control::Count && (c.held[h] & controlBit(p.control)));
      const Rgba& ink = lit ? pal.textAccent : pal.textDim;
      const double lineH = p.h / 2 + 2;
      for (size_t l = 0; l < lines.size(); ++l)
        drawTextEnd(cr, lines[l], x, cy + (l - lines.size() / 2.0) * lineH, p.x + p.w - x, lineH, kHowSize, false,
                    ink.r, ink.g, ink.b);
      if (lines.empty())
        drawTextEnd(cr, p.text, x, p.y, std::min(textW + 2, p.x + p.w - x), p.h, kLabelSize, false, ink.r, ink.g,
                    ink.b);
      x += textW + kit::kSpaceTight;
      if (!how.empty())
        drawText(cr, how, x, p.y, p.x + p.w - x, p.h, kHowSize, false, false, pal.faint.r, pal.faint.g,
                 pal.faint.b);
    } else if (p.kind == Part::Gesture && p.fixed) {
      const double ink[4] = {pal.textDim.r, pal.textDim.g, pal.textDim.b, 1};
      gestureRow(cr, static_cast<int>(i), ink);
    }
  }

  // The notice: the app's message pill, the button's glyph where it has its
  // icon, Undo (a button) inside its right end.
  if (c.noticeBox[2] > 0) {
    const double x = c.noticeBox[0], y = c.noticeBox[1], w = c.noticeBox[2], h = c.noticeBox[3];
    drawFinish(cr, Finish::Pop, x, y, w, h, h / 2);
    const bool turn = c.notice.gesture;
    double gx = x + 28;
    for (size_t i = 0; i < c.noticeGlyphs.size(); ++i) {
      if (i) {
        drawText(cr, "+", gx, y, kPlusW, h, kHowSize, false, true, pal.textDim.r, pal.textDim.g, pal.textDim.b);
        gx += kPlusW;
      }
      paintGlyph(cr, art, c.noticeGlyphs[i], gx + kNoticeGlyph / 2, y + h / 2, kNoticeGlyph);
      gx += kNoticeGlyph;
    }
    if (turn) drawTurn(cr, gx + kit::kSpaceTight + kNoticeTurn / 2, y + h / 2, kNoticeTurn, pal.textAccent);
    const double textX = x + 28 + noticeGlyphsW(c.noticeGlyphs.size(), turn) + kit::kSpace;
    const double undoX = x + w - kit::kSpace - (textWidth(text().stash.undo, kUndoSize, false) + 2 * 20);
    if (c.noticeLines <= 1) {
      drawText(cr, c.noticeText, textX, y, undoX - kit::kSpace - kit::kSpaceTight - textX, h, kPillSize, false,
               false, pal.text.r, pal.text.g, pal.text.b);
    } else {
      const double ink[4] = {pal.text.r, pal.text.g, pal.text.b, 1};
      drawTextLines(cr, c.noticeText, textX, y + (h - c.noticeTextH) / 2, c.noticeWrapW, kPillSize,
                    c.noticeLines, ink, false);
    }
  }
}

// A button's label (its glyph and job), and a job in the chooser (its label
// and the glyphs of the buttons that do it).
void Menu::drawControlPart(cairo_t* cr, const Button& b, const double ink[4]) const {
  if (!controls_ || b.controlPart < 0 || b.controlPart >= static_cast<int>(controls_->parts.size())) return;
  const ControlsPage& c = *controls_;
  const Part& p = c.parts[static_cast<size_t>(b.controlPart)];
  const Palette pal = palette();
  const ControllerArt* art = c.art.get();
  const double cy = b.y + b.h / 2;
  if (p.kind == Part::Label) {
    bool lit = false;
    for (int h = 0; h < 2; ++h)
      lit = lit || ((p.hands >> h & 1) && (c.held[h] & controlBit(p.control)));
    double x = b.x + kit::kSpace;
    if (paintGlyph(cr, art, p.glyphs[0], x + kLabelGlyph / 2, cy, kLabelGlyph)) {
      x += kLabelGlyph + 12;
    } else {  // without SteamVR's glyph: the button's name
      const double w = std::min(textWidth(p.name, kLabelSize, true), b.w / 2);
      drawTextEnd(cr, p.name, x, b.y, w + 2, b.h, kLabelSize, true, pal.textDim.r, pal.textDim.g, pal.textDim.b,
                  ink[3]);
      x += w + 14;
    }
    double r = ink[0], g = ink[1], bl = ink[2];
    if (lit) r = pal.textAccent.r, g = pal.textAccent.g, bl = pal.textAccent.b;
    else if (p.inherited && !b.latched) r = pal.textDim.r, g = pal.textDim.g, bl = pal.textDim.b;
    else if (p.faint && !b.latched) r = pal.faint.r, g = pal.faint.g, bl = pal.faint.b;
    // A gesture's button: the twist's mark at its end, closer to it than
    // text is. Too long, the job is shortened at its end.
    const double end = b.x + b.w - (p.twist ? kit::kSpaceTight + kLabelTurn + kit::kSpacePiece : kit::kSpace);
    drawTextEnd(cr, p.text, x, b.y, end - x, b.h, kLabelSize, false, r, g, bl, ink[3]);
    if (p.twist) drawTurn(cr, b.x + b.w - kit::kSpaceTight - kLabelTurn / 2, cy, kLabelTurn, pal.textAccent, ink[3]);
    return;
  }
  if (p.kind == Part::Gesture) {
    gestureRow(cr, b.controlPart, ink);
    return;
  }
  if (p.kind == Part::Add) {  // its + in the accent, its words dim
    constexpr double icon = 26;
    drawIcon(cr, Icon::Plus, b.x + kit::kSpace + icon / 2, cy, icon, pal.textAccent.r, pal.textAccent.g,
             pal.textAccent.b, ink[3]);
    const double x = b.x + kit::kSpace + icon + 12;
    drawText(cr, p.text, x, b.y, b.x + b.w - kit::kSpace - x, b.h, kLabelSize, false, false, pal.textDim.r,
             pal.textDim.g, pal.textDim.b, ink[3]);
    return;
  }
  if (p.kind != Part::Job) return;
  // The job's name first, then the glyphs of the buttons doing it from the
  // right (before the chosen one's dot), as many as fit beside it: the rest
  // as "+N".
  const double left = b.x + kit::kSpace, end = b.x + b.w - kit::kSpace - 8 - 14;
  constexpr int kChipText = 19;
  constexpr double kGap = 10, kTextGap = 14, kPlus = 16;
  // Where a value works, after its name (faint).
  const double whereW = p.how.empty() ? 0 : textWidth(p.how, kHowSize, false) + kGap;
  const double textW = std::min(textWidth(p.text, kJobSize, false) + whereW, end - left);
  auto glyphShown = [&](const std::string& glyph) { return art && art->glyphs.count(glyph); };
  // A glyph, else (no SteamVR glyphs) the button's name.
  auto chipW = [&](const std::string& glyph, const std::string& name) {
    return glyphShown(glyph) ? kChipGlyph : textWidth(name, kChipText, true);
  };
  auto unitW = [&](size_t i) {
    double w = chipW(p.glyphs[i], p.names[i]);
    if (!p.withs[i].empty()) w += kPlus + chipW(p.withs[i], p.withNames[i]);
    if (!p.tags[i].empty() && glyphShown(p.glyphs[i])) w += 4 + textWidth(p.tags[i], kChipText, true);
    return w;
  };
  const size_t all = p.glyphs.size();
  const double room = end - left - textW - kTextGap;
  // All of them where they fit; else as many as leave room for "+N".
  double total = 0;
  for (size_t i = 0; i < all; ++i) total += (i ? kGap : 0) + unitW(i);
  size_t shown = all;
  if (total > room) {
    double used = 0;
    for (shown = 0; shown < all; ++shown) {
      const double w = used + (shown ? kGap : 0) + unitW(shown);
      if (w + kGap + textWidth("+" + std::to_string(all - shown - 1), kChipText, true) > room) break;
      used = w;
    }
  }
  double right = end;
  if (shown < all) {
    const std::string more = "+" + std::to_string(all - shown);
    const double w = textWidth(more, kChipText, true);
    if (w <= room) {
      drawText(cr, more, right - w, b.y, w + 2, b.h, kChipText, true, false, pal.textDim.r, pal.textDim.g,
               pal.textDim.b, ink[3]);
      right -= w + kGap;
    }
  }
  auto chip = [&](const std::string& glyph, const std::string& name) {
    if (paintGlyph(cr, art, glyph, right - kChipGlyph / 2, cy, kChipGlyph)) {
      right -= kChipGlyph;
      return;
    }
    const double w = textWidth(name, kChipText, true);
    drawText(cr, name, right - w, b.y, w + 2, b.h, kChipText, true, false, pal.textDim.r, pal.textDim.g,
             pal.textDim.b, ink[3]);
    right -= w;
  };
  for (size_t i = shown; i-- > 0;) {
    chip(p.glyphs[i], p.names[i]);
    if (!p.withs[i].empty()) {  // the button held with it (grip) + the button
      drawText(cr, "+", right - kPlus, b.y, kPlus, b.h, kChipText, false, true, pal.faint.r, pal.faint.g,
               pal.faint.b, ink[3]);
      right -= kPlus;
      chip(p.withs[i], p.withNames[i]);
    }
    // The hand's letter before a glyph (a name without one has it).
    if (!p.tags[i].empty() && glyphShown(p.glyphs[i])) {
      const double w = textWidth(p.tags[i], kChipText, true);
      drawText(cr, p.tags[i], right - 4 - w, b.y, w + 2, b.h, kChipText, true, false, pal.textDim.r,
               pal.textDim.g, pal.textDim.b, ink[3]);
      right -= 4 + w;
    }
    right -= kGap;
  }
  const double nameW = std::max(0.0, std::max(right + kGap - kTextGap, left + textW) - left - whereW);
  drawText(cr, p.text, left, b.y, nameW, b.h, kJobSize, false, false, ink[0], ink[1], ink[2], ink[3]);
  if (whereW > 0) {
    const double x = left + std::min(nameW, textWidth(p.text, kJobSize, false)) + kGap;
    drawText(cr, p.how, x, b.y, whereW, b.h, kHowSize, false, false, pal.faint.r, pal.faint.g, pal.faint.b,
             ink[3]);
  }
}

// The chooser for a button's job, Reset's question, and the gestures' editor.
void Menu::layoutControlsDialog() {
  if (!controls_) controls_ = std::make_unique<ControlsPage>();
  ControlsPage& c = *controls_;
  const Strings& t = text();
  const ControlsPageStrings& cp = t.controlsPage;
  const ControlsView& v = view_.controls;
  if (dialog_ == Dialog::EditGesture) {
    layoutGestureEditor();
    return;
  }
  auto close = [this] {
    dialog_ = Dialog::None;
    return MenuCommand{};
  };
  if (dialog_ == Dialog::ResetButtons) {
    DialogLayout dialog(*this, DialogLayout::kNarrow);
    dialog.title(cp.resetTitle);
    dialog.line(typeTitle(v.type, cp), DialogLayout::Ink::Strong);
    dialog.line(gripCombos(v.type) ? cp.resetGripTip : cp.resetTip);
    dialog.button(textButton(t.cancel, Icon::None, close));
    dialog.button(textButton(t.reset, Icon::None,
                             [this, type = v.type] {
                               dialog_ = Dialog::None;
                               controls_->noticeShown = false;
                               MenuCommand cmd;
                               cmd.type = MenuCommand::ResetButtons;
                               cmd.value = static_cast<double>(type);
                               return cmd;
                             },
                             true));
    return;
  }
  const ControlSlot slot = c.open;
  const LayoutControl* lc = slot.type == v.type ? layoutControl(slot.type, slot.hand, slot.control) : nullptr;
  if (!lc) {
    dialog_ = Dialog::None;
    return;
  }
  // Its job; with grip held, kInherit while it has none of its own.
  const int now = shownJob(v.map, slot);
  // Menu's last button: every other job, and Nothing, is greyed out.
  const bool kept = !slot.grip && !v.map.allowed(slot.type, slot.hand, slot.control, Job::None);
  auto pick = [this, slot, &v](int job) {
    // What the slot does, before and after (with grip held and none of its
    // own, what the button does alone), and whether a grip job goes with it
    // (a button set to choose).
    const Job plain = v.map.job(slot.type, slot.hand, slot.control);
    const int shown = shownJob(v.map, slot);
    const Job was = shown == ControlsPage::kInherit ? plain : static_cast<Job>(shown);
    const Job after = job == ControlsPage::kInherit ? plain : static_cast<Job>(job);
    const bool gripGoes = !slot.grip && after == Job::Choose && v.map.hasGrip(slot.type, slot.hand, slot.control);
    ControlsPage::Notice notice{slot, was, after, v.map.button(slot.type, slot.hand, slot.control)};
    if (gripGoes && was == Job::None)  // only its grip job is lost: that one's
      notice = {{slot.type, slot.hand, slot.control, true},
                v.map.gripJob(slot.type, slot.hand, slot.control),
                Job::None,
                notice.before};
    const bool shownNotice = job != shown && (gripGoes || (was != Job::None && was != after));
    return [this, slot, job, notice, shownNotice] {
      dialog_ = Dialog::None;
      ControlsPage& page = *controls_;
      // A notice where a job was replaced or lost (not Nothing).
      page.noticeShown = shownNotice;
      page.notice = notice;
      return setJob(slot, job);
    };
  };

  DialogLayout dialog(*this, DialogLayout::kWide);
  // The header: the button's glyph and name (drawControlsDialog()), the
  // sections in a well at its right.
  dialog.room(kHeadH, [this, &t](double x, double y, double w) {
    ControlsPage& page = *controls_;
    const JobSection sections[] = {JobSection::General, JobSection::Playback, JobSection::Videos,
                                   JobSection::Sound, JobSection::Picture};
    constexpr double in = kit::kSpacePiece;
    double total = in;
    for (JobSection s : sections) total += textWidth(jobSectionTitle(s, t), kSectionSize, false) + 2 * 20 + in;
    const double right = x + w + DialogLayout::kPad - kit::kSpace;
    Button well;
    well.panel = kBrowser;
    well.popup = well.well = true;
    well.x = right - total, well.y = y, well.w = total, well.h = kHeadH;
    well.radius = kit::kCorner;
    buttons_.push_back(well);
    double px = well.x + in;
    for (JobSection s : sections) {
      const char* name = jobSectionTitle(s, t);
      Button b = textButton(name, Icon::None,
                            [this, s] {
                              controls_->section = s;
                              dirty_ = true;
                              return MenuCommand{};
                            },
                            s == page.section);
      b.panel = kBrowser;
      b.popup = b.piece = true;
      b.radius = kit::kPieceCorner;
      b.fontSize = kSectionSize;
      b.x = px, b.y = y + in, b.w = textWidth(name, kSectionSize, false) + 2 * 20, b.h = kHeadH - 2 * in;
      buttons_.push_back(b);
      px += b.w + in;
    }
    page.headX = x, page.headY = y, page.headW = well.x - kit::kSpace - x;
  });
  const char* hand = slot.hand ? t.controls.right : t.controls.left;
  dialog.line(slot.grip ? printed(cp.handWithGrip, hand) : hand);
  if (kept) dialog.line(cp.kept, DialogLayout::Ink::Strong);
  // The section's jobs, in two columns; each shows the buttons doing it now.
  c.parts.erase(std::remove_if(c.parts.begin(), c.parts.end(), [](const Part& p) { return p.kind == Part::Job; }),
                c.parts.end());
  std::vector<Job> jobs;
  for (int j = 1; j < kJobs; ++j)
    if (jobInfo(static_cast<Job>(j)).section == c.section) jobs.push_back(static_cast<Job>(j));
  dialog.room(kit::kSpaceTight + jobRows() * kJobPitch - kit::kSpacePiece,
              [this, jobs, kept, now, pick, &t, &cp, &v](double x, double y, double w) {
                ControlsPage& page = *controls_;
                const ControllerLayout& layout = controllerLayout(v.type);
                const double left = x - DialogLayout::kPad, inside = w + 2 * DialogLayout::kPad;
                const double columnW = (inside - 3 * kit::kSpace) / 2;
                const int rows = static_cast<int>((jobs.size() + 1) / 2);
                for (size_t i = 0; i < jobs.size(); ++i) {
                  const Job job = jobs[i];
                  const bool off = kept && job != Job::Menu;
                  Part part;
                  part.kind = Part::Job;
                  part.text = jobLabel(job, t, v.skipSeconds, v.bigSkipSeconds);
                  // The buttons doing it alone, then with grip held.
                  for (int grip = 0; grip < 2; ++grip)
                    for (const LayoutControl& lc : layout.controls) {
                      if (shownJob(v.map, {v.type, lc.hand, lc.control, grip == 1}) != static_cast<int>(job))
                        continue;
                      const std::string glyph = glyphOf(lc);
                      std::string tag;
                      for (const LayoutControl& other : layout.controls)
                        if (other.hand != lc.hand && glyphOf(other) == glyph) tag = lc.hand ? cp.rightTag : cp.leftTag;
                      if (lc.control == Control::Bumper)
                        tag += std::string(tag.empty() ? "" : " ") + cp.bumperTag;
                      part.glyphs.push_back(glyph);
                      part.tags.push_back(tag);
                      part.withs.push_back(grip == 1 ? "button_grip" : "");
                      part.withNames.push_back(grip == 1 ? cp.gripName : "");
                      // Without glyphs: its name, with its hand's letter where both have it.
                      const std::string name = buttonName(lc, t);
                      std::string letter;
                      for (const LayoutControl& other : layout.controls)
                        if (other.hand != lc.hand && other.control == lc.control)
                          letter = std::string(lc.hand ? cp.rightTag : cp.leftTag) + " ";
                      part.names.push_back(letter + name);
                    }
                  Button b;
                  b.panel = kBrowser;
                  b.popup = true;
                  b.selected = static_cast<int>(job) == now;
                  b.x = left + kit::kSpace + static_cast<double>(i / rows) * (columnW + kit::kSpace);
                  b.y = y + kit::kSpaceTight + static_cast<double>(i % rows) * kJobPitch;
                  b.w = columnW, b.h = kJobH;
                  b.radius = kit::kCorner;
                  b.fontSize = kJobSize;
                  b.disabled = off;
                  const char* tip = jobTip(job, t);
                  b.tooltip = off ? cp.kept : tip ? tip : "";
                  b.controlPart = static_cast<int>(page.parts.size());
                  b.action = pick(static_cast<int>(job));
                  part.x = b.x, part.y = b.y, part.w = b.w, part.h = b.h;
                  page.parts.push_back(part);
                  buttons_.push_back(b);
                }
              });
  // Bottom left: Nothing; with grip held, also back to its own job.
  auto bottomLeft = [this](const char* label, std::function<MenuCommand()> action, double x) {
    Button b = textButton(label, Icon::None, std::move(action));
    b.raised = b.dialog = true;
    b.fontSize = DialogLayout::kLineSize;
    b.h = DialogLayout::kButtonH;
    b.w = textWidth(label, DialogLayout::kLineSize, false) + 2 * DialogLayout::kButtonPad;
    b.x = x;
    b.radius = kit::kCorner;
    return b;
  };
  Button nothing = bottomLeft(t.jobs.nothing, pick(static_cast<int>(Job::None)), DialogLayout::kEdge);
  nothing.latched = now == static_cast<int>(Job::None);
  nothing.disabled = kept;
  nothing.tooltip = kept ? cp.kept : slot.grip ? cp.gripNothingTip : cp.nothingTip;
  dialog.beside(nothing);
  if (slot.grip) {
    Button same = bottomLeft(cp.sameAsAlone, pick(ControlsPage::kInherit),
                             nothing.x + nothing.w + kit::kSpaceTight);
    same.disabled = now == ControlsPage::kInherit;
    same.tooltip = cp.sameAsAloneTip;
    dialog.beside(same);
  }
  dialog.button(textButton(t.cancel, Icon::None, close));
}

// ---------------------------------------------------------------- the gestures' editor

namespace {

using Editor = ControlsPage::Editor;
using Mistake = Editor::Mistake;

// The first step again: what is held now waits to be let go.
void startRecording(Editor& e, const uint32_t held[2]) {
  e.step = 1;
  for (int h = 0; h < 2; ++h) e.ignore[h] = held[h], e.seen[h] = e.shown[h] = 0;
  e.mistake = Mistake::None;
}

int bitCount(uint32_t bits) {
  int n = 0;
  for (; bits; bits &= bits - 1) ++n;
  return n;
}

// What's wrong with these held, while they are held.
Mistake heldMistake(const ControlMap& map, ControllerType type, const uint32_t held[2]) {
  if (held[0] && held[1]) return Mistake::Hands;
  const uint32_t bits = held[0] | held[1];
  if (bitCount(bits) > 2) return Mistake::Many;
  if (bits == controlBit(Control::Trigger)) return Mistake::Trigger;
  // A button that chooses can't be one (choosingButton()).
  const int hand = held[1] ? 1 : 0;
  const bool grip = held[hand] & controlBit(Control::Grip);
  for (int c = 0; c < kControls; ++c) {
    const Control control = static_cast<Control>(c);
    if ((held[hand] & controlBit(control)) && control != Control::Trigger && control != Control::Grip &&
        map.pressJob(type, hand, control, grip) == Job::Choose)
      return Mistake::Chooses;
  }
  return Mistake::None;
}

// One for either controller with a button that chooses on one of them.
bool choosesOnEither(const ControlMap& map, ControllerType type, Gesture g) {
  g.hands = 3;
  for (int h = 0; h < 2; ++h)
    if (choosingButton(map, type, h, g) != Control::Count) return true;
  return false;
}

// A hand's held controls in the order said and shown: grip first, the
// trigger last, the others as the layout has them.
std::vector<Control> orderedControls(ControllerType type, int hand, uint32_t bits) {
  std::vector<Control> out;
  if (bits & controlBit(Control::Grip)) out.push_back(Control::Grip);
  for (const LayoutControl& lc : controllerLayout(type).controls)
    if (lc.hand == hand && lc.control != Control::Grip && lc.control != Control::Trigger &&
        (bits & controlBit(lc.control)))
      out.push_back(lc.control);
  if (bits & controlBit(Control::Trigger)) out.push_back(Control::Trigger);
  return out;
}

// No room for `g` in its column: those there, but the one changed and those
// it replaces, fill it.
bool noRoom(ControllerType type, const std::vector<Gesture>& list, const Gesture& g, int edit) {
  const int column = gestureColumn(g);
  int rows = 0;
  for (size_t i = 0; i < list.size(); ++i)
    rows += static_cast<int>(i) != edit && gestureColumn(list[i]) == column && !list[i].sameHold(g);
  return rows >= gestureColumnRoom(type, column);
}

const char* mistakeText(Mistake m, const GesturePageStrings& gp) {
  switch (m) {
    case Mistake::Hands: return gp.oneController;
    case Mistake::Many: return gp.twoAtMost;
    case Mistake::Trigger: return gp.triggerAlone;
    case Mistake::Chooses: return gp.chooses;
    case Mistake::RoomLeft: return gp.noRoomLeft;
    case Mistake::RoomRight: return gp.noRoomRight;
    case Mistake::RoomEither: return gp.noRoomEither;
    default: return "";
  }
}

// The value step's columns: values, and "" for its heading (`heading`).
// Values in groups and others: the others first, the group under its name
// beside them; Place's flat screen's beside the rest; else at most five in
// one column, more half and half.
struct ValueColumns {
  std::vector<GestureValue> columns[2];
  std::string heading;
};
ValueColumns valueColumns(GestureSection section, const Strings& t) {
  std::vector<GestureValue> all, grouped, plain;
  for (int i = 0; i < kGestureValues; ++i) {
    const GestureValue value = static_cast<GestureValue>(i);
    const GestureValueInfo& info = gestureValueInfo(value);
    if (info.section != section) continue;
    all.push_back(value);
    (info.group ? grouped : plain).push_back(value);
  }
  ValueColumns out;
  if (!grouped.empty() && !plain.empty()) {
    out.columns[0] = plain;
    out.columns[1] = grouped;
    out.heading = gestureValueInfo(grouped[0]).group(t);
  } else if (section == GestureSection::Place) {
    for (const GestureValue value : all)
      out.columns[gestureValueInfo(value).where == GestureWhere::Flat].push_back(value);
  } else if (all.size() <= 5) {
    out.columns[0] = all;
  } else {
    const size_t half = (all.size() + 1) / 2;
    out.columns[0].assign(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(half));
    out.columns[1].assign(all.begin() + static_cast<std::ptrdiff_t>(half), all.end());
  }
  return out;
}

constexpr int kValueRows = 5;

}  // namespace

#ifndef NDEBUG
// The longest value (with where it works) on every gesture, the left
// controller's column filled first (the longest a column gets), then
// Either's and the right one's up to kMaxGestures; or Either's first (its
// user's beside the fixed ones).
GestureMap Menu::fullestGestures(ControllerType type, bool eitherFirst) const {
  const Strings& t = text();
  GestureValue longest = GestureValue::Volume;
  double widest = 0;
  for (int i = 0; i < kGestureValues; ++i) {
    const GestureValue value = static_cast<GestureValue>(i);
    const double w = textWidth(gestureValueLabel(value, t), kLabelSize, false) +
                     textWidth(gestureWhere(value, t), kHowSize, false);
    if (w > widest) widest = w, longest = value;
  }
  GestureMap map;
  std::vector<Control> firsts{Control::Grip}, seconds{Control::Count, Control::Trigger};
  for (const LayoutControl& lc : controllerLayout(type).controls)
    if (!lc.fixed) {
      if (std::find(firsts.begin(), firsts.end(), lc.control) == firsts.end()) firsts.push_back(lc.control);
      if (std::find(seconds.begin(), seconds.end(), lc.control) == seconds.end()) seconds.push_back(lc.control);
    }
  // Each column's with the longest controls' names first (as shown without
  // SteamVR's glyphs).
  auto namesW = [&](const Gesture& g) {
    double w = 0;
    for (const Control c : gestureControls(g))
      w += textWidth(controlName(type, gestureHand(g), c, w > 0, t), kHowSize, true);
    return w;
  };
  const std::array<uint8_t, 3> order = eitherFirst ? std::array<uint8_t, 3>{3, 1, 2} : std::array<uint8_t, 3>{1, 3, 2};
  for (const uint8_t hands : order) {
    std::vector<Gesture> candidates;
    for (const Control first : firsts)
      for (const Control second : seconds) {
        Gesture g;
        g.hands = hands, g.first = first, g.second = second, g.value = longest;
        if (GestureMap::valid(type, g)) candidates.push_back(g);
      }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [&](const Gesture& a, const Gesture& b) { return namesW(a) > namesW(b); });
    for (const Gesture& g : candidates) {
      const std::vector<Gesture>& list = map.list(type);
      if (list.size() >= kMaxGestures || noRoom(type, list, g, -1)) continue;
      bool taken = false;
      for (const Gesture& o : list) taken = taken || o.sameHold(g);
      if (!taken) map.add(type, g);
    }
  }
  return map;
}
#endif

void Menu::openGestureEditor(int edit) {
  ControlsPage& c = *controls_;
  const ControlsView& v = view_.controls;
  const std::vector<Gesture>& list = v.gestures.list(v.type);
  Editor& e = c.editor;
  e = Editor{};
  if (edit >= 0 && static_cast<size_t>(edit) < list.size()) {
    e.edit = edit;
    e.step = 2;
    e.back = true;
    e.gesture = list[static_cast<size_t>(edit)];
    e.side = gestureHand(e.gesture);
    e.section = gestureValueInfo(e.gesture.value).section;
  } else {
    startRecording(e, c.held);
  }
  c.noticeShown = false;
  openDialog(Dialog::EditGesture, "");
}

// The first step: a control held as it began counts once let go; those held
// since the first press are the gesture's. All let go: on to the value, or
// the step again saying what was wrong, those held shown in red.
void Menu::recordGesture() {
  ControlsPage& c = *controls_;
  Editor& e = c.editor;
  const ControlsView& v = view_.controls;
  const ControllerType type = v.type;
  const Editor before = e;
  uint32_t now[2];
  for (int h = 0; h < 2; ++h) {
    e.ignore[h] &= c.held[h];
    now[h] = c.held[h] & ~e.ignore[h] & gestureBits(type, h);
  }
  if (now[0] | now[1]) {
    for (int h = 0; h < 2; ++h) e.seen[h] |= now[h], e.shown[h] = e.seen[h];
    e.mistake = heldMistake(v.map, type, e.seen);
  } else if (e.seen[0] | e.seen[1]) {
    const uint32_t seen[2] = {e.seen[0], e.seen[1]};
    e.seen[0] = e.seen[1] = 0;
    e.mistake = heldMistake(v.map, type, seen);
    if (e.mistake == Mistake::None) {
      const int side = seen[1] ? 1 : 0;
      const std::vector<Control> controls = orderedControls(type, side, seen[side]);
      const std::vector<Gesture>& list = v.gestures.list(type);
      Gesture g;
      g.hands = static_cast<uint8_t>(1 << side);
      g.first = controls[0];
      g.second = controls.size() > 1 ? controls[1] : Control::Count;
      // One for either controller stays so, where both have its buttons.
      const Gesture* edited = e.edit >= 0 ? &list[static_cast<size_t>(e.edit)] : nullptr;
      if (edited && edited->hands == 3 && bothHave(type, g) && !choosesOnEither(v.map, type, g)) g.hands = 3;
      // Its value: the one changed's, or the one it would replace's.
      if (edited) g.value = edited->value;
      else
        for (const Gesture& o : list)
          if (o.sameHold(g)) g.value = o.value;
      Gesture check = g;
      if (check.value == GestureValue::Count) check.value = GestureValue::Volume;
      if (!GestureMap::valid(type, check)) {
        e.shown[0] = e.shown[1] = 0;  // nothing it can be: as if never held
      } else if (noRoom(type, list, g, e.edit)) {
        e.mistake = g.hands == 3 ? Mistake::RoomEither : side ? Mistake::RoomRight : Mistake::RoomLeft;
      } else {
        e.step = 2;
        e.gesture = g;
        e.side = side;
        e.back = true;
        if (g.value != GestureValue::Count) e.section = gestureValueInfo(g.value).section;
      }
    }
  }
  if (e.step != before.step || e.mistake != before.mistake || e.shown[0] != before.shown[0] ||
      e.shown[1] != before.shown[1])
    dirty_ = true;
}

void Menu::layoutGestureEditor() {
  ControlsPage& c = *controls_;
  Editor& e = c.editor;
  const Strings& t = text();
  const GesturePageStrings& gp = t.gesturePage;
  const ControlsPageStrings& cp = t.controlsPage;
  const ControlsView& v = view_.controls;
  const ControllerType type = v.type;
  const std::vector<Gesture>& list = v.gestures.list(type);
  if (e.edit >= static_cast<int>(list.size()) || !gesturesFor(type)) {
    dialog_ = Dialog::None;
    return;
  }
  auto close = [this] {
    dialog_ = Dialog::None;
    return MenuCommand{};
  };
  auto bottomLeft = [this](const char* label, std::function<MenuCommand()> action) {
    Button b = textButton(label, Icon::None, std::move(action));
    b.raised = b.dialog = true;
    b.fontSize = DialogLayout::kLineSize;
    b.h = DialogLayout::kButtonH;
    b.w = textWidth(label, DialogLayout::kLineSize, false) + 2 * DialogLayout::kButtonPad;
    b.x = DialogLayout::kEdge;
    b.radius = kit::kCorner;
    return b;
  };
  c.parts.erase(std::remove_if(c.parts.begin(), c.parts.end(), [](const Part& p) { return p.kind == Part::Job; }),
                c.parts.end());
  e.heading.clear();
  DialogLayout dialog(*this, kEditorW);
  // The header (drawGestureEditor()).
  dialog.room(kHeadH + kit::kSpacePiece, [this, &gp](double x, double y, double w) {
    Editor& e = controls_->editor;
    e.headX = x, e.headY = y, e.headW = w;
    if (e.step != 2) return;
    // Change button, small at its right.
    constexpr int size = 22;
    Button change = textButton(gp.changeButton, Icon::None, [this] {
      ControlsPage& page = *controls_;
      startRecording(page.editor, page.held);
      dirty_ = true;
      return MenuCommand{};
    });
    change.panel = kBrowser;
    change.popup = change.bare = true;
    change.fontSize = size;
    change.w = textWidth(gp.changeButton, size, false) + 2 * kit::kSpace;
    change.h = 36;
    change.x = x + w + DialogLayout::kPad - 24 - change.w;
    change.y = y + (kHeadH - change.h) / 2;
    change.radius = 10;
    change.tooltip = gp.changeButtonTip;
    buttons_.push_back(change);
    e.headW = change.x - kit::kSpace - x;
  });

  if (e.step == 1) {
    dialog.line(e.mistake != Mistake::None ? mistakeText(e.mistake, gp) : gp.hold,
                e.mistake != Mistake::None ? DialogLayout::Ink::Warn : DialogLayout::Ink::Dim);
    dialog.room(kit::kSpaceTight + kEditorDrawH, [this](double x, double y, double w) {
      Editor& e = controls_->editor;
      e.drawX[0] = x, e.drawX[1] = x + w - kEditorDrawW, e.drawY = y + kit::kSpaceTight;
      e.heldX = x + kEditorDrawW + 24, e.heldW = w - 2 * (kEditorDrawW + 24);
    });
    if (e.back) {
      Button back = bottomLeft(t.back, [this] {
        Editor& e = controls_->editor;
        e.step = 2;
        e.mistake = Mistake::None;
        dirty_ = true;
        return MenuCommand{};
      });
      back.tooltip = gp.backTip;
      dialog.beside(back);
    }
    dialog.button(textButton(t.cancel, Icon::None, close));
    return;
  }

  // The second step: which controller, where both have its buttons; the
  // sections; the section's values, each with the gestures doing it now.
  const Gesture& g = e.gesture;
  const int side = e.side;
  if (bothHave(type, g)) {
    dialog.room(kHandsH + kit::kSpaceTight, [this, &t, &gp, &list, &v, type](double x, double y, double) {
      Editor& e = controls_->editor;
      constexpr double in = kit::kSpacePiece;
      constexpr int size = 23;
      const char* names[2] = {e.side ? t.controls.right : t.controls.left, t.controls.either};
      double widths[2], total = in;
      for (int i = 0; i < 2; ++i) total += (widths[i] = textWidth(names[i], size, false) + 2 * 22) + in;
      Button well;
      well.panel = kBrowser;
      well.popup = well.well = true;
      well.x = x - DialogLayout::kPad + kit::kSpace, well.y = y, well.w = total, well.h = kHandsH;
      well.radius = kit::kCorner;
      buttons_.push_back(well);
      double px = well.x + in;
      for (int i = 0; i < 2; ++i) {
        Gesture moved = e.gesture;
        moved.hands = static_cast<uint8_t>(i ? 3 : 1 << e.side);
        const bool full = moved.hands != e.gesture.hands && noRoom(type, list, moved, e.edit);
        const bool chooses = i == 1 && moved.hands != e.gesture.hands && choosesOnEither(v.map, type, moved);
        Button b = textButton(names[i], Icon::None,
                              [this, hands = moved.hands] {
                                controls_->editor.gesture.hands = hands;
                                dirty_ = true;
                                return MenuCommand{};
                              },
                              e.gesture.hands == moved.hands);
        b.panel = kBrowser;
        b.popup = b.piece = true;
        b.radius = kit::kPieceCorner;
        b.fontSize = size;
        b.x = px, b.y = y + in, b.w = widths[i], b.h = kHandsH - 2 * in;
        b.disabled = full || chooses;
        b.tooltip = chooses ? gp.chooses
                    : full  ? (i ? gp.noRoomEither : e.side ? gp.noRoomRight : gp.noRoomLeft)
                            : gp.handTip;
        buttons_.push_back(b);
        px += widths[i] + in;
      }
    });
  } else {
    dialog.line(side ? t.controls.right : t.controls.left);
  }
  dialog.room(kHeadH + kit::kSpaceTight, [this, &t](double x, double y, double) {
    Editor& e = controls_->editor;
    constexpr double in = kit::kSpacePiece;
    double total = in;
    for (int s = 0; s < static_cast<int>(GestureSection::Count); ++s)
      total += textWidth(gestureSectionTitle(static_cast<GestureSection>(s), t), kSectionSize, false) + 2 * 20 + in;
    Button well;
    well.panel = kBrowser;
    well.popup = well.well = true;
    well.x = x - DialogLayout::kPad + kit::kSpace, well.y = y, well.w = total, well.h = kHeadH;
    well.radius = kit::kCorner;
    buttons_.push_back(well);
    double px = well.x + in;
    for (int s = 0; s < static_cast<int>(GestureSection::Count); ++s) {
      const GestureSection section = static_cast<GestureSection>(s);
      const char* name = gestureSectionTitle(section, t);
      Button b = textButton(name, Icon::None,
                            [this, section] {
                              controls_->editor.section = section;
                              dirty_ = true;
                              return MenuCommand{};
                            },
                            section == e.section);
      b.panel = kBrowser;
      b.popup = b.piece = true;
      b.radius = kit::kPieceCorner;
      b.fontSize = kSectionSize;
      b.x = px, b.y = y + in, b.w = textWidth(name, kSectionSize, false) + 2 * 20, b.h = kHeadH - 2 * in;
      buttons_.push_back(b);
      px += b.w + in;
    }
  });
  // Its value now: the one changed's, or the one it would replace's.
  const GestureValue now = g.value;
  auto pick = [this](GestureValue value) {
    return [this, value] {
      ControlsPage& page = *controls_;
      const Editor& e = page.editor;
      const ControlsView& v = view_.controls;
      const ControllerType type = v.type;
      const std::vector<Gesture>& list = v.gestures.list(type);
      dialog_ = Dialog::None;
      Gesture g = e.gesture;
      g.value = value;
      // Already so (one for either controller does it on this one too).
      if (e.edit < 0)
        for (const Gesture& o : list)
          if (o.bits() == g.bits() && o.value == value && (o.hands & g.hands) == g.hands) return MenuCommand{};
      GestureMap after = v.gestures;
      std::vector<Gesture> replaced;
      if (!(e.edit >= 0 ? after.replace(type, static_cast<size_t>(e.edit), g, &replaced) : after.add(type, g, &replaced)))
        return MenuCommand{};
      // The notice, with Undo putting the whole list back: where another
      // gesture went or the one changed got another value. replace() gives
      // the one changed first.
      const size_t other = e.edit >= 0 ? 1 : 0;
      const Gesture* old = e.edit >= 0 ? &replaced[0] : nullptr;
      Gesture said = g;
      GestureValue was = GestureValue::Count, now = value;
      for (size_t i = other; i < replaced.size() && was == GestureValue::Count; ++i)
        if (replaced[i].value != value) was = replaced[i].value;  // its buttons did another
      if (was == GestureValue::Count && old && old->bits() == g.bits() && old->value != value)
        was = old->value;  // its own buttons, another value
      if (was == GestureValue::Count && old && replaced.size() > other &&
          (old->bits() != g.bits() || (old->hands & ~g.hands))) {
        // Moved onto another's buttons doing the same: its own buttons lost it.
        said = *old;
        if (old->bits() == g.bits()) said.hands = old->hands & ~g.hands;
        was = old->value, now = GestureValue::Count;
      }
      page.noticeShown = was != GestureValue::Count;
      page.notice = {};
      page.notice.gesture = true;
      page.notice.changed = said;
      page.notice.wasValue = was;
      page.notice.nowValue = now;
      page.notice.gestures = v.gestures.listLine(type);
      MenuCommand cmd;
      cmd.type = e.edit >= 0 ? MenuCommand::ChangeGesture : MenuCommand::AddGesture;
      cmd.option = static_cast<int>(type) << 17 | g.pack();
      cmd.value = e.edit >= 0 ? e.edit : 0;
      return cmd;
    };
  };
  dialog.room(kit::kSpaceTight + kValueRows * kJobPitch - kit::kSpacePiece,
              [this, now, pick, &t, &gp, &cp, &list, type](double x, double y, double w) {
                ControlsPage& page = *controls_;
                Editor& e = page.editor;
                const double left = x - DialogLayout::kPad, inside = w + 2 * DialogLayout::kPad;
                const double columnW = (inside - 3 * kit::kSpace) / 2;
                const ValueColumns columns = valueColumns(e.section, t);
                for (int col = 0; col < 2; ++col) {
                  const double cx = left + kit::kSpace + col * (columnW + kit::kSpace);
                  double cy = y + kit::kSpaceTight;
                  if (col == 1 && !columns.heading.empty()) {
                    e.heading = columns.heading;
                    e.headingX = cx + kit::kSpace, e.headingY = cy, e.headingW = columnW - 2 * kit::kSpace;
                    cy += kJobPitch;
                  }
                  for (const GestureValue value : columns.columns[col]) {
                    const GestureValueInfo& info = gestureValueInfo(value);
                    Part part;
                    part.kind = Part::Job;
                    part.text = info.label(t);
                    part.how = gestureWhere(value, t);
                    // The gestures doing it now, but the one changed.
                    for (size_t i = 0; i < list.size(); ++i) {
                      const Gesture& o = list[i];
                      if (o.value != value || static_cast<int>(i) == e.edit) continue;
                      const int hand = gestureHand(o);
                      const std::vector<Control> controls = gestureControls(o);
                      const Control last = controls.back();
                      part.glyphs.push_back(controlGlyphName(type, hand, last));
                      part.withs.push_back(controls.size() > 1 ? controlGlyphName(type, hand, controls[0]) : "");
                      part.withNames.push_back(controls.size() > 1 ? controlName(type, hand, controls[0], false, t)
                                                                   : "");
                      const bool side = o.hands != 3 && bothHave(type, o);
                      const std::string tag = side ? (hand ? cp.rightTag : cp.leftTag) : "";
                      part.tags.push_back(tag);
                      part.names.push_back((tag.empty() ? "" : tag + " ") +
                                           controlName(type, hand, last, controls.size() > 1, t));
                    }
                    Button b;
                    b.panel = kBrowser;
                    b.popup = true;
                    b.selected = value == now;
                    b.x = cx, b.y = cy, b.w = columnW, b.h = kJobH;
                    b.radius = kit::kCorner;
                    b.fontSize = kJobSize;
                    b.tooltip = gestureNeeds(value, gp);
                    b.controlPart = static_cast<int>(page.parts.size());
                    b.action = pick(value);
                    part.x = b.x, part.y = b.y, part.w = b.w, part.h = b.h;
                    page.parts.push_back(part);
                    buttons_.push_back(b);
                    cy += kJobPitch;
                  }
                }
              });
  if (e.edit >= 0) {
    Button remove = bottomLeft(gp.remove, [this, type, index = e.edit, old = list[static_cast<size_t>(e.edit)],
                                           before = v.gestures.listLine(type)] {
      dialog_ = Dialog::None;
      ControlsPage& page = *controls_;
      page.noticeShown = true;
      page.notice = {};
      page.notice.gesture = true;
      page.notice.changed = old;
      page.notice.wasValue = old.value;
      page.notice.gestures = before;
      MenuCommand cmd;
      cmd.type = MenuCommand::RemoveGesture;
      cmd.option = static_cast<int>(type);
      cmd.value = index;
      return cmd;
    });
    remove.tooltip = gp.removeGestureTip;
    dialog.beside(remove);
  }
  dialog.button(textButton(t.cancel, Icon::None, close));
}

void Menu::drawGestureEditor(cairo_t* cr) {
  const ControlsPage& c = *controls_;
  const Editor& e = c.editor;
  const Strings& t = text();
  const GesturePageStrings& gp = t.gesturePage;
  const Palette pal = palette();
  const ControllerType type = view_.controls.type;
  const ControllerArt* art = c.art.get();
  const double cy = e.headY + kHeadH / 2;
  const Rgba& faint = pal.faint;
  double x = e.headX;
  if (e.step == 1) {
    // The twist's mark and the title.
    drawTurn(cr, x + 20, cy, 40, pal.textAccent);
    x += 40 + 14;
    drawText(cr, e.back ? gp.changeButton : gp.addTitle, x, e.headY, e.headX + e.headW - x, kHeadH,
             DialogLayout::kTitleSize, true, false, pal.text.r, pal.text.g, pal.text.b);
    const bool bad = e.mistake != Mistake::None;
    const Rgba red = hexColour(0xff6a6e);
    // Both drawings, each control a dot, lit while held (red: what was wrong).
    for (int h = 0; h < 2 && art; ++h) {
      const ControllerArt::Drawing& d = art->hands[h];
      if (!d.image || d.width <= 0 || d.height <= 0) continue;
      // As wide as its place, cut off (fading out) at its bottom.
      const double k = kEditorDrawW / d.width;
      const double dx = e.drawX[h], dy = e.drawY, bottom = dy + std::min(kEditorDrawH, d.height * k);
      cairo_save(cr);
      cairo_rectangle(cr, dx, dy, kEditorDrawW, bottom - dy);
      cairo_clip(cr);
      cairo_pattern_t* image = cairo_pattern_create_for_surface(d.image.get());
      cairo_matrix_t m;
      cairo_matrix_init_scale(&m, menu_geometry::kRasterScale / k, menu_geometry::kRasterScale / k);
      cairo_matrix_translate(&m, -dx, -dy);
      cairo_pattern_set_matrix(image, &m);
      cairo_set_source(cr, image);
      cairo_pattern_t* fade = cairo_pattern_create_linear(0, dy, 0, bottom);
      cairo_pattern_add_color_stop_rgba(fade, 0, 0, 0, 0, 1);
      cairo_pattern_add_color_stop_rgba(fade, 1 - kFadeBottom / (bottom - dy), 0, 0, 0, 1);
      cairo_pattern_add_color_stop_rgba(fade, 1, 0, 0, 0, d.height * k > kEditorDrawH ? 0 : 1);
      cairo_mask(cr, fade);
      cairo_pattern_destroy(fade);
      cairo_pattern_destroy(image);
      cairo_restore(cr);
      for (const LayoutControl& lc : controllerLayout(type).controls) {
        if (lc.hand != h || !(gestureBits(type, h) & controlBit(lc.control))) continue;
        const auto it = d.points.find(controlOf(lc.input));
        if (it == d.points.end()) continue;
        const double px = dx + it->second[0] * k, py = dy + it->second[1] * k;
        if (py > bottom - kFadeBottom / 2) continue;
        const bool on = e.shown[h] & controlBit(lc.control);
        const double r = on ? 11 : 8;
        if (on) {
          cairo_arc(cr, px, py, r + 9, 0, 2 * M_PI);
          setColor(cr, bad ? red : pal.accent, bad ? 0.35 : 0.45);
          cairo_fill(cr);
        }
        cairo_arc(cr, px, py, r + 4, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 6 / 255.0, 8 / 255.0, 12 / 255.0, 0.9);
        cairo_fill(cr);
        cairo_arc(cr, px, py, r, 0, 2 * M_PI);
        if (on) setColor(cr, bad ? red : pal.accent);
        else cairo_set_source_rgba(cr, 1, 1, 1, 0.4);
        cairo_fill(cr);
      }
    }
    // Between them, what is held: its glyphs in a well, its name, its hand.
    const double wx = e.heldX, wy = e.drawY + 120, ww = e.heldW, wh = kHeldBoxH;
    drawFinish(cr, Finish::Sunk, wx, wy, ww, wh, 22);
    const bool held = e.shown[0] | e.shown[1];
    if (bad && held) {
      cairo_new_sub_path(cr);
      roundedRect(cr, wx + 1, wy + 1, ww - 2, wh - 2, 21);
      cairo_set_source_rgba(cr, 1, 120 / 255.0, 124 / 255.0, 0.55);
      cairo_set_line_width(cr, 2);
      cairo_stroke(cr);
    }
    if (!held) {
      drawText(cr, gp.noneHeld, wx, wy, ww, wh, 24, false, true, faint.r, faint.g, faint.b);
      return;
    }
    // Each hand's, the left one's first.
    struct Chip {
      int hand;
      Control control;
    };
    std::vector<Chip> chips;
    for (int h = 0; h < 2; ++h)
      for (const Control control : orderedControls(type, h, e.shown[h])) chips.push_back({h, control});
    // On one line, smaller where they don't fit, then over as many lines as
    // the well holds, the last ending in "+ …" where more don't fit. A name
    // longer than a line is shortened at its end.
    struct Size {
      double glyph;
      int text;
    };
    const Size sizes[2] = {{kHeldGlyph, kHeldNameSize}, {kHeldGlyphSmall, kHeldNameSmall}};
    const double plusW = textWidth("+", kHowSize, false), plusGap = 2 * 12 + plusW;
    const double room = ww - 2 * kit::kSpace;
    auto chipGlyph = [&](const Chip& chip) { return controlGlyphName(type, chip.hand, chip.control); };
    auto chipName = [&](const Chip& chip) { return controlName(type, chip.hand, chip.control, false, t); };
    auto drawn = [&](const Chip& chip) { return art && art->glyphs.count(chipGlyph(chip)); };
    auto chipW = [&](const Chip& chip, const Size& size) {
      return drawn(chip) ? size.glyph : std::min(textWidth(chipName(chip), size.text, true), room);
    };
    auto chipH = [&](const Chip& chip, const Size& size) { return drawn(chip) ? size.glyph : size.text * 1.4; };
    struct Line {
      size_t first, end;
      double w, h;
    };
    auto lay = [&](const Size& size) {
      std::vector<Line> lines;
      for (size_t i = 0; i < chips.size(); ++i) {
        const double w = chipW(chips[i], size);
        if (lines.empty() || lines.back().w + plusGap + w > room)
          lines.push_back({i, i, lines.empty() ? 0 : plusGap - 12, 0});  // a line after the first starts with +
        Line& line = lines.back();
        line.w += (i > line.first ? plusGap : 0) + w;
        line.h = std::max(line.h, chipH(chips[i], size));
        line.end = i + 1;
      }
      return lines;
    };
    Size size = sizes[0];
    std::vector<Line> lines = lay(size);
    if (lines.size() > 1) lines = lay(size = sizes[1]);
    double linesH = 0;
    size_t fit = 0;
    while (fit < lines.size() && linesH + lines[fit].h <= wh - kit::kSpaceTight) linesH += lines[fit++].h;
    fit = std::max<size_t>(fit, 1);
    const bool cut = fit < lines.size();
    const std::string more = "+ …";
    if (cut) {
      lines.resize(fit);
      Line& last = lines.back();
      const double moreW = 12 + textWidth(more, kHowSize, false);
      while (last.end > last.first + 1 && last.w + moreW > room)
        last.w -= plusGap + chipW(chips[--last.end], size);
      last.w += moreW;
    }
    double gy = wy + (wh - linesH) / 2;
    for (size_t l = 0; l < lines.size(); ++l) {
      const Line& line = lines[l];
      double gx = wx + (ww - line.w) / 2;
      const double cy = gy + line.h / 2;
      for (size_t i = line.first; i < line.end; ++i) {
        if (i > line.first || l > 0) {
          drawText(cr, "+", i > line.first ? gx + 12 : gx, cy - line.h / 2, plusW + 2, line.h, kHowSize, false, false,
                   faint.r, faint.g, faint.b);
          gx += i > line.first ? plusGap : plusGap - 12;
        }
        const double w = chipW(chips[i], size);
        if (!bad && drawn(chips[i])) {  // lit: a glow in the accent behind it
          cairo_pattern_t* glow = cairo_pattern_create_radial(gx + w / 2, cy, 0, gx + w / 2, cy, w * 0.75);
          cairo_pattern_add_color_stop_rgba(glow, 0, pal.accent.r, pal.accent.g, pal.accent.b, 0.45);
          cairo_pattern_add_color_stop_rgba(glow, 1, pal.accent.r, pal.accent.g, pal.accent.b, 0);
          cairo_set_source(cr, glow);
          cairo_arc(cr, gx + w / 2, cy, w * 0.75, 0, 2 * M_PI);
          cairo_fill(cr);
          cairo_pattern_destroy(glow);
        }
        if (!paintGlyph(cr, art, chipGlyph(chips[i]), gx + w / 2, cy, size.glyph, bad ? 0.55 : 1))
          drawTextEnd(cr, chipName(chips[i]), gx, cy - line.h / 2, w + 2, line.h, size.text, true, pal.text.r,
                      pal.text.g, pal.text.b, bad ? 0.55 : 1);
        gx += w;
      }
      if (cut && l + 1 == lines.size())
        drawText(cr, more, gx + 12, cy - line.h / 2, textWidth(more, kHowSize, false) + 2, line.h, kHowSize, false,
                 false, faint.r, faint.g, faint.b);
      gy += line.h;
    }
    // Its name: as a gesture's on one controller, else each with its hand.
    std::string name;
    if (!(e.shown[0] && e.shown[1])) {
      const int hand = e.shown[1] ? 1 : 0;
      Gesture g;
      g.hands = static_cast<uint8_t>(1 << hand);
      const std::vector<Control> controls = orderedControls(type, hand, e.shown[hand]);
      if (controls.size() <= 2) {  // as the list will say it
        g.first = controls[0];
        g.second = controls.size() > 1 ? controls[1] : Control::Count;
        name = gestureName(type, g, t);
      } else {
        for (size_t i = 0; i < controls.size(); ++i)
          name += (i ? " + " : "") + controlName(type, hand, controls[i], i > 0, t);
      }
    } else {
      for (size_t i = 0; i < chips.size(); ++i) {
        const std::string n = controlName(type, chips[i].hand, chips[i].control, true, t);
        if (i) name += " + ";
        name += printed(chips[i].hand ? (i ? gp.rightNext : gp.rightName) : (i ? gp.leftNext : gp.leftName), n);
      }
    }
    // Over two lines where one is too narrow.
    name = capitalized(name);
    const int nameLines = wrappedLines(name, ww, kHeldNameSize - 2, 2);
    const double ink[4] = {pal.text.r, pal.text.g, pal.text.b, 1};
    PangoLayout* first = textLayout(cr, name, kHeldNameSize - 2, false, ww, 1, true);
    int nameW = 0, lineH = 0;
    pango_layout_get_pixel_size(first, &nameW, &lineH);
    const double nameY = wy + wh + kit::kSpace;
    drawTextLines(cr, name, wx, nameY + (36 - lineH) / 2.0, ww, kHeldNameSize - 2, nameLines, ink, true);
    const char* hand = e.shown[0] && e.shown[1] ? gp.bothControllers
                       : e.shown[1]             ? t.controls.right
                                                : t.controls.left;
    drawText(cr, hand, wx, nameY + 40 + (nameLines - 1) * lineH, ww, 30, 22, false, true, pal.textDim.r,
             pal.textDim.g, pal.textDim.b);
    return;
  }
  // The value step's header: the glyphs, the twist's mark, the name.
  const Gesture& g = e.gesture;
  const int hand = gestureHand(g);
  const std::vector<Control> controls = gestureControls(g);
  for (size_t i = 0; i < controls.size(); ++i) {
    if (i) {
      const double plusW = textWidth("+", kHowSize, false);
      drawText(cr, "+", x + 4, e.headY, plusW + 2, kHeadH, kHowSize, false, false, faint.r, faint.g, faint.b);
      x += plusW + 8;
    }
    if (paintGlyph(cr, art, controlGlyphName(type, hand, controls[i]), x + kHeadGlyph / 2, cy, kHeadGlyph))
      x += kHeadGlyph + 4;
  }
  drawTurn(cr, x + 4 + 18, cy, 36, pal.textAccent);
  x += 4 + 36 + 14;
  drawText(cr, capitalized(gestureName(type, g, t)), x, e.headY, e.headX + e.headW - x, kHeadH,
           DialogLayout::kTitleSize, true, false, pal.text.r, pal.text.g, pal.text.b);
  if (!e.heading.empty())
    drawText(cr, e.heading, e.headingX, e.headingY, e.headingW, kJobH, 22, true, false, pal.textDim.r,
             pal.textDim.g, pal.textDim.b);
}

void Menu::drawControlsDialog(cairo_t* cr) {
  if (controls_ && dialog_ == Dialog::EditGesture) drawGestureEditor(cr);
  if (!controls_ || dialog_ != Dialog::ChooseJob) return;
  const ControlsPage& c = *controls_;
  const LayoutControl* lc = layoutControl(c.open.type, c.open.hand, c.open.control);
  if (!lc) return;
  const Palette pal = palette();
  const double cy = c.headY + kHeadH / 2;
  double x = c.headX;
  // Its glyph and name; the hand line under it says whether with grip held.
  if (paintGlyph(cr, c.art.get(), glyphOf(*lc), x + kHeadGlyph / 2, cy, kHeadGlyph)) x += kHeadGlyph + 14;
  drawText(cr, buttonName(*lc, text()), x, c.headY, c.headX + c.headW - x, kHeadH, DialogLayout::kTitleSize, true,
           false, pal.text.r, pal.text.g, pal.text.b);
}
