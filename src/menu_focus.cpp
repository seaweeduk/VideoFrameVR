// The menu's stereo focus page, in groups like the other settings pages: on
// the left what holds for all videos (stereo focus on or off, what aims it,
// its marker in the view, the viewer's IPD, motion), on the right the running
// video's own focus (mode, camera separation, manual distance, automatic
// tuning, and the default handling).
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
      view.supported != old.supported || view.unavailable != old.unavailable ||
      view.targetNotice != old.targetNotice || p.enabled != q.enabled ||
      p.showTarget != q.showTarget || p.eyeTracking != q.eyeTracking ||
      p.gazeCorrect != q.gazeCorrect || p.autoIpd != q.autoIpd) {
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

  double x, y;
  // Something the viewer should know, as a row of its own.
  auto note = [&](const std::string& words) {
    addLabel(x + 12, y, rowW - 12, kSettingRowH, words, kHintSize, true);
    y += kSettingStep;
  };

  // For all videos.
  x = left + kGroupPad, y = firstRow(pageTop);
  addToggleRow(x, y, rowW, t.enabled, on, toggle(FocusControl::Enabled, on)).tooltip =
      t.enabledHint;
  y += kSettingStep;
  addChoiceRow(x, y, rowW, 170, t.target, {t.targets[0], t.targets[1]}, p.eyeTracking ? 1 : 0,
               [](int i) { return focusCommand(FocusControl::Target, i)(); }, !on, t.targetHint);
  y += kSettingStep;
  // The eyes were chosen but the head aims: why.
  if (on && !f.targetNotice.empty()) note(f.targetNotice);
  addToggleRow(x, y, rowW, t.showTarget, p.showTarget,
               toggle(FocusControl::ShowTarget, p.showTarget), !on).tooltip = t.showTargetHint;
  y += kSettingStep;
  addToggleRow(x, y, rowW, t.gazeCorrect, p.gazeCorrect,
               toggle(FocusControl::GazeCorrect, p.gazeCorrect)).tooltip = t.gazeCorrectHint;
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
  }
  addGroup(left, pageTop, w, y + 10 - pageTop, t.allTitle);

  // Motion: head translation emulation, for all surround video (not part of
  // the stereo focus, but shown here alongside it).
  const double motionTop = y + 10 + kGap;
  y = firstRow(motionTop);
  addToggleRow(x, y, rowW, t.motion, view_.motionEmulation,
               [] { return MenuCommand{MenuCommand::ToggleMotion}; }).tooltip = t.motionHint;
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
                !view_.motionEmulation, true, t.motionHint);
    y += kSettingStep;
    addLabel(x + 12, y, rowW - 12, 0, t.motionHint, kHintSize, true, 2);
    y += hintHeight(2);
  }
  addGroup(left, motionTop, w, y + 10 - motionTop, t.motionTitle);

  // This video's own (or the defaults it follows).
  x = right + kGroupPad;
  y = firstRow(pageTop);
  // Why its focus does nothing, where that is so.
  if (!f.supported)
    note(t.unsupported);
  else if (automatic && f.unavailable)
    note(text().focusStatus.unavailable);
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
}
