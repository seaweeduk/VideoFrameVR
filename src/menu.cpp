#include "menu_style.h"

#include "file_util.h"
#include "menu_setup.h"
#include "menu_stash.h"
#include "menu_stash_covers.h"
#include "thumbnailer.h"

#include <pango/pangocairo.h>

#include <cstdlib>
#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

// Everything about the tracks the menu shows, to detect changes.
std::string trackSignature(const std::vector<Track>& tracks) {
  std::string s;
  for (const Track& t : tracks)
    s += std::to_string(t.id) + t.type + (t.selected ? "*" : "") + t.title + t.lang + "|";
  return s;
}

// Dragging a list: how far (layout pixels) the pointer has to move along it
// before the list follows and a press stops being a click. The menu is 1.5 m
// away, 26 px to the degree: a steady hand's beam wavers by a few pixels and
// pulling or releasing the trigger tips the controller by more, while
// 24 px (about 0.9°, half a list row) is clearly a move.
constexpr double kDragStart = 24;
// Let go while moving, the list glides on at the pointer's speed over its
// last kGlideWindow seconds (at least kGlideStart px/s; slower is holding
// still with an unsteady hand), slowing by e every kGlideTau seconds until
// it is slower than kGlideStop: a fast flick of 2000 px/s glides on 300 px
// in 0.7 s.
constexpr double kGlideWindow = 0.1, kGlideStart = 120, kGlideTau = 0.15, kGlideStop = 20;

}  // namespace

Menu::Menu() = default;

Menu::~Menu() {
  for (Frame* f : {&browserFrame_, &stripFrame_})
    if (f->surface) cairo_surface_destroy(f->surface);
  for (const auto& star : starImages_) cairo_surface_destroy(star.second);
  for (const auto& layout : textLayouts_) g_object_unref(layout.second);
}

bool Menu::init(Uploader& uploader) {
  uploader_ = &uploader;
  refreshPlaces();
  return canvases_[kMain].init(uploader, kMainWidth, kMainHeight, menu_geometry::kRasterScale) &&
         canvases_[kBrowser].init(uploader, kBrowserWidth, kBrowserHeight,
                                  menu_geometry::kRasterScale, true);
}

bool Menu::hoverRect(PanelId panel, float rect[4], float& radius, bool& danger, float clip[4]) const {
  if ((panel == kBrowser && browserRow_ >= 0) || (panel == kSetup && setup_ && setup_->row >= 0))
    return false;
  if (hovered_ < 0 || hovered_ >= static_cast<int>(buttons_.size())) return false;
  const Button& b = buttons_[hovered_];
  // Sliders show their own state.
  if (b.panel != panel || b.disabled || b.volumeSlider || b.slider != MenuCommand::None) return false;
  // Cut off where drawButtons() cuts the button off.
  clip[0] = clip[1] = -1e6f;
  clip[2] = clip[3] = 1e6f;
  if (b.scrolls) {
    clip[1] = static_cast<float>(listTop_ - 2);
    clip[3] = static_cast<float>(listBottom_);
  } else if (b.side) {
    clip[0] = static_cast<float>(side_.x);
    clip[1] = static_cast<float>(side_.y);
    clip[2] = static_cast<float>(side_.x + side_.w);
    clip[3] = static_cast<float>(side_.y + side_.h);
  }
  if (b.card && !b.ratingPath.empty()) {  // a tile's stars: the tile is pointed at
    for (const Button& tile : buttons_)
      if (tile.card && tile.ratingPath.empty() && tile.panel == b.panel && tile.x <= b.x &&
          tile.y <= b.y && tile.x + tile.w >= b.x + b.w && tile.y + tile.h >= b.y + b.h) {
        rect[0] = static_cast<float>(tile.x);
        rect[1] = static_cast<float>(tile.y);
        rect[2] = static_cast<float>(tile.x + tile.w);
        rect[3] = static_cast<float>(tile.y + tile.h);
        radius = static_cast<float>(tile.radius >= 0 ? tile.radius : kButtonRadius);
        danger = false;
        return true;
      }
  }
  rect[0] = static_cast<float>(b.x);
  rect[1] = static_cast<float>(b.y);
  rect[2] = static_cast<float>(b.x + b.w);
  rect[3] = static_cast<float>(b.y + b.h);
  radius = static_cast<float>(b.radius >= 0 ? b.radius : kButtonRadius);
  danger = b.dangerHover;
  return true;
}

bool Menu::pointedButton() const {
  float rect[4], radius, clip[4];
  bool danger;
  return pointerPanel_ >= 0 &&
         hoverRect(static_cast<PanelId>(pointerPanel_), rect, radius, danger, clip) &&
         buttons_[hovered_].ratingPath.empty();
}

void Menu::languageChanged() {
  dirty_ = true;
  if (setup_) setup_->cuesDrawn = false;
  refreshPlaces();
  if (!stashMenu_) return;
  // Rails and similar-scene reasons keep translated text between redraws.
  stashMenu_->stale = stashMenu_->fullStale = true;
  stashMenu_->performers.stale = true;
  stashMenu_->sceneTab.similarGeneration = ~0ull;
  StashPages::staleHome(*this);
  StashPages::staleStudios(*this);
}

void Menu::update(const MenuView& view) {
  // A new colour redraws the whole menu; dragging a colour slider does that
  // at most ten times a second (the last value always arrives).
  Accent accent = view.accent;
  if (accent != view_.accent) {
    const auto now = std::chrono::steady_clock::now();
    const bool dragging = draggingSlider_ == MenuCommand::SetAccentHue ||
                          draggingSlider_ == MenuCommand::SetAccentBrightness ||
                          draggingSlider_ == MenuCommand::SetAccentSaturation;
    if (dragging && now - accentChanged_ < std::chrono::milliseconds(100)) accent = view_.accent;
    else accentChanged_ = now;
  }
  // Redraw for visible changes only; the time display has one-second steps.
  const PlaybackState& a = view.playback;
  const PlaybackState& b = view_.playback;
  // Playing on only changes the strip.
  const bool playing =
      static_cast<int>(a.position) != static_cast<int>(b.position) ||
      (view.streaming && static_cast<int>(a.bufferedUntil) != static_cast<int>(b.bufferedUntil)) ||
      a.paused != b.paused || std::abs(a.volume - b.volume) > 0.5 || a.muted != b.muted;
  const bool changed =
      std::abs(a.duration - b.duration) > 0.5 || a.title != b.title ||
      trackSignature(a.tracks) != trackSignature(b.tracks) ||
      view.format.projection != view_.format.projection ||
      view.format.stereo != view_.format.stereo || view.halfPacked != view_.halfPacked ||
      view.currentFile != view_.currentFile || view.playbackFailed != view_.playbackFailed ||
      view.discEncrypted != view_.discEncrypted ||
      view.theme != view_.theme || accent != view_.accent ||
      a.videoCodec != b.videoCodec ||
      a.pixelFormat != b.pixelFormat || a.fps != b.fps || a.audioCodec != b.audioCodec ||
      a.audioChannels != b.audioChannels ||
      view.floatingSubtitles != view_.floatingSubtitles || view.streaming != view_.streaming ||
      view.videoWidth != view_.videoWidth ||
      view.videoHeight != view_.videoHeight ||
      view.tvGamma != view_.tvGamma ||
      view.videoMipmaps != view_.videoMipmaps ||
      view.toneLook != view_.toneLook ||
      view.foveationAvailable != view_.foveationAvailable || view.foveation != view_.foveation ||
      view.showFoveation != view_.showFoveation || view.sharpBins != view_.sharpBins ||
      view.foveationLearned != view_.foveationLearned ||
      view.foveationRing != view_.foveationRing || view.sharpDegrees != view_.sharpDegrees ||
      view.foveationOutside != view_.foveationOutside || view.frameReuse != view_.frameReuse ||
      view.advancedSettings != view_.advancedSettings ||
      view.refreshRate != view_.refreshRate ||
      view.foveationMaps != view_.foveationMaps ||
      view.photo != view_.photo || view.slideshowSeconds != view_.slideshowSeconds ||
      view.background != view_.background || view.autoPassthrough != view_.autoPassthrough ||
      view.passthrough != view_.passthrough || view.packedAlpha != view_.packedAlpha ||
      view.maskFormat != view_.maskFormat || view.opacityApplies != view_.opacityApplies ||
      view.maskLayers != view_.maskLayers || view.secondLayerHidden != view_.secondLayerHidden ||
      view.roomMatch != view_.roomMatch || view.roomMatched != view_.roomMatched ||
      view.roomRead != view_.roomRead ||
      view.greenScreen.on != view_.greenScreen.on ||
      view.greenScreen.source != view_.greenScreen.source ||
      view.colourKey != view_.colourKey || view.keyColour != view_.keyColour ||
      view.keyPicking != view_.keyPicking || view.keyPickNote != view_.keyPickNote ||
      view.fillChecked != view_.fillChecked || view.fillChecking != view_.fillChecking ||
      view.greenScreenFound != view_.greenScreenFound ||
      view.resume != view_.resume ||
      view.seekPreview != view_.seekPreview || view.thumbnails != view_.thumbnails ||
      view.showPerformance != view_.showPerformance || view.vibration != view_.vibration ||
      view.releaseUpdate != view_.releaseUpdate || view.releaseVersion != view_.releaseVersion ||
      view.diagnostics != view_.diagnostics || view.diagnosticsFile != view_.diagnosticsFile ||
      view.crashedLastRun != view_.crashedLastRun || view.downloadsTo != view_.downloadsTo ||
      view.softwareDecoding != view_.softwareDecoding || view.stash.card != view_.stash.card ||
      view.deletable != view_.deletable ||
      view.setupBanner != view_.setupBanner || view.showWhatsNew != view_.showWhatsNew ||
      a.chapters.size() != b.chapters.size() || a.chapter != b.chapter;
  // These values change only their slider rows, not the strip or the page's layout.
  if (view.brightness != view_.brightness) sliderChanged(MenuCommand::SetBrightness);
  if (view.contrast != view_.contrast) sliderChanged(MenuCommand::SetContrast);
  if (view.saturation != view_.saturation) sliderChanged(MenuCommand::SetSaturation);
  if (view.balance.warmth != view_.balance.warmth) sliderChanged(MenuCommand::SetWarmth);
  if (view.balance.tint != view_.balance.tint) sliderChanged(MenuCommand::SetTint);
  if (view.balance.exposure != view_.balance.exposure) sliderChanged(MenuCommand::SetExposure);
  if (view.matchColour != view_.matchColour) sliderChanged(MenuCommand::SetMatchColour);
  if (view.matchBrightness != view_.matchBrightness) sliderChanged(MenuCommand::SetMatchBrightness);
  if (view.zoom != view_.zoom) sliderChanged(MenuCommand::SetZoom);
  if (view.originForward != view_.originForward) sliderChanged(MenuCommand::SetOriginForward);
  if (view.opacity != view_.opacity) sliderChanged(MenuCommand::SetOpacity);
  if (view.sharpen != view_.sharpen) sliderChanged(MenuCommand::SetSharpen);
  if (view.screen.distanceCm != view_.screen.distanceCm) sliderChanged(MenuCommand::SetScreenDistance);
  if (view.screen.heightCm != view_.screen.heightCm) sliderChanged(MenuCommand::SetScreenHeight);
  if (view.screen.curvePercent != view_.screen.curvePercent) sliderChanged(MenuCommand::SetScreenCurve);
  const KeySliders &key = view.greenScreen.sliders(view.colourKey),
                   &keyWas = view_.greenScreen.sliders(view_.colourKey);
  if (key.strength != keyWas.strength) sliderChanged(MenuCommand::SetKeyStrength);
  if (key.softness != keyWas.softness) sliderChanged(MenuCommand::SetKeySoftness);
  if (key.spill != keyWas.spill) sliderChanged(MenuCommand::SetKeySpill);
  if (key.shrink != keyWas.shrink) sliderChanged(MenuCommand::SetKeyShrink);
  if (view.subtitleSize != view_.subtitleSize) sliderChanged(MenuCommand::SetSubtitleSize);
  if (view.subtitleRaise != view_.subtitleRaise) sliderChanged(MenuCommand::SetSubtitleRaise);
  if (view.subtitleDistance != view_.subtitleDistance) sliderChanged(MenuCommand::SetSubtitleDistance);
  // The eye size can change while playing (a later Auto resolution, or
  // --resolution-cycle, or the Resolution slider, which redraws nothing
  // itself): only the video settings page shows it.
  if (page_ == Page::VideoSettings &&
      (view.videoResolution != view_.videoResolution ||
       view.videoResolutionEffective != view_.videoResolutionEffective ||
       view.resolutionOverride != view_.resolutionOverride ||
       view.resolutionNotice != view_.resolutionNotice))
    sliderChanged(MenuCommand::SetVideoResolution, kSettingStep + hintHeight(2));
  // Its hint tells that letting go of the slider switches.
  const bool resolutionHeld =
      view.resolutionPending && draggingSlider_ == MenuCommand::SetVideoResolution;
  if (resolutionHeld != resolutionHeld_) {
    resolutionHeld_ = resolutionHeld;
    if (page_ == Page::VideoSettings)
      sliderChanged(MenuCommand::SetVideoResolution, kSettingStep + hintHeight(2));
  }
  // The presets: only the video settings page shows them. Which one is lit
  // changes only its row at the top (unless the list is open); a new listing
  // also the list, or the name being typed's check.
  if (page_ == Page::VideoSettings) {
    const bool lit = view.performancePreset != view_.performancePreset ||
                     view.userPresetShown != view_.userPresetShown;
    if (view.userPresetsListed != view_.userPresetsListed ||
        (lit && dropdown_ == Dropdown::Presets))
      browserChanged();
    else if (lit)
      browserChanged(0, kPagePad + kSettingRowH + 8);
  }
  // ... and the Library's banner names it.
  if (page_ == Page::Library && view.setupBanner &&
      view.performancePreset != view_.performancePreset)
    dirty_ = true;
  if (page_ == Page::Focus) {
    if (!(view.focus == view_.focus)) focusChanged(view.focus);
    if (view.motionEmulation != view_.motionEmulation) browserChanged();
    if (view.motionDistance != view_.motionDistance) sliderChanged(MenuCommand::SetMotionDistance);
  }
  if (library_ && (changed || library_->generation() != currentGeneration_)) {
    const VideoRecord current = library_->record(view.currentFile);
    if (current.rating != current_.rating) stripDirty_ = true;
    current_ = current;
    currentGeneration_ = library_->generation();
  }
  if (view.appFps != view_.appFps) stripDirty_ = true;
  if (accent != view_.accent) accent_ = accentColours(accent);
  view_ = view;
  view_.accent = accent;
  if (changed) dirty_ = true;
  if (playing) stripDirty_ = true;
  // The strip's clock.
  const long minute = static_cast<long>(std::time(nullptr) / 60);
  if (minute != clockMinute_) {
    clockMinute_ = minute;
    stripDirty_ = true;
  }
  // New thumbnails only matter while the file browser shows them. They are
  // drawn in batches: each redraw of the browser costs a little.
  const bool newThumbnails = thumbnailer_ && thumbnailer_->update();
  if (newThumbnails &&
      (page_ == Page::Files || page_ == Page::Library || page_ == Page::Offline || stashPage()))
    thumbnailsPending_ = true;
  if (setup_) SetupPages::update(*this);
  if (stashCovers_) updateStash();
  if (thumbnailsPending_) {
    // Batched: all of the list at most every 0.7 s, where it can't tell
    // which pictures changed.
    const double now = steadySeconds();
    if (now - lastThumbnailRedraw_ > (picturesAlone() ? 0.2 : 0.7)) {
      thumbnailsPending_ = false;
      lastThumbnailRedraw_ = now;
      if (!picturesChanged()) browserChanged(listTop_ - 4, listBottom_ + 4);  // only the list shows them
    }
  }
  if (stash_) StashPages::update(*this);  // its catalog, status and settings
  if (stashMenu_) StashPages::updateSignIn(*this);  // the server editor's, set up or not
  if (offline_ && offline_->generation() != offlineGeneration_) {
    offlineGeneration_ = offline_->generation();
    offlineChanged();
  }

  if (places_.poll()) {
    browserChanged();
    pinsChanged();
  }
  updateNetwork();
  updateStashFolders();
  // A folder read in the background: its entries, later their dates and sizes.
  const bool reading = listing_.reading();
  if (listing_.poll()) {
    // A folder's videos, once their sizes and dates are known, also go into
    // the library's index (if it is under a pin).
    const auto& entries = listing_.entries();
    if (index_ && fileKind_ == FileKind::Video && !listing_.unavailable() &&
        std::none_of(entries.begin(), entries.end(), [](const FolderListing::Entry& e) {
          return !e.directory && e.modified == 0;
        }))
      index_->listed(directory(), entries);
    lookUpRecords();
    arrangeByRecords();
    if (reading) {
      folderRead();
      smbFolderRead();
    } else {
      browserChanged();
    }
  } else if (library_ && library_->generation() != recordsGeneration_) {
    // Ratings and plays changed (or files were found again): shown at once,
    // in the same order.
    const std::vector<VideoRecord> before = std::move(records_);
    lookUpRecords();
    if (page_ == Page::Files) recordsChanged(before);
  }
  // The library view: files found new or gone, Stash's scenes of a new
  // catalog (or none, Stash off; only while no video is open, else at the
  // next visit), what its heading says of the looks, its records changed.
  std::vector<std::string> found, gone;
  if (index_ && index_->poll(found, gone))
    libraryFilesChanged(found, gone);
  else if (page_ == Page::Library && libraryCatalogChanged() && !(index_ && index_->videoOpen()))
    libraryFilesChanged({}, {});
  if (page_ == Page::Library) {
    std::string status = libraryStatus();
    if (status != libraryStatus_) {
      libraryStatus_ = std::move(status);
      dirty_ = true;
    }
    updateLibraryFirstSync();
  }
  if (page_ == Page::Library && library_ && library_->generation() != libraryGeneration_) {
    refreshLibraryRecords();
    browserChanged();
  }
}

bool Menu::panelShown(PanelId panel) const {
  if (setup_) return panel == kSetup;
  switch (panel) {
    case kMain: return true;
    case kBrowser: return (page_ != Page::Main || dialog_ != Dialog::None) && !view_.keyPicking;
    case kSetup: return false;
  }
  return false;
}

void Menu::shownRows(int panel, int& first, int& end) const {
  first = 0;
  if (panel == kBrowser) {
    end = kBrowserHeight;
  } else if (panel == kSetup) {
    end = setup_->rowsShown;
  } else {
    // The strip's body; above it only while a list or tooltip opens there.
    end = kStripBodyY + kStripBodyHeight;
    const bool tooltip = tooltip_ >= 0 && tooltip_ < static_cast<int>(buttons_.size()) &&
                         buttons_[tooltip_].panel == kMain;
    const bool popup = std::any_of(buttons_.begin(), buttons_.end(),
                                   [](const Button& b) { return b.popup && b.panel == kMain; });
    if (!popup && !tooltip) first = kStripBodyY;
  }
}

void Menu::shownColumns(int panel, int& first, int& end) const {
  first = panel == kBrowser ? browserColumn_ : 0;
  end = panel == kBrowser ? kBrowserWidth : panel == kSetup ? kSetupWidth : kMainWidth;
}

void Menu::setPointer(int panel, float u, float v) {
  pointerPanel_ = panel;
  if (panel == kMain) {
    pointerX_ = u * kMainWidth;
    pointerY_ = v * kMainHeight;
  } else if (panel == kBrowser) {
    pointerX_ = u * kBrowserWidth;
    pointerY_ = v * kBrowserHeight;
  } else if (panel == kSetup) {
    pointerX_ = u * kSetupWidth;
    pointerY_ = v * (kSetupHeight + kSetupKeyboardHeight);
  }
  updateHover();
}

void Menu::updateHover() {
  if (browserRow_ >= 0 || (setup_ && setup_->row >= 0)) {  // a new page is being drawn
    hoverQuiet_ = true;
    return;
  }
  // The highlight is drawn by the panel shader, so hovering needs no redraw;
  // only a tooltip appearing or going away does.
  // A list dragged or gliding under the pointer flashes no rows.
  const int hovered =
      pointerPanel_ >= 0 && !listDragging() ? buttonAt(pointerPanel_, pointerX_, pointerY_) : -1;
  if (hovered != hovered_) {
    // A button shown only on hover (and its row) appears or goes.
    for (const int i : {hovered_, hovered})
      for (const int j : {i, i + 1})
        if (j >= 0 && j < static_cast<int>(buttons_.size()) && buttons_[j].hoverOnly)
          browserChanged(buttons_[j].y, buttons_[j].y + buttons_[j].h);
    hovered_ = hovered;
    hoverStart_ = steadySeconds();
    if (tooltip_ >= 0) {
      if (tooltip_ < static_cast<int>(buttons_.size())) tooltipChanged(tooltip_);
      tooltip_ = -1;
    }
  } else if (tooltip_ < 0 && hovered_ >= 0 && !buttons_[hovered_].tooltip.empty() &&
             steadySeconds() - hoverStart_ > 0.6) {
    tooltip_ = hovered_;
    tooltipChanged(tooltip_);
  }
  pointAtRating(hovered);
  updatePreview();
  feelHover(hovered);
}

void Menu::feelHover(int hovered) {
  HoverSpot spot;
  bool live = false;  // clickable, not greyed out
  double fraction;
  if (seekHover(fraction)) {
    spot = {kMain, kMargin, seekLineY(), 0, 0};
    live = true;
  } else if (hovered >= 0) {
    const Button& b = buttons_[hovered];
    spot = {b.panel, b.x, b.y, b.w, b.h};
    live = !b.disabled;
  }
  const bool held = dragging();
  if (live && !held && !hoverQuiet_ && !(spot == hoverSpot_)) feel(Feel::Enter);
  hoverQuiet_ = held;
  hoverSpot_ = spot;
}

bool Menu::dragging() const {
  return draggingSlider_ != MenuCommand::None || draggingVolume_ || draggingSeek_ ||
         draggingScroll_ || listDragging() || ratingDrag_.active ||
         (setup_ && setup_->folders && setup_->folders->drag.moving);
}

// Steam's slider: a tick each 40th of the track, its snap and its end.
void Menu::feelTrack(double value, double lo, double hi, bool detented, double detent,
                     const std::function<double(double)>& fraction) {
  constexpr int kTicks = 40;
  const double before = trackValue_;
  trackValue_ = value;
  if (value == before) return;
  if (value == lo || value == hi) {
    feel(Feel::Edge);
  } else if (detented && before != detent &&
             (value == detent || (before - detent) * (value - detent) < 0)) {
    feel(Feel::Detent);
  } else {
    auto tick = [&](double v) {
      return std::min(kTicks - 1, static_cast<int>(std::clamp(fraction(v), 0.0, 1.0) * kTicks));
    };
    if (tick(value) != tick(before)) feel(Feel::Tick);
  }
}

void Menu::tooltipChanged(int button) {
  const Button& b = buttons_[button];
  if (b.panel == kMain) {
    stripDirty_ = true;
  } else if (b.panel == kBrowser) {
    // Only the rows its tooltip may take, above or below it (drawTooltip()).
    const double width = std::min(1100.0, kBrowserWidth - 20.0) - 40;
    const double lines = std::ceil(textWidth(b.tooltip, kTooltipSize, false) / width) + 1;
    const double h = std::max(50.0, lines * 40 + 18);
    browserChanged(b.y - h - 12, b.y + b.h + h + 12);
  } else {
    dirty_ = true;
  }
}

void Menu::hideTooltipWhileScrolling() {
  hoverStart_ = steadySeconds();
  hoverQuiet_ = true;  // rows passing under a still pointer aren't entered
  if (tooltip_ < 0 || tooltip_ >= static_cast<int>(buttons_.size()) ||
      buttons_[tooltip_].panel != kBrowser)
    return;
  tooltipChanged(tooltip_);
  tooltip_ = -1;
}

MenuCommand Menu::click() {
  if (pointerPanel_ == kBrowser && browserRow_ >= 0) return {};
  if (pointerPanel_ == kSetup && setup_ && setup_->row >= 0) return {};
  if (pointerPanel_ < 0) return {};
  stopPreview();
  // A press stops a gliding list; on the list, that is all it does (as on a
  // touch screen), though it may drag on from there.
  const bool stoppedGlide = listDrag_.gliding;
  listDrag_ = ListDrag{};
  if (setup_ && SetupPages::press(*this)) return {};
  if (dialog_ != Dialog::None) {
    // Modal: only the dialog's own buttons (and notes that scroll) react, a
    // click next to it does nothing.
    const int index = buttonAt(pointerPanel_, pointerX_, pointerY_);
    if ((index < 0 || !buttons_[index].popup) && !onListContent(index)) return {};
  }
  if (dropdown_ != Dropdown::None) {
    // An open dropdown takes the click: an entry is chosen, anything else
    // closes it (but its own pane, between entries).
    const int index = buttonAt(pointerPanel_, pointerX_, pointerY_);
    if (index < 0 && onPopups(pointerPanel_, pointerX_, pointerY_)) return {};
    if (index < 0 || !buttons_[index].popup) {
      dropdown_ = Dropdown::None;
      dirty_ = true;
      return {};
    }
  }
  if (keyPopup_.open) {
    // The keyboard's accents or layouts: a click anywhere else closes them
    // and does what it does.
    const int index = buttonAt(pointerPanel_, pointerX_, pointerY_);
    if (index < 0 || !buttons_[index].popup || !buttons_[index].key) closeKeyPopup();
  }
  if (pointerPanel_ == kMain && std::abs(pointerY_ - seekLineY()) <= kSeekBand &&
      view_.playback.duration > 0 && !view_.photo) {
    // The seek bar: the knob follows the pointer while the trigger is held,
    // the video jumps there on letting go (drag()).
    draggingSeek_ = true;
    lastScrub_ = trackValue_ = seekAtPointer();
    dirty_ = true;
    return {};
  }
  const int index = buttonAt(pointerPanel_, pointerX_, pointerY_);
  const bool onList = onListContent(index);
  if (index < 0) {
    if (onList) pressList(nullptr);  // between rows, it still grabs the list
    return {};
  }
  if (buttons_[index].disabled) return {};
  if (buttons_[index].volumeSlider) {
    draggingVolume_ = true;
    const MenuCommand c = volumeAtPointer(buttons_[index]);
    trackValue_ = c.value;
    return c;
  }
  if (buttons_[index].slider != MenuCommand::None) {
    draggingSlider_ = buttons_[index].slider;
    draggingSliderOption_ = buttons_[index].sliderOption;
    const MenuCommand c = sliderAtPointer(buttons_[index]);
    trackValue_ = c.value;
    return c;
  }
  if (buttons_[index].scrollbar) {
    draggingScroll_ = true;
    scrollToPointer(buttons_[index]);
    return {};
  }
  if (onList && stoppedGlide) {
    pressList(nullptr);
    return {};
  }
  if (!buttons_[index].ratingPath.empty()) {
    startRating(buttons_[index]);
    // Stars take the press at once; moved off them, it drags their list.
    if (onList) pressList(nullptr);
    return {};
  }
  if (onList) {
    pressList(&buttons_[index]);
    return {};
  }
  if (buttons_[index].field) {
    const Button field = buttons_[index];
    return clickField(field);
  }
  if (!buttons_[index].key) dirty_ = true;
  // Copy first: the action may rebuild buttons_.
  const std::function<MenuCommand()> action = buttons_[index].action;
  return action();
}

MenuCommand Menu::drag(bool held) {
  if (keyDown_.at >= 0) keyHeld(held);
  if (keyPopup_.open && keyPopup_.dragging && !held) {
    // Let go after holding a key: over one of its popup's keys, that one;
    // anywhere else it closes, so it never covers the keys typed next.
    const int index = buttonAt(pointerPanel_, pointerX_, pointerY_);
    if (index >= 0 && buttons_[index].popup && buttons_[index].key) {
      const std::function<MenuCommand()> action = buttons_[index].action;
      return action();
    }
    closeKeyPopup();
  }
  if (held && pointerPanel_ == kBrowser && browserRow_ >= 0) return {};
  if (setup_ && setup_->folders && setup_->folders->drag.pressed)
    return SetupPages::drag(*this, held);
  if (ratingDrag_.active) return dragRating(held);
  if (listDrag_.pressed) return dragList(held);
  if (listDrag_.gliding) glideList();
  if (!held) draggingSlider_ = MenuCommand::None;
  if (draggingSlider_ != MenuCommand::None) {
    for (const Button& b : buttons_)
      if (b.slider == draggingSlider_ && b.sliderOption == draggingSliderOption_ &&
          b.panel == pointerPanel_ && !b.disabled) {
        const MenuCommand c = sliderAtPointer(b);
        // Its middle: the marked stop, else 0 when the range goes either way.
        const bool middle = b.sliderDefault >= 0 || (b.sliderMin < 0 && b.sliderMax > 0);
        feelTrack(c.value, b.sliderMin, b.sliderMax, middle, std::max(0, b.sliderDefault),
                  [&b](double v) { return sliderFraction(b, v); });
        return c.value == b.sliderValue ? MenuCommand{} : c;
      }
    return {};
  }
  if (!held) draggingScroll_ = false;
  if (draggingScroll_) {
    if (pointerPanel_ == kBrowser)
      for (const Button& b : buttons_)
        if (b.scrollbar && b.popup == (dialog_ != Dialog::None)) scrollToPointer(b);
    return {};
  }
  if (!held) {
    draggingVolume_ = false;
    if (draggingSeek_ && lastScrub_ >= 0) {
      // Let go: the video jumps to where the knob was dropped.
      draggingSeek_ = false;
      MenuCommand c;
      c.type = MenuCommand::SeekTo;
      c.value = lastScrub_;
      c.option = 1;  // from the seek bar
      return c;
    }
    draggingSeek_ = false;
  }
  if (pointerPanel_ != kMain) return {};
  if (draggingSeek_) {
    // Only the knob and the preview frame follow; the video itself jumps
    // once, on letting go (seeking big videos continuously stutters).
    const double seconds = seekAtPointer(), duration = view_.playback.duration;
    feelTrack(seconds, 0, duration, false, 0, [duration](double v) { return v / duration; });
    if (std::abs(seconds - lastScrub_) >= view_.playback.duration / 1500) {
      lastScrub_ = seconds;
      stripDirty_ = true;
    }
    return {};
  }
  if (!draggingVolume_) return {};
  for (const Button& b : buttons_)
    if (b.volumeSlider) {
      const MenuCommand c = volumeAtPointer(b);
      feelTrack(c.value, 0, 100, false, 0, [](double v) { return v / 100; });
      return c;
    }
  return {};
}

void Menu::cancelDrag() {
  if (ratingDrag_.active) {
    ratingDrag_.active = false;
    ratingChanged(ratingDrag_);
  }
  if (listDrag_.pressed) listDrag_ = ListDrag{};
  if (draggingSeek_) {
    draggingSeek_ = false;
    stripDirty_ = true;
  }
  draggingSlider_ = MenuCommand::None;
  draggingScroll_ = false;
  draggingVolume_ = false;
}

void Menu::scroll(double rows) {
  if (setup_) {  // the browser under it stays as it was
    SetupPages::scroll(*this, rows);
    return;
  }
  if (rows == 0 || !listOpen()) return;
  stopPreview();
  // The stick takes over from a glide, and a row pressed moves away from
  // under the pointer: letting go no longer clicks it.
  listDrag_.gliding = false;
  listDrag_.action = nullptr;
  if (overSide())
    scrollSide(rows * kFileRowH);  // as fast as a list's rows
  else
    scrollRows(rows);
}

void Menu::scrollPixels(double pixels) {
  scrollRows(pixels / (groupedPage() || dialog_ == Dialog::WhatsNew ? kSettingStep : rowHeight_));
}

void Menu::scrollRows(double rows) {
  if (!listOpen() || rows == 0) return;
  if (dialog_ == Dialog::WhatsNew) {  // its notes, as fast as settings rows
    scrollWhatsNew(rows * kSettingStep);
    return;
  }
  if (groupedPage()) {  // as fast as a list of settings rows
    const double position = std::clamp(pageScroll_ + rows * kSettingStep, 0.0, pageScrollMax_);
    if (std::abs(position - pageScroll_) > 0.01) {
      pageScroll_ = position;
      hoverQuiet_ = true;
      browserChanged(listTop_ - 4, listBottom_ + 4);
    }
    return;
  }
  const int maxOffset = std::max(0, listCount_ - visibleRows_);
  // In whole texture pixels (the rest is kept for the next time), so the
  // texture's list can be moved instead of drawn again (shiftList()).
  const double perRow = rowHeight_ * menu_geometry::kRasterScale;
  const double wanted = rows + scrollRemainder_;
  const double step = std::round(wanted * perRow) / perRow;
  const double now = scrollOffset_ + scrollFraction_;
  const double position = std::clamp(now + step, 0.0, static_cast<double>(maxOffset));
  scrollRemainder_ = position == now + step ? wanted - step : 0;
  if (position != now) setListPosition(position);
}

void Menu::setListPosition(double position) {
  stopPreview();
  scrollOffset_ = static_cast<int>(std::floor(position));
  scrollFraction_ = position - scrollOffset_;
  listMovedAt_ = steadySeconds();
  hideTooltipWhileScrolling();
  if (page_ == Page::Files || page_ == Page::Library || page_ == Page::Offline || stashListPage())
    listMoved_ = true;
  else
    browserChanged(listTop_ - 4, listBottom_ + 4);  // only the list moved
}

// What scrolls on a list page or a tall grouped page: its rows, cards and
// their buttons, and the space between them; not its scroll bar, which is
// a button of its own.
bool Menu::onListContent(int index) const {
  if (dialog_ == Dialog::WhatsNew) return listOpen() && index < 0 && overWhatsNewNotes();
  if (!listOpen() || pointerPanel_ != kBrowser || dialog_ != Dialog::None) return false;
  if (index >= 0) return buttons_[index].scrolls || buttons_[index].side;
  if (overSide()) return true;
  const double right = groupedPage() ? kBrowserWidth : listRight_;
  return pointerX_ >= kPageX && pointerX_ <= right && pointerY_ >= listTop_ &&
         pointerY_ <= listBottom_;
}

void Menu::pressList(const Button* row) {
  ListDrag& d = listDrag_;
  d = ListDrag{};
  d.pressed = true;
  d.side = overSide();
  d.pressY = d.lastY = pointerY_;
  if (row) {
    d.action = row->action;
    d.x = row->x;
    d.y = row->y;
    d.w = row->w;
    d.h = row->h;
  }
}

// While the trigger is held after a press on a list: once the pointer moved
// kDragStart along it, the list follows it pixel for pixel (by the stick's
// whole texture pixels, so the texture's list moves on the GPU). Letting go
// unmoved, near the row pressed, clicks it; letting go while moving glides.
MenuCommand Menu::dragList(bool held) {
  ListDrag& d = listDrag_;
  const bool on = pointerPanel_ == kBrowser && listOpen();
  if (!held) {
    if (!d.moving) {
      const bool click = d.action && on && pointerX_ >= d.x - kDragStart &&
                         pointerX_ <= d.x + d.w + kDragStart && pointerY_ >= d.y - kDragStart &&
                         pointerY_ <= d.y + d.h + kDragStart;
      const std::function<MenuCommand()> action = d.action;
      d = ListDrag{};
      if (!click) return {};
      dirty_ = true;
      return action();
    }
    // The speed of the last frames held (letting go may jerk the pointer).
    double velocity = 0;
    if (d.recent.size() >= 2 && d.recent.back().first - d.recent.front().first > 0.02)
      velocity = -(d.recent.back().second - d.recent.front().second) /
                 (d.recent.back().first - d.recent.front().first);
    const bool side = d.side;
    d = ListDrag{};
    if (std::abs(velocity) >= kGlideStart) {
      d.gliding = true;
      d.side = side;
      d.velocity = velocity;
      d.lastTime = steadySeconds();
    } else {
      updateHover();
    }
    return {};
  }
  if (!on) {  // off the browser: the list waits for the pointer to come back
    d.away = true;
    d.recent.clear();
    return {};
  }
  if (!d.moving) {
    if (std::abs(pointerY_ - d.pressY) <= kDragStart) return {};
    d.moving = true;  // no row is clicked now, nor highlighted
    d.action = nullptr;
    d.lastY = pointerY_;
    updateHover();
  }
  if (d.away) {
    d.away = false;
    d.lastY = pointerY_;
  }
  const double moved = pointerY_ - d.lastY;
  d.lastY = pointerY_;
  d.travel += moved;
  const double now = steadySeconds();
  d.recent.emplace_back(now, d.travel);
  while (d.recent.size() > 2 && now - d.recent[1].first >= kGlideWindow) d.recent.pop_front();
  if (moved != 0) {
    if (d.side)
      scrollSide(-moved);
    else
      scrollPixels(-moved);
  }
  return {};
}

// A list let go while moving slows down to a stop, or stops at its end.
void Menu::glideList() {
  ListDrag& d = listDrag_;
  const double now = steadySeconds();
  const double dt = std::clamp(now - d.lastTime, 0.0, 0.1);
  d.lastTime = now;
  const double slowing = std::exp(-dt / kGlideTau);
  const double pixels = d.velocity * kGlideTau * (1 - slowing);
  if (listOpen() && d.side)
    scrollSide(pixels);
  else if (listOpen())
    scrollPixels(pixels);
  d.velocity *= slowing;
  const ScrollExtent e = d.side ? ScrollExtent{side_.position, 0, side_.max} : scrollExtent();
  const bool atEnd = d.velocity < 0 ? e.position <= 1e-6 : e.position >= e.total - e.shown - 1e-6;
  if (!listOpen() || atEnd || std::abs(d.velocity) < kGlideStop) d.gliding = false;
}

void Menu::showPage(Page page) {
  stopPreview();
  if (page_ == Page::Files) saveBrowsing();
  if (page_ == Page::Library)
    libraryPlace_ = listPlace(libraryPaths(), gallery_ ? kGalleryColumns : 1);
  if (page_ == Page::Offline) offlineScroll_ = scrollOffset_ + scrollFraction_;
  if (stashMenu_) StashPages::leavePage(*this);
  // The folder's list keeps its place (openDirectory() restores it); the
  // other list pages restore theirs once shown (showListPage()).
  if (page != Page::Files) {
    scrollOffset_ = 0;
    scrollFraction_ = 0;
  }
  pageScroll_ = 0;
  listDrag_ = ListDrag{};
  page_ = page;
  if (page == Page::Files || page == Page::Library || page == Page::Network ||
      page == Page::Settings || page == Page::Offline || stashListPage())
    lastBrowserPage_ = page;
  editing_ = false;
  smbForm_ = {};
  placeRename_ = {};
  presetName_ = {};
  presetDeleting_.clear();
  // The user's presets as they are now (a file may have been copied in).
  if (page == Page::VideoSettings) command_.type = MenuCommand::ListUserPresets;
  if (page != Page::Files && page != Page::Network) {
    stashFolders_.picking = stashFolders_.checking = stashFolders_.notThere = false;
  }
  dropdown_ = Dropdown::None;
  dialog_ = Dialog::None;  // e.g. reopening the menu never resumes a pending deletion
  dirty_ = true;
}

void Menu::openDialog(Dialog dialog, const std::string& path, const std::string& error) {
  dialog_ = dialog;
  dialogPath_ = path;
  dialogError_ = error;
  dropdown_ = Dropdown::None;
  dirty_ = true;
}

// Searches from the end: later buttons (an open dropdown) are drawn on top.
// Within the pane drawPopups() puts behind the open dropdown.
bool Menu::onPopups(int panel, double px, double py) const {
  double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
  for (const Button& b : buttons_) {
    if (!b.popup || b.panel != panel) continue;
    x0 = std::min(x0, b.x);
    y0 = std::min(y0, b.y);
    x1 = std::max(x1, b.x + b.w);
    y1 = std::max(y1, b.y + b.h);
  }
  return px >= x0 - 12 && px <= x1 + 12 && py >= y0 - 12 && py <= y1 + 12;
}

int Menu::buttonAt(int panel, double px, double py) const {
  if ((panel == kBrowser && browserRow_ >= 0) || (panel == kSetup && setup_ && setup_->row >= 0))
    return -1;
  for (size_t i = buttons_.size(); i-- > 0;) {
    const Button& b = buttons_[i];
    // Popups own hover too: the shader's highlight sits above the Cairo texture.
    if ((dialog_ != Dialog::None || dropdown_ != Dropdown::None) && !b.popup) continue;
    if (b.scrolls && (py < listTop_ - 2 || py > listBottom_)) continue;  // as drawn
    if (b.side && (py < side_.y || py > side_.y + side_.h)) continue;
    if (b.panel == panel && b.action && (!b.disabled || !b.tooltip.empty()) && px >= b.x && px <= b.x + b.w &&
        py >= b.y && py <= b.y + b.h)
      return static_cast<int>(i);
  }
  return -1;
}

// The dialog box is centred on the browser's page, shown for it if closed.
void Menu::layoutDialog() {
  const Strings& t = text();
  const double boxH = dialogHeight(), boxY = (kBrowserHeight - boxH) / 2;
  auto close = [this] {
    dialog_ = Dialog::None;
    return MenuCommand{};
  };

  if (dialog_ == Dialog::WhatsNew) {  // its own size and buttons
    layoutWhatsNew();
    return;
  }
  std::vector<Button> row;
  if (dialog_ == Dialog::SaveTo || dialog_ == Dialog::NoRoom || dialog_ == Dialog::RemoveDownload) {
    layoutOfflineDialog(boxY, row);
  } else if (dialog_ == Dialog::ConfirmDelete || dialog_ == Dialog::Deleting) {
    layoutDeleteDialog(row);
  } else if (dialog_ == Dialog::Update) {
    layoutUpdateDialog(row);
  } else if (dialog_ == Dialog::StashCertificate) {
    StashPages::layoutCertificateDialog(*this, row);
  } else if (dialog_ >= Dialog::KeyRefused) {
    layoutNetworkDialog(row);
  } else if (dialog_ == Dialog::Error || dialog_ == Dialog::Deleted) {
    row.push_back(textButton(t.ok, Icon::None, close));
  } else if (dialog_ == Dialog::ConfirmQuit) {
    row.push_back(textButton(t.cancel, Icon::None, close));
    Button go = textButton(t.quit, Icon::Power, [this] {
      dialog_ = Dialog::None;
      MenuCommand c;
      c.type = MenuCommand::Quit;
      return c;
    });
    go.danger = true;
    row.push_back(go);
  }
  // Centred like addCenteredRow, but marked as part of the modal dialog.
  double total = 0;
  for (const Button& b : row) total += b.w;
  total += kGap * 3 * (row.size() - 1);
  double x = kPageCenterX - total / 2;
  for (Button& b : row) {
    b.panel = kBrowser;
    b.popup = true;
    b.x = x;
    b.y = boxY + boxH - 125;
    b.h = 85;
    buttons_.push_back(b);
    x += b.w + kGap * 3;
  }
}

void Menu::drawDialog(cairo_t* cr) {
  const Strings& t = text();
  const double boxH = dialogHeight();
  const double boxX = kPageCenterX - kDialogW / 2, boxY = (kBrowserHeight - boxH) / 2;
  // Dim the browser behind (if it is open), then the box.
  if (page_ != Page::Main) {
    cairo_rectangle(cr, 0, 0, kBrowserWidth, kBrowserHeight);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
    cairo_fill(cr);
  }
  if (dialog_ == Dialog::WhatsNew) {
    drawWhatsNew(cr);
    drawButtons(cr, kBrowser, true);
    return;
  }
  roundedRect(cr, boxX, boxY, kDialogW, boxH, 30);
  cairo_set_source_rgba(cr, 0.1, 0.1, 0.13, 1);
  cairo_fill(cr);

  const double textX = boxX + 50, textW = kDialogW - 100;
  if (dialog_ == Dialog::SaveTo || dialog_ == Dialog::NoRoom || dialog_ == Dialog::RemoveDownload) {
    drawOfflineDialog(cr, boxX, boxY);
  } else if (dialog_ == Dialog::Update) {
    drawUpdateDialog(cr, textX, boxY);
  } else if (dialog_ == Dialog::StashCertificate) {
    StashPages::drawCertificateDialog(*this, cr, textX, boxY);
  } else if (dialog_ >= Dialog::KeyRefused) {
    drawNetworkDialog(cr, textX, boxY);
  } else if (dialog_ == Dialog::ConfirmQuit) {
    drawText(cr, t.quitTitle, textX, boxY + 90, textW, 60, 38, true, true, 1, 1, 1);
  } else if (dialog_ == Dialog::Error || dialog_ == Dialog::Deleted) {
    drawText(cr, dialog_ == Dialog::Deleted ? t.deletion.deletedTitle : t.deleteFailed, textX,
             boxY + 40, textW, 60, 38, true, true, 1, 1, 1);
    drawText(cr, dialogError_, textX, boxY + 120, textW, 60, 26, false, true, 0.75, 0.75, 0.8);
  } else {
    drawDeleteDialog(cr, textX, boxY);
  }
  drawButtons(cr, kBrowser, true);
}

void Menu::layout(bool browser) {
  layoutPage(browser);
  if (browser && !keyboardShown_) keyPopup_ = {};  // gone with the keyboard
  if (browser && dialog_ != Dialog::None) layoutDialog();  // last, so it is on top
}

void Menu::layoutPage(bool browser) {
  std::vector<Button> old;
  if (!browser) old.swap(buttons_);
  buttons_.clear();
  if (browser) {
    keyboardShown_ = false;
    listRight_ = kMainWidth - kMargin;
    listLeft_ = kPageX + kPagePad;
    listCount_ = 0;
    side_.w = 0;
  }
  layoutMain();
  if (browser) {
    if (page_ != Page::Main) layoutBrowser();
  } else {
    const int hovered = hovered_, tooltip = tooltip_;
    for (size_t i = 0; i < old.size(); ++i) {
      if (old[i].panel != kBrowser) continue;
      if (static_cast<int>(i) == hovered) hovered_ = static_cast<int>(buttons_.size());
      if (static_cast<int>(i) == tooltip) tooltip_ = static_cast<int>(buttons_.size());
      buttons_.push_back(std::move(old[i]));
    }
  }
}

// The navigation column, then the page: the file browser, the library, the
// settings or Stash's pages.
void Menu::layoutBrowser() {
  groups_.clear();
  if (pageAlone()) {
    const size_t first = buttons_.size();
    if (page_ == Page::VideoSettings)
      layoutVideoSettings();
    else
      layoutFocus();
    scrollGroups(first);
    return;
  }
  layoutNavigation();
  if (page_ == Page::Settings) {
    const size_t first = buttons_.size();
    layoutSettings();
    scrollGroups(first);
    return;
  }
  if (page_ == Page::Files) {
    layoutFilesPage();
    return;
  }
  if (page_ == Page::Library) {
    layoutLibraryPage();
    return;
  }
  if (page_ == Page::Network) {
    layoutNetworkPage();
    return;
  }
  if (page_ == Page::Offline) {
    layoutOfflinePage();
    return;
  }
  if (page_ == Page::Places) {
    const size_t first = buttons_.size();
    layoutPlacesPage();
    if (placeRename_.path.empty())
      scrollGroups(first);
    else
      pageScrollMax_ = 0;  // over the keyboard, it fits
    return;
  }
  if (stashPage()) StashPages::layoutPage(*this);
}

// The navigation column: File explorer with the places
// under it (home, videos, the media folder, drives), then the Library (and
// Stash's section), and Settings at the bottom. The chosen row is darker
// with blue text.
void Menu::layoutNavigation() {
  const Strings& t = text();
  const bool folder = page_ == Page::Files;
  const double x = kPagePad - 8, w = menu_geometry::kSourcesWidthPx - 2 * kPagePad + 16;
  auto row = [&](const std::string& label, Icon icon, bool chosen, double y, double h, int size,
                 double indent, std::function<void()> open) -> Button& {
    Button b = textButton(label, icon, [open] {
      open();
      return MenuCommand{};
    });
    b.panel = kBrowser;
    b.nav = true;
    b.on = chosen;
    b.x = x + indent;
    b.y = y;
    b.w = w - indent;
    b.h = h;
    b.fontSize = size;
    b.radius = 14;
    buttons_.push_back(b);
    return buttons_.back();
  };
  double y = kPagePad - 4;
  const bool network = folder && remote::isRemote(directory()) && placeOf(directory()) < 0;
  Button& explorer = row(t.fileExplorer, Icon::Folder, folder && placeOf(directory()) < 0 && !network,
                         y, kNavRowH, 27, 0, [this] { showFileBrowser(); });
  if (places_.ready()) {
    // Edit places: a gear at the heading's end, shown only while pointing at it.
    explorer.w -= kPlaceRowH;
    Button& edit = addRound(Icon::Settings, t.places.edit, [this] {
      showPage(Page::Places);
      return MenuCommand{};
    }, x + w - kPlaceRowH, y + (kNavRowH - kPlaceRowH) / 2, kPlaceRowH);
    edit.panel = kBrowser;
    edit.bare = true;
    edit.hoverOnly = true;
  }
  y += kNavRowH + 4;
  std::vector<const Places::Place*> places;  // those not hidden (Edit places)
  for (const Places::Place& place : places_.places())
    if (!place.hidden) places.push_back(&place);
  // With Stash's section below, only the room the places take.
  const int capacity = stash_ ? 6 : kNavPlaces;
  const int count = static_cast<int>(places.size());
  const bool pages = count > capacity;
  const int shown = pages ? capacity - 1 : count;
  placeOffset_ = pages ? std::clamp(placeOffset_, 0, count - shown) : 0;
  const int placeRows = stash_ ? std::min(capacity, count) : kNavPlaces;
  for (int i = placeOffset_; i < std::min(placeOffset_ + shown, count); ++i) {
    const Places::Place& place = *places[i];
    Button& shortcut = row(place.label, Icon::Folder,
                          folder && directory() == place.path, y, kPlaceRowH, kTextSize,
                          24, [this, path = place.path] { openPlace(path); });
    shortcut.tooltip = place.path;
    shortcut.disabled = !place.available;
    if (offline_ && offline_->serverDown(place.path)) {
      shortcut.crossed = true;
      shortcut.tooltip += std::string("\n") + t.offline.serverDown;
    }
    if (place.pinned) {
      shortcut.w -= kPlaceRowH;
      Button& unpin = addRound(Icon::Pin, t.unpinFolder,
                              [this, path = place.path] {
                                places_.togglePin(path);
                                refreshPlaces();
                                return MenuCommand{};
                              }, x + w - kPlaceRowH, y, kPlaceRowH);
      unpin.panel = kBrowser;
      unpin.bare = true;
      unpin.on = true;
      unpin.hoverOnly = true;
      unpin.tooltip = place.path + "\n";
      if (!place.available) unpin.tooltip += std::string(t.folderUnavailable) + "\n";
      unpin.tooltip += t.unpinFolder;
    }
    y += kPlaceRowH + 2;
  }
  if (pages) {
    Button& previous = addRound(Icon::Previous, t.previousPlaces,
                                [this, shown] {
                                  placeOffset_ = std::max(0, placeOffset_ - shown);
                                  return MenuCommand{};
                                }, x + 24, y, kPlaceRowH);
    previous.panel = kBrowser;
    previous.bare = true;
    previous.disabled = placeOffset_ == 0;
    Button& next = addRound(Icon::Next, t.morePlaces,
                            [this, shown] {
                              placeOffset_ += shown;
                              return MenuCommand{};
                            }, x + w - kPlaceRowH, y, kPlaceRowH);
    next.panel = kBrowser;
    next.bare = true;
    next.disabled = placeOffset_ + shown >= count;
  }
  y = kPagePad + kNavRowH + placeRows * (kPlaceRowH + 2) + 20;
  navLineY_ = y - 10;
  Button& servers = row(t.network.nav, Icon::Globe, page_ == Page::Network || network, y, kNavRowH,
                        27, 0, [this] { showNetwork(); });
  if (offline_ && offline_->allServersDown()) {
    servers.crossed = true;
    servers.tooltip = t.offline.serverDown;
  }
  y += kNavRowH + 4;
  row(t.library, Icon::Library, page_ == Page::Library, y, kNavRowH, 27, 0,
      [this] { showLibrary(); });
  if (offline_) {
    y += kNavRowH + 4;
    const int toGo = offline_->toGo();
    row(t.offline.nav, Icon::Download, page_ == Page::Offline, y, kNavRowH, 27, 0,
        [this] { showOffline(); }).badge = toGo > 0 ? std::to_string(toGo) : "";
  }
  row(t.settings, Icon::Settings, settingsShown(), kBrowserHeight - kPagePad - kNavRowH, kNavRowH,
      27, 0, [this] { showPage(Page::Settings); });
  stashCardY_ = -1;
  if (stash_)
    StashPages::layoutNavigation(*this, x, w, y + kNavRowH + 8,
                                 kBrowserHeight - kPagePad - kNavRowH - 12);
  else if (view_.stash.card)
    StashPages::layoutCard(*this, x, w, kBrowserHeight - kPagePad - kNavRowH - 12);
}

// Which of the navigation's places is this folder, -1 if none (or hidden).
int Menu::placeOf(const std::string& path) const {
  const auto& places = places_.places();
  for (int i = 0; i < static_cast<int>(places.size()); ++i)
    if (places[i].path == path && !places[i].hidden) return i;
  return -1;
}

void Menu::openPlace(const std::string& path) {
  // Returning through the same place resumes its subfolder. Within Files,
  // the shortcut still goes straight to the place itself.
  const fs::path relative = fs::path(directory()).lexically_relative(path);
  if (page_ != Page::Files && !relative.empty() && *relative.begin() != "..") {
    showFileBrowser();
    return;
  }
  fileKind_ = FileKind::Video;
  openDirectory(path);
  showPage(Page::Files);
}

// Looks for the navigation's places again (on a worker thread).
void Menu::refreshPlaces() {
  const Strings& t = text();
  places_.refresh({}, {t.placeHome, t.placeVideos, t.placeFileSystem, t.drives.card,
                       t.drives.usbDrive, t.drives.drive});
}

void Menu::pinsChanged() {
  std::vector<std::string> pins;
  for (const Places::Place& place : places_.places())
    if (place.pinned) pins.push_back(place.path);
  if (!index_ || pins == indexPins_) return;
  indexPins_ = pins;
  index_->setPins(pins);
}

void Menu::drivesChanged(const std::vector<std::string>& removed) {
  refreshPlaces();
  if (setup_) SetupPages::drivesChanged(*this, removed);
  const std::string& dir = directory();
  for (const std::string& point : removed) {
    if (dir != point && dir.rfind(point + "/", 0) != 0) continue;
    const char* home = std::getenv("HOME");
    openDirectory(dir, {home ? home : "/"});  // no longer there: home instead
    break;
  }
}

// A panel's open list or choices: a backdrop, then the entries on top.
void Menu::drawPopups(cairo_t* cr, int panel) {
  double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
  for (const Button& b : buttons_) {
    if (!b.popup || b.panel != panel) continue;
    x0 = std::min(x0, b.x);
    y0 = std::min(y0, b.y);
    x1 = std::max(x1, b.x + b.w);
    y1 = std::max(y1, b.y + b.h);
  }
  if (x1 < x0) return;
  // Glass: a darker pane of glass with a light edge (nearly opaque behind
  // the presets' list, which covers busy rows); Material: a raised surface.
  const double radius = glass() ? std::min(40.0, (y1 - y0 + 24) / 2) : 28;
  roundedRect(cr, x0 - 12, y0 - 12, x1 - x0 + 24, y1 - y0 + 24, radius);
  if (glass())
    cairo_set_source_rgba(cr, 0.06, 0.10, 0.16, dropdown_ == Dropdown::Presets ? 0.94 : 0.72);
  else
    cairo_set_source_rgba(cr, 0.145, 0.169, 0.184, 1.0);
  cairo_fill_preserve(cr);
  if (glass()) {
    cairo_set_source_rgba(cr, 1, 1, 1, 0.35);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke_preserve(cr);
  }
  cairo_new_path(cr);
  drawButtons(cr, panel, true);
}

// The hovered button's name (or a long name in full) in a box just above it,
// below it where there is no room above.
void Menu::drawTooltip(cairo_t* cr, int panel, double panelWidth) {
  // Not while stars are being chosen: it would cover what they are for.
  if (tooltip_ < 0 || tooltip_ >= static_cast<int>(buttons_.size()) ||
      buttons_[tooltip_].panel != panel || ratingDrag_.active)
    return;
  const Button& b = buttons_[tooltip_];
  constexpr int size = kTooltipSize;
  const double maxW = std::min(1100.0, panelWidth - 20);
  PangoLayout* layout = pango_cairo_create_layout(cr);
  PangoFontDescription* font = pango_font_description_from_string(kFont);
  pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
  pango_layout_set_font_description(layout, font);
  pango_layout_set_text(layout, b.tooltip.c_str(), -1);
  pango_layout_set_width(layout, static_cast<int>((maxW - 40) * PANGO_SCALE));
  pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
  int textW = 0, textH = 0;
  pango_layout_get_pixel_size(layout, &textW, &textH);
  const double w = textW + 40, h = std::max(50, textH + 18);
  const double x = std::clamp(b.x + b.w / 2 - w / 2, 10.0, panelWidth - 10 - w);
  const double y = b.y - h - 8 >= 4 ? b.y - h - 8 : b.y + b.h + 8;
  roundedRect(cr, x, y, w, h, 12);
  cairo_set_source_rgba(cr, 0.02, 0.02, 0.03, 0.96);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, 1, 1, 1, 1);
  cairo_move_to(cr, x + 20, y + (h - textH) / 2);
  pango_cairo_show_layout(cr, layout);
  pango_font_description_free(font);
  g_object_unref(layout);
}

// Paints panel backgrounds, replacing what was there (quicker than clearing
// and blending): their flat insides as such, the rest from a cache drawn
// first if the theme changed. Copying all of it would take as long as
// drawing a list at kRasterScale 2.
Palette Menu::palette() const { return paletteFor(view_.theme, accent_); }

void Menu::paintFrame(cairo_t* cr, Frame& frame, int width, int height,
                      const std::vector<FrameBox>& panels) {
  const Palette& pal = palette();
  constexpr int inset = static_cast<int>(kGlow + kPanelRadius) + 2;
  if (!frame.surface || frame.theme != view_.theme) {
    if (frame.surface) cairo_surface_destroy(frame.surface);
    frame.surface = rasterSurface(width, height);
    frame.theme = view_.theme;
    frame.accent = view_.accent;
    frame.rows.assign(height, false);
  } else if (frame.accent != view_.accent) {  // the same surface, its edges repainted below
    frame.accent = view_.accent;
    frame.rows.assign(height, false);
  }
  // Populate only the rows needed now, including on a theme's first page.
  double x0, y0, x1, y1;
  cairo_clip_extents(cr, &x0, &y0, &x1, &y1);
  const int end = std::min(height, static_cast<int>(std::ceil(y1)));
  for (int row = std::max(0, static_cast<int>(std::floor(y0))); row < end;) {
    if (frame.rows[row]) {
      ++row;
      continue;
    }
    const int first = row;
    while (row < end && !frame.rows[row]) frame.rows[row++] = true;
    cairo_t* c = cairo_create(frame.surface);
    cairo_rectangle(c, 0, first, width, row - first);
    cairo_clip(c);
    cairo_set_fill_rule(c, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_rectangle(c, 0, 0, width, height);
    for (const FrameBox& p : panels)
      cairo_rectangle(c, p.x + inset, p.y + inset, p.w - 2 * inset, p.h - 2 * inset);
    cairo_clip(c);  // Only the edges are copied from this cache below.
    cairo_set_operator(c, CAIRO_OPERATOR_CLEAR);  // another colour's edges, after a change
    cairo_paint(c);
    cairo_set_operator(c, CAIRO_OPERATOR_OVER);
    for (const FrameBox& p : panels) panelFrame(c, p.x, p.y, p.w, p.h, pal.panel, pal.edge);
    cairo_destroy(c);
  }
  // Inside the edge, its glow and the rounded corners, a panel is its fill.
  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
  cairo_rectangle(cr, 0, 0, width, height);
  for (const FrameBox& p : panels)
    cairo_rectangle(cr, p.x + inset, p.y + inset, p.w - 2 * inset, p.h - 2 * inset);
  cairo_set_source_surface(cr, frame.surface, 0, 0);
  cairo_fill(cr);
  for (const FrameBox& p : panels)
    cairo_rectangle(cr, p.x + inset, p.y + inset, p.w - 2 * inset, p.h - 2 * inset);
  setColor(cr, pal.panel);
  cairo_fill(cr);
  cairo_restore(cr);
}

// The playback strip.
void Menu::drawMain(cairo_t* cr) {
  paintFrame(cr, stripFrame_, kMainWidth, kMainHeight,
             {{0, kStripBodyY, kMainWidth, kStripBodyHeight}});
  drawMainPage(cr);
  drawButtons(cr, kMain);
  drawPopups(cr, kMain);
  drawTooltip(cr, kMain, kMainWidth);
}

// The browser: the sources column and the page with its headings, labels
// and buttons; with nothing open in it, only a dialog.
void Menu::drawBrowser(cairo_t* cr) {
  if (page_ == Page::Main) {
    if (dialog_ != Dialog::None) drawDialog(cr);
    return;
  }
  const Palette& pal = palette();
  paintFrame(cr, browserFrame_, kBrowserWidth, kBrowserHeight,
             {{0, 0, menu_geometry::kSourcesWidthPx, kBrowserHeight},
              {kPageX, 0, kPageWidth, kBrowserHeight}});

  if (!pageAlone()) {  // the line under the places
    cairo_rectangle(cr, kPagePad, navLineY_, menu_geometry::kSourcesWidthPx - 2 * kPagePad, 1.5);
    setColor(cr, pal.edge, 0.5);
    cairo_fill(cr);
  }
  if (!pageAlone() && stashCardY_ >= 0) {  // the "Use Stash?" card's box
    roundedRect(cr, kPagePad - 8, stashCardY_, menu_geometry::kSourcesWidthPx - 2 * kPagePad + 16,
                124, 18);
    setColor(cr, pal.field, 0.55);
    cairo_fill(cr);
  }
  if (keyboardShown_) {  // the keyboard's own panel
    roundedRect(cr, keyboardRect_[0], keyboardRect_[1], keyboardRect_[2], keyboardRect_[3], 22);
    setColor(cr, pal.field, 0.9);
    cairo_fill_preserve(cr);
    setColor(cr, pal.edge);
    cairo_set_line_width(cr, 3);
    cairo_stroke(cr);
  }
  // Headings and labels.
  if (page_ == Page::Files) {
    drawFilesHeading(cr);
    drawGroups(cr);  // the SMB form's
  } else if (page_ == Page::Library) {
    drawLibraryHeading(cr);
  } else if (page_ == Page::Offline) {
    drawOfflineHeading(cr);
  } else if (stashPage()) {
    StashPages::drawHeading(*this, cr);
  } else {
    drawGroups(cr);
  }

  drawButtons(cr, kBrowser);
  if (side_.w > 0 && side_.max > 0) {  // where the side column is, along its right edge
    const double h = std::max(40.0, side_.h * side_.h / (side_.h + side_.max));
    const double y = side_.y + (side_.h - h) * side_.position / side_.max;
    roundedRect(cr, side_.x + side_.w - 6, y, 6, h, 3);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.4);
    cairo_fill(cr);
  }
  drawListStates(cr);
  if (dialog_ != Dialog::None)
    drawDialog(cr);
  else
    drawPopups(cr, kBrowser);
  drawTooltip(cr, kBrowser, kBrowserWidth);
}

void Menu::browserChanged(double y0, double y1, double x0, double x1) {
  if (setup_) {  // its keyboard and fields
    if (y0 <= 0 && y1 >= kBrowserHeight && x0 <= 0 && x1 >= kBrowserWidth)
      dirty_ = true;
    else
      SetupPages::damaged(*this, y0, y1, x0, x1);
    return;
  }
  const int first = std::max(0, static_cast<int>(std::floor(y0)));
  const int end = std::min(kBrowserHeight, static_cast<int>(std::ceil(y1)));
  const int left = std::max(0, static_cast<int>(std::floor(x0)));
  const int right = std::min(kBrowserWidth, static_cast<int>(std::ceil(x1)));
  if (first >= end || left >= right) return;
  // A page/catalog replacement cancels an unfinished drawing. Small live
  // changes wait until it is published, so they cannot keep restarting it.
  if (browserRow_ >= 0 && first == 0 && end == kBrowserHeight && left == 0 && right == kBrowserWidth)
    dirty_ = true;
  browserFirst_ = browserDirty_ ? std::min(browserFirst_, first) : first;
  browserEnd_ = browserDirty_ ? std::max(browserEnd_, end) : end;
  browserLeft_ = browserDirty_ ? std::min(browserLeft_, left) : left;
  browserRight_ = browserDirty_ ? std::max(browserRight_, right) : right;
  Damage damage{left, first, right, end};
  for (size_t i = 0; i < browserDamage_.size();) {
    const Damage& d = browserDamage_[i];
    if (damage.x0 <= d.x1 && damage.x1 >= d.x0 && damage.y0 <= d.y1 && damage.y1 >= d.y0) {
      damage = {std::min(damage.x0, d.x0), std::min(damage.y0, d.y0),
                std::max(damage.x1, d.x1), std::max(damage.y1, d.y1)};
      browserDamage_.erase(browserDamage_.begin() + i);
      i = 0;
    } else {
      ++i;
    }
  }
  browserDamage_.push_back(damage);
  browserDirty_ = true;
}

void Menu::sliderChanged(MenuCommand::Type type, double below, int option) {
  for (const Button& b : buttons_)
    if (b.panel == kBrowser && b.slider == type && (option < 0 || b.sliderOption == option)) {
      double left = b.x, right = b.x + b.w;
      if (below > 0)  // Resolution's hint spans its settings group.
        for (const Group& g : groups_)
          if (b.x >= g.x && b.x + b.w <= g.x + g.w && b.y >= g.y && b.y < g.y + g.h) {
            left = g.x;
            right = g.x + g.w;
          }
      browserChanged(b.y - 4, b.y + b.h + below + 4, left - 4, right + 4);
    }
}

void Menu::redraw() {
  if (setup_) {
    SetupPages::redraw(*this);
    return;
  }
  const bool all = dirty_;
  const bool strip = all || stripDirty_;
  const bool browser = all || (browserRow_ < 0 && (browserDirty_ || listMoved_ || sideMoved_));
  // A browser change alone redraws and uploads only its rows; scrolling alone
  // moves the list (or the side column) on the GPU (shiftList(), shiftSide()).
  int first = all ? 0 : browserFirst_, end = all ? kBrowserHeight : browserEnd_;
  const bool fullWidth = all || listMoved_ || sideMoved_;
  const int left = fullWidth ? 0 : browserLeft_, right = fullWidth ? kBrowserWidth : browserRight_;
  const bool movedOnly = (listMoved_ || sideMoved_) && !all && !browserDirty_;
  const bool listMoved = listMoved_, sideMoved = sideMoved_;
  if (movedOnly) first = kBrowserHeight, end = 0;
  if (listMoved_) {
    first = std::min(first, static_cast<int>(listTop_) - 4);
    end = std::max(end, static_cast<int>(listBottom_) + 4);
  }
  if (sideMoved_ && !movedOnly) {
    first = std::min(first, static_cast<int>(side_.y) - 4);
    end = std::max(end, static_cast<int>(side_.y + side_.h) + 4);
  }
  dirty_ = stripDirty_ = false;
  std::vector<Damage> damage;
  if (browser) {
    browserDirty_ = listMoved_ = sideMoved_ = false;
    damage.swap(browserDamage_);
  }
  layout(browser);
  // Hover index refers to the new layout.
  if (browserRow_ < 0 || all) {
    hovered_ = pointerPanel_ >= 0 && !listDragging() ? buttonAt(pointerPanel_, pointerX_, pointerY_)
                                                     : -1;
    if (tooltip_ != hovered_) tooltip_ = -1;
  }

  if (panelShown(kMain) && strip) {  // only its rows shown now
    int y0 = 0, y1 = 0;
    shownRows(kMain, y0, y1);
    cairo_t* cr = canvases_[kMain].cr();
    cairo_save(cr);
    cairo_rectangle(cr, 0, y0, kMainWidth, y1 - y0);
    cairo_clip(cr);
    drawMain(cr);  // its frame replaces what was there
    cairo_restore(cr);
    canvases_[kMain].upload(y0, y1);
  }
  if (!panelShown(kBrowser)) {
    browserRow_ = -1;
    return;
  }
  if (!browser) return;
  if (movedOnly) {
    if (sideMoved && !shiftSide()) {  // its columns alone
      drawBrowserPart(static_cast<int>(side_.y) - 4, static_cast<int>(side_.y + side_.h) + 4,
                      static_cast<int>(side_.x) - 4, static_cast<int>(side_.x + side_.w) + 4);
      drawnSide_ = side_.position;
    }
    if (!listMoved || shiftList()) return;
  }
  int area = 0;
  for (const Damage& d : damage) area += (d.x1 - d.x0) * (d.y1 - d.y0);
  // Scrolled while something else changed (a tooltip went, a cover came):
  // the list still moves on the GPU, and the changes are drawn after it,
  // where they were and where it took them.
  if (!all && !movedOnly && (listMoved || sideMoved) &&
      area <= kBrowserWidth * kBrowserHeight / 4) {
    const double listUp =
        drawnScroll_ < 0 ? 0 : (scrollOffset_ + scrollFraction_ - drawnScroll_) * rowHeight_;
    const double sideUp = drawnSide_ < 0 ? 0 : side_.position - drawnSide_;
    if (sideMoved && !shiftSide()) {
      drawBrowserPart(static_cast<int>(side_.y) - 4, static_cast<int>(side_.y + side_.h) + 4,
                      static_cast<int>(side_.x) - 4, static_cast<int>(side_.x + side_.w) + 4);
      drawnSide_ = side_.position;
    }
    if (!listMoved || shiftList()) {
      const int reach = static_cast<int>(std::ceil(std::max(std::abs(listUp), std::abs(sideUp))));
      for (const Damage& d : damage)
        drawBrowserPart(std::max(0, d.y0 - reach), std::min(kBrowserHeight, d.y1 + reach), d.x0,
                        d.x1);
      return;
    }
  }
  if (all || (!listMoved && !sideMoved && area > kBrowserWidth * kBrowserHeight / 4)) {
    if (!canvases_[kBrowser].beginUpdate()) {
      dirty_ = true;
      return;
    }
    browserView_ = view_;
    browserOffset_ = scrollOffset_;
    browserFraction_ = scrollFraction_;
    browserSide_ = side_.position;
    browserPage_ = pageScroll_;
    browserWhatsNew_ = whatsNew_.scroll;
    browserRow_ = 0;
    return;
  }
  if (!fullWidth) {
    for (const Damage& d : damage) drawBrowserPart(d.y0, d.y1, d.x0, d.x1);
  } else {
    drawBrowserPart(first, end, left, right);
  }
  if (first <= listTop_ - 2 && end >= listBottom_) drawnScroll_ = scrollOffset_ + scrollFraction_;
  if (side_.w > 0 && first <= side_.y - 2 && end >= side_.y + side_.h) drawnSide_ = side_.position;
}

// Draws the browser's rows [y0, y1) and columns [x0, x1) and uploads them.
void Menu::drawBrowserPart(int y0, int y1, int x0, int x1) {
  CairoCanvas& canvas = canvases_[kBrowser];
  cairo_t* cr = canvas.cr();
  // A dialog alone is drawn on nothing; pages' frames replace what was there.
  if (page_ == Page::Main) canvas.clear(y0, y1);
  cairo_save(cr);
  cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
  cairo_clip(cr);
  drawBrowser(cr);
  cairo_restore(cr);
  canvas.upload(y0, y1, x0, x1);
}

// Scrolling a folder's (or the library's) list or thumbnails: the texture's
// list moves on the GPU, and only the rows that come into view and the scroll
// bar's track are drawn. Drawing and uploading all of it at kRasterScale 2
// would take most of a frame. False if the list can't be moved: something is
// over it, or it moved by a part of a pixel or by more than it shows.
bool Menu::shiftList() {
  if ((page_ != Page::Files && page_ != Page::Library && page_ != Page::Offline && !stashListPage()) ||
      dialog_ != Dialog::None || ratingDrag_.active || drawnScroll_ < 0)
    return false;
  if (tooltip_ >= 0 && tooltip_ < static_cast<int>(buttons_.size()) &&
      buttons_[tooltip_].panel == kBrowser)
    return false;
  for (const Button& b : buttons_)
    if (b.popup && b.panel == kBrowser) return false;
  const double position = scrollOffset_ + scrollFraction_;
  const double up = (position - drawnScroll_) * rowHeight_;  // layout pixels the list moves up
  const double pixels = up * menu_geometry::kRasterScale;
  const int y0 = static_cast<int>(listTop_) - 2, y1 = static_cast<int>(listBottom_);
  if (std::abs(pixels - std::round(pixels)) > 1e-3 || std::abs(up) >= y1 - y0 - 1) return false;
  // Left of the scroll bar, all of what scrolls: rows, cards and their buttons.
  // (clear of the page's rounded corners).
  const int x0 = static_cast<int>(listLeft_) - 2, x1 = static_cast<int>(listRight_) - 29;
  canvases_[kBrowser].shiftUp(x0, y0, x1, y1, up);
  if (up > 0)
    drawBrowserPart(static_cast<int>(std::floor(y1 - up)), y1, x0, x1);
  else
    drawBrowserPart(y0, static_cast<int>(std::ceil(y0 - up)), x0, x1);
  // Only the track's columns: the rows end well left of them (drawButtons()).
  for (const Button& b : buttons_)
    if (b.panel == kBrowser && b.scrollbar) {
      double tx = 0, tw = 0;
      scrollbarTrack(b, tx, tw);
      drawBrowserPart(static_cast<int>(std::floor(b.y)) - 1, static_cast<int>(std::ceil(b.y + b.h)) + 1,
                      static_cast<int>(std::floor(tx)) - 1, static_cast<int>(std::ceil(tx + tw)) + 1);
    }
  drawnScroll_ = position;
  return true;
}

// The side column, as shiftList(): its texture pixels move up by what it
// scrolled, and only what comes into view is drawn.
bool Menu::shiftSide() {
  if (side_.w <= 0 || dialog_ != Dialog::None || drawnSide_ < 0) return false;
  if (tooltip_ >= 0 && tooltip_ < static_cast<int>(buttons_.size()) &&
      buttons_[tooltip_].panel == kBrowser)
    return false;
  for (const Button& b : buttons_)
    if (b.popup && b.panel == kBrowser) return false;
  const double up = side_.position - drawnSide_;
  const double pixels = up * menu_geometry::kRasterScale;
  const int y0 = static_cast<int>(side_.y), y1 = static_cast<int>(side_.y + side_.h);
  if (std::abs(pixels - std::round(pixels)) > 1e-3 || std::abs(up) >= y1 - y0 - 1) return false;
  const int x0 = static_cast<int>(side_.x), x1 = static_cast<int>(side_.x + side_.w);
  canvases_[kBrowser].shiftUp(x0, y0, x1, y1, up);
  if (up > 0)
    drawBrowserPart(static_cast<int>(std::floor(y1 - up)), y1, x0, x1);
  else
    drawBrowserPart(y0, static_cast<int>(std::ceil(y0 - up)), x0, x1);
  drawBrowserPart(y0, y1, x1 - 7, x1);  // where it is, along its right edge, clear of the rows
  drawnSide_ = side_.position;
  return true;
}

bool Menu::overSide() const {
  return side_.w > 0 && dialog_ != Dialog::WhatsNew && pointerPanel_ == kBrowser &&
         pointerX_ >= side_.x && pointerX_ <= side_.x + side_.w && pointerY_ >= side_.y &&
         pointerY_ <= side_.y + side_.h;
}

// By whole texture pixels (the rest is kept for the next time), as scrollRows().
void Menu::scrollSide(double pixels) {
  const double wanted = pixels + sideRemainder_;
  constexpr double perPixel = menu_geometry::kRasterScale;
  const double step = std::round(wanted * perPixel) / perPixel;
  const double position = std::clamp(side_.position + step, 0.0, side_.max);
  sideRemainder_ = position == side_.position + step ? wanted - step : 0;
  if (position == side_.position) return;
  side_.position = position;
  hideTooltipWhileScrolling();
  sideMoved_ = true;
}

const Texture* Menu::texture(PanelId panel) {
  // Uploads that waited too long for a frame were dropped: all of it again.
  for (CairoCanvas& canvas : canvases_)
    if (canvas.takeLost()) dirty_ = true;
  if (panel == kSetup) {
    if (setup_->canvas.takeLost()) dirty_ = true;
    if (dirty_ || !setup_->damage.empty()) redraw();
    if (setup_->row >= 0) SetupPages::drawBand(*this);
    return setup_->canvas.texture();
  }
  const bool starting = dirty_;
  if (dirty_ || stripDirty_ || (panel == kBrowser && browserRow_ < 0 &&
                              (browserDirty_ || listMoved_ || sideMoved_))) redraw();
  if (panel == kBrowser) {
    if (browserRow_ >= 0 && !starting) {
      const int end = std::min(browserRow_ + 128, kBrowserHeight);
      std::swap(view_, browserView_);
      std::swap(scrollOffset_, browserOffset_);
      std::swap(scrollFraction_, browserFraction_);
      std::swap(side_.position, browserSide_);
      std::swap(pageScroll_, browserPage_);
      std::swap(whatsNew_.scroll, browserWhatsNew_);
      drawBrowserPart(browserRow_, end, 0, kBrowserWidth);
      std::swap(whatsNew_.scroll, browserWhatsNew_);
      std::swap(pageScroll_, browserPage_);
      std::swap(side_.position, browserSide_);
      std::swap(scrollFraction_, browserFraction_);
      std::swap(scrollOffset_, browserOffset_);
      std::swap(view_, browserView_);
      browserRow_ = end;
      if (end == kBrowserHeight) {
        canvases_[kBrowser].finishUpdate();
        browserRow_ = -1;
        browserColumn_ = pageAlone() ? kPageX : 0;
        drawnScroll_ = browserOffset_ + browserFraction_;
        drawnSide_ = browserSide_;
      }
    } else if (browserRow_ < 0 && !dirty_) {
      updatePreview();  // a new layout may have moved another tile under the pointer
      if (browserDirty_) redraw();
      if (stash_ && !preview_.path.empty() && stash_->preview().update()) drawPreview();
    }
  }
  return canvases_[panel].texture();
}
