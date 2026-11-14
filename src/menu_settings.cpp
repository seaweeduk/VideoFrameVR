#include "menu_style.h"

#include "eye_resolution.h"
#include "menu_stash.h"
#include "refresh_rate.h"

#include <cmath>
#include <iterator>

// The grouped settings pages: titled boxes in
// columns on the browser's page, a label and a control per row.
//   Settings: the menu, the view, Stash, subtitles and playback.
//   Video settings: the running video's format and picture, the image
//   settings for all videos, and (in passthrough) removing its background.
// Stereo focus has its own page (menu_focus.cpp). Video settings and stereo
// focus are tabs of the playback strip and show without the sources column.

namespace {

std::string fixed2(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f", v);
  return buf;
}

// Whether the running video's key removes anything (VideoSettings::keyRemoves()).
bool keyRemoves(const MenuView& v) {
  return v.greenScreen.source != KeySource::Automatic || v.colourKey || v.greenScreenFound;
}

}  // namespace

void Menu::layoutSettings() {
  // Stash's server being typed in (its fields over the keyboard), or its
  // ratings and plays being sent: alone.
  if (stashMenu_ && StashPages::layoutSettingsPanel(*this)) return;
  const Strings& t = text();
  const double w = kColumnW - 2 * kGroupPad;
  auto choose = [](MenuCommand::Type type, std::vector<double> values) {
    return [type, values](int i) {
      MenuCommand c;
      c.type = type;
      c.value = values[i];
      return c;
    };
  };

  // The menu: language and the file thumbnails.
  const double pageTop = layoutAdvancedSwitch();
  double x = columnX(0) + kGroupPad, y = firstRow(pageTop);
  double row = y;
  addLabel(x + 12, row, 128, kSettingRowH, t.language);
  Button language = textButton(languageName(languageChoice()) + " ▾", Icon::None, [this] {
    dropdown_ = dropdown_ == Dropdown::Language ? Dropdown::None : Dropdown::Language;
    return MenuCommand{};
  });
  language.panel = kBrowser;
  language.bare = language.on = true;
  language.x = x + 140;
  language.y = row;
  language.w = textWidth(language.label, 26, false) + 40;
  language.h = kSettingRowH;
  language.fontSize = 26;
  language.tooltip = t.languageHint;
  buttons_.push_back(language);
  if (dropdown_ == Dropdown::Language) {
    std::vector<Button> entries;
    for (int i = 0; i < languageCount(); ++i)
      entries.push_back(textButton(languageName(i), Icon::None, [this, i] {
        dropdown_ = Dropdown::None;
        MenuCommand c;
        c.type = MenuCommand::SetLanguage;
        c.value = i;
        return c;
      }, i == languageChoice()));
    addDropdownBelow(language, std::move(entries));
  }
  row += kSettingStep;
  addToggleRow(x, row, w, t.thumbnails, view_.thumbnails,
               command(MenuCommand::ToggleThumbnails)).tooltip = t.thumbnailsHint;
  addLabel(x + 12, row + kSettingStep, w - 12, 0, t.thumbnailsHint, kHintSize, true, 2);
  const double menuBottom = row + kSettingStep + hintHeight(2) + 6;
  addGroup(columnX(0), pageTop, kColumnW, menuBottom - pageTop, t.groupMenu);

  // Appearance: the design (not on the Frame, which has no glass) and the
  // colour, a preset or a hue, brightness and saturation of one's own. Hue
  // is drawn as the colours to choose from (drawSlider()).
  const AccentStrings& at = t.accent;
  const double lookTop = menuBottom + kGap;
  row = firstRow(lookTop);
  if (!kStandaloneHeadset) {
    addChoiceRow(x, row, w, 180, t.design, {t.themes[0], t.themes[1]}, glass() ? 0 : 1,
                 choose(MenuCommand::SetTheme,
                        {static_cast<double>(Theme::Glass), static_cast<double>(Theme::Material)}));
    row += kSettingStep;
  }
  addLabel(x + 12, row, 128, kSettingRowH, at.colour);
  const double pitch = (w - 180) / (kAccentCustom + 1);  // under Design's buttons
  for (int i = 0; i <= kAccentCustom; ++i) {
    Button swatch = textButton("", Icon::None, command(MenuCommand::SetAccent, i));
    swatch.panel = kBrowser;
    swatch.swatch = i;
    swatch.on = view_.accent.preset == i;
    swatch.bare = true;
    swatch.x = x + 180 + i * pitch + (pitch - kSettingRowH) / 2;
    swatch.y = row;
    swatch.w = swatch.h = kSettingRowH;
    swatch.radius = kSettingRowH / 2;
    swatch.tooltip = at.names[i];
    buttons_.push_back(swatch);
  }
  row += kSettingStep;
  if (view_.accent.preset == kAccentCustom) {
    const int hue = view_.accent.hue, light = view_.accent.brightness;
    const int saturation = view_.accent.saturation;
    addValueRow(x, row, w, 180, at.hue, std::to_string(hue) + "°", hue, 0, 359,
                command(MenuCommand::SetAccentHue, (hue + 359) % 360),
                command(MenuCommand::SetAccentHue, (hue + 1) % 360));
    row += kSettingStep;
    addValueRow(x, row, w, 180, at.brightness, std::to_string(light) + " %", light, 0, 100,
                command(MenuCommand::SetAccentBrightness, std::max(0, light - 1)),
                command(MenuCommand::SetAccentBrightness, std::min(100, light + 1)), false, false,
                at.brightnessHint);
    row += kSettingStep;
    addValueRow(x, row, w, 180, at.saturation, std::to_string(saturation) + " %", saturation, 0,
                100, command(MenuCommand::SetAccentSaturation, std::max(0, saturation - 1)),
                command(MenuCommand::SetAccentSaturation, std::min(100, saturation + 1)));
    row += kSettingStep;
  }
  const double lookBottom = row + 10;
  addGroup(columnX(0), lookTop, kColumnW, lookBottom - lookTop, at.group);

  // The view: the night sky, and passthrough for videos made for it.
  x = columnX(1) + kGroupPad;
  addToggleRow(x, y, w, t.background, view_.background, command(MenuCommand::ToggleBackground));
  const AutoPassthroughStrings& pt = t.autoPassthrough;
  const char* ptHint = kStandaloneHeadset ? pt.hint : pt.hintDesktop;
  addToggleRow(x, y + kSettingStep, w, pt.setting, view_.autoPassthrough,
               command(MenuCommand::ToggleAutoPassthrough)).tooltip = ptHint;
  addLabel(x + 12, y + 2 * kSettingStep, w - 12, 0, ptHint, kHintSize, true, 3);
  const double viewBottom = y + 2 * kSettingStep + hintHeight(3) + 6;
  addGroup(columnX(1), pageTop, kColumnW, viewBottom - pageTop, t.groupView);

  // Stash: on or off; while on, its server, switches and state.
  StashPages::layoutSettingsGroup(*this, pageTop);

  // Subtitles: floating, size, height and distance.
  const double subtitlesTop = lookBottom + kGap;
  x = columnX(0) + kGroupPad;
  y = firstRow(subtitlesTop);
  addToggleRow(x, y, w, t.subtitles3d, view_.floatingSubtitles,
               command(MenuCommand::ToggleFloatingSubtitles)).tooltip = t.subtitles3dHint;
  addLabel(x + 12, y + kSettingStep, w - 12, 0, t.subtitles3dHint, kHintSize, true, 2);
  y += kSettingStep + hintHeight(2) + 6;
  char distance[32];
  std::snprintf(distance, sizeof(distance), "%g m", view_.subtitleDistance / 100.0);
  struct Step {
    const char* label;
    std::string value;
    int now, lo, hi, step;
    MenuCommand::Type type;
  };
  const Step steps[] = {
      {t.subtitleSize, std::to_string(view_.subtitleSize) + " %", view_.subtitleSize, 50, 200, 1,
       MenuCommand::SetSubtitleSize},
      {t.subtitleRaise, std::to_string(view_.subtitleRaise), view_.subtitleRaise, -100, 100, 1,
       MenuCommand::SetSubtitleRaise},
      {t.subtitleDistance, distance, view_.subtitleDistance, 25, 600, 1,
       MenuCommand::SetSubtitleDistance}};
  for (const Step& s : steps) {
    addValueRow(x, y, w, 270, s.label, s.value, s.now, s.lo, s.hi,
                command(s.type, std::max(s.lo, s.now - s.step)),
                command(s.type, std::min(s.hi, s.now + s.step)), false, false,
                s.type == MenuCommand::SetSubtitleRaise ? t.tipSubtitleRaise : "");
    y += kSettingStep;
  }
  addGroup(columnX(0), subtitlesTop, kColumnW, y + 10 - subtitlesTop, t.subtitles);
  layoutDiagnostics(y + 10 + kGap, 0);

  // Playback: resuming videos where they were left, the seek bar's preview,
  // the performance readout and controller vibration.
  const double top = viewBottom + kGap;
  x = columnX(1) + kGroupPad;
  y = firstRow(top);
  addToggleRow(x, y, w, t.resume, view_.resume, command(MenuCommand::ToggleResume))
      .tooltip = t.resumeHint;
  addLabel(x + 12, y + kSettingStep, w - 12, 0, t.resumeHint, kHintSize, true, 2);
  y += kSettingStep + hintHeight(2) + 6;
  const char* previewHint = kStandaloneHeadset && !view_.seekPreviewAvailable
                                ? t.seekPreviewUnavailable : t.seekPreviewHint;
  addToggleRow(x, y, w, t.seekPreview, view_.seekPreview && view_.seekPreviewAvailable,
               command(MenuCommand::ToggleSeekPreview), !view_.seekPreviewAvailable)
      .tooltip = previewHint;
  addLabel(x + 12, y + kSettingStep, w - 12, 0, previewHint, kHintSize, true, 2);
  y += kSettingStep + hintHeight(2) + 6;
  addToggleRow(x, y, w, t.showPerformance, view_.showPerformance,
               command(MenuCommand::ToggleShowPerformance)).tooltip = t.showPerformanceHint;
  addLabel(x + 12, y + kSettingStep, w - 12, 0, t.showPerformanceHint, kHintSize, true, 2);
  y += kSettingStep + hintHeight(2) + 6;
  addToggleRow(x, y, w, t.vibration, view_.vibration, command(MenuCommand::ToggleVibration))
      .tooltip = t.vibrationHint;
  addLabel(x + 12, y + kSettingStep, w - 12, 0, t.vibrationHint, kHintSize, true, 2);
  y += kSettingStep + hintHeight(2) + 6;
  // The drive offline downloads go to (menu_offline.cpp).
  if (offline_ && !offline_->drives().empty()) {
    addLabel(x + 12, y, w - 12, kSettingRowH, t.offline.setting);
    y += kSettingStep;
    std::vector<std::string> names, roots;
    int chosen = 0;
    for (const Offline::Drive& d : offline_->drives()) {
      if (d.root == downloadRoot()) chosen = static_cast<int>(names.size());
      names.push_back(driveName(d, false));
      roots.push_back(d.root);
    }
    addChoiceRow(x, y, w, 0, "", names, chosen,
                 [roots](int i) {
                   MenuCommand c;
                   c.type = MenuCommand::SetDownloadsTo;
                   c.path = roots[i];
                   return c;
                 },
                 false, t.offline.settingHint);
    addLabel(x + 12, y + kSettingStep, w - 12, 0, t.offline.settingHint, kHintSize, true, 2);
    y += kSettingStep + hintHeight(2) + 6;
  }
  addGroup(columnX(1), top, kColumnW, y - top, t.groupPlayback);
}

// Help: What's new (if this build has notes for this device), and the logs
// and settings saved for a bug report, and how that went.
void Menu::layoutDiagnostics(double top, int column) {
  using State = DiagnosticsExport::State;
  const DiagnosticsStrings& t = text().diagnostics;
  const State state = view_.diagnostics;
  const double x = columnX(column) + kGroupPad, w = kColumnW - 2 * kGroupPad;
  double y = firstRow(top);
  static const bool notes = !allReleaseNotes().empty();
  double saveX = x;
  if (notes) {
    Button whatsNew = textButton(text().whatsNew.title, Icon::None, [this] {
      showWhatsNew(allReleaseNotes());
      return MenuCommand{};
    });
    whatsNew.panel = kBrowser;
    whatsNew.x = x;
    whatsNew.y = y;
    whatsNew.h = kSettingRowH;
    whatsNew.fontSize = kTextSize;
    buttons_.push_back(whatsNew);
    saveX = x + whatsNew.w + kGap;
  }
  Button save = textButton(state == State::Saving ? t.saving : t.save, Icon::None,
                           command(MenuCommand::SaveDiagnostics));
  save.panel = kBrowser;
  save.x = saveX;
  save.y = y;
  save.h = kSettingRowH;
  save.fontSize = kTextSize;
  save.disabled = state == State::Saving;
  save.tooltip = t.hint;
  buttons_.push_back(save);
  y += kSettingStep;
  char saved[512];
  std::snprintf(saved, sizeof(saved), t.saved, view_.diagnosticsFile.c_str());
  const std::string line = state == State::Saved    ? saved
                           : state == State::Failed ? t.failed
                           : view_.crashedLastRun   ? t.crashedHint
                                                    : "";
  const double bottom = line.empty() ? y + 10 : y + hintHeight(4) + 6;
  if (!line.empty())
    addLabel(x + 12, y, w - 12, 0, line, kHintSize, state != State::Failed, 4);
  addGroup(columnX(column), top, kColumnW, bottom - top, t.group);
}

// One switch for every page's advanced rows, alone in a row at the top
// right of each page that hides some (and of Settings).
double Menu::layoutAdvancedSwitch() {
  const Strings& t = text();
  addToggleRow(columnX(2) + kGroupPad, kPagePad, kColumnW - 2 * kGroupPad, t.advancedSettings,
               view_.advancedSettings, command(MenuCommand::ToggleAdvancedSettings))
      .tooltip = t.tipAdvanced;
  return kPagePad + kSettingRowH + kGap;
}

// The update dialog: a newer release of the managed GitHub install, and once
// the user updates, how that goes. The check itself never shows.
void Menu::layoutUpdateDialog(std::vector<Button>& row) {
  using State = ReleaseUpdate::State;
  const State state = view_.releaseUpdate;
  const ReleaseUpdateStrings& t = text().releaseUpdate;
  auto close = [this] {
    dialog_ = Dialog::None;
    return MenuCommand{};
  };
  const bool failed = state == State::DownloadFailed;
  if (state != State::Available && state != State::Downloading && state != State::Restarting &&
      !failed) {
    row.push_back(textButton(text().whatsNew.close, Icon::None, close));
    return;
  }
  const bool busy = state == State::Downloading || state == State::Restarting;
  Button later = textButton(failed ? text().whatsNew.close : t.notNow, Icon::None, close);
  later.disabled = busy;
  row.push_back(later);
  Button go = textButton(failed ? t.tryAgain : t.updateNow, Icon::None,
                         command(MenuCommand::DownloadReleaseUpdate), true);
  go.disabled = busy;
  row.push_back(go);
}

void Menu::drawUpdateDialog(cairo_t* cr, double textX, double boxY) {
  using State = ReleaseUpdate::State;
  const State state = view_.releaseUpdate;
  const ReleaseUpdateStrings& t = text().releaseUpdate;
  const std::string& version = view_.releaseVersion;
  const double textW = kDialogW - 100;
  drawText(cr, t.notice, textX, boxY + 40, textW, 60, 38, true, true, 1, 1, 1);
  drawText(cr, stashPrinted(state == State::Ready ? t.ready : t.offer, version), textX, boxY + 120,
           textW, 50, 26, false, true, 0.75, 0.75, 0.8);
  if (state == State::DownloadFailed)
    drawText(cr, t.downloadFailed, textX, boxY + 180, textW, 44, 26, true, true, 1.0, 0.45, 0.4);
  else if (state == State::Downloading || state == State::Restarting)
    drawText(cr, stashPrinted(state == State::Downloading ? t.downloading : t.restarting, version),
             textX, boxY + 180, textW, 44, 26, true, true, 0.85, 0.85, 0.9);
}

void Menu::layoutVideoSettings() {
  const Strings& t = text();
  const double w = kColumnW - 2 * kGroupPad;

  // A preset's name being typed: alone, over the keyboard.
  if (presetName_.open) {
    layoutPresetName();
    return;
  }
  // On the Frame the foveation rows can be hidden (desktop: nothing here
  // is); the presets' row starts at the left.
  if (kStandaloneHeadset) layoutAdvancedSwitch();
  const double pageTop = layoutPresetRow();
  // Format: projection and 3D layout as tiles, then the screen's switches.
  double x = columnX(0) + kGroupPad, y = firstRow(pageTop);
  constexpr double gap = 8, tileH = 84;
  const double tileW = (w - 2 * gap) / 3;
  auto tile = [&](int i, int row, Icon icon, const char* caption, const char* name, bool chosen,
                  std::function<MenuCommand()> action) {
    Button b;
    b.panel = kBrowser;
    b.x = x + i * (tileW + gap);
    b.y = y + row * (tileH + gap);
    b.w = tileW;
    b.h = tileH;
    b.radius = 14;
    b.icon = icon;
    b.caption = caption;
    b.tooltip = name;
    b.selected = chosen;
    b.action = std::move(action);
    buttons_.push_back(b);
  };
  const Icon projections[] = {Icon::ProjFlat,   Icon::Proj180,    Icon::Proj360,
                              Icon::Fisheye190, Icon::Fisheye200, Icon::Cube,
                              Icon::Fisheye180};
  int slot = 0;
  for (const Projection p : kProjectionOrder) {
    const int i = static_cast<int>(p);
    tile(slot % 3, slot / 3, projections[i], t.projectionShort[i], t.projections[i],
         view_.format.projection == p, command(MenuCommand::SetProjection, i));
    ++slot;
  }
  const Icon layouts[] = {Icon::StereoMono, Icon::StereoSbs, Icon::StereoTb};
  for (int i = 0; i < 3; ++i)
    tile(i, 3, layouts[i], t.stereoShort[i], t.stereo[i],
         static_cast<int>(view_.format.stereo) == i, command(MenuCommand::SetStereo, i));
  y += 4 * (tileH + gap) + 8;
  // Only the flat-screen projection (2D or 3D) has eyes that may be squeezed
  // to half size (switches between half and full packing), and a screen.
  const bool flat = view_.format.projection == Projection::Flat;
  const bool tb = view_.format.stereo == StereoLayout::TopBottom;
  addToggleRow(x, y, w, tb ? t.halfTb : t.halfSbs, view_.halfPacked,
               command(MenuCommand::SetPacking,
                       static_cast<int>(view_.halfPacked ? Packing::Full : Packing::Half)),
               !flat || view_.format.stereo == StereoLayout::Mono)
      .tooltip = t.halfPackedHint;
  addToggleRow(x, y + kSettingStep, w, t.passthrough, view_.passthrough,
               command(MenuCommand::TogglePassthrough))
      .tooltip = t.passthroughHint;
  const double formatBottom = y + 2 * kSettingStep + 10;
  addGroup(columnX(0), pageTop, kColumnW, formatBottom - pageTop, t.format);
  layoutScreenGroup(formatBottom + kGap, !flat);

  // Picture of the running video.
  x = columnX(1) + kGroupPad;
  y = firstRow(pageTop);
  struct Step {
    const char* label;
    std::string value;
    int now, lo, hi, step;
    MenuCommand::Type type;
    bool disabled;
    const char* tooltip = "";
  };
  const int zoom = static_cast<int>(std::lround(view_.zoom * 100));
  const ColourBalance& balance = view_.balance;
  const ColourBalanceStrings& cb = t.colourBalance;
  const Step picture[] = {
      {t.brightness, std::to_string(view_.brightness), view_.brightness, -100, 100, kPictureStep,
       MenuCommand::SetBrightness, false},
      {t.contrast, std::to_string(view_.contrast), view_.contrast, -100, 100, kPictureStep,
       MenuCommand::SetContrast, false},
      {t.saturation, std::to_string(view_.saturation), view_.saturation, -100, 100, kPictureStep,
       MenuCommand::SetSaturation, false},
      {cb.warmth, std::to_string(balance.warmth), balance.warmth, -100, 100, kPictureStep,
       MenuCommand::SetWarmth, false, cb.tipWarmth},
      {cb.tint, std::to_string(balance.tint), balance.tint, -100, 100, kPictureStep,
       MenuCommand::SetTint, false, cb.tipTint},
      {cb.exposure, std::to_string(balance.exposure), balance.exposure, -100, 100, kPictureStep,
       MenuCommand::SetExposure, false, cb.tipExposure},
      // Only 180°/360° video has a view to zoom.
      {t.zoom, std::to_string(zoom) + " %", zoom, 50, 250, kZoomStepPercent, MenuCommand::SetZoom,
       flat}};
  for (const Step& s : picture) {
    addValueRow(x, y, w, 230, s.label, s.value, s.now, s.lo, s.hi,
                command(s.type, std::max(s.lo, s.now - s.step)),
                command(s.type, std::min(s.hi, s.now + s.step)), s.disabled, false, s.tooltip);
    y += kSettingStep;
  }
  {
    const int now = view_.originForward, limit = kMaxViewpointPercent, step = kViewpointStepPercent;
    addValueRow(x, y, w, 230, t.viewpoint.label, viewpointText(now), now, -limit, limit,
                command(MenuCommand::SetOriginForward, std::max(-limit, now - step)),
                command(MenuCommand::SetOriginForward, std::min(limit, now + step)), flat, false,
                t.viewpoint.tip);
    y += kSettingStep;
  }
  Button reset = textButton(t.reset, Icon::None, command(MenuCommand::ResetPicture));
  reset.panel = kBrowser;
  reset.x = x;
  reset.y = y;
  reset.h = kSettingRowH;
  reset.fontSize = kTextSize;
  reset.tooltip = cb.tipReset;
  buttons_.push_back(reset);
  addLabel(reset.x + reset.w + 20, y, w - reset.w - 20, 0, t.pictureHint, kHintSize, true, 2);
  const double pictureBottom = y + hintHeight(2) + 16;
  addGroup(columnX(1), pageTop, kColumnW, pictureBottom - pageTop, t.picture);

  // The image, for all videos.
  x = columnX(2) + kGroupPad;
  y = firstRow(pageTop);
  addToggleRow(x, y, w, t.videoMipmaps, view_.videoMipmaps,
               command(MenuCommand::ToggleVideoMipmaps))
      .tooltip = kStandaloneHeadset ? t.tipMipmapsFrame : t.tipMipmaps;
  y += kSettingStep;
  // Takes effect from the next frame; mipmaps bypass it.
  addChoiceRow(x, y, w, 180, t.antiShimmer,
               {t.antiShimmerLevels[0], t.antiShimmerLevels[1], t.antiShimmerLevels[2]},
               static_cast<int>(view_.antiShimmer),
               [](int i) {
                 MenuCommand c;
                 c.type = MenuCommand::SetAntiShimmer;
                 c.value = i;
                 return c;
               },
               view_.videoMipmaps, view_.videoMipmaps ? t.antiShimmerMipmaps : t.antiShimmerHint);
  if (kStandaloneHeadset && !view_.videoMipmaps)
    for (size_t i = buttons_.size() - 2; i < buttons_.size(); ++i)
      buttons_[i].tooltip = t.tipAntiShimmerFrame;
  y += kSettingStep;
  const char* shimmerHint = view_.videoMipmaps  ? t.antiShimmerMipmaps
                            : kStandaloneHeadset ? t.antiShimmerFrameHint
                                                 : t.antiShimmerHint;
  addLabel(x + 12, y, w - 12, 0, shimmerHint, kHintSize, true, 2);
  y += hintHeight(2) + 6;
  addValueRow(x, y, w, 230, t.sharpen, view_.sharpen > 0 ? fixed2(view_.sharpen / 100.0) : t.off,
              view_.sharpen, 0, 300,
              command(MenuCommand::SetSharpen, std::max(0, view_.sharpen - kSharpenStep)),
              command(MenuCommand::SetSharpen, std::min(300, view_.sharpen + kSharpenStep)),
              false, false, kStandaloneHeadset ? t.tipSharpenFrame : t.tipSharpen);
  y += kSettingStep;
  addToggleRow(x, y, w, t.toneLook, view_.toneLook, command(MenuCommand::ToggleToneLook))
      .tooltip = t.toneLookHint;
  y += kSettingStep;
  // Photos are not drawn by mpv, so its gamma conversion does not reach them.
  addToggleRow(x, y, w, t.tvGamma, view_.tvGamma, command(MenuCommand::ToggleTvGamma), view_.photo)
      .tooltip = t.tvGammaHint;
  y += kSettingStep;
  // The eye images' size (video and menus) as a factor with its pixels
  // across; switched to once the choice settles (main.cpp).
  {
    const int now = view_.videoResolution;
    const MenuCommand::Type type = MenuCommand::SetVideoResolution;
    const auto* stops = view_.resolutionStops;
    const int index = stops
                          ? static_cast<int>(std::lower_bound(stops->begin(), stops->end(), now) - stops->begin())
                          : 0;
    const int last = stops ? static_cast<int>(stops->size()) - 1 : 0;
    addValueRow(x, y, w, 230, t.eyeResolution, "", now, kResolutionLow, kResolutionHigh,
                command(type, stops ? std::max(0, index - 1) : nextResolutionStep(now, -1)),
                command(type, stops ? std::min(last, index + 1) : nextResolutionStep(now, 1)),
                false, false, kStandaloneHeadset ? t.tipResolutionFrame : t.tipResolution);
    Button& field = buttons_.back();
    field.slider = type;
    field.sliderValue = stops ? index : now;
    field.sliderMin = stops ? 0 : kResolutionLow;
    field.sliderMax = stops ? last : kResolutionHigh;
    field.sliderStep = stops ? 1 : kResolutionStep;
    field.sliderText = resolutionText(stops ? now * 100.0 / view_.resolutionBase : now, view_.resolutionBase);
    if (stops) {
      const int normal = kDefaultVideoResolution * kPanelWidth / 100;
      field.sliderDefault = static_cast<int>(
          std::lower_bound(stops->begin(), stops->end(), normal) - stops->begin());
    }
    field.action = [] { return MenuCommand{}; };  // clickable; click() reads the slider
    y += kSettingStep;
  }
  char hint[192];
  bool resolutionStatus = true;
  if (resolutionHeld_)
    std::snprintf(hint, sizeof(hint), "%s", t.resolutionPending);
  else if (view_.resolutionOverride > 0)
    std::snprintf(hint, sizeof(hint), t.resolutionOverride, view_.resolutionOverride);
  else if (view_.videoResolutionEffective < view_.videoResolutionActive)
    std::snprintf(hint, sizeof(hint), t.resolutionLimited, view_.videoResolutionEffective / 100.0);
  else if (kStandaloneHeadset && view_.resolutionNotice)
    std::snprintf(hint, sizeof(hint), t.resolutionChanged, view_.resolutionBase,
                  view_.resolutionStops ? view_.videoResolution / double(view_.resolutionBase)
                                        : view_.videoResolution / 100.0);
  else
    resolutionStatus = false;
  if (resolutionStatus) {
    addLabel(x + 12, y, w - 12, 0, hint, kHintSize, true, 2);
    y += hintHeight(2) + 6;
  }
  if (view_.refreshRate > 0) {
    std::vector<std::string> rates;
    for (const int hz : kRefreshRates) {
      std::snprintf(hint, sizeof(hint), t.refreshRateChoice, hz);
      rates.push_back(hint);
    }
    // A rate set in SteamVR that is none of these: none lit.
    const int chosen = static_cast<int>(std::find(std::begin(kRefreshRates), std::end(kRefreshRates),
                                                  view_.refreshRate) - std::begin(kRefreshRates));
    addChoiceRow(x, y, w, 240, t.refreshRate, rates, chosen,
                 [](int i) {
                   MenuCommand c;
                   c.type = MenuCommand::SetRefreshRate;
                   c.value = kRefreshRates[i];
                   return c;
                 }, false, t.tipRefreshRate);
    y += kSettingStep;
  }
  if (kStandaloneHeadset && view_.advancedSettings) {
    addToggleRow(x, y, w, t.frameReuse, view_.frameReuse, command(MenuCommand::ToggleFrameReuse))
        .tooltip = t.frameReuseHint;
    y += kSettingStep;
  }
  if (kStandaloneHeadset && view_.foveationAvailable) {
    std::vector<std::string> levels{t.foveationLevels[0], t.foveationLevels[1],
                                    t.foveationLevels[2], t.foveationLevels[3]};
    if (view_.foveation == FoveationLevel::Custom) levels.push_back(t.foveationLevels[4]);
    addLabel(x + 12, y, w - 12, kSettingRowH, t.foveation);
    y += kSettingStep;
    addChoiceRow(x, y, w, 0, t.foveation, levels,
                 static_cast<int>(view_.foveation),
                 [](int i) {
                   MenuCommand c;
                   c.type = MenuCommand::SetFoveation;
                   c.value = i;
                   return c;
                 }, false, t.foveationHint);
    const size_t first = buttons_.size() - levels.size();
    buttons_[first].tooltip = t.tipFoveationOffFrame;
    int previous = 0;
    for (int i = 1; i < 4; ++i) {
      int bins, outside;
      bool ring;
      foveationPreset(static_cast<FoveationLevel>(i), bins, ring, outside);
      const int map = view_.foveationMaps[foveationMapIndex(true, bins, ring, outside)];
      if (map == previous) {
        buttons_[first + i].disabled = true;
        std::snprintf(hint, sizeof(hint), t.foveationSame, t.foveationLevels[i - 1]);
        buttons_[first + i].tooltip = hint;
      }
      previous = map;
    }
    if (view_.foveation == FoveationLevel::Custom) buttons_.back().action = {};
    y += kSettingStep;
    addLabel(x + 12, y, w - 12, 0, t.foveationHint, kHintSize, true, 2);
    y += hintHeight(2) + 6;
  }
  if (kStandaloneHeadset && view_.advancedSettings && view_.foveationAvailable) {
    const bool off = view_.foveation == FoveationLevel::Off;
    const auto map = [&](int bins, bool ring, int outside) {
      return view_.foveationMaps[foveationMapIndex(!off, bins, ring, outside)];
    };
    const int current = map(view_.sharpBins, view_.foveationRing, view_.foveationOutside);
    auto unchanged = [&](Button& button, bool same, const char* name) {
      if (!same) return;
      button.disabled = true;
      std::snprintf(hint, sizeof(hint), t.foveationSame, off ? t.off : name);
      button.tooltip = hint;
    };
    const int chosen = view_.sharpBins == 1 ? 0 : 1;
    addChoiceRow(x, y, w, 230, t.sharpArea, {t.sharpBins[0], t.sharpBins[1]}, chosen,
                 [](int i) {
                   MenuCommand c;
                   c.type = MenuCommand::SetSharpBins;
                   c.value = i == 0 ? 1 : 3;
                   return c;
                 }, off, t.tipSharpArea);
    unchanged(buttons_[buttons_.size() - 2 + (1 - chosen)],
              current == map(chosen == 0 ? 3 : 1, view_.foveationRing, view_.foveationOutside),
              t.sharpBins[chosen]);
    y += kSettingStep;
    if (view_.sharpDegrees[0] > 0) {
      std::snprintf(hint, sizeof(hint), t.sharpSizeHint, t.sharpBins[chosen],
                    view_.sharpDegrees[chosen], view_.sharpDegrees[2]);
      addLabel(x + 12, y, w - 12, 0, hint, kHintSize, true);
      y += hintHeight(1);
    }
    Button& ring = addToggleRow(x, y, w, t.foveationRing, view_.foveationRing,
                                command(MenuCommand::ToggleFoveationRing), off);
    ring.tooltip = t.tipFoveationRing;
    unchanged(ring, current == map(view_.sharpBins, !view_.foveationRing, view_.foveationOutside),
              view_.foveationRing ? t.on : t.off);
    y += kSettingStep;
    const int outside = view_.foveationOutside == 2 ? 0 : 1;
    addChoiceRow(x, y, w, 230, t.foveationOutside, {t.foveationRates[0], t.foveationRates[1]}, outside,
                 [](int i) {
                   MenuCommand c;
                   c.type = MenuCommand::SetFoveationOutside;
                   c.value = i == 0 ? 2 : 4;
                   return c;
                 }, off, t.tipFoveationOutside);
    unchanged(buttons_[buttons_.size() - 2 + (1 - outside)],
              current == map(view_.sharpBins, view_.foveationRing, outside == 0 ? 4 : 2),
              t.foveationRates[outside]);
    y += kSettingStep;
    addToggleRow(x, y, w, t.showFoveation, view_.showFoveation,
                 command(MenuCommand::ToggleShowFoveation)).tooltip = t.showFoveationHint;
    y += kSettingStep;
    addToggleRow(x, y, w, t.focus.gazeCorrect, view_.foveationLearned,
                 command(MenuCommand::ToggleFoveationLearned), off).tooltip = t.foveationLearnedHint;
    y += kSettingStep;
  }
  const double imageBottom = y;
  addGroup(columnX(2), pageTop, kColumnW, imageBottom - pageTop, t.imageAllVideos);

  if (!view_.passthrough && !view_.opacityApplies) return;

  // Passthrough: the video's opacity (blended only by the headset itself,
  // with its own camera, below 100 % with passthrough off too, so shown
  // wherever it applies), then with passthrough on removing its background
  // or its own mask, on or off, and while on their settings (the key's
  // sliders only while it removes something), then Reset with Match the
  // room's switch (the Frame), and while that is on its strengths, under
  // the picture. Rows that can't apply are left out.
  const double top = pictureBottom + kGap;
  x = columnX(1) + kGroupPad;
  y = firstRow(top);
  // 0..100, with − and + a step of 1 either side of the slider.
  auto slider = [&](const char* label, MenuCommand::Type type, int value, bool disabled,
                    const char* tooltip, const char* unit = "") {
    addValueRow(x, y, w, 230, label, std::to_string(value) + unit, value, 0, 100,
                command(type, std::max(0, value - 1)), command(type, std::min(100, value + 1)),
                disabled, false, tooltip);
    y += kSettingStep;
  };
  if (view_.opacityApplies)
    slider(t.opacity, MenuCommand::SetOpacity, view_.opacity, false, t.opacityHint);
  const bool own = view_.passthrough && view_.packedAlpha && view_.maskFormat;
  if (view_.passthrough) {
    // Removing the background and the video's own mask do the same, one at
    // a time: while either is on, the other's switch is hidden. Removing's
    // comes first, so its switch stays put as the mask's goes. The mask's
    // shows only for its format (side-by-side fisheye).
    if (!own) {
      addToggleRow(x, y, w, t.greenScreenOn, view_.greenScreen.on,
                   command(MenuCommand::ToggleGreenScreen))
          .tooltip = t.greenScreenHint;
      y += kSettingStep;
    }
    if (view_.maskFormat && (own || !view_.greenScreen.on)) {
      addToggleRow(x, y, w, t.packedMask, view_.packedAlpha, command(MenuCommand::TogglePackedMask))
          .tooltip = t.packedMaskHint;
      y += kSettingStep;
    }
    if (own && view_.maskLayers) {
      addToggleRow(x, y, w, t.secondLayer, !view_.secondLayerHidden,
                   command(MenuCommand::ToggleSecondLayer))
          .tooltip = t.secondLayerHint;
      y += kSettingStep;
    }
    if (!own && view_.greenScreen.on) {
      layoutKeyBackground(x, y, w);
      y += kSettingStep;
    }
    if (!own && view_.greenScreen.on && keyRemoves(view_)) {
      const MenuCommand::Type types[] = {MenuCommand::SetKeyStrength, MenuCommand::SetKeyShrink,
                                         MenuCommand::SetKeySoftness, MenuCommand::SetKeySpill};
      const KeySliders& key = view_.greenScreen.sliders(view_.colourKey);
      const int values[] = {key.strength, key.shrink, key.softness, key.spill};
      const char* names[] = {t.keyStrength, t.keyShrink, t.keySoftness, t.keySpill};
      for (int i = 0; i < 4; ++i) slider(names[i], types[i], values[i], false, t.tipKey[i]);
    }
  }
  // How far Match the room goes, under its switch while it is on.
  const bool strengths = view_.roomMatch != RoomMatch::Hidden && view_.roomMatched;
  Button resetKey = textButton(t.reset, Icon::None, command(MenuCommand::ResetPassthrough));
  resetKey.panel = kBrowser;
  resetKey.x = x;
  resetKey.y = y;
  resetKey.h = kSettingRowH;
  resetKey.fontSize = kTextSize;
  resetKey.tooltip = t.greenScreenResetHint;
  const bool keyOn = view_.passthrough && !own && view_.greenScreen.on;
  const bool keyShown = keyOn && keyRemoves(view_);
  resetKey.disabled = !keyShown && !view_.opacityApplies && !strengths;
  // With the key on removing nothing, Reset shows only for what else it resets.
  const bool resetShown = !(keyOn && resetKey.disabled);
  if (resetShown) buttons_.push_back(resetKey);
  // Match the room (the Frame), a switch, on the same row, and while it is
  // on Read again after it: then there is no room for the own mask's hint
  // (the mask's switch, on, says it). The switch's label says when it reads
  // or failed.
  const RoomMatch room = view_.roomMatch;
  const bool ownHint = own && room == RoomMatch::Hidden;
  if (room != RoomMatch::Hidden) {
    const bool again = view_.roomMatched && (room == RoomMatch::Ready || room == RoomMatch::Off);
    double right = x + w;
    if (again) {
      Button read = iconButton(Icon::Refresh, command(MenuCommand::ReadRoomAgain));
      read.panel = kBrowser;
      read.w = read.h = kSettingRowH;
      read.x = right - read.w;
      read.y = y;
      read.disabled = room == RoomMatch::Off;
      read.tooltip = room == RoomMatch::Off ? cb.tipMatchRoomHidden : cb.readAgain;
      buttons_.push_back(read);
      right = read.x - 8;
    }
    const char* label = room == RoomMatch::Reading  ? cb.reading
                        : room == RoomMatch::Failed ? cb.failed
                                                    : cb.matchRoom;
    // Its label and pill, but never into Reset (a longer label is cut short).
    const double left = std::max(right - (textWidth(label, kTextSize, false) + 104),
                                 resetShown ? resetKey.x + resetKey.w + 12 : x);
    Button& match = addToggleRow(left, y, right - left, label, view_.roomMatched,
                                 command(MenuCommand::ToggleMatchRoom),
                                 room == RoomMatch::Off && !view_.roomMatched && !view_.roomRead);
    match.tooltip = match.disabled ? cb.tipMatchRoomHidden : cb.tipMatchRoom;
  }
  if (ownHint)
    addLabel(resetKey.x + resetKey.w + 20, y, w - resetKey.w - 20, 0, t.greenScreenOwnMask,
             kHintSize, true, 2);
  if (resetShown || room != RoomMatch::Hidden) y += ownHint ? hintHeight(2) + 6 : kSettingStep;
  if (strengths) {
    const int share[] = {view_.matchColour, view_.matchBrightness};
    const MenuCommand::Type types[] = {MenuCommand::SetMatchColour, MenuCommand::SetMatchBrightness};
    const char* labels[] = {cb.matchColour, cb.matchBrightness};
    const char* tips[] = {cb.tipMatchColour, cb.tipMatchBrightness};
    for (int i = 0; i < 2; ++i) slider(labels[i], types[i], share[i], false, tips[i], " %");
  }
  addGroup(columnX(1), top, kColumnW, y + 10 - top, t.passthroughGroup);
}

// The background key's row: the choice, and in a swatch before it what it
// removes now: the colour keyed, the green keyer's greens, or for Automatic
// with nothing detected a crossed-out circle (nothing removed); pressed,
// the choices: Automatic, Green screen, or (surround video, where it can be
// pointed at) picking a colour (the app takes over until one is).
void Menu::layoutKeyBackground(double x, double y, double w) {
  const Strings& t = text();
  const GreenScreen& key = view_.greenScreen;
  constexpr double labelW = 230;
  addLabel(x + 12, y, labelW - 12, kSettingRowH, t.keyBackground);
  // Automatic: the colour the check found, else the green screen it found,
  // else nothing; while it looks with nothing found yet, an empty swatch
  // saying so.
  const bool checking = !keyRemoves(view_) && !view_.fillChecked && view_.fillChecking;
  const char* state = key.source == KeySource::Picked      ? t.keyPicked
                      : key.source == KeySource::GreenScreen ? t.keySources[1]
                      : checking                             ? t.keyChecking
                                                             : t.keySources[0];
  Button opener = textButton(std::string(state) + " ▾", Icon::None, [this] {
    dropdown_ = dropdown_ == Dropdown::KeyBackground ? Dropdown::None : Dropdown::KeyBackground;
    return MenuCommand{};
  });
  opener.panel = kBrowser;
  opener.bare = opener.on = true;
  opener.alignLeft = true;
  opener.keySwatch = view_.colourKey ? (view_.keyColour.r << 16 | view_.keyColour.g << 8 |
                                        view_.keyColour.b)
                     : checking        ? kPendingKeySwatch
                     : !keyRemoves(view_) ? kNoKeySwatch
                                          : kGreenKeySwatch;
  opener.x = x + labelW;
  opener.y = y;
  opener.w = w - labelW;
  opener.h = kSettingRowH;
  opener.tooltip = t.keyBackgroundHint;
  buttons_.push_back(opener);
  if (dropdown_ != Dropdown::KeyBackground) return;
  std::vector<Button> entries;
  const KeySource sources[] = {KeySource::Automatic, KeySource::GreenScreen, KeySource::Picked};
  const int choices = view_.format.projection == Projection::Flat ? 2 : 3;
  for (int i = 0; i < choices; ++i) {
    entries.push_back(textButton(t.keySources[i], Icon::None, [this, i, sources] {
      dropdown_ = Dropdown::None;
      if (sources[i] == KeySource::Picked) return command(MenuCommand::PickKeyColour)();
      return command(MenuCommand::SetKeyBackground, static_cast<int>(sources[i]))();
    }, key.source == sources[i]));
    entries.back().tooltip = t.keySourceHints[i];
  }
  addDropdownBelow(opener, std::move(entries), true);
}

void Menu::layoutScreenGroup(double top, bool disabled) {
  const Strings& t = text();
  const double w = kColumnW - 2 * kGroupPad;
  const double x = columnX(0) + kGroupPad;
  double y = firstRow(top);
  const ScreenView& s = view_.screen;
  auto hundredths = [](float v) { return static_cast<int>(std::lround(v * 100)); };
  // Screen controls: distance and height in centimetres, and the curve
  // from 0 (flat) to 1 (bent around you).
  struct Step {
    const char* label;
    std::string value;
    int now, lo, hi, step;
    MenuCommand::Type type;
  };
  const Step steps[] = {
      {t.screenHeight, std::to_string(s.heightCm) + " cm", s.heightCm,
       hundredths(ScreenSize::kMinHeight), hundredths(ScreenSize::kMaxHeight), kScreenStepCm,
       MenuCommand::SetScreenHeight},
      {t.screenDistance, std::to_string(s.distanceCm) + " cm", s.distanceCm,
       hundredths(ScreenSize::kMinDistance), hundredths(ScreenSize::kMaxDistance), kScreenStepCm,
       MenuCommand::SetScreenDistance},
      {t.screenCurve, fixed2(s.curvePercent / 100.0), s.curvePercent, 0,
       hundredths(ScreenSize::kMaxCurve), kCurveStepPercent, MenuCommand::SetScreenCurve}};
  for (const Step& r : steps) {
    addValueRow(x, y, w, 230, r.label, r.value, r.now, r.lo, r.hi,
                command(r.type, std::max(r.lo, r.now - r.step)),
                command(r.type, std::min(r.hi, r.now + r.step)), disabled, false,
                r.type == MenuCommand::SetScreenHeight ? t.tipScreenHeight
                : r.type == MenuCommand::SetScreenCurve ? t.tipScreenCurve : "");
    y += kSettingStep;
  }
  Button reset = textButton(t.reset, Icon::None, command(MenuCommand::ResetScreen));
  reset.panel = kBrowser;
  reset.x = x;
  reset.y = y;
  reset.h = kSettingRowH;
  reset.fontSize = kTextSize;
  reset.disabled = disabled || s == ScreenView::of(ScreenSize{});
  reset.tooltip = t.screenResetHint;
  buttons_.push_back(reset);
  y += kSettingStep;
  addLabel(x + 12, y, w - 12, 0, t.screenHint, kHintSize, true, 3).disabled = disabled;
  addGroup(columnX(0), top, kColumnW, y + hintHeight(3) + 6 - top, t.groupScreen);
}

// The settings pages: they share the browser's Settings source.
bool Menu::settingsShown() const { return page_ == Page::Settings; }
