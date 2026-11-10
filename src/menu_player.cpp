#include "menu_style.h"

#include "menu_stash.h"
#include "platform.h"

#include <ctime>

// The playback strip: along the top the clock,
// the tabs that open the larger panels above it and what is playing; across
// the strip a band that is the timeline, with the file name and the time on
// it; under it the tools on the left, the transport in the middle, then
// delete and the volume on the right. It is always there while the menu is
// open and never moves.

namespace {

std::string upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return s;
}

}  // namespace

void Menu::layoutMain() {
  if (view_.keyPicking) {
    layoutKeyPick();
    return;
  }
  const Strings& t = text();
  const PlaybackState& p = view_.playback;

  // Tabs: each opens its panel above the strip, or closes it if it is open.
  double x = kTabX;
  auto tab = [&](Icon icon, const char* label, bool open, std::function<void()> show) {
    Button b = textButton(label, icon,
                          [this, open, show] {
                            if (open)
                              showPage(Page::Main);
                            else
                              show();
                            return MenuCommand{};
                          },
                          open);
    b.tab = true;
    b.x = x;
    b.y = kTabY;
    b.w = textWidth(label, 26, false) + 80;
    b.h = kTabH;
    b.radius = kTabH / 2;
    b.fontSize = 26;
    buttons_.push_back(b);
    x += b.w + 10;
  };
  const bool browsing =
      page_ == Page::Files || page_ == Page::Library || page_ == Page::Settings;
  tab(Icon::MenuLines, t.tabMainMenu, browsing, [this] { showBrowser(); });
  tab(Icon::FormatDome, t.videoSettings, page_ == Page::VideoSettings,
      [this] { showPage(Page::VideoSettings); });
  tab(Icon::Focus, t.stereoFocus, page_ == Page::Focus, [this] { showPage(Page::Focus); });
  if (stash_) StashPages::layoutSceneTab(*this, x);  // Stash's Scene tab, while it is on
  Button quit = textButton(t.quitApp, Icon::Power, [this] {
    openDialog(Dialog::ConfirmQuit, "");
    return MenuCommand{};
  });
  quit.tab = true;
  quit.x = x;
  quit.y = kTabY;
  quit.w = textWidth(t.quitApp, 26, false) + 80;
  quit.h = kTabH;
  quit.radius = kTabH / 2;
  quit.fontSize = 26;
  quit.dangerHover = true;
  buttons_.push_back(quit);
  infoX_ = x + quit.w + 20;

  // What is playing: "5760 × 2880 · HEVC 10-bit · 59.9 fps" (the format is on
  // the video settings).
  std::string size, codec, rate;
  if (view_.videoWidth > 0)
    size = std::to_string(view_.videoWidth) + " × " + std::to_string(view_.videoHeight);
  if (!p.videoCodec.empty() && !view_.photo)
    codec = upper(p.videoCodec) + (p.pixelFormat.find("10") != std::string::npos ? " 10-bit" : "");
  if (p.fps > 0 && !view_.photo) {  // photos report a meaningless 1 fps
    char fps[32];
    std::snprintf(fps, sizeof(fps), "%.3g fps", p.fps);
    rate = fps;
  }

  // Labels after the tabs, where they fit: software decoding in red, its
  // reason in the tooltip; a found update, or else a crash the run before,
  // as the way to its button in Settings, in the summary's place.
  const double fpsWidth =
      view_.appFps > 0 ? textWidth(std::to_string(view_.appFps) + " FPS", 22, false) + 24 : 0;
  auto label = [&](const char* text, std::function<MenuCommand()> action) -> Button* {
    const double width = textWidth(text, 22, false) + 32;
    if (infoX_ + width > kMainWidth - kMargin - fpsWidth) return nullptr;
    Button b = textButton(text, Icon::None, std::move(action));
    b.x = infoX_;
    b.y = kTabY;
    b.w = width;
    b.h = kTabH;
    b.fontSize = 22;
    buttons_.push_back(b);
    infoX_ += width + 16;
    return &buttons_.back();
  };
  if (view_.softwareDecoding) {
    if (Button* b = label(t.softwareDecoding.label, [] { return MenuCommand{}; })) {
      b->danger = true;
      // The Frame decodes only H.264 and HEVC in hardware.
      if (kStandaloneHeadset && p.videoCodec != "h264" && p.videoCodec != "hevc") {
        char reason[256];
        std::snprintf(reason, sizeof(reason), t.softwareDecoding.noHardware,
                      upper(p.videoCodec).c_str());
        b->tooltip = reason;
      } else {
        b->tooltip = t.softwareDecoding.unavailable;
      }
    }
  }
  const bool update = view_.releaseUpdate == ReleaseUpdate::State::Available ||
                      view_.releaseUpdate == ReleaseUpdate::State::DownloadFailed;
  const bool notice = (update || view_.crashedLastRun) &&
                      label(update ? t.releaseUpdate.notice : t.diagnostics.crashed, [this, update] {
                        if (update) showUpdate();
                        else showPage(Page::Settings);
                        return MenuCommand{};
                      });
  // What is playing in the room left, without the codec, then the size, if
  // it doesn't fit.
  infoText_.clear();
  if (!notice) {
    for (const auto& parts : {std::vector<std::string>{size, codec, rate},
                              std::vector<std::string>{size, rate}, std::vector<std::string>{rate}}) {
      std::string text;
      for (const std::string& s : parts)
        if (!s.empty()) text += (text.empty() ? "" : " · ") + s;
      if (textWidth(text, 22, false) <= kMainWidth - kMargin - fpsWidth - infoX_) {
        infoText_ = text;
        break;
      }
    }
  }

  layoutTools();

  // Transport in the middle: previous, back, play/pause, stop, forward, next;
  // photos: previous, slideshow, stop and next. Stop closes what is open and
  // leaves the scene empty.
  const double y = kRowY + (kPlayButton - kToolSize) / 2;
  const int count = view_.photo ? 4 : 6;
  double left = (kMainWidth - (count - 1) * (kToolSize + kTransportGap) - kPlayButton) / 2;
  auto icon = [&](Icon i, const char* tip, std::function<MenuCommand()> action) -> Button& {
    Button& b = addRound(i, tip, std::move(action), left, y, kToolSize);
    b.bare = true;
    left += kToolSize + kTransportGap;
    return b;
  };
  auto play = [&](Icon i, const std::string& tip, std::function<MenuCommand()> action) {
    addRound(i, tip, std::move(action), left, kRowY, kPlayButton, true);
    left += kPlayButton + kTransportGap;
  };
  double slideshowLeft = 0;
  icon(Icon::Previous, t.tipPrevious, command(MenuCommand::PreviousVideo));
  if (view_.photo) {
    // The slideshow button opens the choice of seconds; while the slideshow
    // runs it is a pause button that stops it.
    const bool running = view_.slideshowSeconds > 0;
    std::string tip = t.tipSlideshow;
    if (running) tip = std::string(t.tipPause) + " (" + std::to_string(view_.slideshowSeconds) + " s)";
    std::function<MenuCommand()> action = [this] {
      dropdown_ = dropdown_ == Dropdown::Slideshow ? Dropdown::None : Dropdown::Slideshow;
      return MenuCommand{};
    };
    if (running) action = command(MenuCommand::SetSlideshow, 0);
    slideshowLeft = left;
    play(running ? Icon::Pause : Icon::Slideshow, tip, std::move(action));
  } else {
    icon(Icon::Back10, t.tipBack10, command(MenuCommand::Seek, -10));
    play(p.paused ? Icon::Play : Icon::Pause, p.paused ? t.tipPlay : t.tipPause,
         command(MenuCommand::TogglePause));
  }
  icon(Icon::Stop, t.tipStop, command(MenuCommand::Stop)).disabled = view_.currentFile.empty();
  if (!view_.photo) icon(Icon::Forward10, t.tipForward10, command(MenuCommand::Seek, 10));
  icon(Icon::Next, t.tipNext, command(MenuCommand::NextVideo));

  // Delete the playing file, between a video's Next and the speaker, so it
  // stays put for photos' shorter transport. It opens its dialog at once; the
  // app fills it in (askDelete()).
  if (view_.deletable != MenuView::Deletable::No) {
    const bool can = view_.deletable == MenuView::Deletable::Yes;
    const char* tip = view_.deletable == MenuView::Deletable::NeedsSendToStash
                          ? t.deletion.tipNeedsSendToStash
                      : view_.deletable == MenuView::Deletable::StashDown ? t.deletion.tipStashDown
                      : view_.photo                                       ? t.tipDeletePhoto
                                                                          : t.tipDelete;
    const double videoNextEnd = (kMainWidth + 5 * (kToolSize + kTransportGap) + kPlayButton) / 2;
    Button& trash = addRound(Icon::Trash, tip,
                             [this] {
                               deleteInfo_ = {};
                               deleteInfo_.file = view_.currentFile;
                               deleteInfo_.name = view_.currentFile.substr(
                                   view_.currentFile.find_last_of('/') + 1);
                               deleteInfo_.photo = view_.photo;
                               openDialog(Dialog::ConfirmDelete, view_.currentFile);
                               MenuCommand c;
                               c.type = MenuCommand::RequestDelete;
                               c.path = view_.currentFile;
                               return c;
                             },
                             (videoNextEnd + kVolumeX - kToolSize) / 2, y, kToolSize);
    trash.bare = true;
    trash.dangerHover = can;
    trash.disabled = !can;
  }

  // Volume on the right: the speaker mutes, the slider sets it.
  Button& mute = addRound(p.muted ? Icon::Mute : Icon::VolumeUp, t.tipMute,
                          command(MenuCommand::ToggleMute), kVolumeX, y, kToolSize, p.muted);
  mute.bare = !p.muted;
  Button volume;
  volume.x = kVolumeX + kToolSize + 6;
  volume.y = kRowY;
  volume.w = kMainWidth - kMargin + 4 - volume.x;
  volume.h = kPlayButton;
  volume.radius = kPlayButton / 2;
  volume.volumeSlider = true;
  volume.tooltip = t.tipVolume;
  volume.action = [] { return MenuCommand{}; };  // handled in click()/drag()
  buttons_.push_back(volume);

  if (view_.photo && dropdown_ == Dropdown::Slideshow) {
    // The seconds as a strip above the slideshow button.
    Button opener;
    opener.x = slideshowLeft;
    opener.y = kRowY;
    opener.w = opener.h = kPlayButton;
    std::vector<Button> entries;
    for (int seconds : kSlideshowSeconds) {
      Button e;
      e.label = seconds == 0 ? std::string(t.off) : std::to_string(seconds) + " s";
      e.selected = view_.slideshowSeconds == seconds;
      e.action = [this, seconds] {
        dropdown_ = Dropdown::None;
        MenuCommand c;
        c.type = MenuCommand::SetSlideshow;
        c.value = seconds;
        return c;
      };
      entries.push_back(e);
    }
    addDropdown(opener, entries);
  }
}

// The tools left of the transport: recenter, the running video's rating,
// and small icons for the audio track, subtitles and chapters, each opening
// its list above it.
void Menu::layoutTools() {
  const Strings& t = text();
  const PlaybackState& p = view_.playback;
  const double y = kRowY + (kPlayButton - kToolSize) / 2;
  double x = kMargin;
  auto tool = [&](Icon icon, const std::string& tip, std::function<MenuCommand()> action,
                  bool on) -> Button& {
    Button& b = addRound(icon, tip, std::move(action), x, y, kToolSize, on);
    b.bare = !on;
    x += kToolSize + 8;
    return b;
  };
  auto toggle = [this](Dropdown d) {
    return [this, d] {
      dropdown_ = dropdown_ == d ? Dropdown::None : d;
      return MenuCommand{};
    };
  };
  tool(Icon::Recenter, t.recenter, command(MenuCommand::Recenter), false);
  if (!view_.currentFile.empty() && !view_.streaming && !view_.photo && library_ &&
      library_->available()) {
    Button stars = ratingButton(view_.currentFile, current_.rating, kMain,
                                x - 4 + kRatingLeadW, y, kStripStarW, kToolSize);
    buttons_.push_back(stars);
    x += kRatingLeadW + stars.w + 4;
  }

  // The selected tracks, named in the tooltips. The audio and subtitle lists
  // are only there when there is something to choose from.
  const Track* audio = nullptr;
  const Track* subtitle = nullptr;
  int audioTracks = 0;
  bool anySubtitles = false;
  for (const Track& track : p.tracks) {
    if (track.type == "audio") ++audioTracks;
    anySubtitles = anySubtitles || track.type == "sub";
    if (track.selected && track.type == "audio") audio = &track;
    if (track.selected && track.type == "sub") subtitle = &track;
  }
  std::vector<std::pair<Dropdown, size_t>> opens;  // each list and its opener
  if (audioTracks > 1 && !view_.photo) {
    std::string label = audio ? upper(audio->codec) : std::string(t.noAudio);
    if (audio && p.audioChannels == 1) label += " Mono";
    else if (audio && p.audioChannels == 2) label += " Stereo";
    else if (audio && p.audioChannels == 6) label += " 5.1";
    else if (audio && p.audioChannels == 8) label += " 7.1";
    if (audio) label += " (" + std::string(t.track) + " " + std::to_string(audio->id) + ")";
    tool(Icon::Audio, label, toggle(Dropdown::Audio), dropdown_ == Dropdown::Audio);
    opens.emplace_back(Dropdown::Audio, buttons_.size() - 1);
  }
  if (anySubtitles && !view_.photo) {
    tool(Icon::Subtitles,
         std::string(t.subtitles) + ": " + (subtitle ? trackName(*subtitle) : std::string(t.off)),
         toggle(Dropdown::Subtitles), dropdown_ == Dropdown::Subtitles);
    opens.emplace_back(Dropdown::Subtitles, buttons_.size() - 1);
  }
  if (p.chapters.size() > 1 && !view_.photo) {
    tool(Icon::Chapters,
         std::string(t.chapter) + " " + std::to_string(std::max(0, p.chapter) + 1) + "/" +
             std::to_string(p.chapters.size()),
         toggle(Dropdown::Chapters), dropdown_ == Dropdown::Chapters);
    opens.emplace_back(Dropdown::Chapters, buttons_.size() - 1);
  }

  // The open list, above its icon.
  const auto open = std::find_if(opens.begin(), opens.end(),
                                 [&](const auto& o) { return o.first == dropdown_; });
  if (open == opens.end()) return;
  const Button opener = buttons_[open->second];
  std::vector<Button> entries;
  auto entry = [&](const std::string& label, bool selected, std::function<MenuCommand()> action) {
    Button e;
    e.label = label;
    e.selected = selected;
    e.action = [this, action] {
      dropdown_ = Dropdown::None;
      return action();
    };
    entries.push_back(e);
  };
  if (dropdown_ == Dropdown::Chapters) {
    // Up to 15 chapters around the current one.
    const int count = static_cast<int>(p.chapters.size());
    const int first = std::clamp(p.chapter - 7, 0, std::max(0, count - 15));
    for (int i = first; i < std::min(count, first + 15); ++i) {
      const Chapter& c = p.chapters[i];
      entry(std::to_string(i + 1) + ".  " + (c.title.empty() ? "" : c.title + "  ·  ") +
                formatTime(c.time),
            i == p.chapter, command(MenuCommand::SeekTo, c.time));
    }
  } else if (dropdown_ == Dropdown::Audio) {
    for (const Track& track : p.tracks)
      if (track.type == "audio")
        entry(trackName(track) + (track.codec.empty() ? "" : "  ·  " + upper(track.codec)),
              track.selected, command(MenuCommand::SelectAudio, static_cast<double>(track.id)));
  } else {
    entry(t.off, subtitle == nullptr, command(MenuCommand::SelectSubtitle, 0));
    for (const Track& track : p.tracks)
      if (track.type == "sub")
        entry(trackName(track), track.selected,
              command(MenuCommand::SelectSubtitle, static_cast<double>(track.id)));
    entry(std::string(t.subtitleFromFile) + " …", false, [this] {
      openFiles(FileKind::Subtitle);
      return MenuCommand{};
    });
  }
  addDropdownList(opener, entries);
}

// The tabs' panel "Main menu": the browser where it was last.
void Menu::showBrowser() {
  if (!showListPage(lastBrowserPage_)) showPage(lastBrowserPage_);
}

void Menu::showPlayedFrom() {
  if (!showListPage(playedFrom_)) showBrowser();
}

MenuCommand Menu::openVideo(const std::string& path) {
  playedFrom_ = page_;
  opening_ = path;
  showPage(Page::Main);
  opening_.clear();
  MenuCommand c;
  c.type = MenuCommand::OpenFile;
  c.path = path;
  return c;
}

bool Menu::showListPage(Page page) {
  switch (page) {
    case Page::Files: showFileBrowser(); return true;
    case Page::Library: showLibrary(); return true;
    case Page::Offline:
      if (!offline_) return false;
      showOffline();
      return true;
    default: return stash_ && stashMenu_ && StashPages::showAgain(*this, page);
  }
}

// "English (eng)", "eng" or "Track 2".
std::string Menu::trackName(const Track& track) const {
  std::string s = !track.title.empty() ? track.title
                  : !track.lang.empty() ? track.lang
                                        : std::string(text().track) + " " + std::to_string(track.id);
  if (!track.title.empty() && !track.lang.empty()) s += " (" + track.lang + ")";
  return s;
}

double Menu::seekLineY() const { return kBandY + kBandH / 2; }

double Menu::seekAtPointer() const {
  const double fraction = std::clamp((pointerX_ - kMargin) / (kMainWidth - 2 * kMargin), 0.0, 1.0);
  return fraction * view_.playback.duration;
}

bool Menu::seekHover(double& fraction) const {
  if (pointerPanel_ != kMain || dialog_ != Dialog::None || dropdown_ != Dropdown::None ||
      view_.playback.duration <= 0 || view_.photo)
    return false;
  // While dragging the knob, the preview stays even if the pointer drifts off the bar.
  if (!draggingSeek_ && std::abs(pointerY_ - seekLineY()) > kSeekBand) return false;
  fraction = std::clamp((pointerX_ - kMargin) / (kMainWidth - 2 * kMargin), 0.0, 1.0);
  return true;
}

void Menu::seekBarPoint(double fraction, float& u, float& v) const {
  u = static_cast<float>((kMargin + std::clamp(fraction, 0.0, 1.0) * (kMainWidth - 2 * kMargin)) /
                         kMainWidth);
  v = static_cast<float>(seekLineY() / kMainHeight);
}

// The clock and what is playing along the top, then the timeline band: a
// field filled blue up to the position (the downloaded part of a stream
// lighter beyond it), chapter marks, the file name on its left and the
// time on its right.
void Menu::drawMainPage(cairo_t* cr) {
  if (view_.keyPicking) {
    drawKeyPick(cr);
    return;
  }
  const Strings& t = text();
  const PlaybackState& p = view_.playback;
  const Palette& pal = palette();
  char clock[16];
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  std::strftime(clock, sizeof(clock), "%H:%M", &local);
  drawText(cr, clock, kMargin, kTabY, kTabX - kMargin - 10, kTabH, kTextSize, false, false, pal.text.r,
           pal.text.g, pal.text.b);
  // The frames the app draws per second, in the top right corner;
  // what is playing between it and the tabs.
  double infoRight = kMainWidth - kMargin;
  if (view_.appFps > 0) {
    const std::string fps = std::to_string(view_.appFps) + " FPS";
    const double fpsW = textWidth(fps, 22, false);
    drawText(cr, fps, infoRight - fpsW - 2, kTabY, fpsW + 4, kTabH, 22, false, false, pal.text.r,
             pal.text.g, pal.text.b);
    infoRight -= fpsW + 24;
  }
  drawText(cr, infoText_, infoX_, kTabY, infoRight - infoX_, kTabH, 22, false, false,
           pal.textDim.r, pal.textDim.g, pal.textDim.b);

  const double x = kMargin, w = kMainWidth - 2 * kMargin;
  roundedRect(cr, x, kBandY, w, kBandH, 12);
  setColor(cr, pal.field);
  cairo_fill_preserve(cr);
  const bool timeline = !view_.photo && p.duration > 0;  // photos have no time to show or seek in
  if (timeline) {
    // While it is dragged, the band shows where it is being dragged to.
    const double shown = draggingSeek_ && lastScrub_ >= 0 ? lastScrub_ : p.position;
    const double fraction = std::clamp(shown / p.duration, 0.0, 1.0);
    cairo_save(cr);
    cairo_clip(cr);
    if (view_.streaming) {
      cairo_rectangle(cr, x, kBandY, w * std::clamp(p.bufferedUntil / p.duration, 0.0, 1.0),
                      kBandH);
      setColor(cr, pal.buffered);
      cairo_fill(cr);
    }
    cairo_rectangle(cr, x, kBandY, w * fraction, kBandH);
    setColor(cr, pal.accent);
    cairo_fill(cr);
    if (p.chapters.size() > 1) {
      for (const Chapter& c : p.chapters)
        if (c.time > 0 && c.time < p.duration)
          cairo_rectangle(cr, x + w * c.time / p.duration - 1.5, kBandY, 3, kBandH);
      cairo_set_source_rgba(cr, 1, 1, 1, 0.45);
      cairo_fill(cr);
    }
    cairo_restore(cr);
  }
  cairo_new_path(cr);
  const std::string title = view_.currentFile.empty() ? t.noVideo
                            : view_.playbackFailed    ? (view_.discEncrypted ? t.discEncrypted : t.playbackFailed)
                            : p.title.empty()         ? t.loading
                                                      : p.title;
  double titleRight = x + w - 24;
  if (timeline) {
    const double shownTime = draggingSeek_ && lastScrub_ >= 0 ? lastScrub_ : p.position;
    const std::string time = formatTime(shownTime) + " / " + formatTime(p.duration);
    const double timeW = textWidth(time, 27, false);
    drawText(cr, time, titleRight - timeW - 2, kBandY, timeW + 4, kBandH, 27, false, false, 1, 1,
             1);
    titleRight -= timeW + 32;
  }
  if (view_.playbackFailed)  // in the red of failures elsewhere in the menu
    drawText(cr, title, x + 24, kBandY, titleRight - x - 24, kBandH, 27, false, false, 1.0, 0.55, 0.5);
  else
    drawText(cr, StashPages::playingTitle(*this, title), x + 24, kBandY, titleRight - x - 24,
             kBandH, 27, false, false, 1, 1, 1);
}

// Picking the background key's colour: the browser is hidden, and the
// strip says what to do, with Cancel under it (the menu button cancels too).
void Menu::layoutKeyPick() {
  Button cancel = textButton(text().cancel, Icon::None, command(MenuCommand::CancelKeyPick));
  cancel.w = std::max(cancel.w, 220.0);
  cancel.h = 64;
  cancel.x = (kMainWidth - cancel.w) / 2;
  cancel.y = kStripBodyY + kStripBodyHeight - cancel.h - 28;
  buttons_.push_back(cancel);
}

void Menu::drawKeyPick(cairo_t* cr) {
  const Strings& t = text();
  const Palette& pal = palette();
  const double x = kMargin, w = kMainWidth - 2 * kMargin;
  drawText(cr, t.keyPick, x, kStripBodyY + 26, w, 48, 30, true, true, pal.text.r, pal.text.g,
           pal.text.b);
  // Under it what it does, or (brighter) why the last press took no colour.
  const KeyPickNote note = view_.keyPickNote;
  const char* detail = note == KeyPickNote::Uneven       ? t.keyPickUneven
                       : note == KeyPickNote::Unreadable ? t.keyPickUnreadable
                                                         : t.keyPickDetail;
  const Rgba& c = note == KeyPickNote::None ? pal.textDim : pal.text;
  const double rgba[4] = {c.r, c.g, c.b, c.a};
  drawTextLines(cr, detail, x, kStripBodyY + 80, w, kTextSize, 2, rgba, true);
}
