// Stereo focus: a page like Video settings, built from the menu's parts, in
// three columns: what holds for all videos (stereo focus on or off, what
// aims it, its marker in the view, the viewer's IPD), the running video's
// own focus (mode, camera separation, manual distance, automatic tuning,
// and the defaults), and motion. It never scrolls (the debug fit check says
// where it would).
#include "menu_parts.h"
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

// Tenths of a unit, as the unit: millimetres as centimetres, an IPD's.
std::string tenths(int v, const char* unit) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f%s", v / 10.0, unit);
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
#ifndef NDEBUG
  if (focusFitChecked_ != &text()) checkFocusFit();
#endif
  const Strings& strings = text();
  const FocusStrings& t = strings.focus;
  const FocusView& f = view_.focus;
  const FocusSettings& s = f.settings;
  const FocusPreferences& p = f.preferences;
  auto next = [](FocusControl c, int value, int direction) {
    return focusCommand(c, nextFocusStep(c, value, direction));
  };
  auto toggle = [](FocusControl c, bool on) { return focusCommand(c, on ? 0 : 1); };
  // A value of the focus: its control is the command's option, so that a
  // change redraws only its row (focusChanged()).
  auto value = [&](GroupLayout& group, const char* label, FocusControl c, int now, int lo, int hi,
                   const std::string& text, std::function<std::string(int)> format) {
    Row row = group.value(label, MenuCommand::SetFocus, now, lo, hi, text, 1, next(c, now, -1),
                          next(c, now, 1));
    row.option(static_cast<int>(c)).format([format](int v, bool) { return format(v); });
    return row;
  };
  // A control that changes nothing now is greyed out.
  const bool on = p.enabled;
  const bool manual = on && s.mode == FocusMode::Manual;
  const bool automatic = on && s.mode == FocusMode::Automatic;
  // Rows only for those tuning it; hidden ones keep working as set.
  const bool advanced = view_.advancedSettings;
  // The player keeps a closed video's title: no file, no title.
  const bool open = !view_.currentFile.empty();
  PageLayout page(*this, strings.stereoFocus, open ? view_.playback.title : std::string());
  page.pageSwitch(strings.advancedSettings, advanced,
                  command(MenuCommand::ToggleAdvancedSettings), strings.tipAdvanced);

  // For all videos: stereo focus on or off, what aims it, its marker, the
  // gaze correction, and (advanced) the viewer's IPD: the headset's, or a
  // value of its own.
  {
    GroupLayout all = page.group(0, t.allTitle);
    all.labelsFor({t.target, t.ipd});
    all.toggle(t.enabled, on, toggle(FocusControl::Enabled, on)).tip(t.enabledHint);
    all.choice(t.target, {t.targets[0], t.targets[1]}, p.eyeTracking ? 1 : 0,
               [](int i) { return focusCommand(FocusControl::Target, i)(); })
        .tip(t.targetHint)
        .disable(!on);
    // The eyes were chosen but the head aims: why.
    if (on && !f.targetNotice.empty()) all.note(f.targetNotice, true);
    all.toggle(t.showTarget, p.showTarget, toggle(FocusControl::ShowTarget, p.showTarget))
        .tip(t.showTargetHint)
        .disable(!on);
    all.toggle(t.gazeCorrect, p.gazeCorrect, toggle(FocusControl::GazeCorrect, p.gazeCorrect))
        .tip(t.gazeCorrectHint);
    if (advanced) {
      all.toggle(t.autoIpd, p.autoIpd, toggle(FocusControl::AutoIpd, p.autoIpd))
          .tip(t.autoIpdHint)
          .disable(!on);
      const int ipd = p.autoIpd && f.headsetIpdTenthMm > 0 ? f.headsetIpdTenthMm : p.ipdTenthMm;
      value(all, t.ipd, FocusControl::Ipd, ipd, 400, 900, tenths(ipd, " mm"),
            [](int v) { return tenths(v, " mm"); })
          .tip(t.ipdHint)
          .disable(!on);
    }
  }

  // This video's own (or the defaults it follows): why it does nothing,
  // where that is so; its mode, camera separation, manual distance and the
  // automatic focus's minimum distance; (advanced) the rest of its tuning;
  // and the defaults.
  {
    GroupLayout video = page.group(1, t.videoTitle);
    if (!f.supported)
      video.note(t.unsupported, true);
    else if (automatic && f.unavailable)
      video.note(strings.focusStatus.unavailable, true);
    // The modes' well as wide as it can be (its label's own width), the
    // values' fields lined up under it.
    video.labelsFor({t.mode});
    Row mode = video.choice(t.mode, {t.modes[0], t.modes[1], t.modes[2]}, static_cast<int>(s.mode),
                            [](int i) { return focusCommand(FocusControl::Mode, i)(); });
    for (int i = 0; i < 3; ++i) mode.piece(i).tooltip = t.modeTips[i];
    mode.disable(!on);
    std::vector<std::string> labels{t.baseline, t.distance, t.tuningLabels[0]};
    if (advanced)
      labels.insert(labels.end(), std::begin(t.tuningLabels) + 1, std::end(t.tuningLabels));
    video.labelsFor(labels);
    value(video, t.baseline, FocusControl::Baseline, s.baselineMm, 10, 120,
          tenths(s.baselineMm, " cm"), [](int v) { return tenths(v, " cm"); })
        .tip(t.baselineHint)
        .disable(!manual && !automatic);
    value(video, t.distance, FocusControl::Distance, s.distanceCm, 10, 1000,
          distanceText(s.distanceCm), distanceText)
        .logarithmic()
        .tip(t.distanceHint)
        .disable(!manual);
    {
      auto degrees = [](int v) {
        char text[16];
        std::snprintf(text, sizeof(text), "%+d°", v);
        return std::string(text);
      };
      struct Tuning {
        FocusControl control;
        int now, lo, hi;
        bool logarithmic;
        std::function<std::string(int)> format;
      };
      const Tuning tuning[] = {
          {FocusControl::MinDistance, s.minCm, 5, 100000, true, distanceText},
          {FocusControl::MaxDistance, s.maxCm, 5, 100000, true, distanceText},
          {FocusControl::Transition, s.speed, 10, 1000, true, hundredths},
          {FocusControl::Sensitivity, s.sensitivity, 50, 200, false, hundredths},
          {FocusControl::Accuracy, s.accuracy, 1, 128, true, [](int v) { return std::to_string(v); }},
          {FocusControl::Pitch, s.pitchDeg, -45, 45, false, degrees},
          {FocusControl::Yaw, s.yawDeg, -45, 45, false, degrees}};
      // The minimum distance always; the rest with Advanced settings on.
      const size_t shown = advanced ? std::size(tuning) : 1;
      for (size_t i = 0; i < shown; ++i) {
        const Tuning& r = tuning[i];
        Row row = value(video, t.tuningLabels[i], r.control, r.now, r.lo, r.hi, r.format(r.now),
                        r.format);
        if (r.logarithmic) row.logarithmic();
        row.tip(t.tuningHints[i]).disable(!automatic);
      }
    }
    video.buttons({{t.makeDefault, t.makeDefaultHint, focusCommand(FocusControl::MakeDefault), !on},
                   {t.useDefault, t.useDefaultHint, focusCommand(FocusControl::UseDefault),
                    !on || !f.own}});
    video.note(f.own ? t.ownHint : t.defaultHint, true).disable(!on);
  }

  // Motion: head translation emulation, for all surround video (not part of
  // the stereo focus, but shown beside it), and how far away the picture is
  // taken to be.
  {
    GroupLayout motion = page.group(2, t.motionTitle);
    motion.labelsFor({t.motionDistance});
    motion.toggle(t.motion, view_.motionEmulation, command(MenuCommand::ToggleMotion))
        .tip(t.motionHint);
    const int md = view_.motionDistance;
    motion
        .value(t.motionDistance, MenuCommand::SetMotionDistance, md, 5, 1000, distanceText(md), 1,
               command(MenuCommand::SetMotionDistance, nextMotionDistance(md, -1)),
               command(MenuCommand::SetMotionDistance, nextMotionDistance(md, 1)))
        .format([](int v, bool) { return distanceText(v); })
        .logarithmic()
        .tip(t.motionHint)
        .disable(!view_.motionEmulation);
  }
}

// The page with every row that can show at once: on, the notes that can
// show (each of the two that exclude each other), laid out and taken away
// again.
double Menu::focusBottom(bool advanced, bool check) {
  const MenuView saved = view_;
  const PageHead head = pageHead_;
  const double top = pageTop_;
  const size_t buttons = buttons_.size(), groups = groups_.size();
  const FocusStatusStrings& fs = text().focusStatus;

  // A new row that shows only in some state: switch that state on here too.
  // Forgotten, the panel grows to fit the first time the row shows
  // (layoutBrowser()) and stays that tall.
  MenuView& v = view_;
  FocusView& f = v.focus;
  v.advancedSettings = advanced;
  f.preferences.enabled = true;
  f.settings.mode = FocusMode::Automatic;
  f.targetNotice = *std::max_element(std::begin(fs.targets), std::end(fs.targets),
                                     [this](const char* a, const char* b) {
                                       return textWidth(a, kit::kNoteSize, false) <
                                              textWidth(b, kit::kNoteSize, false);
                                     });
  pageTop_ = 0;
  double bottom = 0;
  auto measure = [&] {
    const size_t first = groups_.size();
    layoutFocus();
    for (size_t i = first; i < groups_.size(); ++i) bottom = std::max(bottom, groups_[i].y + groups_[i].h);
  };
  f.supported = false;
  f.own = true;
#ifndef NDEBUG
  if (check) fitCase_ = " (every row, unsupported)";
#endif
  measure();
  f.supported = f.unavailable = true;
  f.own = false;
#ifndef NDEBUG
  if (check) fitCase_ = " (every row, unavailable)";
#endif
  measure();
#ifndef NDEBUG
  fitCase_ = nullptr;
#endif
  (void)check;

  buttons_.erase(buttons_.begin() + static_cast<std::ptrdiff_t>(buttons), buttons_.end());
  groups_.erase(groups_.begin() + static_cast<std::ptrdiff_t>(groups), groups_.end());
  pageHead_ = head;
  pageTop_ = top;
  view_ = saved;
  return bottom;
}

#ifndef NDEBUG
// Says loudly where Stereo focus with every row doesn't fit (Advanced
// settings on).
void Menu::checkFocusFit() {
  focusFitChecked_ = &text();
  focusBottom(true, true);
}
#endif
