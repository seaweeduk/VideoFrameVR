#include "menu_parts.h"
#include "menu_style.h"

#include "menu_stash.h"
#include "platform.h"

#include <ctime>

// The playback strip: along the top the clock,
// the tabs that open the larger panels above it and what is playing; across
// the strip a band that is the timeline, with the file name and the time on
// it; under it the tools on the left, the transport in the middle, then
// what happens at the end of a video, delete and the volume on the right.
// It is always there while the menu is open and never moves.

namespace {

std::string upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return s;
}

// The codecs the Frame decodes in hardware: AV1 too where its decoder takes it.
bool hardwareCodec(const std::string& codec, bool av1) {
  return codec == "h264" || codec == "hevc" || (av1 && codec == "av1");
}

}  // namespace

void Menu::layoutMain() {
  if (view_.keyPicking) {
    layoutKeyPick();
    return;
  }
  const Strings& t = text();
  const PlaybackState& p = view_.playback;
  StripLayout strip(*this);
  using namespace strip;

  // The top row: the clock; the tabs, each opening its panel above the strip
  // or closing it if it is open; labels; what is playing; Quit at the end.
  strip.beginTabs(kLeft + kInset + textWidth("00:00", kClockSize, false) + kit::kSpace);
  auto tab = [&](Icon icon, const char* label, bool open, std::function<void()> show) {
    return strip.tab(label, icon, open, [this, open, show] {
      if (open)
        showPage(Page::Main);
      else
        show();
      return MenuCommand{};
    });
  };
  // Any of the main menu's own pages (Stash's Scene tab, Video settings and
  // Stereo focus have tabs of their own).
  const bool browsing = page_ == Page::Files || page_ == Page::Library || page_ == Page::Settings ||
                        page_ == Page::Network || page_ == Page::Offline ||
                        page_ == Page::Places || stashListPage();
  tab(Icon::MenuLines, t.tabMainMenu, browsing, [this] { showBrowser(); });
  tab(Icon::FormatDome, t.videoSettings, page_ == Page::VideoSettings,
      [this] { showPage(Page::VideoSettings); });
  tab(Icon::Focus, t.stereoFocus, page_ == Page::Focus, [this] { showPage(Page::Focus); });
  if (stash_) StashPages::layoutSceneTab(*this, strip);  // Stash's Scene tab, while it is on
  infoX_ = strip.endTabs() + kit::kSpace;
  const Button& quit = strip.quit(kRight, t.quitApp, [this] {
    openDialog(Dialog::ConfirmQuit, "");
    return MenuCommand{};
  });
  // The frames the app draws per second just before Quit (drawMainPage()).
  infoRight_ = quit.x - kit::kSpace;
  if (view_.appFps > 0)
    infoRight_ -= textWidth(std::to_string(view_.appFps) + " FPS", kInfoSize, false) + kit::kSpace;

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
  auto chip = [&](const char* text, bool bad, std::function<MenuCommand()> action) -> Button* {
    const double width = strip.chipWidth(text);
    if (infoX_ + width > infoRight_) return nullptr;
    Button& b = strip.chip(infoX_, text, bad, std::move(action));
    infoX_ += width + kit::kSpace;
    return &b;
  };
  if (view_.softwareDecoding) {
    if (Button* b = chip(t.softwareDecoding.label, true, [] { return MenuCommand{}; })) {
      // The Frame decodes only H.264 and HEVC in hardware.
      if (kStandaloneHeadset && !hardwareCodec(p.videoCodec, view_.hardwareAv1)) {
        char reason[256];
        std::snprintf(reason, sizeof(reason), t.softwareDecoding.noHardware,
                      upper(p.videoCodec).c_str());
        b->tooltip = reason;
      } else {
        b->tooltip = t.softwareDecoding.unavailable;
      }
    }
  }
  // The video's funscript: red while no device plays it, and the way to
  // its settings.
  if (view_.scriptStatus.script) {
    const bool ready = view_.scriptStatus.link == ScriptLink::Ready;
    if (Button* b = chip(t.scripts.label, !ready, [this] {
          showScriptSettings();
          return MenuCommand{};
        })) {
      if (ready) {
        char playing[512];
        std::snprintf(playing, sizeof(playing), t.scripts.tipPlaying, view_.scriptStatus.device.c_str());
        b->tooltip = playing;
      } else {
        b->tooltip = std::string(t.scripts.tipNoDevice) + "\n" + scriptStatusText();
      }
    }
  }
  const bool update = view_.releaseUpdate == ReleaseUpdate::State::Available ||
                      view_.releaseUpdate == ReleaseUpdate::State::DownloadFailed;
  const bool notice = (update || view_.crashedLastRun) &&
                      chip(update ? t.releaseUpdate.notice : t.diagnostics.crashed, false,
                           [this, update] {
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
      if (textWidth(text, kInfoSize, false) <= infoRight_ - infoX_) {
        infoText_ = text;
        break;
      }
    }
  }

  // The band: the timeline, pointed as a whole (click() and drag() seek).
  if (!view_.photo && p.duration > 0) {
    Button band;
    band.band = true;
    band.x = kLeft;
    band.y = kBandY;
    band.w = kRight - kLeft;
    band.h = kBandH;
    band.radius = kit::kCorner;
    band.action = [] { return MenuCommand{}; };
    buttons_.push_back(band);
  }

  layoutTools(strip);

  // Transport in the middle: previous, back, play/pause, stop, forward, next;
  // photos: previous, slideshow, stop and next. Stop closes what is open and
  // leaves the scene empty.
  const int count = view_.photo ? 4 : 6;
  double x = (kMainWidth - (count - 1) * (kButton + kButtonGap) - StripLayout::kPlayW) / 2;
  auto round = [&](Icon i, const char* tip, std::function<MenuCommand()> action) -> Button& {
    Button& b = strip.round(x, i, tip, std::move(action));
    x += b.w + kButtonGap;
    return b;
  };
  auto play = [&](Icon i, const std::string& tip, std::function<MenuCommand()> action) -> Button& {
    Button& b = strip.play(x, i, tip, std::move(action));
    x += b.w + kButtonGap;
    return b;
  };
  // Nothing open: nothing to play, seek, stop, or step from (Previous and
  // Next go from the open file).
  const bool none = view_.currentFile.empty();
  round(Icon::Previous, t.tipPrevious, command(MenuCommand::PreviousVideo)).disabled = none;
  const bool slideshowOpen = view_.photo && dropdown_ == Dropdown::Slideshow;
  Button slideshow;
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
    slideshow = play(running ? Icon::Pause : Icon::Slideshow, tip, std::move(action));
  } else {
    round(Icon::Back10, t.tipBack10, command(MenuCommand::Seek, -10)).disabled = none;
    const bool paused = p.paused || none;
    Button& playPause = play(paused ? Icon::Play : Icon::Pause, paused ? t.tipPlay : t.tipPause,
                             command(MenuCommand::TogglePause));
    if (none) {  // as Stop beside it: flush, faded
      playPause.primary = false;
      playPause.bare = true;
      playPause.disabled = true;
    }
  }
  round(Icon::Stop, t.tipStop, command(MenuCommand::Stop)).disabled = none;
  if (!view_.photo) round(Icon::Forward10, t.tipForward10, command(MenuCommand::Seek, 10)).disabled = none;
  round(Icon::Next, t.tipNext, command(MenuCommand::NextVideo)).disabled = none;

  // What happens when a video ends, after its Next: the choice as the icon,
  // latched for those that last only until the player quits (Repeat,
  // Shuffle), and the choices in a list above it. Photos have their
  // slideshow instead, and Delete stays where it is for videos.
  const double afterX = (kMainWidth + 5 * (kButton + kButtonGap) + StripLayout::kPlayW) / 2 + kButtonGap;
  static constexpr Icon kAfterIcons[] = {Icon::StopAtEnd, Icon::Repeat, Icon::PlayNext,
                                         Icon::Shuffle};
  const int after = static_cast<int>(view_.afterVideo);
  const bool passing = view_.afterVideo == AfterVideo::Repeat || view_.afterVideo == AfterVideo::Shuffle;
  Button afterButton;
  if (!view_.photo)
    afterButton = strip.round(afterX, kAfterIcons[after], t.afterVideo.choices[after],
                              [this] {
                                dropdown_ = dropdown_ == Dropdown::AfterVideo ? Dropdown::None
                                                                              : Dropdown::AfterVideo;
                                return MenuCommand{};
                              },
                              passing || dropdown_ == Dropdown::AfterVideo);

  // Delete the playing file, a cluster on its own. It opens its dialog at
  // once; the app fills it in (askDelete()).
  if (view_.deletable != MenuView::Deletable::No) {
    const bool can = view_.deletable == MenuView::Deletable::Yes;
    const char* tip = view_.deletable == MenuView::Deletable::NeedsSendToStash
                          ? t.deletion.tipNeedsSendToStash
                      : view_.deletable == MenuView::Deletable::StashDown ? t.deletion.tipStashDown
                      : view_.photo                                       ? t.tipDeletePhoto
                                                                          : t.tipDelete;
    Button& trash = strip.round(afterX + kButton + kCluster, Icon::Trash, tip, [this] {
      deleteInfo_ = {};
      deleteInfo_.file = view_.currentFile;
      deleteInfo_.name = view_.currentFile.substr(view_.currentFile.find_last_of('/') + 1);
      deleteInfo_.photo = view_.photo;
      openDialog(Dialog::ConfirmDelete, view_.currentFile);
      MenuCommand c;
      c.type = MenuCommand::RequestDelete;
      c.path = view_.currentFile;
      return c;
    });
    trash.dangerHover = can;
    trash.disabled = !can;
  }

  // Volume on the right: the speaker mutes, the slider sets it, its number
  // last (drawVolumeSlider()).
  const double volumeX = kRight - kInset - kNumberW - kit::kSpace - kVolumeW;
  strip.round(volumeX - 12 - kButton, p.muted ? Icon::Mute : Icon::VolumeUp, t.tipMute,
              command(MenuCommand::ToggleMute), p.muted);
  strip.volume(volumeX, t.tipVolume);

  if (slideshowOpen) {  // the seconds above the slideshow button
    std::vector<StripLayout::Item> items;
    for (int seconds : kSlideshowSeconds) {
      StripLayout::Item e;
      e.label = seconds == 0 ? std::string(t.off) : std::to_string(seconds) + " s";
      e.chosen = view_.slideshowSeconds == seconds;
      e.action = [this, seconds] {
        dropdown_ = Dropdown::None;
        MenuCommand c;
        c.type = MenuCommand::SetSlideshow;
        c.value = seconds;
        return c;
      };
      items.push_back(e);
    }
    strip.list(slideshow, items);
  }
  if (!view_.photo && dropdown_ == Dropdown::AfterVideo) {
    std::vector<StripLayout::Item> items;
    for (const AfterVideo choice : {AfterVideo::Next, AfterVideo::Shuffle, AfterVideo::Repeat,
                                    AfterVideo::Stop}) {
      const int i = static_cast<int>(choice);
      StripLayout::Item e;
      e.icon = kAfterIcons[i];
      e.label = t.afterVideo.choices[i];
      e.tooltip = t.afterVideo.tips[i];
      e.chosen = i == after;
      e.action = [this, i] {
        dropdown_ = Dropdown::None;
        return command(MenuCommand::SetAfterVideo, i)();
      };
      items.push_back(e);
    }
    strip.list(afterButton, items);
  }
}

// The tools left of the transport: recenter, the running video's rating,
// and the audio track, subtitles and chapters, each opening its list above it.
void Menu::layoutTools(StripLayout& strip) {
  const Strings& t = text();
  const PlaybackState& p = view_.playback;
  double x = strip::kLeft;
  auto tool = [&](Icon icon, const std::string& tip, std::function<MenuCommand()> action,
                  bool on) -> Button& {
    Button& b = strip.round(x, icon, tip, std::move(action), on);
    x += b.w + strip::kButtonGap;
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
    strip.stars(x, view_.currentFile, current_.rating);
    x += StripLayout::kStarsW + strip::kButtonGap;
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

  // The open list, above its button.
  const auto open = std::find_if(opens.begin(), opens.end(),
                                 [&](const auto& o) { return o.first == dropdown_; });
  if (open == opens.end()) return;
  const Button opener = buttons_[open->second];
  std::vector<StripLayout::Item> items;
  auto item = [&](const std::string& label, const std::string& sub, bool chosen,
                  std::function<MenuCommand()> action) {
    StripLayout::Item e;
    e.label = label;
    e.sub = sub;
    e.chosen = chosen;
    e.action = [this, action] {
      dropdown_ = Dropdown::None;
      return action();
    };
    items.push_back(e);
  };
  if (dropdown_ == Dropdown::Chapters) {
    // Up to 15 chapters around the current one.
    const int count = static_cast<int>(p.chapters.size());
    const int first = std::clamp(p.chapter - 7, 0, std::max(0, count - 15));
    for (int i = first; i < std::min(count, first + 15); ++i) {
      const Chapter& c = p.chapters[i];
      item(std::to_string(i + 1) + "." + (c.title.empty() ? "" : "  " + c.title),
           formatTime(c.time), i == p.chapter, command(MenuCommand::SeekTo, c.time));
    }
  } else if (dropdown_ == Dropdown::Audio) {
    for (const Track& track : p.tracks)
      if (track.type == "audio")
        item(trackName(track), upper(track.codec), track.selected,
             command(MenuCommand::SelectAudio, static_cast<double>(track.id)));
  } else {
    item(t.off, "", subtitle == nullptr, command(MenuCommand::SelectSubtitle, 0));
    for (const Track& track : p.tracks)
      if (track.type == "sub")
        item(trackName(track), "", track.selected,
             command(MenuCommand::SelectSubtitle, static_cast<double>(track.id)));
    item(std::string(t.subtitleFromFile) + " …", "", false, [this] {
      openFiles(FileKind::Subtitle);
      return MenuCommand{};
    });
  }
  strip.list(opener, items);
}

// The tabs' panel "Main menu": the browser where it was last.
void Menu::showBrowser() {
  if (!showListPage(lastBrowserPage_)) showPage(lastBrowserPage_);
}

void Menu::showPlayedFrom() {
  if (!showListPage(playedFrom_)) showBrowser();
}

MenuCommand Menu::openVideo(const std::string& path, bool shuffle) {
  playedFrom_ = page_;
  opening_ = path;
  showPage(Page::Main);
  opening_.clear();
  MenuCommand c;
  c.type = MenuCommand::OpenFile;
  c.path = path;
  c.option = shuffle ? 1 : 0;
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

double Menu::seekLineY() const { return strip::kBandY + strip::kBandH / 2; }

double Menu::seekAtPointer() const {
  const double fraction =
      std::clamp((pointerX_ - strip::kLeft) / (strip::kRight - strip::kLeft), 0.0, 1.0);
  return fraction * view_.playback.duration;
}

// Popups (an open list's entries and the pane behind them, a dialog's
// buttons, the keyboard's accents) are drawn over the bar and take the
// pointer first. While a key colour is picked the strip shows no bar.
bool Menu::onSeekBand() const {
  if (pointerPanel_ != kMain || std::abs(pointerY_ - seekLineY()) > strip::kSeekBand ||
      view_.playback.duration <= 0 || view_.photo || view_.keyPicking)
    return false;
  const int index = buttonAt(kMain, pointerX_, pointerY_);
  return (index < 0 || !buttons_[index].popup) && !onPopups(kMain, pointerX_, pointerY_);
}

bool Menu::seekHover(double& fraction) const {
  if (pointerPanel_ != kMain || dialog_ != Dialog::None || dropdown_ != Dropdown::None ||
      view_.playback.duration <= 0 || view_.photo || view_.keyPicking)
    return false;
  // While dragging the knob, the preview stays even if the pointer drifts off the bar.
  if (!draggingSeek_ && !onSeekBand()) return false;
  fraction = std::clamp((pointerX_ - strip::kLeft) / (strip::kRight - strip::kLeft), 0.0, 1.0);
  return true;
}

void Menu::seekBarPoint(double fraction, float& u, float& v) const {
  u = static_cast<float>(
      (strip::kLeft + std::clamp(fraction, 0.0, 1.0) * (strip::kRight - strip::kLeft)) / kMainWidth);
  v = static_cast<float>(seekLineY() / kMainHeight);
}

Menu::BandMarks Menu::bandMarks() const {
  using namespace strip;
  const PlaybackState& p = view_.playback;
  const double x = kLeft, w = kRight - kLeft;
  BandMarks m;
  m.timeline = !view_.photo && p.duration > 0;  // photos have no time to show or seek in
  m.dragging = draggingSeek_;
  // While it is dragged, the band shows where it is being dragged to.
  const double shown = draggingSeek_ && lastScrub_ >= 0 ? lastScrub_ : p.position;
  m.fraction = m.timeline ? std::clamp(shown / p.duration, 0.0, 1.0) : 0;
  m.covered = std::floor((x + w * m.fraction) * menu_geometry::kRasterScale) / menu_geometry::kRasterScale;
  m.titleRight = x + w - kInset;
  if (!m.timeline) return m;
  if (view_.streaming) m.buffered = w * std::clamp(p.bufferedUntil / p.duration, 0.0, 1.0);
  m.at = formatTime(shown);
  m.of = " / " + formatTime(p.duration);
  m.atW = textWidth(m.at, kBandSize, false);
  m.ofW = textWidth(m.of, kBandSize, false);
  m.titleRight -= m.atW + m.ofW + 24;
  return m;
}

// The clock, what is playing and the frame rate along the top, then the
// band: the timeline, a well filled in the accent up to the position (the
// downloaded part of a stream lighter beyond it), chapter marks, the head;
// the title on its left and the time on its right.
void Menu::drawMainPage(cairo_t* cr) {
  if (view_.keyPicking) {
    drawKeyPick(cr);
    return;
  }
  using namespace strip;
  const Strings& t = text();
  const PlaybackState& p = view_.playback;
  const Palette& pal = palette();
  char clock[16];
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  std::strftime(clock, sizeof(clock), "%H:%M", &local);
  drawText(cr, clock, kLeft + kInset, kTabsY, textWidth("00:00", kClockSize, false) + 4, kTabsH,
           kClockSize, false, false, pal.text.r, pal.text.g, pal.text.b);
  drawText(cr, infoText_, infoX_, kTabsY, infoRight_ - infoX_, kTabsH, kInfoSize, false, false,
           pal.textDim.r, pal.textDim.g, pal.textDim.b);
  if (view_.appFps > 0) {  // the frames the app draws per second, before Quit
    const std::string fps = std::to_string(view_.appFps) + " FPS";
    drawText(cr, fps, infoRight_ + kit::kSpace, kTabsY, textWidth(fps, kInfoSize, false) + 4, kTabsH,
             kInfoSize, false, false, pal.faint.r, pal.faint.g, pal.faint.b);
  }

  const double x = kLeft, w = kRight - kLeft;
  const BandMarks marks = bandMarks();
  const bool timeline = marks.timeline;
  const double fraction = marks.fraction, covered = marks.covered;
  // The well only where the fill doesn't cover it (and its lit edges).
  const bool skip = covered > x + kit::kCorner;
  if (skip) {
    const double reach = MenuFinishes::reach(Finish::Sunk) + 2;
    cairo_save(cr);
    cairo_rectangle(cr, covered, kBandY - reach, x + w + reach - covered, kBandH + 2 * reach);
    cairo_rectangle(cr, x - reach, kBandY - reach, covered - x + reach, reach);
    cairo_rectangle(cr, x - reach, kBandY + kBandH, covered - x + reach, reach);
    cairo_clip(cr);
  }
  drawFinish(cr, Finish::Sunk, x, kBandY, w, kBandH, kit::kCorner, stripPanel());
  if (skip) cairo_restore(cr);
  if (timeline) {
    if (view_.streaming) {
      const double buffered = marks.buffered;
      if (buffered >= 0.5) {
        barPath(cr, x, kBandY, buffered, kBandH, kit::kCorner, w);
        setColor(cr, pal.buffered);
        cairo_fill(cr);
      }
    }
    drawAccentFill(cr, x, kBandY, w * fraction, kBandH, 0.92, kit::kCorner, w);
    if (p.chapters.size() > 1) {
      for (const Chapter& c : p.chapters)
        if (c.time > 0 && c.time < p.duration)
          cairo_rectangle(cr, std::clamp(x + w * c.time / p.duration, x + kit::kCorner, x + w - kit::kCorner) - 1,
                          kBandY + kBandH - 12, 2, 12);
      cairo_set_source_rgba(cr, 1, 1, 1, 0.34);
      cairo_fill(cr);
    }
    // Held, the head grows.
    if (fraction > 0) {
      const double in = draggingSeek_ ? 2 : 6;
      drawHead(cr, x + w * fraction, kBandY + in, kBandH - 2 * in, draggingSeek_ ? 6 : 4);
    }
  }
  cairo_new_path(cr);
  const std::string title = view_.currentFile.empty() ? t.noVideo
                            : view_.playbackFailed    ? (view_.discEncrypted ? t.discEncrypted : t.playbackFailed)
                            : p.title.empty()         ? t.loading
                                                      : p.title;
  if (timeline) {  // the time, the duration dimmer
    const double right = x + w - kInset, atW = marks.atW, ofW = marks.ofW;
    drawText(cr, marks.of, right - ofW - 2, kBandY, ofW + 4, kBandH, kBandSize, false, false, 1, 1,
             1, 0.62);
    drawText(cr, marks.at, right - ofW - atW - 2, kBandY, atW + 4, kBandH, kBandSize, false, false,
             1, 1, 1);
  }
  const double titleW = marks.titleRight - x - kInset;
  if (view_.playbackFailed)  // in the red of failures elsewhere in the menu
    drawText(cr, title, x + kInset, kBandY, titleW, kBandH, kBandSize, true, false, 1.0, 0.55, 0.5);
  else
    drawText(cr, StashPages::playingTitle(*this, title), x + kInset, kBandY, titleW, kBandH,
             kBandSize, true, false, 1, 1, 1);
}

// Picking the background key's colour: the browser is hidden, and the
// strip says what to do, with Cancel under it (the menu button cancels too).
void Menu::layoutKeyPick() {
  Button cancel = textButton(text().cancel, Icon::None, command(MenuCommand::CancelKeyPick));
  cancel.w = std::max(cancel.w, 220.0);
  cancel.h = strip::kRowH;
  cancel.x = (kMainWidth - cancel.w) / 2;
  cancel.y = strip::kRowY;
  cancel.radius = kit::kCorner;
  buttons_.push_back(cancel);
}

void Menu::drawKeyPick(cairo_t* cr) {
  const Strings& t = text();
  const Palette& pal = palette();
  const double x = strip::kLeft, w = strip::kRight - strip::kLeft;
  drawText(cr, t.keyPick, x, strip::kTabsY, w, strip::kTabsH, kit::kTitleSize, true, true,
           pal.text.r, pal.text.g, pal.text.b);
  // Under it what it does, or (brighter) why the last press took no colour.
  const KeyPickNote note = view_.keyPickNote;
  const char* detail = note == KeyPickNote::Uneven       ? t.keyPickUneven
                       : note == KeyPickNote::Unreadable ? t.keyPickUnreadable
                                                         : t.keyPickDetail;
  const Rgba& c = note == KeyPickNote::None ? pal.textDim : pal.text;
  const double rgba[4] = {c.r, c.g, c.b, c.a};
  drawTextLines(cr, detail, x, strip::kBandY, w, kit::kRowSize, 2, rgba, true);
}
