#include "playback_session.h"

#include "controls.h"
#include "deletion.h"
#include "file_util.h"
#include "library.h"
#include "location.h"
#include "media_server.h"
#include "menu.h"
#include "notices.h"
#include "offline.h"
#include "passthrough.h"
#include "photo_preloader.h"
#include "remote.h"
#include "running_video.h"
#include "seek_preview.h"
#include "stash.h"
#include "stash_stream.h"
#include "stick_seek.h"
#include "strings.h"
#include "video_memory.h"
#include "video_player.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <utility>

PlaybackSession::PlaybackSession(VideoPlayer& player, Settings& settings, Library& library,
                                 VideoMemory& memory, Offline& offline,
                                 const std::unique_ptr<Stash>& stash, ScriptPlayer& scripts,
                                 RunningVideo& running, Menu& menu, Notices& notices,
                                 PhotoPreloader& photoPreloader, SeekPreview& seekPreview,
                                 Deletion& deletion, Passthrough& passthrough, StickSeek& stickSeek,
                                 GestureInput& gestureInput, GestureInput::Host& gestureHost,
                                 ControlInput& controlInput)
    : player(player), settings(settings), library(library), memory(memory), offline(offline),
      stash(stash), scripts(scripts), running(running), menu(menu), notices(notices),
      photoPreloader(photoPreloader), seekPreview(seekPreview), deletion(deletion),
      passthrough(passthrough), stickSeek(stickSeek), gestureInput(gestureInput),
      gestureHost(gestureHost), controlInput(controlInput), afterChosen(settings.afterVideo) {
  applyAfterVideo();
}

void PlaybackSession::say(std::string text) { notices.say(std::move(text), glfwGetTime()); }

void PlaybackSession::videoChanged() {
  gestureInput.cancel(gestureHost);  // a twist under way was the last video's
  gestureInput.hide();
  controlInput.dropFileTaps();  // ... and a press waiting to skip or stop it
  notices.videoChanged();
}

void PlaybackSession::startShuffle() {
  shuffleSeed = (static_cast<uint64_t>(seedSource()) << 32 | seedSource()) | 1;
  playStart = openedAs;
  playStartFile = running.currentFile;
}

// mpv starts a repeated video over by itself; it never reaches its end. Not
// a partial download: the end of its data isn't its end.
void PlaybackSession::applyAfterVideo() {
  player.setLoop(afterVideo() == AfterVideo::Repeat && !isPartialDownload(running.currentFile));
}

// Keeps where the running video is (none near either end), once mpv has
// said where it is: leaving it sooner keeps the position it had.
void PlaybackSession::saveResume() {
  if (!videoOpened || !player.positionKnown()) return;
  const PlaybackState& p = player.playback();
  const double point = resumePoint(p.position, p.duration);
  resumeSaveAt = glfwGetTime() + 60;
  if (point == resumeSaved) return;
  resumeSaved = point;
  library.setResume(running.currentFile, point);
}

void PlaybackSession::keepResume() {
  saveResume();
  videoOpened = false;
}

bool PlaybackSession::open(const std::string& requested, double startAt) {
  // A video downloaded to this device plays from its copy, however it was
  // chosen. Not for a link: what is behind it may have changed.
  std::string oshash = isLink(requested) ? std::string() : library.oshashOf(requested);
  if (oshash.empty() && stash && stash->catalog())
    if (const int scene = stashSceneAt(*stash->catalog(), requested); scene >= 0)
      oshash = stash->catalog()->scenes()[scene].oshash;
  // A scene streamed from Stash is its file's video. Not without the
  // server it was listed on.
  if (isStashScene(requested)) {
    if (!stashSceneId(requested) || oshash.empty()) {
      std::printf("Server scene not available\n");
      return false;
    }
    library.locate(requested, oshash);
  }
  const std::string copy = offline.copyOf(requested, oshash);
  const std::string path = copy.empty() ? requested : copy;
  openedAs = requested;
  openedAt = startAt;
  neighbor = {};
  nextPhoto = {};
  neighborStep = 0;
  running.saveUnsaved();
  keepResume();
  library.close(running.currentFile);
  deletion.dropDialog();
  running.currentFile = path;
  running.streamOshash = isStashScene(path) ? oshash : std::string();
  scripts.open(isPhotoFile(path) ? std::string() : path);
  running.formatChosenByUser = false;
  running.format = guessVideoFormat(path, 0, 0);
  // Its settings from the library, else as kept before it (videos.conf),
  // which the library takes over. A file the library doesn't know yet may
  // be a moved one with settings of its own: they apply once it is found.
  running.video = VideoSettings{};
  openingRecord = library.record(path).id;
  // A link's content is found as a file's is, after it starts playing. It
  // is its address until then, and without it: what is kept for that
  // applies now.
  if (!isPhotoFile(path)) library.open(path);
  awaitingRecord = library.awaited(path);
  const bool identityComing = awaitingRecord && !isLink(path);
  if (!library.settings(path, running.video)) {
    VideoSettings legacy;
    if (memory.get(path, legacy)) {
      // Queue the fallback before user edits can create settings. A known
      // path must still resolve to the same identity before it can use it.
      library.carryOverSettings(path, legacy, identityComing ? openingRecord : 0);
      if (!openingRecord || !identityComing) running.video = legacy;
    }
  }
  awaitingResume = awaitingRecord && settings.resume && !isPhotoFile(path) && startAt < 0;
  resolvedResume = -1;
  passthrough.videoChanged(isLink(path) || isPartialDownload(path));
  videoChanged();
  running.applyVideoSettings();
  knownWidth = knownHeight = 0;  // re-detect the format once the size is known
  knownViews = TwoViews::No;
  formatDetected = false;
  stickSeek.cancel();
  if (isPhotoFile(path)) {
    // Photos are shown by photoPreloader, not mpv: decoded in the
    // background, the next one (after the last the first, as the slideshow
    // goes) ahead, so switching shows it at once.
    player.stop();
    photoPreloader.request(path, {});
    nextPhoto = mediaNeighbor(path, 1, true);
    photoSized = false;
  } else {
    // A newly chosen video always plays, even if the last one was paused,
    // from where it was asked to start, else where it was left if the
    // library knows (streams start over).
    double start = startAt;
    if (startAt < 0 && settings.resume && recognised()) {
      const VideoRecord r = library.record(path);
      start = resumePoint(r.resumeSeconds, r.durationSeconds);
    }
    player.setPaused(false);
    applyAfterVideo();
    player.load(path, std::max(0.0, start));
    if (startAt >= 0) std::printf("Starting at %s\n", clockTime(start).c_str());
    else if (start > 0) std::printf("Resuming at %s\n", clockTime(start).c_str());
  }
  durationKept = false;
  // As for links, Stash's stream opens its preview at once: the first one
  // pointed at then comes about 0.4 s sooner over a LAN.
  seekPreview.videoChanged(path, isStream(path));
  nextSlideAt = glfwGetTime() + slideshowSeconds;  // each photo gets its full time
  return running.video.hasFormat;
}

void PlaybackSession::close() {
  if (running.currentFile.empty()) return;
  deletion.dropDialog();
  running.saveUnsaved();
  keepResume();
  library.close(running.currentFile);
  player.stop();
  stickSeek.cancel();
  seekPreview.close();
  running.currentFile.clear();
  running.streamOshash.clear();
  scripts.open({});
  running.video.passthrough = false;
  running.video.opacity = 100;  // the empty scene isn't blended; the video keeps its saved value
  videoChanged();
  neighbor = {};
  nextPhoto = {};
  neighborStep = 0;
  awaitingRecord = false;
  awaitingResume = false;
}

void PlaybackSession::rememberFormat() {
  running.formatChosenByUser = true;
  running.video.hasFormat = true;
  running.video.format = running.format;
  running.storeVideo(VideoSettings::kFormat);
}

void PlaybackSession::start(const std::string& file) {
  startScene = isStashScene(file) ? file : "";
  if (!file.empty() && startScene.empty()) {
    open(file);
    startShuffle();
  }
}

void PlaybackSession::step(int step, bool atEnd) {
  const bool replaced = endStep;  // Previous or Next pressed while an end's step is under way
  endStep = false;
  if (running.currentFile.empty()) return;
  if (isLink(running.currentFile)) {
    say(strings().jobs.notForStreams);
    return;
  }
  const bool shuffle = afterVideo() == AfterVideo::Shuffle && !isPhotoFile(running.currentFile);
  // Opened from a list in the menu (Stash's scenes): through that list.
  std::string next;
  if (!menu.listNeighbor(openedAs, step, next, {shuffle ? shuffleSeed : 0, playStart, atEnd})) {
    neighborStep = neighbor.valid() && !neighborWrap && !replaced ? neighborStep + step : step;
    neighborWrap = false;
    neighbor = mediaNeighbor(running.currentFile, neighborStep, false,
                             {shuffle ? shuffleSeed : 0, playStartFile, atEnd});
    endStep = atEnd;
    endStepRestarts = player.counts().restarts;
    return;
  }
  if (!next.empty()) {
    open(next);
  } else {
    std::printf("No %s video in this list\n", step > 0 ? "next" : "previous");
    if (atEnd) menuAtEnd = true;
    else say(step > 0 ? strings().jobs.lastVideo : strings().jobs.firstVideo);
  }
}

void PlaybackSession::openedByHand(const MenuCommand& c) {
  if (listShuffle != (c.option == 1)) {
    listShuffle = c.option == 1;
    applyAfterVideo();
  }
  startShuffle();
}

void PlaybackSession::chooseAfterVideo(const MenuCommand& c) {
  afterChosen = static_cast<AfterVideo>(std::clamp(static_cast<int>(c.value), 0, 3));
  if (afterChosen == AfterVideo::Stop || afterChosen == AfterVideo::Next) {
    settings.afterVideo = afterChosen;
    saveSettings(settings);
  }
  listShuffle = false;
  applyAfterVideo();
  startShuffle();  // from the playing video
  if (endStep) {  // at an end going on: the new choice decides instead
    neighbor = {};
    neighborStep = 0;
    endStep = false;
    wasAtEnd = false;
  }
}

void PlaybackSession::setSlideshow(const MenuCommand& c) {
  slideshowSeconds = static_cast<int>(c.value);
  if (!slideshowSeconds && neighborWrap) neighbor = {};
  nextSlideAt = glfwGetTime() + slideshowSeconds;
}

void PlaybackSession::stepRating(int halves) {
  const JobStrings& said = strings().jobs;
  if (!recognised()) {
    say(isPartialDownload(running.currentFile) ? said.notForDownloads : said.notForStreams);
    return;
  }
  if (!library.available()) {
    say(said.ratingsLocked);
    return;
  }
  menu.stepRating(running.currentFile, halves);
  if (const float rating = library.record(running.currentFile).rating; rating > 0) {
    char stars[16];
    std::snprintf(stars, sizeof(stars), "%g", rating);
    std::string text(std::max(std::snprintf(nullptr, 0, said.rating, stars), 0), '\0');
    std::snprintf(text.data(), text.size() + 1, said.rating, stars);
    say(std::move(text));
  } else {
    say(said.notRated);
  }
}

bool PlaybackSession::handle(const MenuCommand& c) {
  switch (c.type) {
    case MenuCommand::TogglePause: player.togglePause(); break;
    case MenuCommand::SetPaused: player.setPaused(c.value != 0); break;
    case MenuCommand::FrameStep:
      // Not while the stick seeks (its release may play on).
      if (!stickSeek.held() && player.frameStep(c.value < 0)) userSeeked();
      break;
    case MenuCommand::StepRating: stepRating(c.value < 0 ? -1 : 1); break;
    case MenuCommand::Stop:
      close();
      menu.showPlayedFrom();
      break;
    case MenuCommand::Seek:
      userSeeked();
      player.seekBy(c.value);
      break;
    case MenuCommand::SeekTo: {
      userSeeked();
      // Chapters, and short jumps along the seek bar, land exactly; a long
      // jump lands on the keyframe before the time: much faster (exact
      // seeks decode from it, up to seconds on the Frame), and closer than
      // a pointer on the bar can aim.
      constexpr double kExactJump = 30;  // seconds
      const bool fromBar = c.option == 1;
      player.seekTo(c.value, !fromBar || std::abs(c.value - player.playback().position) <= kExactJump);
      break;
    }
    case MenuCommand::ToggleMute: player.toggleMute(); break;
    case MenuCommand::SetVolume:
      player.setVolume(static_cast<int>(c.value));
      if (player.playback().muted) player.setMuted(false);  // a volume set is heard
      break;
    case MenuCommand::PreviousVideo: step(-1); break;
    case MenuCommand::NextVideo: step(1); break;
    case MenuCommand::SetProjection:
      running.format.projection = static_cast<Projection>(static_cast<int>(c.value));
      rememberFormat();
      printFormat(running.format, "manual");
      break;
    case MenuCommand::SetStereo:
      running.format.stereo = static_cast<StereoLayout>(static_cast<int>(c.value));
      rememberFormat();
      printFormat(running.format, "manual");
      break;
    case MenuCommand::SetPacking:
      running.format.packing = static_cast<Packing>(static_cast<int>(c.value));
      rememberFormat();
      printFormat(running.format, "manual");
      break;
    case MenuCommand::SetSlideshow: setSlideshow(c); break;
    case MenuCommand::SetAfterVideo: chooseAfterVideo(c); break;
    case MenuCommand::SelectAudio:
      player.selectAudio(static_cast<int64_t>(c.value));
      break;
    case MenuCommand::SelectSubtitle:
      player.selectSubtitle(static_cast<int64_t>(c.value));
      break;
    case MenuCommand::LoadSubtitle: player.addSubtitle(c.path); break;
    case MenuCommand::RequestDelete:
      if (deletion.request(c, running.currentFile, running.streamOshash)) slideshowSeconds = 0;
      break;
    case MenuCommand::CancelDelete: deletion.cancel(running.currentFile); break;
    case MenuCommand::DeleteFile: deletion.confirm(c, running.currentFile); break;
    default: return false;
  }
  return true;
}

void PlaybackSession::pollFailure() {
  // A Stash scene's file on a server that doesn't play plays from Stash's
  // stream instead (once: the stream is no server's file), if that serves it.
  // Not one the decoder refused: the stream is the same video.
  if (player.failed() && !player.decoderRefused() && remote::isRemote(running.currentFile) && stash &&
      stash->catalog())
    if (const int i = stash->catalog()->sceneByPath(running.currentFile); i >= 0) {
      const StashScene& scene = stash->catalog()->scenes()[i];
      if (!scene.oshashes.empty() && scene.oshashes.front() == scene.oshash) {
        std::printf("Server failed: the scene plays from its media server's stream\n");
        const std::string listed = openedAs;
        open(stashStream(scene), openedAt);
        openedAs = listed;
      }
    }
}

bool PlaybackSession::recognised() const {
  const std::string& f = running.currentFile;
  return (!isLink(f) && !isPartialDownload(f)) || !library.oshashOf(f).empty();
}

void PlaybackSession::countPlay(bool opened) {
  // A play counts each time a video file opens (a resumed one too;
  // pausing, looping and seeking don't). Photos and links kept by their
  // address don't; a link whose content is still to be found counts, and
  // its play goes to that content, or nowhere.
  if (opened && !running.currentFile.empty() && !isPhotoFile(running.currentFile) &&
      (recognised() || library.awaited(running.currentFile))) {
    library.recordPlay(running.currentFile);
    videoOpened = true;
    videoOpenedAt = glfwGetTime();
    resumeSaveAt = glfwGetTime() + 60;
    resumeSaved = -2;
  }
  // Every minute, where it is: kept even if VideoFrameVR is killed.
  if (videoOpened && glfwGetTime() >= resumeSaveAt) saveResume();
  if (videoOpened && !durationKept && player.playback().duration > 0) {
    durationKept = true;
    library.setDuration(running.currentFile, player.playback().duration);
  }
}

void PlaybackSession::recordArrived() {
  if (awaitingResume) {
    const VideoRecord r = library.record(running.currentFile);
    resolvedResume = resumePoint(r.resumeSeconds, r.durationSeconds);
  }
  VideoSettings kept;
  bool hasSettings = library.settings(running.currentFile, kept);
  if (!hasSettings && openingRecord == library.record(running.currentFile).id &&
      memory.get(running.currentFile, kept)) {
    library.carryOverSettings(running.currentFile, kept);
    hasSettings = true;
  }
  // The save timer may still hold user edits when identity arrives.
  // Restore the other fields without replacing those pending changes.
  if (hasSettings) kept.take(running.video, running.unsavedParts & ~VideoSettings::kScreen);
  if (hasSettings && kept != running.video) {
    // Without a format of its own, the file's is detected again.
    if (running.video.hasFormat && !kept.hasFormat) {
      running.formatChosenByUser = false;
      knownWidth = -1;
      formatDetected = false;
    }
    running.video = kept;
    running.applyVideoSettings();
  }
}

void PlaybackSession::pollRecord(bool libraryChanged) {
  if (libraryChanged && awaitingRecord && !library.awaited(running.currentFile)) {
    awaitingRecord = false;
    // A link kept by its address after all has nothing to apply.
    if (recognised()) recordArrived();
    else awaitingResume = false;
  }
  // Identity can arrive before mpv opens, or shortly after playback starts.
  // A user seek or more than three seconds of playback ends this open's resume.
  if (awaitingResume && !awaitingRecord && videoOpened) {
    awaitingResume = false;
    if (settings.resume && resolvedResume > 0 && glfwGetTime() - videoOpenedAt < 3 &&
        player.playback().position < 3) {
      player.seekTo(resolvedResume, true);
      std::printf("Resuming at %s\n", clockTime(resolvedResume).c_str());
    }
  }
}

void PlaybackSession::poll() {
  if (!startScene.empty() && stash && stash->catalog()) {
    open(startScene);
    startShuffle();
    startScene.clear();
  }
  if (nextPhoto.valid() &&
      nextPhoto.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    photoPreloader.request(running.currentFile, nextPhoto.get());
  // Sought back from the end, or seeking since: it doesn't go on.
  if (endStep && (!player.playback().endReached || stickSeek.active() ||
                  player.counts().restarts != endStepRestarts)) {
    neighbor = {};
    neighborStep = 0;
    endStep = false;
  }
  if (neighbor.valid() &&
      neighbor.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    const std::string next = neighbor.get();
    if (!next.empty())
      open(next);
    else if (!neighborWrap)
      std::printf("No %s video in this folder\n", neighborStep > 0 ? "next" : "previous");
    if (next.empty() && !neighborWrap && !endStep)
      say(neighborStep > 0 ? strings().jobs.lastVideo : strings().jobs.firstVideo);
    if (next.empty() && endStep) menuAtEnd = true;
    endStep = false;
    neighborStep = 0;
    nextSlideAt = glfwGetTime() + slideshowSeconds;
  }
  // mpv keeps a video that reached its end paused on its last frame. Play
  // the next video and Shuffle go on from there; with no next one, as for
  // Stop, the menu opens.
  const bool atEnd = player.playback().endReached && !running.currentFile.empty() &&
                     !isPhotoFile(running.currentFile);
  if (atEnd != wasAtEnd) {
    const AfterVideo after = afterVideo();
    // Not at the end of a partial download's data: the rest may still come.
    if (atEnd && (after == AfterVideo::Next || after == AfterVideo::Shuffle) &&
        !isLink(running.currentFile) && !isPartialDownload(running.currentFile))
      step(1, true);
    else
      menuAtEnd = atEnd;
  }
  wasAtEnd = atEnd;
  // Slideshow: the next photo in the folder, after the last one the first.
  if (slideshowSeconds > 0 && isPhotoFile(running.currentFile) &&
      glfwGetTime() >= nextSlideAt && !neighbor.valid()) {
    neighborStep = 1;
    neighborWrap = true;
    neighbor = mediaNeighbor(running.currentFile, 1, true);
  }
}

void PlaybackSession::detectFormat() {
  // Re-guess the format once the real frame size is known.
  // And once mpv says how it decodes a 3D Blu-ray video's two views. Not
  // from the last file's while the next one opens.
  const TwoViews views = player.playback().views;
  if (player.sized() &&
      (player.width() != knownWidth || player.height() != knownHeight || views != knownViews)) {
    knownWidth = player.width();
    knownHeight = player.height();
    knownViews = views;
    if (!running.formatChosenByUser) {
      const VideoFormat guess = guessVideoFormat(running.currentFile, knownWidth, knownHeight, views);
      if (!formatDetected || guess.projection != running.format.projection ||
          guess.stereo != running.format.stereo || guess.packing != running.format.packing)
        printFormat(guess, "detected");
      running.format = guess;
      formatDetected = true;
    }
  }
}

void PlaybackSession::photoShown(int photoWidth, int photoHeight) {
  if (!photoSized) {  // the size is known now: guess the format
    photoSized = true;
    std::printf("Photo size: %dx%d\n", photoWidth, photoHeight);
    if (!running.formatChosenByUser) {
      running.format = guessVideoFormat(running.currentFile, photoWidth, photoHeight);
      printFormat(running.format, "detected");
    }
  }
}

PlaybackStatus PlaybackSession::status() const {
  const PlaybackState& p = player.playback();
  const bool open = !running.currentFile.empty(), photo = isPhotoFile(running.currentFile);
  const bool video = open && !photo;
  const bool known = video && player.positionKnown();
  const bool ended = known && p.endReached;
  const bool playing = video && !p.paused && !player.asleep() && !ended && !player.loading() && !player.failed();
  return {running.currentFile, openedAs, open, photo, video, known ? p.position : 0,
          known ? p.duration : 0, p.paused, ended, player.loading(), player.failed(), player.seeking(),
          playing, playing && !player.seeking()};
}

void PlaybackSession::fill(MenuView& view) const {
  view.streaming = isLink(running.currentFile) || isPartialDownload(running.currentFile);
  view.recordless = !recognised();
  // A scene from Stash's stream only through Stash (docs/alpha/delete-file.md).
  // A scene played from its server: only through a server that deletes.
  view.deletable = running.currentFile.empty() || view.streaming ? MenuView::Deletable::No
                   : !isStashScene(running.currentFile)         ? MenuView::Deletable::Yes
                   : !mediaServer(settings.stash.kind).abilities().deleteFiles
                       ? MenuView::Deletable::No
                   : !settings.stash.writeBack              ? MenuView::Deletable::NeedsSendToStash
                   : offline.stashDown()                    ? MenuView::Deletable::StashDown
                                                            : MenuView::Deletable::Yes;
  view.slideshowSeconds = slideshowSeconds;
  view.afterVideo = afterVideo();
}
