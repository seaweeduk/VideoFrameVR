#include "control_rules.h"

#include "strings.h"

namespace {

int bitCount(uint32_t bits) {
  int n = 0;
  for (; bits; bits &= bits - 1) ++n;
  return n;
}

}  // namespace

using Mistake = GestureMistake;

const LayoutControl* layoutControl(ControllerType type, int hand, Control control) {
  for (const LayoutControl& c : controllerLayout(type).controls)
    if (c.hand == hand && c.control == control) return &c;
  return nullptr;
}

std::string glyphOf(const LayoutControl& c) { return controlGlyph(c.input, ControlAction::Select, false); }

bool gripCombos(ControllerType type) {
  for (const LayoutControl& lc : controllerLayout(type).controls)
    if (!lc.fixed && lc.control != Control::StickClick) return true;
  return false;
}

const char* typeTitle(ControllerType type, const ControlsPageStrings& t) {
  switch (type) {
    case ControllerType::Frame: return t.frame;
    case ControllerType::Index: return t.index;
    case ControllerType::Touch: return t.touch;
    default: return t.simple;
  }
}

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

const char* glyphName(const std::string& glyph, const ControlsPageStrings& cp) {
  if (glyph == "button_grip") return cp.gripName;
  if (glyph == "button_trigger") return cp.triggerName;
  if (glyph == "button_thumbstick_click") return cp.stickClickName;
  if (glyph.rfind("button_thumbstick", 0) == 0) return cp.stickName;
  return "";
}

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

std::vector<Control> gestureControls(const Gesture& g) {
  std::vector<Control> out{g.first};
  if (g.second != Control::Count) out.push_back(g.second);
  return out;
}

int gestureHand(const Gesture& g) { return g.hands & 2 ? 1 : 0; }

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

std::string gestureName(ControllerType type, const Gesture& g, const Strings& t, bool sentence) {
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

const char* gestureWhere(GestureValue value, const Strings& t) {
  if (value == GestureValue::Zoom) return "";
  switch (gestureValueInfo(value).where) {
    case GestureWhere::Surround: return t.gesturePage.surround;
    case GestureWhere::Flat: return t.controlsPage.heightHow;
    default: return "";
  }
}

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

bool gesturesFor(ControllerType type) {
  for (const LayoutControl& lc : controllerLayout(type).controls)
    if (!lc.fixed || lc.control == Control::Grip) return true;
  return false;
}

uint32_t gestureBits(ControllerType type, int hand) {
  uint32_t bits = 0;
  for (const LayoutControl& lc : controllerLayout(type).controls)
    if (lc.hand == hand && (!lc.fixed || lc.control == Control::Grip || lc.control == Control::Trigger))
      bits |= controlBit(lc.control);
  return bits;
}

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

bool choosesOnEither(const ControlMap& map, ControllerType type, Gesture g) {
  g.hands = 3;
  for (int h = 0; h < 2; ++h)
    if (choosingButton(map, type, h, g) != Control::Count) return true;
  return false;
}

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
