#include "passthrough.h"

#include "file_util.h"
#include "library.h"
#include "menu_command.h"
#include "menu_view.h"
#include "see_through.h"
#include "settings.h"
#include "strings.h"
#include "xr_context.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>

namespace {

// Passthrough switched on by itself says so this long.
constexpr double kPassthroughNoticeShown = 5.0;

}  // namespace

bool Passthrough::roomShown() const {
  return xr.alphaBlendSupported() && (video.passthrough || video.opacity < 100);
}

void Passthrough::videoChanged(bool stream) {
  this->stream = stream;
  passthroughSettled = false;
  greenCheckAsked = false;
  fillCheckPending = false;
  videoCheckAt = 0;
  passthroughShownUntil = 0;
}

// Passthrough switched on for a video made for it, saying so the first time ever.
void Passthrough::passthroughByItself(AutoPassthrough why) {
  if (why == AutoPassthrough::None || video.passthrough) return;
  video.passthrough = true;
  if (why != AutoPassthrough::Mask && video.greenScreen.isDefault(video.colourKeyed()))
    video.greenScreen.on = true;
  const AutoPassthroughStrings& t = strings().autoPassthrough;
  passthroughWhy = why == AutoPassthrough::Mask ? t.mask
                   : why == AutoPassthrough::Fill ? t.fill
                                                  : t.greenScreen;
  if (!settings.passthroughNoticeSeen) {
    passthroughShownUntil = glfwGetTime() + kPassthroughNoticeShown;
    settings.passthroughNoticeSeen = true;
    saveSettings(settings);
  }
  std::printf("Passthrough: on by itself (%s)\n",
              why == AutoPassthrough::Mask   ? "mask"
              : why == AutoPassthrough::Fill ? "flat background"
                                             : "green screen");
}

void Passthrough::settle(bool autoPassthroughOn, bool ready) {
  if (passthroughSettled || !ready || currentFile.empty()) return;
  passthroughSettled = true;
  const std::string& path = currentFile;
  if (autoPassthroughOn && !isPhotoFile(path) && !stream)
    passthroughByItself(autoPassthrough(path, format, video));
  // The fill check, for the key's Automatic, also where passthrough is on by hand.
  const bool fillWanted = fillCheckWanted(path, format, video) &&
                          (autoPassthroughOn || video.passthrough);
  if (!greenCheckAsked && !isPhotoFile(path) && !stream &&
      ((autoPassthroughOn && greenCheckWanted(path, format, video)) || fillWanted)) {
    greenCheckAsked = fillCheckPending = true;
    videoCheckAt = glfwGetTime() + 2.0;
    videoCheckKind = VideoCheck::Kind::GreenScreen;
  }
  // Whatever the setting: a mask's second layer shows whenever passthrough does.
  if (!isPhotoFile(path) && !stream && layerCheckWanted(path, format, video)) {
    videoCheckAt = glfwGetTime() + 2.0;
    videoCheckKind = VideoCheck::Kind::MaskLayers;
    fillCheckPending = false;  // a green screen check under way is replaced
  }
}

void Passthrough::poll(bool autoPassthroughOn) {
  if (videoCheckAt > 0 && glfwGetTime() >= videoCheckAt) {
    videoCheckAt = 0;
    videoCheck.start(currentFile, format, videoCheckKind);
  }
  if (VideoCheck::Result checked; videoCheck.result(checked)) {
    // Kept for the video, even if another plays by now: it runs once.
    VideoSettings result;
    const bool current = checked.path == currentFile;
    if (current && checked.kind == VideoCheck::Kind::GreenScreen) fillCheckPending = false;
    if (!checked.answered) {
      // None: it runs again next time.
    } else if (checked.kind == VideoCheck::Kind::GreenScreen) {
      result.greenCheckVersion = VideoCheck::kGreenVersion;
      result.greenScreenFound = checked.found;
      result.fillCheckVersion = VideoCheck::kFillVersion;
      result.fillFound = checked.fillFound;
      if (checked.fillFound) result.fillColour = checked.fillColour;
      const unsigned parts = VideoSettings::kGreenCheck | VideoSettings::kFillCheck;
      library.setSettings(checked.path, result, parts);
      if (current) {
        video.take(result, parts);
        if (autoPassthroughOn)
          passthroughByItself(autoPassthrough(checked.path, format, video));
      }
    } else if (!layersSeen.count(checked.path)) {  // else found while it played
      result.maskLayersVersion = VideoCheck::kLayersVersion;
      result.maskLayersFound = checked.found;
      library.setSettings(checked.path, result, VideoSettings::kMaskLayers);
      if (current) video.take(result, VideoSettings::kMaskLayers);
    }
  }
}

void Passthrough::switchedByHand() {
  passthroughShownUntil = 0;
  // On by hand after the video settled: the fill check its key's
  // Automatic needs, if it wasn't asked for then.
  const std::string& path = currentFile;
  if (video.passthrough && passthroughSettled && !greenCheckAsked && !path.empty() &&
      !isPhotoFile(path) && !stream && fillCheckWanted(path, format, video)) {
    greenCheckAsked = fillCheckPending = true;
    videoCheckAt = glfwGetTime();
    videoCheckKind = VideoCheck::Kind::GreenScreen;
  }
}

const char* Passthrough::notice(double now) const {
  return now < passthroughShownUntil && passthroughWhy && video.passthrough ? passthroughWhy
                                                                            : nullptr;
}

// A new reading, unless one runs (which keeps the count it was asked with).
void Passthrough::readRoom() {
  if (roomLight.start()) roomAsked = balanceEdits;
}

void Passthrough::applyRoomMatch() {
  if (roomMatched && roomReading)
    roomBalance.match(roomReading->light, roomReading->colour, roomReading->white,
                      settings.matchColour / 100.0f, settings.matchBrightness / 100.0f);
}

void Passthrough::toggle() {
  if (!roomShown()) return;  // the switch is disabled while the room is hidden
  roomFailedUntil = 0;
  roomMatched = !roomMatched;
  if (roomMatched) {
    roomBalance = video.balance;
    roomAsked = balanceEdits;  // a reading still under way is for this one
    if (roomReading) applyRoomMatch();
    else readRoom();
  }
}

void Passthrough::readAgain() {
  roomFailedUntil = 0;
  if (roomMatched && roomShown()) readRoom();
}

unsigned Passthrough::setBalance(const MenuCommand& c) {
  // The video's, saved, or while the room's is matched the room's, not.
  const bool matching = roomMatched && roomShown();
  ColourBalance& balance = matching ? roomBalance : video.balance;
  const int value = std::clamp(static_cast<int>(c.value), -100, 100);
  // Each its own part, as brightness etc.: an edit before the video's
  // record is found leaves the other two as saved.
  const bool warmth = c.type == MenuCommand::SetWarmth, tint = c.type == MenuCommand::SetTint;
  (warmth ? balance.warmth : tint ? balance.tint : balance.exposure) = value;
  if (matching) {
    ++balanceEdits;
    return 0;
  }
  return warmth ? VideoSettings::kWarmth
         : tint ? VideoSettings::kTint
                : VideoSettings::kExposure;
}

void Passthrough::pictureReset() {
  if (roomShown()) {  // a hidden room keeps its switch for when it shows again
    roomMatched = false;
    ++balanceEdits;
  }
}

void Passthrough::resetMatch() {
  if (roomReadable && roomMatched && roomShown()) {
    const Settings defaults;
    settings.matchColour = defaults.matchColour;
    settings.matchBrightness = defaults.matchBrightness;
    saveSettings(settings);
    applyRoomMatch();
  }
}

// A room reading that ended: kept for the run and matched while the
// switch is on (unless the sliders were moved or reset since it was asked
// for), or a moment saying it failed. The switch stays as it was set:
// with no earlier reading to use nothing is matched yet, and Read again
// tries once more.
void Passthrough::pollRoom() {
  if (std::optional<RoomLight::Reading> reading; roomLight.take(reading)) {
    if (reading) {
      roomReading = reading;
      if (roomAsked == balanceEdits) applyRoomMatch();
    } else {
      roomFailedUntil = glfwGetTime() + 4;
    }
  }
}

bool Passthrough::handle(const MenuCommand& c) {
  switch (c.type) {
    case MenuCommand::ToggleMatchRoom:  // off: the video's own balance again
      toggle();
      break;
    case MenuCommand::ReadRoomAgain:
      readAgain();
      break;
    case MenuCommand::SetMatchColour:
      settings.matchColour =
          std::clamp(static_cast<int>(c.value), value_range::kPercent.least, value_range::kPercent.most);
      saveSettings(settings);
      applyRoomMatch();
      break;
    case MenuCommand::SetMatchBrightness:
      settings.matchBrightness =
          std::clamp(static_cast<int>(c.value), value_range::kPercent.least, value_range::kPercent.most);
      saveSettings(settings);
      applyRoomMatch();
      break;
    case MenuCommand::ToggleAutoPassthrough:
      settings.autoPassthrough = !settings.autoPassthrough;
      saveSettings(settings);
      resettle();  // for the video playing too
      break;
    case MenuCommand::ToggleKeyGreen:
      settings.keyGreen = !settings.keyGreen;
      saveSettings(settings);
      resettle();  // Automatic passthrough, for the video playing
      break;
    default: return false;
  }
  return true;
}

void Passthrough::fill(MenuView& view, bool blended, bool roomMatching, double now) const {
  view.autoPassthrough = settings.autoPassthrough;
  view.keyGreen = settings.keyGreen;
  view.fillChecking = fillCheckPending;
  view.balance = roomMatching ? roomBalance : video.balance;
  view.roomMatch = !roomReadable            ? RoomMatch::Hidden
                   : !blended               ? RoomMatch::Off
                   : roomLight.reading()    ? RoomMatch::Reading
                   : now < roomFailedUntil ? RoomMatch::Failed
                                            : RoomMatch::Ready;
  view.roomMatched = roomMatched;
  view.matchColour = settings.matchColour;
  view.matchBrightness = settings.matchBrightness;
}
