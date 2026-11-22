#include "button_jobs.h"

#include "controls.h"
#include "focus_settings.h"
#include "library.h"
#include "menu.h"
#include "playback_state.h"
#include "running_video.h"
#include "see_through.h"
#include "strings.h"
#include "video_player.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

// The buttons' Zoom in and out: a press steps the Zoom (180°/360°) setting
// as the menu's slider does; held past kZoomDelay, it goes on at kZoomRate
// until let go.
constexpr double kZoomDelay = 0.4;  // seconds
constexpr double kZoomRate = 40;    // percent a second

template <typename... Args>
std::string printed(const char* format, Args... args) {
  std::string text(std::max(std::snprintf(nullptr, 0, format, args...), 0), '\0');
  std::snprintf(text.data(), text.size() + 1, format, args...);
  return text;
}

}  // namespace

std::string ButtonJobs::seeThroughKey() const {
  const VideoRecord r = library.record(running.currentFile);
  return r.known() ? "#" + std::to_string(r.id) : running.currentFile;
}

JobAction ButtonJobs::press(Job job, int hand, const Context& context) {
  JobAction action;
  const auto say = [&](std::string text) { action.said = std::move(text); };
  const auto sayf = [&](const char* format, auto... args) { say(printed(format, args...)); };
  // A switch whose change doesn't show by itself: as it will be. Mute and
  // Passthrough change later (mpv's, and where the settings are).
  const auto switching = [&](const char* name, bool on) {
    sayf(strings().jobs.switched, name, on ? strings().on : strings().off);
  };
  const JobInfo& info = jobInfo(job);
  const JobStrings& said = strings().jobs;
  const bool photo = context.photo, passthroughAvailable = context.passthroughAvailable;
  const double now = context.now;
  MenuCommand& c = action.command;
  c = info.command;
  if (info.length != JobLength::None)
    c.value *= info.length == JobLength::Skip ? settings.skipSeconds : settings.bigSkipSeconds;
  const PlaybackState& p = player.playback();
  switch (job) {
    case Job::PlayPause:
      // A photo's is the slideshow's: it stops one running.
      if (photo) {
        c = {};
        if (context.slideshow) c = {MenuCommand::SetSlideshow, 0.0};
        else say(said.notForPhotos);
      }
      break;
    case Job::PreviousChapter:
    case Job::NextChapter: {
      if (player.loading()) break;
      if (p.chapters.empty()) {
        say(said.noChapters);
        break;
      }
      // Back: to the start of this chapter first, more than 5 s into it.
      const bool into = job == Job::PreviousChapter && p.chapter >= 0 &&
                        p.chapter < static_cast<int>(p.chapters.size()) &&
                        p.position - p.chapters[p.chapter].time > 5;
      const int to = p.chapter + (job == Job::NextChapter ? 1 : into ? 0 : -1);
      if (to >= 0 && to < static_cast<int>(p.chapters.size()))
        c = {MenuCommand::SeekTo, p.chapters[to].time};
      else
        say(to < 0 ? said.firstChapter : said.lastChapter);
      break;
    }
    case Job::AfterVideo:
      // As the strip's list, the next choice.
      c = {MenuCommand::SetAfterVideo, static_cast<double>((static_cast<int>(context.afterVideo) + 1) % 4)};
      break;
    case Job::VolumeUp:
    case Job::VolumeDown: {
      // As + and - do, within the volume gesture's 0..100.
      const long volume = std::lround(p.volume);
      const long to = job == Job::VolumeUp ? std::max(volume, std::min(volume + 5, 100L))
                                           : std::max(volume - 5, 0L);
      c = {MenuCommand::SetVolume, static_cast<double>(to)};
      sayf(said.volume, static_cast<int>(to));
      break;
    }
    case Job::NextAudioTrack:
    case Job::Subtitles: {
      if (player.loading()) break;  // the last file's tracks until it opens
      const char* type = job == Job::Subtitles ? "sub" : "audio";
      std::vector<const Track*> tracks;
      int chosen = -1;
      for (const Track& t : p.tracks)
        if (t.type == type) {
          if (t.selected) chosen = static_cast<int>(tracks.size());
          tracks.push_back(&t);
        }
      const Strings& words = strings();
      if (job == Job::NextAudioTrack) {
        if (tracks.size() > 1) {
          const Track& next = *tracks[(chosen + 1) % tracks.size()];
          c = {MenuCommand::SelectAudio, static_cast<double>(next.id)};
          sayf(said.audioTrack, trackName(next, words).c_str());
        } else {
          say(said.noOtherAudio);
        }
      } else if (chosen >= 0) {
        lastSubtitle = tracks[chosen]->id;
        c = {MenuCommand::SelectSubtitle, 0};
        sayf(said.switched, words.subtitles, words.off);
      } else if (!tracks.empty()) {
        const Track* on = tracks[0];
        for (const Track* t : tracks)
          if (t->id == lastSubtitle) on = t;
        c = {MenuCommand::SelectSubtitle, static_cast<double>(on->id)};
        sayf(said.switched, words.subtitles, trackName(*on, words).c_str());
      } else {
        say(said.noSubtitles);
      }
      break;
    }
    case Job::Mute: switching(said.sound, p.muted); break;
    case Job::Passthrough:
      if (!passthroughAvailable) {
        say(said.noRoom);
        c = {};
      } else {
        switching(said.passthrough, !running.video.passthrough);
      }
      break;
    case Job::PeekRoom:
      if (!context.alphaBlend) say(said.noRoom);
      break;
    case Job::SeeThrough:
      // Solid, and back to the see-through it had.
      if (!context.alphaBlend) {
        say(said.noRoom);
        break;
      }
      if (running.video.opacity < 100) {
        c = {MenuCommand::SetOpacity, 100.0};
      } else {
        // (stored before the library knew it: by where it is)
        auto last = lastSeeThrough.find(seeThroughKey());
        if (last == lastSeeThrough.end()) last = lastSeeThrough.find(running.currentFile);
        c = {MenuCommand::SetOpacity, last != lastSeeThrough.end() ? last->second : 50.0};
      }
      action.showOpacity = true;
      break;
    case Job::MatchRoom:
      // Where the menu shows its switch: where the room's light is read.
      if (!context.roomReadable) {
        say(said.notOnHeadset);
        c = {};
      } else if (!context.roomShown) {
        say(said.needsPassthrough);
      } else {
        switching(strings().colourBalance.matchRoom, !context.roomMatched);
      }
      break;
    case Job::ResetScreen:
      // As Video settings' Screen group: a flat screen's only.
      if (running.format.projection != Projection::Flat) {
        say(said.flatOnly);
        c = {};
      }
      break;
    case Job::RemoveBackground:
      // Where its switch shows: passthrough on, without the video's own mask.
      if (!passthroughAvailable)
        say(said.noRoom);
      else if (!running.video.passthrough)
        say(said.needsPassthrough);
      else if (packedMask(running.currentFile, running.video) && maskFormat(running.format))
        say(said.ownMask);
      else {
        c = {MenuCommand::ToggleGreenScreen};
        switching(strings().greenScreenOn, !running.video.greenScreen.on);
      }
      break;
    case Job::ResetPicture: say(said.pictureReset); break;
    case Job::ZoomIn:
    case Job::ZoomOut:
      if (running.format.projection == Projection::Flat) {
        say(said.notForFlat);
        break;
      }
      zoomHold = {job, hand, now, now, running.currentFile};
      running.zoomBy(job == Job::ZoomIn ? VideoSettings::kZoomStepPercent : -VideoSettings::kZoomStepPercent);
      action.showRing = GestureValue::Zoom;
      break;
    case Job::StereoFocus:
    case Job::ShowFocusTarget: {
      const bool stereo = job == Job::StereoFocus;
      if (!stereo && !settings.focusPrefs.enabled) {  // its switch is greyed out
        say(said.needsFocus);
        break;
      }
      c = {MenuCommand::SetFocus,
           (stereo ? settings.focusPrefs.enabled : settings.focusPrefs.showTarget) ? 0.0 : 1.0};
      c.option = static_cast<int>(stereo ? FocusControl::Enabled : FocusControl::ShowTarget);
      break;
    }
    default: break;
  }
  return action;
}

std::string ButtonJobs::saidAfter(Job job, AfterVideo afterVideo) const {
  std::string said;
  const Strings& words = strings();
  const auto say = [&](const char* text) { said = text; };
  const auto switched = [&](const char* name, bool on) {
    said = printed(words.jobs.switched, name, on ? words.on : words.off);
  };
  switch (job) {
    case Job::AfterVideo: say(words.afterVideo.choices[static_cast<int>(afterVideo)]); break;
    case Job::Scripts: switched(words.scripts.group, settings.scripts.enabled); break;
    case Job::Motion: switched(words.focus.motion, settings.motionEmulation); break;
    case Job::ToneLook: switched(words.toneLook, settings.toneLook); break;
    case Job::TvGamma: switched(words.tvGamma, settings.tvGamma); break;
    case Job::StereoFocus: switched(words.focus.enabled, settings.focusPrefs.enabled); break;
    default: break;
  }
  return said;
}

void ButtonJobs::keepSeeThrough() {
  if (running.video.opacity < 100 && !running.currentFile.empty() &&
      (running.video.opacity != seeThroughSeen || running.currentFile != seeThroughFile))
    lastSeeThrough[seeThroughKey()] = running.video.opacity;
  seeThroughFile = running.currentFile;
  seeThroughSeen = running.video.opacity;
}

bool ButtonJobs::holdZoom(const JobInput& jobs, double now) {
  if (zoomHold.hand >= 0) {
    if (running.currentFile != zoomHold.file) {
      zoomHold.hand = -1;  // stored as the video changed
    } else if (const int other = 1 - zoomHold.hand;
               running.format.projection != Projection::Flat && !jobs.isHeld(zoomHold.job, zoomHold.hand) &&
               (jobs.isHeld(Job::ZoomIn, other) || jobs.isHeld(Job::ZoomOut, other))) {
      // Let go while the other hand holds a zoom too: it goes on with that.
      zoomHold.job = jobs.isHeld(Job::ZoomIn, other) ? Job::ZoomIn : Job::ZoomOut;
      zoomHold.hand = other;
      zoomHold.at = now;
    } else if (running.format.projection == Projection::Flat || !jobs.isHeld(zoomHold.job, zoomHold.hand)) {
      // In whole percents, as the slider sets it.
      running.video.zoom = std::round(running.video.zoom * 100) / 100;
      running.screen.zoom = running.video.zoom;
      running.storeVideoSoon(VideoSettings::kZoom);
      zoomHold.hand = -1;
    } else if (now > zoomHold.start + kZoomDelay) {
      const double from = std::max(zoomHold.at, zoomHold.start + kZoomDelay);
      running.zoomBy((zoomHold.job == Job::ZoomIn ? kZoomRate : -kZoomRate) * (now - from));
      zoomHold.at = now;
      return true;
    }
  }
  return false;
}
