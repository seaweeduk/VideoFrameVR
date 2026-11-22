#include "menu_parts.h"

#include "eye_resolution.h"
#include "foveation_level.h"
#include "refresh_rate.h"

#include <cmath>
#include <iterator>

// Video settings: a page in two sections, built from the menu's parts.
//   Display: the presets, the flat screen, the image, performance and
//   (on the Frame) foveation, for all videos.
//   This video: its format, its picture and passthrough, saved with it;
//   greyed out while no video is open.
// Rows that can't apply are greyed out, or left out where they would only
// say so; the page never scrolls (the debug fit check says where it would).

using namespace value_range;

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

MenuCommand valued(MenuCommand::Type type, int value) {
  MenuCommand c;
  c.type = type;
  c.value = value;
  return c;
}

}  // namespace

Menu::ResolutionStatus Menu::resolutionStatus(const MenuView& view) {
  if (view.resolutionOverride > 0) return ResolutionStatus::Override;
  if (view.videoResolutionEffective < view.videoResolutionActive) return ResolutionStatus::Limited;
  if (kStandaloneHeadset && view.resolutionNotice) return ResolutionStatus::Changed;
  return ResolutionStatus::None;
}

std::string Menu::resolutionNote(const MenuView& view) const {
  const Strings& t = text();
  char line[192] = "";
  switch (resolutionStatus(view)) {
    case ResolutionStatus::None: break;
    case ResolutionStatus::Override:
      std::snprintf(line, sizeof(line), t.resolutionOverride, view.resolutionOverride);
      break;
    case ResolutionStatus::Limited:
      std::snprintf(line, sizeof(line), t.resolutionLimited, view.videoResolutionEffective / 100.0);
      break;
    case ResolutionStatus::Changed:
      std::snprintf(line, sizeof(line), t.resolutionChanged, view.resolutionBase,
                    view.resolutionStops ? view.videoResolution / double(view.resolutionBase)
                                         : view.videoResolution / 100.0);
      break;
  }
  return line;
}

int Menu::resolutionNoteLines(const MenuView& view) const {
  const std::string note = resolutionNote(view);
  return note.empty() ? 0 : wrappedLines(note, PageLayout::kNoteW, kit::kNoteSize, kResolutionNoteLines);
}

void Menu::layoutVideoSettings() {
#ifndef NDEBUG
  if (fitChecked_ != &text()) checkVideoSettingsFit();
#endif
  presetBox_[2] = 0;
  // A preset's name being typed: alone, over the keyboard.
  if (presetName_.open) {
    layoutPresetName();
    return;
  }
  layoutVideoSection(videoSection_);
}

void Menu::layoutVideoSection(VideoSection section) {
  const Strings& t = text();
  // The player keeps a closed video's title, so the header follows what greys
  // out This video.
  const bool open = !view_.currentFile.empty();
  PageLayout page(*this, t.videoSettings, open ? view_.playback.title : std::string());
  page.sections({t.display, t.thisVideo}, static_cast<int>(section), [this](int i) {
    videoSection_ = videoSectionPicked_ = static_cast<VideoSection>(i);
    dropdown_ = Dropdown::None;
    dirty_ = true;
  });
  if (!open)
    page.disableSection(static_cast<int>(VideoSection::ThisVideo), t.thisVideoClosed);
  // On the Frame Display's advanced rows can be hidden (desktop: nothing here
  // is, nor in This video).
  if (kStandaloneHeadset && section == VideoSection::Display)
    page.pageSwitch(t.advancedSettings, view_.advancedSettings,
                    command(MenuCommand::ToggleAdvancedSettings), t.tipAdvanced);
  const bool flat = view_.format.projection == Projection::Flat;
  const ColourBalanceStrings& cb = t.colourBalance;

  if (section == VideoSection::ThisVideo) {
    // Format: projection and 3D layout as tiles, then whether flat 3D's eyes
    // are squeezed to half size (only it has eyes that may be).
    {
      GroupLayout format = page.group(0, t.format);
      const Icon projections[] = {Icon::ProjFlat,   Icon::Proj180,    Icon::Proj360,
                                  Icon::Fisheye190, Icon::Fisheye200, Icon::Cube,
                                  Icon::Fisheye180};
      std::vector<Tile> tiles;
      for (const Projection p : kProjectionOrder) {
        const int i = static_cast<int>(p);
        tiles.push_back({projections[i], t.projectionShort[i], {}, t.projections[i],
                         view_.format.projection == p, command(MenuCommand::SetProjection, i)});
      }
      format.tiles(4, kit::kTileMax, tiles);
      const Icon layouts[] = {Icon::StereoMono, Icon::StereoSbs, Icon::StereoTb};
      tiles.clear();
      for (int i = 0; i < 3; ++i)
        tiles.push_back({layouts[i], t.stereoShort[i], {}, t.stereo[i],
                         static_cast<int>(view_.format.stereo) == i,
                         command(MenuCommand::SetStereo, i)});
      format.tiles(3, kit::kTileMax, tiles);
      const bool tb = view_.format.stereo == StereoLayout::TopBottom;
      format
          .toggle(tb ? t.halfTb : t.halfSbs, view_.halfPacked,
                  command(MenuCommand::SetPacking,
                          static_cast<int>(view_.halfPacked ? Packing::Full : Packing::Half)))
          .tip(t.halfPackedHint)
          .disable(!flat || view_.format.stereo == StereoLayout::Mono);
    }

    // Picture of the running video. Only 180°/360° video has a view to zoom
    // and a viewpoint to move.
    {
      GroupLayout picture =
          page.group(1, t.picture, {t.reset, cb.tipReset, command(MenuCommand::ResetPicture)});
      const ColourBalance& balance = view_.balance;
      struct Value {
        const char* label;
        int now;
        MenuCommand::Type type;
        const char* tooltip;
      };
      const Value values[] = {
          {t.brightness, view_.brightness, MenuCommand::SetBrightness, ""},
          {t.contrast, view_.contrast, MenuCommand::SetContrast, ""},
          {t.saturation, view_.saturation, MenuCommand::SetSaturation, ""},
          {cb.warmth, balance.warmth, MenuCommand::SetWarmth, cb.tipWarmth},
          {cb.tint, balance.tint, MenuCommand::SetTint, cb.tipTint},
          {cb.exposure, balance.exposure, MenuCommand::SetExposure, cb.tipExposure}};
      for (const Value& v : values)
        picture.value(v.label, v.type, v.now, kPicture.least, kPicture.most, std::to_string(v.now), kPicture.step)
            .tip(v.tooltip);
      const int zoom = static_cast<int>(std::lround(view_.zoom * 100));
      picture
          .value(t.zoom, MenuCommand::SetZoom, zoom, kZoom.least, kZoom.most,
                 std::to_string(zoom) + " %", kZoom.step)
          .format(withUnit(" %"))
          .mark(100)
          .disable(flat);
      picture
          .value(t.viewpoint.label, MenuCommand::SetOriginForward, view_.originForward,
                 kViewpoint.least, kViewpoint.most, viewpointText(view_.originForward),
                 kViewpoint.step)
          .format([](int v, bool tick) { return tick ? std::to_string(v) : viewpointText(v); })
          .tip(t.viewpoint.tip)
          .disable(flat);
    }

    layoutPassthroughGroup(page, 2);
    return;
  }

  // Display: the presets, then the flat screen.
  layoutPresetGroup(page, 0);
  {
    const ScreenView& s = view_.screen;
    GroupLayout screen = page.group(
        0, t.groupScreen,
        {t.reset, t.screenResetHint, command(MenuCommand::ResetScreen),
         !flat || s == ScreenView::of(ScreenSize{})});
    // Height and distance in centimetres, and the curve from 0 (flat) to 1
    // (bent around you).
    screen
        .value(t.screenHeight, MenuCommand::SetScreenHeight, s.heightCm, kScreenHeight.least,
               kScreenHeight.most, std::to_string(s.heightCm) + " cm", kScreenHeight.step)
        .format(withUnit(" cm"))
        .tip(t.tipScreenHeight)
        .disable(!flat);
    screen
        .value(t.screenDistance, MenuCommand::SetScreenDistance, s.distanceCm,
               kScreenDistance.least, kScreenDistance.most, std::to_string(s.distanceCm) + " cm",
               kScreenDistance.step)
        .format(withUnit(" cm"))
        .disable(!flat);
    screen
        .value(t.screenCurve, MenuCommand::SetScreenCurve, s.curvePercent, kScreenCurve.least,
               kScreenCurve.most, fixed2(s.curvePercent / 100.0), kScreenCurve.step)
        .format([](int v, bool) { return fixed2(v / 100.0); })
        .tip(t.tipScreenCurve)
        .disable(!flat);
    screen.note(t.screenHint).disable(!flat);
  }

  // The image.
  {
    GroupLayout image = page.group(1, t.image);
    image
        .toggle(t.videoMipmaps, view_.videoMipmaps, command(MenuCommand::ToggleVideoMipmaps))
        .tip(kStandaloneHeadset ? t.tipMipmapsFrame : t.tipMipmaps);
    // Takes effect from the next frame; mipmaps bypass it.
    image
        .choice(t.antiShimmer,
                {t.antiShimmerLevels[0], t.antiShimmerLevels[1], t.antiShimmerLevels[2]},
                static_cast<int>(view_.antiShimmer),
                [](int i) { return valued(MenuCommand::SetAntiShimmer, i); })
        .tip(view_.videoMipmaps         ? t.antiShimmerMipmaps
             : kStandaloneHeadset ? t.tipAntiShimmerFrame
                                  : t.antiShimmerHint)
        .disable(view_.videoMipmaps);
    image.note(view_.videoMipmaps  ? t.antiShimmerMipmaps
               : kStandaloneHeadset ? t.antiShimmerFrameHint
                                    : t.antiShimmerHint);
    image
        .value(t.sharpen, MenuCommand::SetSharpen, view_.sharpen, kSharpen.least, kSharpen.most,
               view_.sharpen > 0 ? fixed2(view_.sharpen / 100.0) : t.off, kSharpen.step)
        .format([off = std::string(t.off)](int v, bool tick) {
          return v > 0 || tick ? fixed2(v / 100.0) : off;
        })
        .tip(kStandaloneHeadset ? t.tipSharpenFrame : t.tipSharpen);
    image.toggle(t.toneLook, view_.toneLook, command(MenuCommand::ToggleToneLook))
        .tip(t.toneLookHint);
    // Photos are not drawn by mpv, so its gamma conversion does not reach them.
    image.toggle(t.tvGamma, view_.tvGamma, command(MenuCommand::ToggleTvGamma))
        .tip(t.tvGammaHint)
        .disable(view_.photo);
  }

  // Performance: the eye images' size (video and menus) as a factor with its
  // pixels across, switched to once the choice settles (main.cpp), and what
  // it says about it; the refresh rate; on the Frame, repeated frames.
  {
    GroupLayout performance = page.group(2, t.performance);
    const int now = view_.videoResolution;
    const MenuCommand::Type type = MenuCommand::SetVideoResolution;
    const auto* stops = view_.resolutionStops;
    const int index =
        stops ? static_cast<int>(std::lower_bound(stops->begin(), stops->end(), now) - stops->begin())
              : 0;
    const int last = stops ? static_cast<int>(stops->size()) - 1 : 0;
    Row resolution = performance.value(
        t.eyeResolution, type, stops ? index : now, stops ? 0 : kResolutionLow,
        stops ? last : kResolutionHigh,
        resolutionText(stops ? now * 100.0 / view_.resolutionBase : now, view_.resolutionBase),
        stops ? 1 : kResolutionStep,
        command(type, stops ? std::max(0, index - 1) : nextResolutionStep(now, -1)),
        command(type, stops ? std::min(last, index + 1) : nextResolutionStep(now, 1)));
    resolution.tip(kStandaloneHeadset ? t.tipResolutionFrame : t.tipResolution);
    resolution.format([stops, base = view_.resolutionBase](int v, bool tick) {
      const double percent =
          stops ? (*stops)[std::clamp<size_t>(v, 0, stops->size() - 1)] * 100.0 / base : v;
      return tick ? fixed2(percent / 100.0) + "×" : resolutionText(percent, base);
    });
    if (stops) {
      const int normal = kDefaultVideoResolution * kPanelWidth / 100;
      resolution.mark(static_cast<int>(std::lower_bound(stops->begin(), stops->end(), normal) -
                                       stops->begin()));
    }
    // Only with something to say: its coming or going, or a line more or
    // less, redraws the page (Menu::update()).
    const std::string note = resolutionNote(view_);
    if (!note.empty()) performance.note(note, true, kResolutionNoteLines);
    if (view_.refreshRate > 0) {
      std::vector<std::string> rates;
      char rate[64];
      for (const int hz : kRefreshRates) {
        std::snprintf(rate, sizeof(rate), t.refreshRateChoice, hz);
        rates.push_back(rate);
      }
      // A rate set in SteamVR that is none of these: none lit.
      const int chosen = static_cast<int>(
          std::find(std::begin(kRefreshRates), std::end(kRefreshRates), view_.refreshRate) -
          std::begin(kRefreshRates));
      performance
          .choice(t.refreshRate, rates, chosen,
                  [](int i) { return valued(MenuCommand::SetRefreshRate, kRefreshRates[i]); })
          .tip(t.tipRefreshRate);
    }
    if (kStandaloneHeadset && view_.advancedSettings)
      performance.toggle(t.frameReuse, view_.frameReuse, command(MenuCommand::ToggleFrameReuse))
          .tip(t.frameReuseHint);
  }

  if (kStandaloneHeadset && view_.foveationAvailable) layoutFoveationGroup(page, 2);
}

// Foveation (the Frame): its level, and with Advanced settings on how it is
// made. A choice that would give the same map as the one before it (or as
// now) is greyed out, its tooltip saying so.
void Menu::layoutFoveationGroup(PageLayout& page, int column) {
  const Strings& t = text();
  char hint[192];
  GroupLayout foveation = page.group(column, t.foveation);
  std::vector<std::string> levels{t.foveationLevels[0], t.foveationLevels[1],
                                  t.foveationLevels[2], t.foveationLevels[3]};
  const bool custom = view_.foveation == FoveationLevel::Custom;
  if (custom) levels.push_back(t.foveationLevels[4]);
  Row level = foveation.choice({}, levels, static_cast<int>(view_.foveation),
                               [](int i) { return valued(MenuCommand::SetFoveation, i); });
  level.tip(t.foveationHint);
  level.piece(0).tooltip = t.tipFoveationOffFrame;
  int previous = 0;
  for (int i = 1; i < 4; ++i) {
    int bins, outside;
    bool ring;
    foveationPreset(static_cast<FoveationLevel>(i), bins, ring, outside);
    const int map = view_.foveationMaps[foveationMapIndex(true, bins, ring, outside)];
    if (map == previous) {
      level.piece(i).disabled = true;
      std::snprintf(hint, sizeof(hint), t.foveationSame, t.foveationLevels[i - 1]);
      level.piece(i).tooltip = hint;
    }
    previous = map;
  }
  if (custom) level.piece(4).action = {};
  foveation.note(t.foveationHint);
  if (!view_.advancedSettings) return;

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
  Row area = foveation.choice(t.sharpArea, {t.sharpBins[0], t.sharpBins[1]}, chosen, [](int i) {
    return valued(MenuCommand::SetSharpBins, i == 0 ? 1 : 3);
  });
  area.tip(t.tipSharpArea).disable(off);
  unchanged(area.piece(1 - chosen),
            current == map(chosen == 0 ? 3 : 1, view_.foveationRing, view_.foveationOutside),
            t.sharpBins[chosen]);
  if (view_.sharpDegrees[0] > 0) {
    std::snprintf(hint, sizeof(hint), t.sharpSizeHint, t.sharpBins[chosen],
                  view_.sharpDegrees[chosen], view_.sharpDegrees[2]);
    foveation.note(hint, true, 1);
  }
  Row ring = foveation.toggle(t.foveationRing, view_.foveationRing,
                              command(MenuCommand::ToggleFoveationRing));
  ring.tip(t.tipFoveationRing).disable(off);
  unchanged(ring.control(),
            current == map(view_.sharpBins, !view_.foveationRing, view_.foveationOutside),
            view_.foveationRing ? t.on : t.off);
  const int outside = view_.foveationOutside == 2 ? 0 : 1;
  Row rates = foveation.choice(t.foveationOutside, {t.foveationRates[0], t.foveationRates[1]},
                               outside, [](int i) {
                                 return valued(MenuCommand::SetFoveationOutside, i == 0 ? 2 : 4);
                               });
  rates.tip(t.tipFoveationOutside).disable(off);
  unchanged(rates.piece(1 - outside),
            current == map(view_.sharpBins, view_.foveationRing, outside == 0 ? 4 : 2),
            t.foveationRates[outside]);
  foveation.toggle(t.showFoveation, view_.showFoveation, command(MenuCommand::ToggleShowFoveation))
      .tip(t.showFoveationHint);
  foveation
      .toggle(t.focus.gazeCorrect, view_.foveationLearned,
              command(MenuCommand::ToggleFoveationLearned))
      .tip(t.foveationLearnedHint)
      .disable(off);
}

// Passthrough: its switch, and while it is on (or the headset blends the
// video over the room itself, below 100 % opacity, with passthrough off
// too) the video's opacity, removing its background or its own mask, one
// at a time, and their settings (the key's sliders only while it removes
// something), then Match the room (the Frame) and how far it goes. Rows
// that can't apply are left out.
void Menu::layoutPassthroughGroup(PageLayout& page, int column) {
  const Strings& t = text();
  const ColourBalanceStrings& cb = t.colourBalance;
  const bool rows = view_.passthrough || view_.opacityApplies;
  const bool own = view_.passthrough && view_.packedAlpha && view_.maskFormat;
  const bool keyOn = view_.passthrough && !own && view_.greenScreen.on;
  const bool keyShown = keyOn && keyRemoves(view_);
  // Match the room's strengths, under its switch while it is on and applies
  // (the room is shown).
  const bool strengths = view_.roomMatch != RoomMatch::Hidden &&
                         view_.roomMatch != RoomMatch::Off && view_.roomMatched;
  // Reset, only while there is something it resets.
  Action reset;
  if (rows && (keyShown || view_.opacityApplies || strengths))
    reset = {t.reset, t.greenScreenResetHint, command(MenuCommand::ResetPassthrough)};
  GroupLayout group = page.group(column, t.passthroughGroup, reset);
  Row on = group.toggle(t.passthrough, view_.passthrough, command(MenuCommand::TogglePassthrough));
  if (view_.passthrough) on.tip(t.passthroughHint);
  // Nothing shows the room: the runtime doesn't blend, and no key green.
  const bool available = view_.passthroughAvailable;
  if (!available) on.disable().tip(t.autoPassthrough.unavailable);
  if (!rows) {
    group.note(available ? t.passthroughHint : t.autoPassthrough.unavailable);
    return;
  }
  // 0..100, with − and + a step of 1.
  auto slider = [&](const char* label, MenuCommand::Type type, int value, const char* tooltip,
                    const char* unit = "") {
    group.value(label, type, value, kPercent.least, kPercent.most, std::to_string(value) + unit,
                kPercent.step)
        .format(withUnit(unit))
        .tip(tooltip);
  };
  if (view_.opacityApplies) slider(t.opacity, MenuCommand::SetOpacity, view_.opacity, t.opacityHint);
  if (view_.passthrough) {
    // Removing the background and the video's own mask do the same, one at
    // a time: while either is on, the other's switch is hidden. Removing's
    // comes first, so its switch stays put as the mask's goes. The mask's
    // shows only for its format (side-by-side fisheye).
    if (!own)
      group.toggle(t.greenScreenOn, view_.greenScreen.on, command(MenuCommand::ToggleGreenScreen))
          .tip(t.greenScreenHint);
    if (view_.maskFormat && (own || !view_.greenScreen.on))
      group.toggle(t.packedMask, view_.packedAlpha, command(MenuCommand::TogglePackedMask))
          .tip(t.packedMaskHint);
    if (own && view_.maskLayers)
      group.toggle(t.secondLayer, !view_.secondLayerHidden, command(MenuCommand::ToggleSecondLayer))
          .tip(t.secondLayerHint);
    if (own) group.note(t.greenScreenOwnMask, true);
    if (keyOn) layoutKeyBackground(group);
    if (keyShown) {
      const MenuCommand::Type types[] = {MenuCommand::SetKeyStrength, MenuCommand::SetKeyShrink,
                                         MenuCommand::SetKeySoftness, MenuCommand::SetKeySpill};
      const KeySliders& key = view_.greenScreen.sliders(view_.colourKey);
      const int values[] = {key.strength, key.shrink, key.softness, key.spill};
      const char* names[] = {t.keyStrength, t.keyShrink, t.keySoftness, t.keySpill};
      for (int i = 0; i < 4; ++i) slider(names[i], types[i], values[i], t.tipKey[i]);
    }
  }
  // Match the room (the Frame): its switch, whose label says when it reads
  // or failed, and while it is on Read again in its row.
  const RoomMatch room = view_.roomMatch;
  if (room != RoomMatch::Hidden) {
    const char* label = room == RoomMatch::Reading  ? cb.reading
                        : room == RoomMatch::Failed ? cb.failed
                                                    : cb.matchRoom;
    // A passthrough setting: with the room hidden it doesn't apply, and shows
    // as set for when the room is shown again.
    const bool hidden = room == RoomMatch::Off;
    Row match = group.toggle(label, view_.roomMatched, command(MenuCommand::ToggleMatchRoom));
    match.tip(hidden ? cb.tipMatchRoomHidden : cb.tipMatchRoom).disable(hidden);
    if (view_.roomMatched && (room == RoomMatch::Ready || room == RoomMatch::Off))
      match.inner(Icon::Refresh, room == RoomMatch::Off ? cb.tipMatchRoomHidden : cb.readAgain,
                  command(MenuCommand::ReadRoomAgain), room == RoomMatch::Off);
  }
  if (strengths) {
    slider(cb.matchColour, MenuCommand::SetMatchColour, view_.matchColour, cb.tipMatchColour, " %");
    slider(cb.matchBrightness, MenuCommand::SetMatchBrightness, view_.matchBrightness,
           cb.tipMatchBrightness, " %");
  }
}

// The background key's row: what it removes now, chosen from a list. A
// swatch before its name: the colour keyed, the green keyer's greens, or
// for Automatic with nothing detected a crossed-out circle (nothing
// removed); while it looks with nothing found yet, an empty ring. The
// list: Automatic, Green screen, or (surround video, where it can be
// pointed at) picking a colour (the app takes over until one is).
void Menu::layoutKeyBackground(GroupLayout& group) {
  const Strings& t = text();
  const GreenScreen& key = view_.greenScreen;
  const bool checking = !keyRemoves(view_) && !view_.fillChecked && view_.fillChecking;
  const char* state = key.source == KeySource::Picked        ? t.keyPicked
                      : key.source == KeySource::GreenScreen ? t.keySources[1]
                      : checking                             ? t.keyChecking
                                                             : t.keySources[0];
  const bool open = dropdown_ == Dropdown::KeyBackground;
  Row row = group.select(t.keyBackground, state, [this] {
    dropdown_ = dropdown_ == Dropdown::KeyBackground ? Dropdown::None : Dropdown::KeyBackground;
    return MenuCommand{};
  }, open);
  row.tip(t.keyBackgroundHint);
  Button& opener = row.control();
  opener.keySwatch = view_.colourKey ? (view_.keyColour.r << 16 | view_.keyColour.g << 8 |
                                        view_.keyColour.b)
                     : checking           ? kPendingKeySwatch
                     : !keyRemoves(view_) ? kNoKeySwatch
                                          : kGreenKeySwatch;
  if (!open) return;
  const Button below = opener;
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
  addDropdownBelow(below, std::move(entries), true);
}

// Both sections with every row that can show at once (the ones that exclude
// each other: the longer), laid out and taken away again.
double Menu::videoSettingsBottom(bool advanced, bool check) {
  const MenuView saved = view_;
  const Dropdown dropdown = dropdown_;
  const PageHead head = pageHead_;
  const double top = pageTop_;
  const size_t buttons = buttons_.size(), groups = groups_.size();
  static const std::vector<UserPreset> one(1);
  static const std::vector<int> stops{2304, 2688, 3072, 3456, 3780, 4096, 4480};

  // A new row that shows only in some state: switch that state on here too.
  // Forgotten, the panel grows to fit the first time the row shows
  // (layoutBrowser()) and stays that tall.
  MenuView& v = view_;
  v.format.projection = Projection::Flat;
  v.format.stereo = StereoLayout::SideBySide;
  // This video: passthrough on, its opacity, the key removing a colour,
  // Match the room on with Read again.
  v.passthrough = v.opacityApplies = true;
  v.packedAlpha = v.maskFormat = false;
  v.greenScreen.on = true;
  v.greenScreen.source = KeySource::Picked;
  v.roomMatch = RoomMatch::Ready;
  v.roomMatched = true;
  // Display: Custom, the user's presets listed (My presets, under Save as
  // where it doesn't fit beside it), the longest resolution note, every
  // rate, foveation in Custom.
  v.performancePreset = PerformancePreset::Custom;
  v.userPresets = &one;
  v.userPresetsListed = 1;
  v.userPresetShown = -1;
  v.resolutionStops = &stops;
  v.resolutionBase = 2160;
  v.videoResolution = 3780;
  v.resolutionOverride = 4480;
  v.refreshRate = kRefreshRates[0];
  v.advancedSettings = advanced;
  v.foveationAvailable = true;
  v.foveation = FoveationLevel::Custom;
  v.sharpDegrees = {20, 34, 110};
  dropdown_ = Dropdown::None;
  pageTop_ = 0;
#ifndef NDEBUG
  if (check) fitCase_ = " (This video, every row)";
#endif
  layoutVideoSection(VideoSection::ThisVideo);
#ifndef NDEBUG
  if (check) fitCase_ = " (Display, every row)";
#endif
  layoutVideoSection(VideoSection::Display);
#ifndef NDEBUG
  fitCase_ = nullptr;
#endif
  (void)check;
  double bottom = 0;
  for (size_t i = groups; i < groups_.size(); ++i) bottom = std::max(bottom, groups_[i].y + groups_[i].h);

  buttons_.erase(buttons_.begin() + static_cast<std::ptrdiff_t>(buttons), buttons_.end());
  groups_.erase(groups_.begin() + static_cast<std::ptrdiff_t>(groups), groups_.end());
  pageHead_ = head;
  pageTop_ = top;
  view_ = saved;
  dropdown_ = dropdown;
  return bottom;
}

#ifndef NDEBUG
// Says loudly where Video settings with every row doesn't fit (Advanced
// settings on).
void Menu::checkVideoSettingsFit() {
  fitChecked_ = &text();
  videoSettingsBottom(true, true);
}
#endif

// A page alone is laid out from its top down; its bottom stays.
double Menu::pageAloneTop() {
  AloneTop& a = aloneTop_;
  const bool advanced = view_.advancedSettings;
  if (a.strings != &text() || a.page != page_ || a.advanced != advanced) {
    const double bottom = page_ == Page::Focus ? focusBottom(advanced, false)
                                               : videoSettingsBottom(advanced, false);
    a = {&text(), page_, advanced, std::max(0.0, std::floor(PageLayout::kBottom - bottom))};
  }
  return a.top;
}
