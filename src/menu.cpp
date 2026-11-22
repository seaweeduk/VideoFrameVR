#include "menu_parts.h"

#include "file_util.h"
#include "menu_controls.h"
#include "menu_list_parts.h"
#include "menu_setup.h"
#include "menu_stash.h"
#include "menu_stash_covers.h"
#include "thumbnailer.h"
#include "value_adjust.h"

#include <pango/pangocairo.h>

#include <cstdio>
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
  return init();
}

bool Menu::init() {
  refreshPlaces();
  return initCanvas(canvases_[kMain], kMainWidth, kMainHeight, menu_geometry::kRasterScale) &&
         initCanvas(canvases_[kBrowser], kBrowserWidth, kBrowserHeight, menu_geometry::kRasterScale, true);
}

bool Menu::initCanvas(CairoCanvas& canvas, int width, int height, float scale, bool scrolls) {
  return uploader_ ? canvas.init(*uploader_, width, height, scale, scrolls) : canvas.init(width, height, scale);
}

void Menu::draw(PanelId panel, cairo_t* cr) {
  if (panel == kSetup)
    SetupPages::draw(*this, cr);
  else if (panel == kMain)
    drawMain(cr);
  else
    drawBrowser(cr);
}

const std::vector<Menu::Button>& Menu::layOut() {
  if (setup_)
    SetupPages::layout(*this);
  else
    layout(true);
  return buttons_;
}

bool Menu::working() const {
  if (!places_.ready() || places_.looking() || listing_.reading() || discovery_.scanning()) return true;
  if ((controls_ && controls_->loading.running()) || (hints_ && hints_->loading.running())) return true;
  if (!setup_) return false;
  if (setup_->controls.loading.running()) return true;
  if (!setup_->folders) return false;
  if (setup_->folders->finding.running()) return true;
  for (const auto& [path, folder] : setup_->folders->folders)
    if (folder.reading.running()) return true;
  return false;
}

bool Menu::hoverRect(PanelId panel, float rect[4], float& radius, bool& danger, float clip[4]) const {
  if ((panel == kBrowser && browserRow_ >= 0) || (panel == kSetup && setup_ && setup_->row >= 0))
    return false;
  if (hovered_ < 0 || hovered_ >= static_cast<int>(buttons_.size())) return false;
  const Button& b = buttons_[hovered_];
  // Sliders show their own state.
  // A red label on the strip has only its tooltip.
  if (b.panel != panel || b.disabled || b.volumeSlider || b.slider != MenuCommand::None ||
      b.chip == 1)
    return false;
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
  if (b.scrollbar) {  // its thumb alone is raised
    double x = 0, w = 0, y = 0, h = 0;
    scrollbarTrack(b, x, w);
    scrollbarThumb(b, y, h);
    rect[0] = static_cast<float>(x);
    rect[1] = static_cast<float>(y);
    rect[2] = static_cast<float>(x + w);
    rect[3] = static_cast<float>(y + h);
    radius = static_cast<float>(w / 2);
    danger = false;
    return true;
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
  // Playing on only changes the band, and the transport only the strip.
  const bool ticked =
      static_cast<int>(a.position) != static_cast<int>(b.position) ||
      (view.streaming && static_cast<int>(a.bufferedUntil) != static_cast<int>(b.bufferedUntil));
  // ... as do the chapters replaced by as many others (their marks, their list).
  const bool playing = a.paused != b.paused || std::abs(a.volume - b.volume) > 0.5 || a.muted != b.muted ||
                       a.chaptersRevision != b.chaptersRevision;
  const bool changed =
      std::abs(a.duration - b.duration) > 0.5 || a.title != b.title ||
      trackSignature(a.tracks) != trackSignature(b.tracks) ||
      view.format.projection != view_.format.projection ||
      view.format.stereo != view_.format.stereo || view.halfPacked != view_.halfPacked ||
      view.currentFile != view_.currentFile || view.playbackFailed != view_.playbackFailed ||
      view.discEncrypted != view_.discEncrypted || view.decoderBusy != view_.decoderBusy ||
      accent != view_.accent ||
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
      view.afterVideo != view_.afterVideo ||
      view.background != view_.background || view.autoPassthrough != view_.autoPassthrough ||
      view.keyGreen != view_.keyGreen || view.passthroughAvailable != view_.passthroughAvailable ||
      view.passthrough != view_.passthrough || view.packedAlpha != view_.packedAlpha ||
      view.maskFormat != view_.maskFormat || view.opacityApplies != view_.opacityApplies ||
      view.maskLayers != view_.maskLayers || view.secondLayerHidden != view_.secondLayerHidden ||
      view.roomMatch != view_.roomMatch || view.roomMatched != view_.roomMatched ||
      view.greenScreen.on != view_.greenScreen.on ||
      view.greenScreen.source != view_.greenScreen.source ||
      view.colourKey != view_.colourKey || view.keyColour != view_.keyColour ||
      view.keyPicking != view_.keyPicking || view.keyPickNote != view_.keyPickNote ||
      view.fillChecked != view_.fillChecked || view.fillChecking != view_.fillChecking ||
      view.greenScreenFound != view_.greenScreenFound ||
      view.resume != view_.resume || view.freeDecoder != view_.freeDecoder ||
      view.seekPreview != view_.seekPreview || view.thumbnails != view_.thumbnails ||
      view.showPerformance != view_.showPerformance || view.vibration != view_.vibration ||
      view.releaseUpdate != view_.releaseUpdate || view.releaseVersion != view_.releaseVersion ||
      view.diagnostics != view_.diagnostics || view.diagnosticsFile != view_.diagnosticsFile ||
      view.crashedLastRun != view_.crashedLastRun || view.downloadsTo != view_.downloadsTo ||
      view.softwareDecoding != view_.softwareDecoding || view.stash.card != view_.stash.card ||
      view.scripts.enabled != view_.scripts.enabled || view.scripts.address != view_.scripts.address ||
      view.scriptStatus != view_.scriptStatus ||
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
  if (view.scripts.offsetMs != view_.scripts.offsetMs) sliderChanged(MenuCommand::SetScriptOffset);
  // One pushes the other along to keep their gap.
  if (view.scripts.rangeLow != view_.scripts.rangeLow || view.scripts.rangeHigh != view_.scripts.rangeHigh) {
    sliderChanged(MenuCommand::SetScriptRangeLow);
    sliderChanged(MenuCommand::SetScriptRangeHigh);
  }
  if (const auto pageUpdate = pageKind().update) pageUpdate(*this, view);
  updateControls(view);
  if (library_ && (changed || library_->generation() != currentGeneration_)) {
    const VideoRecord current = library_->record(view.currentFile);
    if (current.rating != current_.rating) stripDirty_ = true;
    current_ = current;
    currentGeneration_ = library_->generation();
  }
  if (view.appFps != view_.appFps) stripDirty_ = true;
  if (accent != view_.accent) {
    accent_ = accentColours(accent);
    finishes_.setAccent(accent_);
  }
  view_ = view;
  view_.accent = accent;
  // This video goes with the video (changed: the page is laid out again).
  if (view_.currentFile.empty() && videoSection_ == VideoSection::ThisVideo) {
    videoSection_ = VideoSection::Display;
    if (page_ == Page::VideoSettings) dropdown_ = Dropdown::None;
  }
  if (changed) dirty_ = true;
  if (playing) stripDirty_ = true;
  if (ticked) bandTicked_ = true;
  // The strip's clock.
  const long minute = static_cast<long>(std::time(nullptr) / 60);
  if (minute != clockMinute_) {
    clockMinute_ = minute;
    clockTicked_ = true;
  }
  // New thumbnails only matter while the file browser shows them. They are
  // drawn in batches: each redraw of the browser costs a little.
  const bool newThumbnails = thumbnailer_ && thumbnailer_->update();
  if (newThumbnails && pageIs(PageKind::kThumbnails)) thumbnailsPending_ = true;
  if (setup_) SetupPages::update(*this);
  if (stashCovers_) updateStash();
  if (thumbnailsPending_ && browserRow_ < 0) {
    // Batched: all of the list at most every 0.7 s, where it can't tell
    // which pictures changed. A page being drawn in bands is compared once
    // published: until then its pictures stay as its bands drew them (pictureOf()).
    const double now = steadySeconds();
    if (now - lastThumbnailRedraw_ > (picturesAlone() ? 0.2 : 0.7)) {
      thumbnailsPending_ = false;
      lastThumbnailRedraw_ = now;
      if (!picturesChanged()) browserChanged(listTop_ - 4, listBottom_ + 4);  // only the list shows them
    }
  }
  if (stash_) StashPages::update(*this);  // its catalog, status and settings
  if (stashMenu_) StashPages::updateSignIn(*this);  // the server editor's, set up or not
  // New pictures and downloads moving on ask a hidden browser for nothing:
  // what its texture shows is no longer known.
  if (!panelShown(kBrowser) &&
      (newThumbnails || (stashCovers_ && stashCovers_->stash().imagesArrived()) ||
       (offline_ && offline_->generation() != offlineGeneration_)))
    publishedKnown_ = false;
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
  if (index_) index_->setStash(stashCatalog());
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

void Menu::updateVideoSettings(const MenuView& view) {
  // The eye size can change while playing (a later Auto resolution, or
  // --resolution-cycle, or the Resolution slider, which redraws nothing
  // itself): only the video settings page shows it.
  if (videoSection_ == VideoSection::Display) {
    const ResolutionStatus status = resolutionStatus(view);
    const ResolutionStatus was = resolutionStatus(view_);
    if (status != was || view.videoResolution != view_.videoResolution ||
        view.videoResolutionEffective != view_.videoResolutionEffective ||
        view.resolutionOverride != view_.resolutionOverride ||
        view.resolutionNotice != view_.resolutionNotice) {
      // Its note coming or going, or taking a line more or less, moves the
      // rows under it, so all of the page is drawn again; a new text in it
      // only its own rows.
      const int lines = resolutionNoteLines(view);
      if (lines != resolutionNoteLines(view_))
        browserChanged();
      else
        sliderChanged(MenuCommand::SetVideoResolution, kit::kRowStep + lines * kit::kNoteLine);
    }
  }
  // The presets: only Video settings' Display shows them. Which one is
  // lit changes only their group (unless the list is open); a new listing
  // also the list, or the name being typed's check.
  const bool lit = view.performancePreset != view_.performancePreset ||
                   view.userPresetShown != view_.userPresetShown;
  if (view.userPresetsListed != view_.userPresetsListed ||
      (lit && dropdown_ == Dropdown::Presets))
    browserChanged();
  else if (lit && presetBox_[2] > 0)
    browserChanged(presetBox_[1] - 4, presetBox_[1] + presetBox_[3] + 4, presetBox_[0] - 4,
                   presetBox_[0] + presetBox_[2] + 4);
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
    first = drawnTop_;
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
          hoverOnlyChanged(buttons_[j]);
    hovered_ = hovered;
    hoverStart_ = steadySeconds();
    if (tooltip_ >= 0) {
      if (tooltip_ < static_cast<int>(buttons_.size())) tooltipChanged(tooltip_);
      tooltip_ = -1;
    }
  } else if (tooltip_ < 0 && hovered_ >= 0 && !buttons_[hovered_].tooltip.empty() &&
             !adjust_.held && heldFade() == 0 && steadySeconds() - hoverStart_ > 0.6) {
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
    // Only the rows its tooltip and its shadow may take, above or below it
    // (drawTooltip()).
    const double width = std::min(1100.0, kBrowserWidth - 20.0) - 36;
    const double lines = std::ceil(textWidth(b.tooltip, kTooltipSize, false) / width) + 1;
    const double h = std::max(50.0, lines * kit::kTipLine + 24);
    const double reach = MenuFinishes::reach(Finish::Tip);
    browserChanged(b.y - h - 12 - reach, b.y + b.h + h + 12 + reach);
  } else {
    dirty_ = true;
  }
}

// Only its box and what drawButtons() lets it draw out of it, not the
// whole width of its row: a pointer moving down a list passes a row every
// few frames.
void Menu::hoverOnlyChanged(const Button& b) {
  const PointedPart p = hoverOnlyPart(b);
  browserChanged(p.y0, p.y1, p.x0, p.x1);
}

Menu::PointedPart Menu::hoverOnlyPart(const Button& b) const {
  Finish finish = Finish::Raised;
  const double shadow = buttonFinish(b, finish) ? MenuFinishes::reach(finish) : 0;
  const double reachX = std::max(kButtonReachX, shadow), reach = std::max(kButtonReach, shadow);
  return {b.x - reachX, b.y - reach, b.x + b.w + reachX, b.y + b.h + reach, b.scrolls, b.side};
}

// As drawButtons() and drawRating() show them.
int Menu::pointedParts(PointedPart parts[3]) const {
  int count = 0;
  for (const int i : {hovered_, hovered_ + 1})
    if (hovered_ >= 0 && i < static_cast<int>(buttons_.size()) && buttons_[i].hoverOnly &&
        buttons_[i].panel == kBrowser)
      parts[count++] = hoverOnlyPart(buttons_[i]);
  if (ratingHover_.active && ratingHover_.panel == kBrowser)
    for (const Button& b : buttons_)
      if (ratingHover_.is(b)) {
        double box[4];
        ratingBox(ratingHover_, box);
        parts[count++] = {box[0], box[1], box[2], box[3], b.scrolls, b.side};
        break;
      }
  return count;
}

void Menu::hideTooltipWhileScrolling() {
  hoverStart_ = scrolledAt_ = steadySeconds();
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
  if (onSeekBand()) {
    // The seek bar: the knob follows the pointer while the trigger is held,
    // the video jumps there on letting go (drag()).
    draggingSeek_ = true;
    lastScrub_ = trackValue_ = seekAtPointer();
    dirty_ = true;
    return {};
  }
  const int index = buttonAt(pointerPanel_, pointerX_, pointerY_);
  const bool onList = onListContent(index);
  if (index >= 0 && index == hovered_) {  // shown pressed while the trigger holds it
    const Button& b = buttons_[index];
    pressSpot_ = {b.panel, b.x, b.y, b.w, b.h};
    pressChanged(pressSpot_);
  }
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
    if (buttons_[index].magnify) return pressValue(index);
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
  triggerHeld_ = held;
  if (!held && pressSpot_.panel >= 0) {
    const HoverSpot spot = pressSpot_;
    pressSpot_ = {};
    pressChanged(spot);
  }
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
  // While a new page is drawn, held input waits, except a value already held:
  // its rows are laid out, and the last stop before letting go is the one kept.
  if (held && pointerPanel_ == kBrowser && browserRow_ >= 0 && !adjust_.held &&
      draggingSlider_ == MenuCommand::None)
    return {};
  if (setup_ && setup_->folders && setup_->folders->drag.pressed)
    return SetupPages::drag(*this, held);
  if (ratingDrag_.active) return dragRating(held);
  if (listDrag_.pressed) return dragList(held);
  if (listDrag_.gliding) glideList();
  if (adjust_.held) return dragValue(held);
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
    // The held head goes back to its size: the band's box (tickBoxes()).
    if (draggingSeek_) bandTicked_ = true;
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
  if (pressSpot_.panel >= 0) {
    const HoverSpot spot = pressSpot_;
    pressSpot_ = {};
    pressChanged(spot);
  }
  if (ratingDrag_.active) {
    ratingDrag_.active = false;
    ratingChanged(ratingDrag_);
  }
  if (listDrag_.pressed) listDrag_ = ListDrag{};
  if (draggingSeek_) {
    draggingSeek_ = false;
    stripDirty_ = true;
  }
  if (adjust_.held) releaseValue();
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
      scrolledAt_ = steadySeconds();
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
  // To the top and to the end are greyed while there is nowhere to go.
  const double max = std::max(0, listCount_ - visibleRows_);
  const double now = scrollOffset_ + scrollFraction_;
  if ((now <= 0) != (position <= 0) || (now >= max) != (position >= max))
    for (const Button& b : buttons_)
      if (b.panel == kBrowser && (b.icon == Icon::JumpTop || b.icon == Icon::JumpBottom))
        browserChanged(b.y - 2, b.y + b.h + 2, b.x - 2, b.x + b.w + 2);
  scrollOffset_ = static_cast<int>(std::floor(position));
  scrollFraction_ = position - scrollOffset_;
  listMovedAt_ = steadySeconds();
  hideTooltipWhileScrolling();
  if (page_ == Page::Files || page_ == Page::Library || page_ == Page::Offline || networkList() ||
      stashListPage())
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

// By Page, in its order: flags, layout, drawHeading, leave, update.
const Menu::PageKind Menu::kPages[] = {
    {0, nullptr, nullptr, nullptr, nullptr},  // Main: no page in the browser
    {PageKind::kGrouped | PageKind::kKept,  // Settings
     [](Menu& m) {
       const size_t first = m.buttons_.size();
       m.layoutSettings();
       m.scrollGroups(first);
     },
     nullptr, StashPages::leaveSettings, nullptr},
    {PageKind::kAlone | PageKind::kGrouped,  // VideoSettings
     [](Menu& m) { m.layoutVideoSettings(); }, nullptr, nullptr,
     [](Menu& m, const MenuView& view) { m.updateVideoSettings(view); }},
    {PageKind::kList | PageKind::kPictures | PageKind::kThumbnails | PageKind::kKept,  // Files
     [](Menu& m) { m.layoutFilesPage(); },
     [](Menu& m, cairo_t* cr) {
       m.drawFilesHeading(cr);
       m.drawGroups(cr);  // the SMB form's
     },
     [](Menu& m) { m.saveBrowsing(); }, nullptr},
    {PageKind::kList | PageKind::kPictures | PageKind::kThumbnails | PageKind::kKept,  // Library
     [](Menu& m) { m.layoutLibraryPage(); },
     [](Menu& m, cairo_t* cr) { m.drawLibraryHeading(cr); },
     [](Menu& m) { m.libraryPlace_ = m.listPlace(m.libraryPaths(), m.gallery_ ? kGalleryColumns : 1); },
     [](Menu& m, const MenuView& view) {
       // Its banner names the performance preset.
       if (view.setupBanner && view.performancePreset != m.view_.performancePreset) m.dirty_ = true;
     }},
    {PageKind::kAlone | PageKind::kGrouped,  // Focus
     [](Menu& m) { m.layoutFocus(); }, nullptr, nullptr,
     [](Menu& m, const MenuView& view) {
       if (!(view.focus == m.view_.focus)) m.focusChanged(view.focus);
       if (view.motionEmulation != m.view_.motionEmulation) m.browserChanged();
       if (view.motionDistance != m.view_.motionDistance) m.sliderChanged(MenuCommand::SetMotionDistance);
     }},
    {PageKind::kStash | PageKind::kList | PageKind::kPictures | PageKind::kThumbnails |
         PageKind::kKept,  // StashScenes
     StashPages::layoutScenes,
     [](Menu& m, cairo_t* cr) { StashPages::drawScenesHeading(m, cr); }, StashPages::leaveScenes,
     nullptr},
    {PageKind::kStash | PageKind::kList | PageKind::kThumbnails | PageKind::kKept,  // StashHome
     StashPages::layoutHome, [](Menu& m, cairo_t* cr) { StashPages::drawHomeHeading(m, cr); },
     StashPages::leaveHome, nullptr},
    {PageKind::kStash | PageKind::kList | PageKind::kThumbnails | PageKind::kKept,  // StashPerformers
     StashPages::layoutPerformers,
     [](Menu& m, cairo_t* cr) { StashPages::drawPerformersHeading(m, cr); },
     StashPages::leavePerformers, nullptr},
    {PageKind::kStash | PageKind::kList | PageKind::kThumbnails | PageKind::kKept,  // StashPerformer
     StashPages::layoutPerformer,
     [](Menu& m, cairo_t* cr) { StashPages::drawPerformerHeading(m, cr); },
     StashPages::leavePerformer, nullptr},
    {PageKind::kStash | PageKind::kList | PageKind::kThumbnails | PageKind::kKept,  // StashStudios
     StashPages::layoutStudios, [](Menu& m, cairo_t* cr) { StashPages::drawStudiosHeading(m, cr); },
     StashPages::leaveStudios, nullptr},
    {PageKind::kStash | PageKind::kList | PageKind::kThumbnails | PageKind::kKept,  // StashStudio
     StashPages::layoutStudio, [](Menu& m, cairo_t* cr) { StashPages::drawStudioHeading(m, cr); },
     StashPages::leaveStudio, nullptr},
    {PageKind::kList | PageKind::kKept,  // Network
     [](Menu& m) { m.layoutNetworkPage(); }, nullptr, nullptr, nullptr},
    {PageKind::kList | PageKind::kPictures | PageKind::kThumbnails | PageKind::kKept,  // Offline
     [](Menu& m) { m.layoutOfflinePage(); },
     [](Menu& m, cairo_t* cr) { m.drawOfflineHeading(cr); },
     [](Menu& m) { m.offlineScroll_ = m.scrollOffset_ + m.scrollFraction_; }, nullptr},
    {PageKind::kGrouped,  // Places
     [](Menu& m) {
       const size_t first = m.buttons_.size();
       m.layoutPlacesPage();
       if (m.placeRename_.path.empty())
         m.scrollGroups(first);
       else
         m.pageScrollMax_ = 0;  // over the keyboard, it fits
     },
     nullptr, nullptr, nullptr},
    {PageKind::kStash | PageKind::kThumbnails,  // StashSceneDetails
     StashPages::layoutSceneDetails,
     [](Menu& m, cairo_t* cr) { StashPages::drawSceneDetailsHeading(m, cr); },
     StashPages::leaveSceneDetails, nullptr},
};

void Menu::showPage(Page page) {
  static_assert(std::size(kPages) == static_cast<size_t>(Page::StashSceneDetails) + 1);
  stopPreview();
  // Stash's pages are left only while its menu is there.
  if (const auto leave = pageKind().leave; leave && (stashMenu_ || !stashPage())) leave(*this);
  // The folder's list keeps its place (openDirectory() restores it); the
  // other list pages restore theirs once shown (showListPage()).
  if (page != Page::Files) {
    scrollOffset_ = 0;
    scrollFraction_ = 0;
  }
  pageScroll_ = 0;
  listDrag_ = ListDrag{};
  // Video settings opens on Display, or while a video is open on the
  // section picked last.
  if (page == Page::VideoSettings && page_ != page)
    videoSection_ = view_.currentFile.empty() ? VideoSection::Display : videoSectionPicked_;
  // Settings opens on General (showStashSettings(): on Stash).
  if (page == Page::Settings && page_ != page) settingsSection_ = SettingsSection::General;
  // A change's notice on the Controls page is for the visit it was made in.
  if (controls_) controls_->noticeShown = false;
  page_ = page;
  if (pageIs(PageKind::kKept)) lastBrowserPage_ = page;
  editing_ = false;
  smbForm_ = {};
  placeRename_ = {};
  presetName_ = {};
  scriptAddress_ = {};
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

// The dialog box (DialogLayout) is centred on the browser's page, shown for
// it if closed.
void Menu::layoutDialog() {
  const Strings& t = text();
  auto close = [this] {
    dialog_ = Dialog::None;
    return MenuCommand{};
  };
  if (dialog_ == Dialog::WhatsNew) {  // its own size, its notes scrolling
    layoutWhatsNew();
    return;
  }
  if (dialog_ == Dialog::ChooseJob || dialog_ == Dialog::ResetButtons || dialog_ == Dialog::EditGesture) {
    layoutControlsDialog();
    return;
  }
  const bool question = dialog_ == Dialog::ConfirmQuit || dialog_ == Dialog::Error ||
                        dialog_ == Dialog::Deleted;
  DialogLayout dialog(*this, question ? DialogLayout::kNarrow : DialogLayout::kWide);
  if (dialog_ == Dialog::SaveTo || dialog_ == Dialog::NoRoom || dialog_ == Dialog::RemoveDownload) {
    layoutOfflineDialog(dialog);
  } else if (dialog_ == Dialog::ConfirmDelete || dialog_ == Dialog::Deleting) {
    layoutDeleteDialog(dialog);
  } else if (dialog_ == Dialog::Update) {
    layoutUpdateDialog(dialog);
  } else if (dialog_ == Dialog::DecoderBusy) {
    dialog.title(t.decoderBusy.title);
    dialog.line(t.decoderBusy.explain, DialogLayout::Ink::Dim, 4);
    // Always: this time, and from now on without asking (value 1).
    auto free = [this](bool always) {
      return [this, always] {
        dialog_ = Dialog::None;
        MenuCommand c;
        c.type = MenuCommand::FreeDecoder;
        c.value = always ? 1 : 0;
        return c;
      };
    };
    dialog.button(textButton(t.cancel, Icon::None, close));
    dialog.button(textButton(t.decoderBusy.always, Icon::None, free(true)));
    dialog.button(textButton(t.decoderBusy.free, Icon::None, free(false), true));
  } else if (dialog_ == Dialog::StashCertificate) {
    // Like an SFTP server's changed key: connecting anyway is left of Don't
    // connect, the one to take, and red only when pointed.
    const StashSetupStrings& s = t.stashSetup;
    dialog.title(stashPrinted(s.untrustedTitle, dialogPath_), Icon::Warning);
    dialog.line(s.untrusted, DialogLayout::Ink::Dim, 3);
    dialog.line(s.untrustedRisk, DialogLayout::Ink::Warn, 2);
    std::vector<Button> row;
    StashPages::layoutCertificateDialog(*this, row);
    std::stable_partition(row.begin(), row.end(), [](const Button& b) { return !b.selected; });
    for (Button& b : row) {
      b.dangerHover = b.danger;
      b.danger = false;
      dialog.button(b);
    }
  } else if (dialog_ >= Dialog::KeyRefused) {
    layoutNetworkDialog(dialog);
  } else if (dialog_ == Dialog::Error || dialog_ == Dialog::Deleted) {
    dialog.title(dialog_ == Dialog::Deleted ? t.deletion.deletedTitle : t.deleteFailed);
    dialog.line(dialogError_);
    dialog.button(textButton(t.ok, Icon::None, close, true));
  } else if (dialog_ == Dialog::ConfirmQuit) {
    dialog.title(t.quitTitle);
    dialog.button(textButton(t.cancel, Icon::None, close));
    Button go = textButton(t.quit, Icon::Power, [this] {
      dialog_ = Dialog::None;
      MenuCommand c;
      c.type = MenuCommand::Quit;
      return c;
    });
    go.danger = true;
    dialog.button(go);
  }
}

bool Menu::covered(int panel, double r[4]) const {
  double box[4], inset = kit::kGroupCorner;
  if (panel == kBrowser && dialog_ != Dialog::None) {
    if (dialog_ == Dialog::WhatsNew || page_ == Page::Main) return false;
    const DialogShown& d = dialogShown_;
    box[0] = d.x, box[1] = d.y, box[2] = d.w, box[3] = d.h;
    inset = kit::kPanelCorner;
  } else if (panel == kBrowser && loupeBox(box)) {
    inset = adjust::kLoupeCorner;
  } else if (!popupBox(panel, box)) {
    return false;
  }
  r[0] = std::ceil(box[0] + inset), r[1] = std::ceil(box[1] + inset);
  r[2] = std::floor(box[0] + box[2] - inset) - r[0], r[3] = std::floor(box[1] + box[3] - inset) - r[1];
  return r[2] > 0 && r[3] > 0;
}

void Menu::drawDialog(cairo_t* cr) {
  const Palette& pal = palette();
  // Dim the browser behind (if it is open; not under the box), then the box
  // raised over it.
  if (page_ != Page::Main) {
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_rectangle(cr, 0, 0, kBrowserWidth, kBrowserHeight);
    double box[4];
    if (covered(kBrowser, box)) cairo_rectangle(cr, box[0], box[1], box[2], box[3]);
    cairo_set_source_rgba(cr, 4 / 255.0, 6 / 255.0, 10 / 255.0, 0.62);
    cairo_fill(cr);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
  }
  const DialogShown& d = dialogShown_;
  drawFinish(cr, Finish::Pop, d.x, d.y, d.w, d.h, kit::kPanelCorner);
  for (const DialogShown::Text& t : d.texts) {
    if (t.mono) {
      drawMono(cr, t.text, t.x, t.y, t.w, t.size, t.ink);
      continue;
    }
    double x = t.x, w = t.w;
    const double* ink = t.ink;
    const double white[4] = {pal.text.r, pal.text.g, pal.text.b, 1};
    if (t.icon != Icon::None) {  // a warning's: red, the title white after it
      drawIcon(cr, t.icon, x + t.size / 2.0, t.y + t.h / 2, t.size, ink[0], ink[1], ink[2]);
      x += t.size + 14;
      w -= t.size + 14;
      ink = white;
    }
    if (t.lines > 1)  // the lines' boxes are DialogLayout::kLineH apart
      drawTextLines(cr, t.text, x, t.y + 1, w, t.size, t.lines, ink, false, 0, t.bold);
    else
      drawText(cr, t.text, x, t.y, w, t.h, t.size, t.bold, false, ink[0], ink[1], ink[2]);
  }
  if (dialog_ == Dialog::WhatsNew) drawWhatsNew(cr);
  if (dialog_ == Dialog::ChooseJob || dialog_ == Dialog::EditGesture) drawControlsDialog(cr);
  drawButtons(cr, kBrowser, true);
}

void Menu::layout(bool browser) {
  if (browser) pageTop_ = 0;  // a page alone's (layoutBrowser())
  layoutPage(browser);
  if (browser && !keyboardShown_) keyPopup_ = {};  // gone with the keyboard
  if (browser && dialog_ != Dialog::None) layoutDialog();  // last, so it is on top
  if (browser) panelTop_ = pageAlone() ? alonePanelTop() : 0;
}

// Room above an open list's box and a dialog's, as the page's edge has.
double Menu::alonePanelTop() const {
  double top = pageTop_, box[4];
  if (dialog_ == Dialog::WhatsNew)
    top = std::min(top, whatsNewBox().y - kit::kSpaceWide);
  else if (dialog_ != Dialog::None)
    top = std::min(top, dialogShown_.y - kit::kSpaceWide);
  if (popupBox(kBrowser, box)) top = std::min(top, box[1] - kit::kPanelInset - kit::kSpace);
  return std::max(0.0, std::floor(top));
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
  pageHead_ = {};
  if (pageAlone()) {
    // A preset's name typed over the keyboard: the whole height, as the
    // keyboard needs it.
    const bool naming = page_ == Page::VideoSettings && presetName_.open;
    pageTop_ = naming ? 0 : pageAloneTop();
    const size_t first = buttons_.size();
    auto layoutAlone = [&] { pageKind().layout(*this); };
    layoutAlone();
    // A row the every-row measure missed: the kept top rises by what is
    // missing, once, and the page is laid out again from there.
    double bottom = 0;
    for (const Group& g : groups_) bottom = std::max(bottom, g.y + g.h);
    if (!naming && pageTop_ > 0 && bottom > PageLayout::kBottom + 0.5) {
      const double missing = std::ceil(bottom - PageLayout::kBottom);
#ifndef NDEBUG
      std::fprintf(stderr, "[fit] %s reaches %.0f px lower than its every-row measure: add the new "
                   "row's state to it (the panel grows to fit)\n",
                   pageHead_.title.c_str(), missing);
#endif
      aloneTop_.top = pageTop_ = std::max(0.0, pageTop_ - missing);
      buttons_.erase(buttons_.begin() + static_cast<std::ptrdiff_t>(first), buttons_.end());
      groups_.clear();
      pageHead_ = {};
      layoutAlone();
    }
    scrollGroups(first);
    return;
  }
  layoutNavigation();
  pageKind().layout(*this);
}

// The navigation column (the kit's): File explorer with the places under it
// (home, videos, the media folder, drives; pages of them when there are
// many), a rule, then Network, the Library and Offline (and Stash's
// section), and Settings at the bottom. The chosen row is raised, its icon
// in the accent.
Menu::Button& Menu::navRow(const std::string& label, Icon icon, bool chosen, double y, bool place,
                           std::function<void()> open) {
  Button b;
  b.label = label;
  b.icon = icon;
  b.action = [open] {
    open();
    return MenuCommand{};
  };
  b.panel = kBrowser;
  b.nav = true;
  b.on = chosen;
  b.listLook = static_cast<int>(place ? ListLook::NavPlace : ListLook::NavSection);
  b.dimInk = place && !chosen;
  b.x = kNavLeft + (place ? kNavIndent : 0);
  b.y = y;
  b.w = kNavWidth - (place ? kNavIndent : 0);
  b.h = place ? kPlaceRowH : kNavRowH;
  b.fontSize = place ? kit::kPlaceSize : kit::kNavSize;
  b.radius = kit::kCorner;
  buttons_.push_back(b);
  return buttons_.back();
}

void Menu::layoutNavigation() {
  const Strings& t = text();
  const bool folder = page_ == Page::Files;
  constexpr double x = kNavLeft, w = kNavWidth;
  double y = kNavTop;
  const bool network = folder && remote::isRemote(directory()) && placeOf(directory()) < 0;
  Button& explorer = navRow(t.fileExplorer, Icon::Folder,
                            folder && placeOf(directory()) < 0 && !network, y, false,
                            [this] { showFileBrowser(); });
  if (places_.ready()) {
    // Edit places: a gear at the heading's end, shown only while pointing at it.
    explorer.inner = kNavRowH;
    Button& edit = addRound(Icon::Settings, t.places.edit, [this] {
      showPage(Page::Places);
      return MenuCommand{};
    }, x + w - 4 - kNavButton, y + (kNavRowH - kNavButton) / 2, kNavButton);
    edit.panel = kBrowser;
    edit.bare = true;
    edit.hoverOnly = true;
    edit.iconSize = 24;
  }
  y += kNavRowH + kit::kSpacePiece;
  std::vector<const Places::Place*> places;  // those not hidden (Edit places)
  for (const Places::Place& place : places_.places())
    if (!place.hidden) places.push_back(&place);
  // With Stash's section below, only the room the places take; with the
  // "Use Stash?" card, a row less.
  const int capacity = stash_ ? 6 : view_.stash.card ? kNavPlaces - 1 : kNavPlaces;
  const int count = static_cast<int>(places.size());
  const bool pages = count > capacity;
  const int shown = pages ? capacity - 1 : count;
  placeOffset_ = pages ? std::clamp(placeOffset_, 0, count - 1) / shown * shown : 0;
  for (int i = placeOffset_; i < std::min(placeOffset_ + shown, count); ++i) {
    const Places::Place& place = *places[i];
    Button& shortcut = navRow(place.label, placeIcon(place.kind), folder && directory() == place.path,
                              y, true, [this, path = place.path] { openPlace(path); });
    shortcut.tooltip = place.path;
    shortcut.disabled = !place.available;
    if (offline_ && offline_->serverDown(place.path)) {
      shortcut.crossed = true;
      shortcut.tooltip += std::string("\n") + t.offline.serverDown;
    }
    if (place.pinned) {
      // Its pin unpins it, and stays usable while the place is unavailable.
      shortcut.inner = kPlaceRowH;
      constexpr double pin = 40;
      Button& unpin = addRound(Icon::Pin, t.unpinFolder,
                               [this, path = place.path] {
                                 places_.togglePin(path);
                                 refreshPlaces();
                                 return MenuCommand{};
                               }, x + w - 4 - pin, y + (kPlaceRowH - pin) / 2, pin);
      unpin.panel = kBrowser;
      unpin.bare = true;
      unpin.listLook = static_cast<int>(ListLook::PlacePin);
      unpin.iconSize = 22;
      unpin.tooltip = place.path + "\n";
      if (!place.available) unpin.tooltip += std::string(t.folderUnavailable) + "\n";
      unpin.tooltip += t.unpinFolder;
    }
    y += kPlaceRowH + kit::kSpacePiece;
  }
  if (pages) {
    // On a row of its own after a whole page, so it doesn't move.
    y = kNavTop + kNavRowH + kit::kSpacePiece + shown * (kPlaceRowH + kit::kSpacePiece);
    Button& previous = addRound(Icon::ChevronLeft, t.previousPlaces,
                                [this, shown] {
                                  placeOffset_ = std::max(0, placeOffset_ - shown);
                                  return MenuCommand{};
                                }, x + kNavIndent, y, kNavButton);
    previous.panel = kBrowser;
    previous.bare = true;
    previous.iconSize = 26;
    previous.disabled = placeOffset_ == 0;
    Button& next = addRound(Icon::Chevron, t.morePlaces,
                            [this, shown] {
                              placeOffset_ += shown;
                              return MenuCommand{};
                            }, x + w - kNavButton, y, kNavButton);
    next.panel = kBrowser;
    next.bare = true;
    next.iconSize = 26;
    next.disabled = placeOffset_ + shown >= count;
    Button dots;
    dots.panel = kBrowser;
    dots.listLook = static_cast<int>(ListLook::PagerDots);
    dots.x = x + kNavIndent + kNavButton;
    dots.y = y;
    dots.w = w - kNavIndent - 2 * kNavButton;
    dots.h = kNavButton;
    dots.sliderValue = placeOffset_ / shown;
    dots.sliderMax = (count + shown - 1) / shown;
    buttons_.push_back(dots);
    y += kPlaceRowH + kit::kSpacePiece;
  }
  y += kit::kSpaceTight;
  navLineY_ = y;
  y += kNavRuleH + 12;
  Button& servers = navRow(t.network.nav, Icon::Globe, page_ == Page::Network || network, y, false,
                           [this] { showNetwork(); });
  if (offline_ && offline_->allServersDown()) {
    servers.crossed = true;
    servers.tooltip = t.offline.serverDown;
  }
  y += kNavRowH + kit::kSpacePiece;
  navRow(t.library, Icon::Library, page_ == Page::Library, y, false, [this] { showLibrary(); });
  y += kNavRowH + kit::kSpacePiece;
  if (offline_) {
    const int toGo = offline_->toGo();
    navRow(t.offline.nav, Icon::Download, page_ == Page::Offline, y, false,
           [this] { showOffline(); }).badge = toGo > 0 ? stashNumber(toGo) : "";
    y += kNavRowH + kit::kSpacePiece;
  }
  constexpr double settingsY = kBrowserHeight - kNavTop - kNavRowH;
  navRow(t.settings, Icon::Settings, settingsShown(), settingsY, false,
         [this] { showPage(Page::Settings); });
  stashCardY_ = -1;
  if (stash_)
    StashPages::layoutNavigation(*this, y, settingsY - 12);
  else if (view_.stash.card)
    StashPages::layoutCard(*this, x, w, settingsY - 12);
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
bool Menu::popupBox(int panel, double box[4]) const {
  double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
  for (const Button& b : buttons_) {
    if (!b.popup || b.panel != panel) continue;
    x0 = std::min(x0, b.x);
    y0 = std::min(y0, b.y);
    x1 = std::max(x1, b.x + b.w);
    y1 = std::max(y1, b.y + b.h);
  }
  if (x1 < x0) return false;
  const double pad = panel == kMain ? kit::kPopPad : 12;  // the strip's lists are the kit's
  box[0] = x0 - pad, box[1] = y0 - pad, box[2] = x1 - x0 + 2 * pad, box[3] = y1 - y0 + 2 * pad;
  return true;
}

// Clips what is drawn next to the panel but what an open dialog or list
// hides (covered()); true if it did (cairo_restore() to end it).
bool Menu::clipCovered(cairo_t* cr, int panel, double width, double height) const {
  double box[4];
  if (!covered(panel, box)) return false;
  cairo_save(cr);
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_rectangle(cr, box[0], box[1], box[2], box[3]);
  cairo_clip(cr);
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
  return true;
}

void Menu::drawPopups(cairo_t* cr, int panel) {
  double box[4];
  if (!popupBox(panel, box)) return;
  // An open list: raised over the page, its shadow on it.
  drawFinish(cr, Finish::Pop, box[0], box[1], box[2], box[3], kit::kGroupCorner);
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
  // A page shown alone hides the sources column: keep the box on the page.
  const double left = panel == kBrowser && pageAlone() ? kPageX : 0;
  const double maxW = std::min(1100.0, panelWidth - left - 20);
  PangoLayout* layout = pango_cairo_create_layout(cr);
  PangoFontDescription* font = pango_font_description_from_string(kFont);
  pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
  pango_layout_set_font_description(layout, font);
  pango_layout_set_text(layout, b.tooltip.c_str(), -1);
  pango_layout_set_width(layout, static_cast<int>((maxW - 36) * PANGO_SCALE));
  pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
  int textW = 0, textH = 0;
  pango_layout_get_pixel_size(layout, &textW, &textH);
  const double w = textW + 36, h = std::max(50, textH + 24);
  const double x = std::clamp(b.x + b.w / 2 - w / 2, left + 10, panelWidth - 10 - w);
  const double top = panel == kBrowser ? panelTop_ : 0;
  const double y = b.y - h - 8 >= top + 4 ? b.y - h - 8 : b.y + b.h + 8;
  drawFinish(cr, Finish::Tip, x, y, w, h, 12);
  if (panel == kBrowser) tooltipBox_[0] = x, tooltipBox_[1] = y, tooltipBox_[2] = w, tooltipBox_[3] = h;
  setColor(cr, palette().text);
  cairo_move_to(cr, x + 18, onPixel(y + (h - textH) / 2));
  pango_cairo_show_layout(cr, layout);
  pango_font_description_free(font);
  g_object_unref(layout);
}

// Paints panel backgrounds, replacing what was there (quicker than clearing
// and blending): their insides as their gradient, the rest from a cache of
// their edges and corners. Copying all of it would take as long as drawing
// a list at kRasterScale 2.
Palette Menu::palette() const { return paletteFor(accent_); }

const MenuFinishes::Behind* Menu::stripPanel() {
  if (!stripBehind_.paint)
    stripBehind_ = {kStripBodyY + kit::kPanelInset, kStripBodyY + kStripBodyHeight - kit::kPanelInset,
                    [this](cairo_t* cr) { paintStrip(cr); }};
  return &stripBehind_;
}

void Menu::paintStrip(cairo_t* cr) {
  paintFrame(cr, stripFrame_, kMainWidth, kMainHeight, {{0, kStripBodyY, kMainWidth, kStripBodyHeight}});
}

void Menu::paintFrame(cairo_t* cr, Frame& frame, int width, int height,
                      const std::vector<FrameBox>& panels) {
  constexpr int inset = static_cast<int>(kit::kPanelInset + kit::kPanelCorner) + 2;
  if (!frame.surface) frame.surface = rasterSurface(width, height);
  if (frame.panels != panels) {  // its edges again, as they are now
    frame.rows.assign(height, false);
    frame.panels = panels;
  }
  // Populate only the rows needed now, including on the first page.
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
    // Only the edges are copied from this cache below: each panel drawn
    // within the four rectangles of its edge alone, a third of the cost of
    // drawing it within one clip of everything but the panels' insides.
    cairo_t* c = cairo_create(frame.surface);
    // What the rows had for panels placed otherwise goes first.
    cairo_set_operator(c, CAIRO_OPERATOR_CLEAR);
    cairo_rectangle(c, 0, first, width, row - first);
    cairo_fill(c);
    cairo_set_operator(c, CAIRO_OPERATOR_OVER);
    for (const FrameBox& p : panels) {
      const int side = std::max(0, p.h - 2 * inset);
      const int edges[4][4] = {{p.x, p.y, p.w, inset},
                               {p.x, p.y + p.h - inset, p.w, inset},
                               {p.x, p.y + inset, inset, side},
                               {p.x + p.w - inset, p.y + inset, inset, side}};
      for (const auto& e : edges) {
        const int top = std::max(e[1], first), bottom = std::min(e[1] + e[3], row);
        if (bottom <= top || e[2] <= 0) continue;
        cairo_save(c);
        cairo_rectangle(c, e[0], top, e[2], bottom - top);
        cairo_clip(c);
        MenuFinishes::panel(c, p.x, p.y, p.w, p.h, kit::kPanelInset, kit::kPanelCorner, p.flat);
        cairo_restore(c);
      }
    }
    cairo_destroy(c);
  }
  // Inside its edge and the rounded corners, a panel is its gradient.
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
    MenuFinishes::fillPanel(cr, p.x + inset, p.y + inset, p.x + p.w - inset, p.y + p.h - inset,
                            p.y + kit::kPanelInset, p.y + p.h - kit::kPanelInset, p.flat);
  cairo_restore(cr);
}

void Menu::paintBrowserFrame(cairo_t* cr) {
  paintFrame(cr, browserFrame_, kBrowserWidth, kBrowserHeight,
             {{0, 0, menu_geometry::kSourcesWidthPx, kBrowserHeight},
              {kPageX, static_cast<int>(panelTop_), kPageWidth,
               kBrowserHeight - static_cast<int>(panelTop_), scrollBand()}});
}

MenuFinishes::Flat Menu::scrollBand() const {
  if (page_ != Page::Files && page_ != Page::Library && page_ != Page::Offline && !networkList() &&
      !stashPage())
    return {};
  if (page_ == Page::Files && smbForm_.kind != SmbForm::Kind::None) return {};
  MenuFinishes::Flat band{listTop_ - 2, listBottom_};
  if (side_.w > 0) {
    band.top = std::min(band.top, side_.y);
    band.bottom = std::max(band.bottom, side_.y + side_.h);
  }
  return band;
}

void Menu::pressChanged(const HoverSpot& spot) {
  const auto b = std::find_if(buttons_.begin(), buttons_.end(), [&spot](const Button& c) {
    return HoverSpot{c.panel, c.x, c.y, c.w, c.h} == spot;
  });
  // A key redraws itself as keyDown_ comes and goes (keyChanged()).
  if (b == buttons_.end() || !pressDrawn(*b) || b->keyCap) return;
  // Its shadow at rest goes as it is pushed in, and comes back.
  Finish finish = Finish::Raised;
  const double rest = buttonFinish(*b, finish) ? MenuFinishes::reach(finish) : 0;
  const double reach = std::max({rest, MenuFinishes::reach(Finish::Pressed), 2.0}) + 2;
  const double x0 = b->x - reach, y0 = b->y - reach, x1 = b->x + b->w + reach, y1 = b->y + b->h + reach;
  if (b->panel == kBrowser) {
    browserChanged(y0, y1, x0, x1);
    return;
  }
  stripChanged(x0, y0, x1, y1);
}

void Menu::stripChanged(double x0, double y0, double x1, double y1) {
  double box[4] = {x0, y0, x1, y1};
  for (int i = 0; i < stripParts_;) {
    double* p = stripPart_[i];
    const bool overlaps = box[0] <= p[2] && box[2] >= p[0] && box[1] <= p[3] && box[3] >= p[1];
    if (!overlaps && (stripParts_ < kStripParts || i < stripParts_ - 1)) {
      ++i;
      continue;
    }
    box[0] = std::min(box[0], p[0]), box[1] = std::min(box[1], p[1]);
    box[2] = std::max(box[2], p[2]), box[3] = std::max(box[3], p[3]);
    std::copy(stripPart_[stripParts_ - 1], stripPart_[stripParts_ - 1] + 4, p);
    --stripParts_;
    i = 0;  // the larger box may meet one already passed
  }
  std::copy(box, box + 4, stripPart_[stripParts_++]);
}

// The time, the fill and head, a stream's downloaded part: only where they
// changed since drawn. The clock: its box.
void Menu::tickBoxes(bool band, bool clock) {
  using namespace strip;
  if (clock)
    stripChanged(kLeft + kInset - 4, kTabsY - 4,
                 kLeft + kInset + textWidth("00:00", kClockSize, false) + 8, kTabsY + kTabsH + 4);
  if (!band || view_.keyPicking) return;
  const BandMarks now = bandMarks();
  const BandMarks& was = bandDrawn_;
  const double x = kLeft, w = kRight - kLeft;
  // Shadows and lit edges, and half of a held head.
  const double reach = std::max({MenuFinishes::reach(Finish::Sunk), MenuFinishes::reach(Finish::Filled),
                                 MenuFinishes::reach(Finish::Head)}) + 8;
  const double y0 = kBandY - reach, y1 = kBandY + kBandH + reach;
  if (!bandDrawnKnown_ || now.timeline != was.timeline || now.dragging != was.dragging ||
      now.of != was.of || now.titleRight != was.titleRight) {  // the title's room too
    stripChanged(x - reach, y0, x + w + reach, y1);
  } else if (now.timeline) {
    if (now.fraction != was.fraction) {
      double left = x + w * std::min(now.fraction, was.fraction) - reach;
      // The well is drawn whole under a fill that ends near its start.
      if ((now.covered > x + kit::kCorner) != (was.covered > x + kit::kCorner)) left = x - reach;
      stripChanged(left, y0, x + w * std::max(now.fraction, was.fraction) + reach, y1);
    }
    if (now.buffered != was.buffered) {
      const double low = std::min(now.buffered, was.buffered);
      // Its left end is rounded smaller while it is short.
      const double left = low < 2 * kit::kCorner + 2 ? x - reach : x + low - kit::kCorner - reach;
      stripChanged(left, y0, x + std::max(now.buffered, was.buffered) + reach, y1);
    }
    if (now.at != was.at) stripChanged(now.titleRight + 12, y0, x + w + reach, y1);
  }
  bandDrawn_ = now;
  bandDrawnKnown_ = true;
}

size_t Menu::stripLook() const {
  size_t h = std::hash<std::string>{}(infoText_);
  const auto mix = [&h](size_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
  const std::hash<double> number;
  const std::hash<std::string> text;
  mix(number(infoX_));
  mix(number(infoRight_));
  for (const Button& b : buttons_) {
    if (b.panel != kMain) continue;
    for (const double v : {b.x, b.y, b.w, b.h, b.radius, b.iconSize, static_cast<double>(b.rating)})
      mix(number(v));
    for (const std::string* s : {&b.label, &b.detail, &b.tooltip, &b.caption, &b.badge}) mix(text(*s));
    mix(static_cast<size_t>(b.icon));
    unsigned flags = 0;
    for (const bool f : {b.selected, b.popup, b.raised, b.danger, b.dangerHover, b.bare, b.primary,
                         b.latched, b.disabled, b.crossed, b.on, b.tab, b.listItem, b.band,
                         b.volumeSlider, b.hoverOnly, b.dialog})
      flags = flags << 1 | f;
    mix(flags);
    mix(static_cast<size_t>(b.chip) << 8 | static_cast<size_t>(b.fontSize));
  }
  return h;
}

size_t Menu::browserLook(double scroll, double page, double side, double notes) const {
  size_t h = static_cast<size_t>(page_) << 8 | static_cast<size_t>(dialog_);
  const auto mix = [&h](size_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
  const std::hash<double> number;
  const std::hash<std::string> text;
  for (const double v : {panelTop_, scroll, page, side, notes, stashCardY_}) mix(number(v));
  // What the page draws besides its buttons: its heading, the folder, and
  // what a list shows in place of its rows.
  Icon state = Icon::None;
  std::string heading, hint;
  listState(state, heading, hint);
  for (const std::string* s : {&heading, &hint}) mix(text(*s));
  for (const std::string* s : {&pageHead_.title, &pageHead_.about, &pageHead_.status}) mix(text(*s));
  mix(static_cast<size_t>(state));
  if (page_ == Page::Files) {
    mix(text(directory()));
    mix(static_cast<size_t>(fileKind_) << 2 | static_cast<size_t>(stashFolders_.picking) << 1 |
        static_cast<size_t>(stashFolders_.notThere));
  }
  // ... and what it draws as pointed at or pressed (pointedParts()).
  if (tooltip_ >= 0 && tooltip_ < static_cast<int>(buttons_.size()) && buttons_[tooltip_].panel == kBrowser)
    for (const double v : {buttons_[tooltip_].x, buttons_[tooltip_].y}) mix(number(v));
  if (ratingHover_.active && ratingHover_.panel == kBrowser)
    for (const double v : {ratingHover_.x, ratingHover_.y, static_cast<double>(ratingHover_.rating)}) mix(number(v));
  if (pressSpot_.panel == kBrowser)
    for (const double v : {pressSpot_.x, pressSpot_.y}) mix(number(v));
  for (size_t i = 0; i < buttons_.size(); ++i) {
    const Button& b = buttons_[i];
    if (b.panel != kBrowser) continue;
    if (b.hoverOnly) mix(hovered_ == static_cast<int>(i) || hovered_ == static_cast<int>(i) - 1);
    for (const double v : {b.x, b.y, b.w, b.h, static_cast<double>(b.rating), static_cast<double>(b.sliderValue),
                           static_cast<double>(b.offlineLook)})
      mix(number(v));
    for (const std::string* s : {&b.label, &b.detail, &b.thumbPath, &b.caption, &b.badge, &b.byline, &b.size,
                                 &b.date, &b.plays, &b.length, &b.sliderText})
      mix(text(*s));
    mix(static_cast<size_t>(b.icon) << 3 | static_cast<size_t>(b.selected) << 2 |
        static_cast<size_t>(b.disabled) << 1 | static_cast<size_t>(b.on));
  }
  return h;
}

bool Menu::browserReady() const {
  if (!canvases_[kBrowser].texture()) return false;
  return browserRow_ < 0 || (publishedKnown_ && !lookStale_ && drawingLook() == publishedLook_);
}

// Scrolled as when its drawing began (redraw()).
size_t Menu::drawingLook() const {
  if (!drawingLookKnown_)
    drawingLook_ = browserLook(browserOffset_ + browserFraction_, browserPage_, browserSide_, browserWhatsNew_);
  drawingLookKnown_ = true;
  return drawingLook_;
}

// The playback strip.
void Menu::drawMain(cairo_t* cr) {
  const bool hidden = clipCovered(cr, kMain, kMainWidth, kMainHeight);
  paintStrip(cr);
  drawMainPage(cr);
  drawButtons(cr, kMain);
  if (hidden) cairo_restore(cr);
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
  if (drawLoupeOnly(cr)) return;
  const Palette& pal = palette();
  const bool hidden = clipCovered(cr, kBrowser, kBrowserWidth, kBrowserHeight);
  paintBrowserFrame(cr);

  if (!pageAlone()) {  // the rule under the places
    roundedRect(cr, kNavLeft + kit::kSpace, navLineY_, kNavWidth - 2 * kit::kSpace, kNavRuleH,
                kNavRuleH / 2);
    setColor(cr, pal.rule);
    cairo_fill(cr);
  }
  if (!pageAlone() && stashCardY_ >= 0) {  // the "Use Stash?" card's box
    groupFrame(cr, kNavLeft, stashCardY_, kNavWidth, 124);
  }
  drawKeyboard(cr);
  // Headings and labels.
  if (const auto heading = pageKind().drawHeading) {
    heading(*this, cr);
  } else {
    drawGroups(cr);
    if (controlsShown()) drawControls(cr);
  }

  drawButtons(cr, kBrowser);
  if (side_.w > 0 && side_.max > 0) {  // where the side column is, along its right edge, as a list's bar
    const double w = kScrollTrackW, x = side_.x + side_.w - w;
    drawFinish(cr, Finish::Sunk, x, side_.y, w, side_.h, w / 2);
    const double h = std::max(kScrollThumbMin, side_.h * side_.h / (side_.h + side_.max));
    const double y = side_.y + (side_.h - h) * side_.position / side_.max;
    cairo_pattern_t* fill = cairo_pattern_create_linear(0, y, 0, y + h);
    cairo_pattern_add_color_stop_rgba(fill, 0, 1, 1, 1, 0.34);
    cairo_pattern_add_color_stop_rgba(fill, 1, 1, 1, 1, 0.22);
    roundedRect(cr, x, y, w, h, w / 2);
    cairo_set_source(cr, fill);
    cairo_fill(cr);
    cairo_pattern_destroy(fill);
  }
  drawListStates(cr);
  if (hidden) cairo_restore(cr);
  drawLoupe(cr);
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
      double loupe[4];
      if (b.magnify && loupeBox(loupe) && below <= 0) {
        // Magnify covers the field but what sticks out at either side.
        const double y0 = b.y - 4, y1 = b.y + b.h + 4;
        if (left - 4 < loupe[0]) browserChanged(y0, y1, left - 4, loupe[0]);
        if (right + 4 > loupe[0] + loupe[2]) browserChanged(y0, y1, loupe[0] + loupe[2], right + 4);
        continue;
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
  // A pressed control (pressChanged()), the second ticking over and the
  // clock's minute (tickBoxes()) redraw only their boxes.
  bool stripAll = all || stripDirty_;
  const bool bandTicked = bandTicked_, clockTicked = clockTicked_;
  const bool ticked = bandTicked || clockTicked;
  const bool strip = stripAll || stripParts_ > 0 || ticked;
  const bool asked = browserDirty_ || listMoved_ || sideMoved_;
  const bool browser = all || (browserRow_ < 0 && asked);
  // A browser change alone redraws and uploads only its rows; scrolling alone
  // moves the list (or the side column) on the GPU (shiftList(), shiftSide()).
  int first = all ? 0 : browserFirst_, end = all ? kBrowserHeight : browserEnd_;
  const bool fullWidth = all || listMoved_ || sideMoved_;
  const int left = fullWidth ? 0 : browserLeft_, right = fullWidth ? kBrowserWidth : browserRight_;
  const bool movedOnly = (listMoved_ || sideMoved_) && !all && !browserDirty_;
  const bool listMoved = listMoved_, sideMoved = sideMoved_;
  if (movedOnly) first = kBrowserHeight, end = 0;
  if (listMoved_) {
    if (dialog_ == Dialog::WhatsNew) {
      const WhatsNewBox box = whatsNewBox();
      first = std::min(first, static_cast<int>(box.y) - 4);
      end = std::max(end, static_cast<int>(box.y + box.h) + 4);
    } else {
      first = std::min(first, static_cast<int>(listTop_) - 4);
      end = std::max(end, static_cast<int>(listBottom_) + 4);
    }
  }
  if (sideMoved_ && !movedOnly) {
    first = std::min(first, static_cast<int>(side_.y) - 4);
    end = std::max(end, static_cast<int>(side_.y + side_.h) + 4);
  }
  dirty_ = stripDirty_ = bandTicked_ = clockTicked_ = false;
  std::vector<Damage> damage;
  if (browser) {
    browserDirty_ = listMoved_ = sideMoved_ = false;
    damage.swap(browserDamage_);
  }
  const bool tipOnBrowser = tooltip_ >= 0 && tooltip_ < static_cast<int>(buttons_.size()) &&
                            buttons_[tooltip_].panel == kBrowser;
  const bool tipOnStrip = tooltip_ >= 0 && tooltip_ < static_cast<int>(buttons_.size()) &&
                          buttons_[tooltip_].panel == kMain;
  // Laid out again, the strip's controls must look as they were drawn for
  // the second or the minute to redraw only their boxes.
  const size_t look = !stripAll && ticked ? stripLook() : 0;
  // My presets' button takes a row of its own or not as its name changes
  // (layoutPresetGroup()), moving what is under it in its column.
  const bool presets = browser && !all && page_ == Page::VideoSettings && presetBox_[2] > 0;
  const double presetWas[4] = {presetBox_[0], presetBox_[1], presetBox_[2], presetBox_[3]};
  const double columnWas = presets ? presetColumnBottom() : 0;
  PointedPart pointedWas[3];
  const int pointedWasCount = browser ? pointedParts(pointedWas) : 0;
  layout(browser);
  if (!stripAll && ticked && stripLook() != look) stripAll = true;
  // Where the page's panel is flat moved (scrollBand()): all of it again.
  // ... and where a page alone's panel starts moved.
  const bool whole = all || (browser && (!(scrollBand() == drawnBand_) ||
                                         static_cast<int>(panelTop_) != browserTop_));
  if (presets && !whole && presetBox_[2] > 0 &&
      (presetBox_[1] != presetWas[1] || presetBox_[3] != presetWas[3])) {
    const double y0 = std::min(presetBox_[1], presetWas[1]) - 4;
    const double y1 = std::max(presetColumnBottom(), columnWas) + 4;
    const double x0 = std::min(presetBox_[0], presetWas[0]) - 4;
    const double x1 = std::max(presetBox_[0] + presetBox_[2], presetWas[0] + presetWas[2]) + 4;
    damage.swap(browserDamage_);  // drawn now, with the rest
    browserChanged(y0, y1, x0, x1);
    damage.swap(browserDamage_);
    browserDirty_ = false;
    first = std::min(first, std::max(0, static_cast<int>(std::floor(y0))));
    end = std::max(end, std::min(kBrowserHeight, static_cast<int>(std::ceil(y1))));
  }
  // Hover index refers to the new layout.
  if (browserRow_ < 0 || all) {
    hovered_ = pointerPanel_ >= 0 && !listDragging() ? buttonAt(pointerPanel_, pointerX_, pointerY_)
                                                     : -1;
    if (tooltip_ != hovered_) {
      // Its box goes too: drawn above a list's first row it is over the tools
      // row, which a list moved on the GPU doesn't redraw.
      if (tipOnBrowser) {
        const double reach = MenuFinishes::reach(Finish::Tip);
        const double* box = tooltipBox_;
        browserChanged(box[1] - reach, box[1] + box[3] + reach, box[0] - reach, box[0] + box[2] + reach);
      }
      if (tipOnStrip && ticked) stripAll = true;  // as the whole strip drawn each second did
      tooltip_ = -1;
    }
  }
  // What shows only while pointed at went with its row as the list moved, or
  // is on another row now: drawn again where it went and where it is, after
  // the move.
  std::vector<Damage> pointed;
  if (browser && !whole) {
    PointedPart now[3];
    const int count = pointedParts(now);
    const double listUp = listMoved && drawnScroll_ >= 0
                              ? (scrollOffset_ + scrollFraction_ - drawnScroll_) * rowHeight_
                              : 0;
    const double sideUp = sideMoved && drawnSide_ >= 0 ? side_.position - drawnSide_ : 0;
    for (int i = 0; i < pointedWasCount; ++i) {
      const double up = pointedWas[i].scrolls ? listUp : pointedWas[i].side ? sideUp : 0;
      pointedWas[i].y0 -= up;
      pointedWas[i].y1 -= up;
    }
    const auto add = [&](const PointedPart& p, const PointedPart* others, int n) {
      for (int i = 0; i < n; ++i)
        if (p.x0 == others[i].x0 && p.y0 == others[i].y0 && p.x1 == others[i].x1 && p.y1 == others[i].y1)
          return;
      const Damage d{std::max(0, static_cast<int>(std::floor(p.x0))), std::max(0, static_cast<int>(std::floor(p.y0))),
                     std::min(kBrowserWidth, static_cast<int>(std::ceil(p.x1))),
                     std::min(kBrowserHeight, static_cast<int>(std::ceil(p.y1)))};
      if (d.x0 < d.x1 && d.y0 < d.y1) pointed.push_back(d);
    };
    for (int i = 0; i < pointedWasCount; ++i) add(pointedWas[i], now, count);
    for (int i = 0; i < count; ++i) add(now[i], pointedWas, pointedWasCount);
  }

  if (!stripAll && ticked) tickBoxes(bandTicked, clockTicked);
  if (panelShown(kMain) && strip) {  // only its rows shown now
    int first = 0, end = 0;
    shownRows(kMain, first, end);
    Damage boxes[kStripParts];
    int count = 0;
    if (stripAll) {
      boxes[count++] = {0, first, kMainWidth, end};
    } else {
      for (int i = 0; i < stripParts_; ++i) {
        const double* p = stripPart_[i];
        Damage d{std::max(0, static_cast<int>(std::floor(p[0]))), std::max(first, static_cast<int>(std::floor(p[1]))),
                 std::min(kMainWidth, static_cast<int>(std::ceil(p[2]))), std::min(end, static_cast<int>(std::ceil(p[3])))};
        if (d.x0 >= d.x1 || d.y0 >= d.y1) continue;
        // Whole pixels apart may meet: one box, so no pixel is drawn twice.
        for (int j = 0; j < count;) {
          const Damage& o = boxes[j];
          if (d.x0 < o.x1 && o.x0 < d.x1 && d.y0 < o.y1 && o.y0 < d.y1) {
            d = {std::min(d.x0, o.x0), std::min(d.y0, o.y0), std::max(d.x1, o.x1), std::max(d.y1, o.y1)};
            boxes[j] = boxes[--count];
            j = 0;
          } else {
            ++j;
          }
        }
        boxes[count++] = d;
      }
    }
    if (count > 0) {
      cairo_t* cr = canvases_[kMain].cr();
      cairo_save(cr);
      for (int i = 0; i < count; ++i)
        cairo_rectangle(cr, boxes[i].x0, boxes[i].y0, boxes[i].x1 - boxes[i].x0, boxes[i].y1 - boxes[i].y0);
      cairo_clip(cr);
      drawMain(cr);  // its frame replaces what was there
      cairo_restore(cr);
      for (int i = 0; i < count; ++i) canvases_[kMain].upload(boxes[i].y0, boxes[i].y1, boxes[i].x0, boxes[i].x1);
    }
    if (stripAll && !view_.keyPicking) {
      bandDrawn_ = bandMarks();
      bandDrawnKnown_ = true;
    }
  }
  stripParts_ = 0;
  if (!panelShown(kBrowser)) {
    // Changes asked of it are dropped, and so are those while it is hidden
    // after the redraw that hid it: what its texture shows is not known.
    if (browser && (asked || browserHidden_)) publishedKnown_ = false;
    browserHidden_ = true;
    browserRow_ = -1;
    return;
  }
  browserHidden_ = false;
  if (!browser) return;
  const auto shiftContent = [this] {
    return dialog_ == Dialog::WhatsNew ? shiftWhatsNew() : shiftList();
  };
  const auto drawPointed = [&] {
    for (const Damage& d : pointed) drawBrowserPart(d.y0, d.y1, d.x0, d.x1);
  };
  if (movedOnly && !whole) {
    if (sideMoved && !shiftSide()) {  // its columns alone
      drawBrowserPart(static_cast<int>(side_.y) - 4, static_cast<int>(side_.y + side_.h) + 4,
                      static_cast<int>(side_.x) - 4, static_cast<int>(side_.x + side_.w) + 4);
      drawnSide_ = side_.position;
    }
    if (!listMoved || shiftContent()) {
      drawPointed();
      return;
    }
  }
  int area = 0;
  for (const Damage& d : damage) area += (d.x1 - d.x0) * (d.y1 - d.y0);
  // Scrolled while something else changed (a tooltip went, a cover came):
  // the list still moves on the GPU, and the changes are drawn after it,
  // where they were and where it took them.
  if (!whole && !movedOnly && (listMoved || sideMoved) &&
      area <= kBrowserWidth * kBrowserHeight / 4) {
    const double listUp = dialog_ == Dialog::WhatsNew
                              ? whatsNew_.scroll - whatsNew_.drawn
                              : drawnScroll_ < 0
                                    ? 0
                                    : (scrollOffset_ + scrollFraction_ - drawnScroll_) * rowHeight_;
    const double sideUp = drawnSide_ < 0 ? 0 : side_.position - drawnSide_;
    if (sideMoved && !shiftSide()) {
      drawBrowserPart(static_cast<int>(side_.y) - 4, static_cast<int>(side_.y + side_.h) + 4,
                      static_cast<int>(side_.x) - 4, static_cast<int>(side_.x + side_.w) + 4);
      drawnSide_ = side_.position;
    }
    if (!listMoved || shiftContent()) {
      const int reach = static_cast<int>(std::ceil(std::max(std::abs(listUp), std::abs(sideUp))));
      for (const Damage& d : damage)
        drawBrowserPart(std::max(0, d.y0 - reach), std::min(kBrowserHeight, d.y1 + reach), d.x0,
                        d.x1);
      drawPointed();
      return;
    }
  }
  if (whole || (!listMoved && !sideMoved && area > kBrowserWidth * kBrowserHeight / 4)) {
    if (asked) publishedKnown_ = false;  // not drawn into the texture shown meanwhile
    if (!canvases_[kBrowser].beginUpdate()) {
      dirty_ = true;
      return;
    }
    drawnBand_ = scrollBand();
    // All of it drawn anew, a band at a time: what its pictures and logos
    // show is what the bands paint (pictureOf(), drawStudioMark()).
    shownPictures_.clear();
    logosComing_.clear();
    browserView_ = view_;
    browserOffset_ = scrollOffset_;
    browserFraction_ = scrollFraction_;
    browserSide_ = side_.position;
    browserPage_ = pageScroll_;
    browserWhatsNew_ = whatsNew_.scroll;
    // From the panel's top (a little above, the edge's margin clear).
    browserTop_ = static_cast<int>(panelTop_);
    browserRow_ = std::max(0, browserTop_ - static_cast<int>(kit::kSpace));
    drawingLookKnown_ = false;
    return;
  }
  if (!fullWidth) {
    for (const Damage& d : damage) drawBrowserPart(d.y0, d.y1, d.x0, d.x1);
  } else {
    drawBrowserPart(first, end, left, right);
  }
  drawPointed();
  if (first <= listTop_ - 2 && end >= listBottom_) drawnScroll_ = scrollOffset_ + scrollFraction_;
  if (side_.w > 0 && first <= side_.y - 2 && end >= side_.y + side_.h) drawnSide_ = side_.position;
  if (dialog_ == Dialog::WhatsNew && fullWidth) whatsNew_.drawn = whatsNew_.scroll;
}

// Draws the browser's rows [y0, y1) and columns [x0, x1) and uploads them.
void Menu::drawBrowserPart(int y0, int y1, int x0, int x1) {
  CairoCanvas& canvas = canvases_[kBrowser];
  cairo_t* cr = canvas.cr();
  if (browserRow_ < 0) lookStale_ = true;  // in place
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
  if ((page_ != Page::Files && page_ != Page::Library && page_ != Page::Offline && !networkList() &&
       !stashListPage()) ||
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
  // Left of the scroll bar, all of what scrolls: rows, cards and their buttons,
  // the playing one's shadow (clear of the page's rounded corners).
  const int x0 = static_cast<int>(std::floor(listLeft_ - kListShadowX));
  const int x1 = static_cast<int>(std::ceil(listRight_ - kListRightCut));
  canvases_[kBrowser].shiftUp(x0, y0, x1, y1, up);
  if (up > 0)
    drawBrowserPart(static_cast<int>(std::floor(y1 - up)), y1, x0, x1);
  else
    drawBrowserPart(y0, static_cast<int>(std::ceil(y0 - up)), x0, x1);
  // Only the track's columns (the rows end well left of them, drawButtons()),
  // and of them only where the thumb was and is now: the track near the
  // panel's edge is costly to paint whole on every step.
  for (const Button& b : buttons_)
    if (b.panel == kBrowser && b.scrollbar) {
      double tx = 0, tw = 0, thumbY = 0, thumbH = 0;
      scrollbarTrack(b, tx, tw);
      scrollbarThumb(b, thumbY, thumbH);
      const ScrollExtent e = scrollExtent(&b);
      const double range = e.total - e.shown;
      const double was =
          b.y + (b.h - thumbH) * (range > 0 ? std::clamp(drawnScroll_ / range, 0.0, 1.0) : 0.0);
      double top = std::max(b.y, std::min(was, thumbY));
      double bottom = std::min(b.y + b.h, std::max(was, thumbY) + thumbH);
      const bool pressed = drawnPressed(b);
      if (pressed && pressShrinks(b)) {  // held, drawn shrunk about its middle (drawButtons())
        const double middle = b.y + b.h / 2;
        top = middle + (top - middle) * kPressedScale;
        bottom = middle + (bottom - middle) * kPressedScale;
      }
      drawBrowserPart(static_cast<int>(std::floor(top)) - 1, static_cast<int>(std::ceil(bottom)) + 1,
                      static_cast<int>(std::floor(tx)) - 1, static_cast<int>(std::ceil(tx + tw)) + 1);
      // Held, its pushed-in surface reaches into the columns the rows moved in.
      if (pressed && b.x < x1) drawBrowserPart(y0, y1, static_cast<int>(std::floor(b.x)) - 1, x1);
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
  // Where it is, along its right edge, clear of the rows.
  drawBrowserPart(y0, y1, x1 - static_cast<int>(kScrollTrackW) - 1, x1);
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
    if (canvas.takeLost()) {
      dirty_ = true;
      if (&canvas == &canvases_[kBrowser]) publishedKnown_ = false;
    }
  if (panel == kSetup) {
    if (setup_->canvas.takeLost()) dirty_ = true;
    if (dirty_ || !setup_->damage.empty()) redraw();
    if (setup_->row >= 0) SetupPages::drawBand(*this);
    return setup_->canvas.texture();
  }
  const bool starting = dirty_;
  if (dirty_ || stripDirty_ || stripParts_ > 0 || bandTicked_ || clockTicked_ ||
      (panel == kBrowser && browserRow_ < 0 && (browserDirty_ || listMoved_ || sideMoved_)))
    redraw();
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
        publishedLook_ = drawingLook();
        publishedKnown_ = true;
        lookStale_ = false;
        browserRow_ = -1;
        browserColumn_ = pageAlone() ? kPageX : 0;
        drawnTop_ = browserTop_;
        drawnScroll_ = browserOffset_ + browserFraction_;
        drawnSide_ = browserSide_;
        if (dialog_ == Dialog::WhatsNew) whatsNew_.drawn = browserWhatsNew_;
      }
    } else if (browserRow_ < 0 && !dirty_) {
      updatePreview();  // a new layout may have moved another tile under the pointer
      if (browserDirty_) redraw();
      if (stash_ && !preview_.path.empty() && stash_->preview().update()) drawPreview();
    }
  }
  // Drawn in place with nothing left to draw (and no preview frame, until
  // its stop is drawn): the texture is the page as it is.
  if (lookStale_ && browserRow_ < 0 && !dirty_ && !browserDirty_ && !listMoved_ && !sideMoved_ &&
      preview_.path.empty() && panelShown(kBrowser)) {
    publishedLook_ = browserLook(scrollOffset_ + scrollFraction_, pageScroll_, side_.position, whatsNew_.scroll);
    lookStale_ = false;
  }
  return canvases_[panel].texture();
}
