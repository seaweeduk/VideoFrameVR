// The menu's stereo focus page, in groups like the other settings pages: on
// the left what holds for all videos (stereo focus on or off, what aims it,
// the viewer's IPD, diagnostics, motion), on the right the running video's
// own focus (mode, camera separation, manual distance, automatic tuning, and
// the default handling) and its live state.
#include "menu_style.h"
#include "motion.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>

namespace {

std::function<MenuCommand()> focusCommand(FocusControl control, double value = 0) {
  return [control, value] {
    MenuCommand c;
    c.type = MenuCommand::SetFocus;
    c.option = static_cast<int>(control);
    c.value = value;
    return c;
  };
}

std::string distanceText(int cm) {
  char buf[32];
  if (cm < 100)
    std::snprintf(buf, sizeof(buf), "%d cm", cm);
  else
    std::snprintf(buf, sizeof(buf), "%g m", cm / 100.0);
  return buf;
}

std::string hundredths(int v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%.2f", v / 100.0);
  return buf;
}

}  // namespace

void Menu::focusChanged(const FocusView& view) {
  const FocusView& old = view_.focus;
  const auto& p = view.preferences;
  const auto& q = old.preferences;
  if (view.settings.mode != old.settings.mode || view.own != old.own ||
      view.supported != old.supported || p.enabled != q.enabled ||
      p.diagnostics != q.diagnostics || p.eyeTracking != q.eyeTracking ||
      p.gazeDot != q.gazeDot || p.gazeCorrect != q.gazeCorrect || p.autoIpd != q.autoIpd) {
    browserChanged();
    return;
  }
  const auto& s = view.settings;
  const auto& t = old.settings;
  auto value = [&](int now, int before, FocusControl control) {
    if (now != before) sliderChanged(MenuCommand::SetFocus, 0, static_cast<int>(control));
  };
  value(s.distanceCm, t.distanceCm, FocusControl::Distance);
  value(s.baselineMm, t.baselineMm, FocusControl::Baseline);
  value(s.minCm, t.minCm, FocusControl::MinDistance);
  value(s.maxCm, t.maxCm, FocusControl::MaxDistance);
  value(s.speed, t.speed, FocusControl::Transition);
  value(s.sensitivity, t.sensitivity, FocusControl::Sensitivity);
  value(s.accuracy, t.accuracy, FocusControl::Accuracy);
  value(s.pitchDeg, t.pitchDeg, FocusControl::Pitch);
  value(s.yawDeg, t.yawDeg, FocusControl::Yaw);
  value(p.ipdTenthMm, q.ipdTenthMm, FocusControl::Ipd);
  value(view.headsetIpdTenthMm, old.headsetIpdTenthMm, FocusControl::Ipd);
  if ((view.status != old.status || view.targetStatus != old.targetStatus) && !groups_.empty()) {
    const Group& state = groups_.back();
    browserChanged(state.y - 4, state.y + state.h + 4, state.x - 4, state.x + state.w + 4);
  }
}

void Menu::layoutFocus() {
  const FocusStrings& t = text().focus;
  const FocusView& f = view_.focus;
  const FocusSettings& s = f.settings;
  const FocusPreferences& p = f.preferences;
  const double w = (Menu::kPageWidth - 2 * kPagePad - kGap) / 2;
  const double rowW = w - 2 * kGroupPad, labelW = 330;
  const double left = Menu::kPageX + kPagePad, right = left + w + kGap;
  auto next = [](FocusControl c, int value, int direction) {
    return focusCommand(c, nextFocusStep(c, value, direction));
  };
  auto toggle = [](FocusControl c, bool on) { return focusCommand(c, on ? 0 : 1); };
  // A control that changes nothing now is greyed out.
  const bool on = p.enabled;
  const bool manual = on && s.mode == FocusMode::Manual;
  const bool automatic = on && s.mode == FocusMode::Automatic;
  // Rows only for those tuning it; hidden ones keep working as set.
  const bool advanced = view_.advancedSettings;
  const double pageTop = layoutAdvancedSwitch();

  // For all videos.
  double x = left + kGroupPad, y = firstRow(pageTop);
  addToggleRow(x, y, rowW, t.enabled, on, toggle(FocusControl::Enabled, on)).tooltip =
      t.enabledHint;
  y += kSettingStep;
  addChoiceRow(x, y, rowW, 170, t.target, {t.targets[0], t.targets[1]}, p.eyeTracking ? 1 : 0,
               [](int i) { return focusCommand(FocusControl::Target, i)(); }, !on, t.targetHint);
  y += kSettingStep;
  addToggleRow(x, y, rowW, t.gazeDot, p.gazeDot, toggle(FocusControl::GazeDot, p.gazeDot));
  y += kSettingStep;
  if (advanced) {
    // The viewer's IPD: the headset's, or a value of its own.
    addToggleRow(x, y, rowW, t.autoIpd, p.autoIpd, toggle(FocusControl::AutoIpd, p.autoIpd), !on)
        .tooltip = t.autoIpdHint;
    y += kSettingStep;
    const int ipd = p.autoIpd && f.headsetIpdTenthMm > 0 ? f.headsetIpdTenthMm : p.ipdTenthMm;
    char ipdText[16];
    std::snprintf(ipdText, sizeof(ipdText), "%.1f mm", ipd / 10.0);
    addValueRow(x, y, rowW, labelW, t.ipd, ipdText, ipd, 400, 900,
                next(FocusControl::Ipd, ipd, -1), next(FocusControl::Ipd, ipd, 1), !on, false,
                t.ipdHint);
    y += kSettingStep;
    addToggleRow(x, y, rowW, t.diagnostics, p.diagnostics,
                 toggle(FocusControl::Diagnostics, p.diagnostics), !on).tooltip = t.diagnosticsHint;
    y += kSettingStep;
  }
  addGroup(left, pageTop, w, y + 10 - pageTop, t.allTitle);

  // Motion: head translation emulation, for all surround video.
  const double motionTop = y + 10 + kGap;
  y = firstRow(motionTop);
  addToggleRow(x, y, rowW, t.motion, view_.motionEmulation,
               [] { return MenuCommand{MenuCommand::ToggleMotion}; }).tooltip = t.motionHint;
  y += kSettingStep;
  addToggleRow(x, y, rowW, t.motionFollowsFocus, view_.motionFollowsFocus,
               [] { return MenuCommand{MenuCommand::ToggleMotionFollowsFocus}; },
               !view_.motionEmulation).tooltip = t.motionFollowsFocusHint;
  y += kSettingStep;
  auto motionDistance = [](int cm) {
    return [cm] {
      MenuCommand c;
      c.type = MenuCommand::SetMotionDistance;
      c.value = cm;
      return c;
    };
  };
  const int md = view_.motionDistance;
  if (advanced) {
    addValueRow(x, y, rowW, labelW, t.motionDistance, distanceText(md), md, 5, 1000,
                motionDistance(nextMotionDistance(md, -1)), motionDistance(nextMotionDistance(md, 1)),
                 !view_.motionEmulation || view_.motionFollowsFocus, true, t.motionHint);
    y += kSettingStep;
    addLabel(x + 12, y, rowW - 12, 0,
             view_.motionFollowsFocus ? t.motionFollowsFocusHint : t.motionHint, kHintSize, true, 2);
    y += hintHeight(2);
  }
  addGroup(left, motionTop, w, y + 10 - motionTop, t.motionTitle);

  // This video's own (or the defaults it follows).
  x = right + kGroupPad;
  y = firstRow(pageTop);
  addChoiceRow(x, y, rowW, 170, t.mode, {t.modes[0], t.modes[1], t.modes[2]},
               static_cast<int>(s.mode), [](int i) { return focusCommand(FocusControl::Mode, i)(); },
                !on);
  for (size_t i = 0; i < 3; ++i) buttons_[buttons_.size() - 3 + i].tooltip = t.modeTips[i];
  y += kSettingStep;
  char camera[16];
  std::snprintf(camera, sizeof(camera), "%.1f cm", s.baselineMm / 10.0);
  addValueRow(x, y, rowW, labelW, t.baseline, camera, s.baselineMm, 10, 120,
              next(FocusControl::Baseline, s.baselineMm, -1),
              next(FocusControl::Baseline, s.baselineMm, 1), !manual && !automatic, false, t.baselineHint);
  y += kSettingStep;
  addValueRow(x, y, rowW, labelW, t.distance, distanceText(s.distanceCm),
              s.distanceCm, 10, 1000, next(FocusControl::Distance, s.distanceCm, -1),
              next(FocusControl::Distance, s.distanceCm, 1), !manual, true, t.distanceHint);
  y += kSettingStep;
  char pitch[16], yaw[16];
  std::snprintf(pitch, sizeof(pitch), "%+d°", s.pitchDeg);
  std::snprintf(yaw, sizeof(yaw), "%+d°", s.yawDeg);
  struct Row {
    std::string value;
    FocusControl control;
    int current, lo, hi;
    bool logarithmic;
  };
  const Row tuning[] = {
      {distanceText(s.minCm), FocusControl::MinDistance, s.minCm, 5, 100000, true},
      {distanceText(s.maxCm), FocusControl::MaxDistance, s.maxCm, 5, 100000, true},
      {hundredths(s.speed), FocusControl::Transition, s.speed, 10, 1000, true},
      {hundredths(s.sensitivity), FocusControl::Sensitivity, s.sensitivity, 50, 200, false},
      {std::to_string(s.accuracy), FocusControl::Accuracy, s.accuracy, 1, 128, true},
      {pitch, FocusControl::Pitch, s.pitchDeg, -45, 45, false},
      {yaw, FocusControl::Yaw, s.yawDeg, -45, 45, false}};
  for (size_t i = 0; advanced && i < std::size(tuning); ++i) {
    const Row& r = tuning[i];
    addValueRow(x, y, rowW, labelW, t.tuningLabels[i], r.value, r.current, r.lo, r.hi,
                next(r.control, r.current, -1), next(r.control, r.current, 1), !automatic,
                r.logarithmic, t.tuningHints[i]);
    y += kSettingStep;
  }
  Button makeDefault = textButton(t.makeDefault, Icon::None, focusCommand(FocusControl::MakeDefault));
  Button useDefault = textButton(t.useDefault, Icon::None, focusCommand(FocusControl::UseDefault));
  makeDefault.tooltip = t.makeDefaultHint;
  useDefault.tooltip = t.useDefaultHint;
  makeDefault.x = x;
  useDefault.x = x + makeDefault.w + kGap;
  makeDefault.disabled = !on;
  useDefault.disabled = !on || !f.own;
  for (Button* b : {&makeDefault, &useDefault}) {
    b->panel = kBrowser;
    b->y = y;
    b->h = kSettingRowH;
    b->fontSize = kTextSize;
    buttons_.push_back(*b);
  }
  const double hintX = useDefault.x + useDefault.w + 20;
  addLabel(hintX, y, x + rowW - hintX, kSettingRowH, f.own ? t.ownHint : t.defaultHint, kHintSize,
           true)
      .disabled = !on;
  y += kSettingStep;
  addGroup(right, pageTop, w, y + 10 - pageTop, t.videoTitle);

  // What it is doing now.
  const double stateTop = y + 10 + kGap;
  y = firstRow(stateTop);
  addLabel(x + 12, y, rowW - 12, 0, !f.supported ? t.unsupported : on ? f.status : t.offForAll,
           kTextSize, !f.supported, 2);
  y += 80;
  if (f.supported && automatic) {
    addLabel(x + 12, y, rowW - 12, 0, f.targetStatus, kTextSize, false, 2);
    y += 80;
  }
  addGroup(right, stateTop, w, y - stateTop, t.stateTitle);
}
