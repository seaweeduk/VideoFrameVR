#include "controls.h"

#include "settings.h"
#include "strings.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using Source = ControllerBindings::Source;

const ControlInfo kControlTable[] = {
    {"trigger", "Trigger"},         {"bumper", "Bumper"},           {"grip", "Grip"},
    {"a_button", "A button"},       {"b_button", "B button"},       {"x_button", "X button"},
    {"y_button", "Y button"},       {"menu_button", "Menu button"}, {"view_button", "View button"},
    {"dpad_up", "D-pad up"},        {"dpad_down", "D-pad down"},    {"dpad_left", "D-pad left"},
    {"dpad_right", "D-pad right"},  {"thumbstick_click", "Stick click"},
};
static_assert(sizeof(kControlTable) / sizeof(kControlTable[0]) == kControls, "a ControlInfo per Control");

MenuCommand command(MenuCommand::Type type, double value = 0) {
  MenuCommand c;
  c.type = type;
  c.value = value;
  return c;
}

using S = JobSection;
using N = JobNeeds;
using L = JobLength;
using M = MenuCommand;

const JobInfo kJobTable[] = {
    {"none", S::None, [](const Strings& t) { return t.jobs.nothing; }, N::Nothing, L::None, {}},
    {"menu", S::General, [](const Strings& t) { return t.jobs.menu; }, N::Nothing, L::None, {}},
    {"recenter", S::General, [](const Strings& t) { return t.jobs.recenter; }, N::Nothing,
     L::None, {}},
    {"choose", S::General, [](const Strings& t) { return t.jobs.choose; }, N::Nothing, L::None,
     {}},
    {"play_pause", S::Playback, [](const Strings& t) { return t.jobs.playPause; }, N::File,
     L::None, command(M::TogglePause)},
    {"stop", S::Playback, [](const Strings& t) { return t.jobs.stop; }, N::File, L::None,
     command(M::Stop)},
    {"skip_back", S::Playback, [](const Strings& t) { return t.jobs.back; }, N::Video, L::Skip,
     command(M::Seek, -1)},
    {"skip_forward", S::Playback, [](const Strings& t) { return t.jobs.forward; }, N::Video,
     L::Skip, command(M::Seek, 1)},
    {"big_skip_back", S::Playback, [](const Strings& t) { return t.jobs.back; }, N::Video,
     L::BigSkip, command(M::Seek, -1)},
    {"big_skip_forward", S::Playback, [](const Strings& t) { return t.jobs.forward; }, N::Video,
     L::BigSkip, command(M::Seek, 1)},
    {"frame_back", S::Playback, [](const Strings& t) { return t.jobs.frameBack; }, N::Video,
     L::None, command(M::FrameStep, -1)},
    {"frame_forward", S::Playback, [](const Strings& t) { return t.jobs.frameForward; }, N::Video,
     L::None, command(M::FrameStep, 1)},
    {"previous_chapter", S::Playback, [](const Strings& t) { return t.jobs.previousChapter; },
     N::Video, L::None, {}},
    {"next_chapter", S::Playback, [](const Strings& t) { return t.jobs.nextChapter; }, N::Video,
     L::None, {}},
    {"from_start", S::Playback, [](const Strings& t) { return t.jobs.fromStart; }, N::Video,
     L::None, command(M::SeekTo, 0)},
    {"funscripts", S::Playback, [](const Strings& t) { return t.jobs.scripts; }, N::Nothing,
     L::None, command(M::ToggleScripts)},
    {"previous_video", S::Videos, [](const Strings& t) { return t.jobs.previousVideo; }, N::File,
     L::None, command(M::PreviousVideo)},
    {"next_video", S::Videos, [](const Strings& t) { return t.jobs.nextVideo; }, N::File, L::None,
     command(M::NextVideo)},
    {"after_video", S::Videos, [](const Strings& t) { return t.jobs.afterVideo; }, N::Nothing,
     L::None, {}},
    {"rating_up", S::Videos, [](const Strings& t) { return t.jobs.ratingUp; }, N::Video, L::None,
     command(M::StepRating, 1)},
    {"rating_down", S::Videos, [](const Strings& t) { return t.jobs.ratingDown; }, N::Video,
     L::None, command(M::StepRating, -1)},
    {"delete_video", S::Videos, [](const Strings& t) { return t.jobs.deleteVideo; }, N::File, L::None, {}},
    {"volume_up", S::Sound, [](const Strings& t) { return t.jobs.volumeUp; }, N::Nothing, L::None,
     {}},
    {"volume_down", S::Sound, [](const Strings& t) { return t.jobs.volumeDown; }, N::Nothing,
     L::None, {}},
    {"mute", S::Sound, [](const Strings& t) { return t.jobs.mute; }, N::Nothing, L::None,
     command(M::ToggleMute)},
    {"next_audio_track", S::Sound, [](const Strings& t) { return t.jobs.nextAudioTrack; },
     N::Video, L::None, {}},
    {"subtitles", S::Sound, [](const Strings& t) { return t.jobs.subtitles; }, N::Video, L::None,
     {}},
    {"passthrough", S::Picture, [](const Strings& t) { return t.jobs.passthrough; }, N::Nothing,
     L::None, command(M::TogglePassthrough)},
    {"peek_room", S::Picture, [](const Strings& t) { return t.jobs.peekRoom; }, N::Nothing,
     L::None, {}, true},
    {"opacity", S::Picture, [](const Strings& t) { return t.jobs.opacity; }, N::File, L::None, {}},
    {"match_room", S::Picture, [](const Strings& t) { return t.jobs.matchRoom; },
     N::Nothing, L::None, command(M::ToggleMatchRoom)},
    {"remove_background", S::Picture, [](const Strings& t) { return t.jobs.removeBackground; }, N::File,
     L::None, {}},
    {"tone_look", S::Picture, [](const Strings& t) { return t.jobs.toneLook; }, N::Nothing, L::None,
     command(M::ToggleToneLook)},
    {"tv_gamma", S::Picture, [](const Strings& t) { return t.tvGamma; }, N::Nothing, L::None,
     command(M::ToggleTvGamma)},
    {"motion", S::Picture, [](const Strings& t) { return t.jobs.motion; }, N::Nothing, L::None,
     command(M::ToggleMotion)},
    {"zoom_in", S::Picture, [](const Strings& t) { return t.jobs.zoomIn; }, N::File, L::None, {}, true},
    {"zoom_out", S::Picture, [](const Strings& t) { return t.jobs.zoomOut; }, N::File, L::None,
     {}, true},
    {"stereo_focus", S::Picture, [](const Strings& t) { return t.jobs.stereoFocus; }, N::Nothing,
     L::None, {}},
    {"show_focus_target", S::Picture, [](const Strings& t) { return t.jobs.showFocusTarget; },
     N::Nothing, L::None, {}},
    {"reset_screen", S::Picture, [](const Strings& t) { return t.jobs.resetScreen; }, N::File,
     L::None, command(M::ResetScreen)},
    {"reset_picture", S::Picture, [](const Strings& t) { return t.jobs.resetPicture; }, N::File,
     L::None, command(M::ResetPicture)},
};
static_assert(sizeof(kJobTable) / sizeof(kJobTable[0]) == kJobs, "a JobInfo per Job");

// The same control on both hands, doing the same.
void both(std::vector<LayoutControl>& out, Control control, const char* input, Job job,
          bool fixed = false) {
  out.push_back({control, 0, input, job, fixed});
  out.push_back({control, 1, input, job, fixed});
}

std::vector<ControllerLayout> makeLayouts() {
  using C = Control;
  std::vector<ControllerLayout> layouts(kControllerTypes);
  {
    ControllerLayout& l = layouts[static_cast<int>(ControllerType::Frame)];
    l = {"frame", "/interaction_profiles/valve/frame_controller_valve", true, {}};
    both(l.controls, C::Trigger, "/input/trigger/value", Job::Choose, true);
    both(l.controls, C::Bumper, "/input/shoulder/click", Job::None);
    both(l.controls, C::Grip, "/input/squeeze/value", Job::None, true);
    both(l.controls, C::StickClick, "/input/thumbstick/click", Job::Recenter);
    // The left D-pad as the right's face buttons, by where they are.
    l.controls.insert(l.controls.end(), {
        {C::View, 0, "/input/view/click", Job::Menu, false},
        {C::DpadDown, 0, "/input/dpad_down/click", Job::PlayPause, false},
        {C::DpadLeft, 0, "/input/dpad_left/click", Job::Stop, false},
        {C::DpadRight, 0, "/input/dpad_right/click", Job::Menu, false},
        {C::DpadUp, 0, "/input/dpad_up/click", Job::NextVideo, false},
        {C::Menu, 1, "/input/menu/click", Job::Menu, false},
        {C::A, 1, "/input/a/click", Job::PlayPause, false},
        {C::X, 1, "/input/x/click", Job::Stop, false},
        {C::B, 1, "/input/b/click", Job::Menu, false},
        {C::Y, 1, "/input/y/click", Job::NextVideo, false},
    });
  }
  {
    ControllerLayout& l = layouts[static_cast<int>(ControllerType::Index)];
    l = {"index", "/interaction_profiles/valve/index_controller", true, {}};
    both(l.controls, C::Trigger, "/input/trigger/click", Job::Choose, true);
    both(l.controls, C::Grip, "/input/squeeze/value", Job::None, true);
    both(l.controls, C::A, "/input/a/click", Job::PlayPause);
    both(l.controls, C::B, "/input/b/click", Job::Menu);
    both(l.controls, C::StickClick, "/input/thumbstick/click", Job::Recenter);
  }
  {
    ControllerLayout& l = layouts[static_cast<int>(ControllerType::Touch)];
    l = {"touch", "/interaction_profiles/oculus/touch_controller", true, {}};
    both(l.controls, C::Trigger, "/input/trigger/value", Job::Choose, true);
    both(l.controls, C::Grip, "/input/squeeze/value", Job::None, true);
    both(l.controls, C::StickClick, "/input/thumbstick/click", Job::Recenter);
    l.controls.insert(l.controls.end(), {
        {C::Menu, 0, "/input/menu/click", Job::Menu, false},
        {C::X, 0, "/input/x/click", Job::PlayPause, false},
        {C::Y, 0, "/input/y/click", Job::Passthrough, false},
        {C::A, 1, "/input/a/click", Job::PlayPause, false},
        {C::B, 1, "/input/b/click", Job::Menu, false},
    });
  }
  {
    ControllerLayout& l = layouts[static_cast<int>(ControllerType::Simple)];
    l = {"simple", "/interaction_profiles/khr/simple_controller", false, {}};
    both(l.controls, C::Trigger, "/input/select/click", Job::Choose, true);
    both(l.controls, C::Menu, "/input/menu/click", Job::Menu, true);
  }
  return layouts;
}

const std::vector<ControllerLayout>& layouts() {
  static const std::vector<ControllerLayout> all = makeLayouts();
  return all;
}

const LayoutControl* find(ControllerType type, int hand, Control control) {
  if (type == ControllerType::Count) return nullptr;
  for (const LayoutControl& c : controllerLayout(type).controls)
    if (c.hand == hand && c.control == control) return &c;
  return nullptr;
}

const LayoutControl* layoutControlOf(ControllerType type, int hand, Control control) {
  return find(type, hand, control);
}

Job jobById(const std::string& id) {
  for (int j = 0; j < kJobs; ++j)
    if (id == kJobTable[j].id) return static_cast<Job>(j);
  return Job::Count;
}

const char* const kHands[2] = {"left_", "right_"};
const char* const kGrip = "grip_";  // before a hand: its job with grip held

const char* const kMovementIds[kMovements] = {"twist"};

// A gesture value's rate: its whole range in about 270° of twist, three
// quarters of a turn each way being about as far as a wrist goes.
constexpr double acrossTurn(ValueRange r) { return (r.most - r.least) / 270.0; }
// Its detents: at each tenth of the range, or each step if that is coarser.
constexpr int tenth(ValueRange r) {
  return (r.most - r.least) / 10 > r.step ? (r.most - r.least + 5) / 10 : r.step;
}

std::vector<GestureValueInfo> makeGestureValues() {
  namespace vr = value_range;
  using G = GestureSection;
  using W = GestureWhere;
  using F = GestureFormat;
  const Settings defaults;
  const ScreenSize screen;
  constexpr ValueRange kRating{0, 10, 1};  // half stars
  constexpr ValueRange kScrub{0, 0, 1};    // seconds, to main.cpp's end
  std::vector<GestureValueInfo> v = {
      {"volume", G::Sound, [](const Strings& t) { return t.controlsPage.volume; }, nullptr, W::Anywhere,
       vr::kVolume, 1.0, 5, false, kNoMark, F::Percent, M::SetVolume},
      {"opacity", G::Room, [](const Strings& t) { return t.opacity; }, nullptr, W::File, vr::kPercent,
       50.0 / 90, tenth(vr::kPercent), false, 100, F::Percent, M::SetOpacity},
      {"match_brightness", G::Room, [](const Strings& t) { return t.colourBalance.matchBrightness; },
       nullptr, W::Anywhere, vr::kPercent, acrossTurn(vr::kPercent), tenth(vr::kPercent), false,
       defaults.matchBrightness, F::Percent, M::SetMatchBrightness},
      {"match_colour", G::Room, [](const Strings& t) { return t.colourBalance.matchColour; }, nullptr,
       W::Anywhere, vr::kPercent, acrossTurn(vr::kPercent), tenth(vr::kPercent), false,
       defaults.matchColour, F::Percent, M::SetMatchColour},
      {"key_strength", G::Room, [](const Strings& t) { return t.keyStrength; },
       [](const Strings& t) { return t.greenScreenOn; }, W::File, vr::kPercent, acrossTurn(vr::kPercent),
       tenth(vr::kPercent), false, kNoMark, F::Number, M::SetKeyStrength},
      {"key_edges", G::Room, [](const Strings& t) { return t.keySoftness; },
       [](const Strings& t) { return t.greenScreenOn; }, W::File, vr::kPercent, acrossTurn(vr::kPercent),
       tenth(vr::kPercent), false, kNoMark, F::Number, M::SetKeySoftness},
      {"key_spill", G::Room, [](const Strings& t) { return t.keySpill; },
       [](const Strings& t) { return t.greenScreenOn; }, W::File, vr::kPercent, acrossTurn(vr::kPercent),
       tenth(vr::kPercent), false, kNoMark, F::Number, M::SetKeySpill},
      {"key_shrink", G::Room, [](const Strings& t) { return t.keyShrink; },
       [](const Strings& t) { return t.greenScreenOn; }, W::File, vr::kPercent, acrossTurn(vr::kPercent),
       tenth(vr::kPercent), false, kNoMark, F::Number, M::SetKeyShrink},
      {"brightness", G::Picture, [](const Strings& t) { return t.brightness; }, nullptr, W::File,
       vr::kPicture, acrossTurn(vr::kPicture), tenth(vr::kPicture), true, kNoMark, F::Signed,
       M::SetBrightness},
      {"contrast", G::Picture, [](const Strings& t) { return t.contrast; }, nullptr, W::File,
       vr::kPicture, acrossTurn(vr::kPicture), tenth(vr::kPicture), true, kNoMark, F::Signed,
       M::SetContrast},
      {"saturation", G::Picture, [](const Strings& t) { return t.saturation; }, nullptr, W::File,
       vr::kPicture, acrossTurn(vr::kPicture), tenth(vr::kPicture), true, kNoMark, F::Signed,
       M::SetSaturation},
      {"warmth", G::Picture, [](const Strings& t) { return t.colourBalance.warmth; }, nullptr, W::File,
       vr::kPicture, acrossTurn(vr::kPicture), tenth(vr::kPicture), true, kNoMark, F::Signed,
       M::SetWarmth},
      {"tint", G::Picture, [](const Strings& t) { return t.colourBalance.tint; }, nullptr, W::File,
       vr::kPicture, acrossTurn(vr::kPicture), tenth(vr::kPicture), true, kNoMark, F::Signed, M::SetTint},
      {"exposure", G::Picture, [](const Strings& t) { return t.colourBalance.exposure; }, nullptr,
       W::File, vr::kPicture, acrossTurn(vr::kPicture), tenth(vr::kPicture), true, kNoMark, F::Signed,
       M::SetExposure},
      {"sharpen", G::Picture, [](const Strings& t) { return t.sharpen; }, nullptr, W::Anywhere,
       vr::kSharpen, acrossTurn(vr::kSharpen), tenth(vr::kSharpen), false, defaults.sharpen,
       F::Hundredths, M::SetSharpen},
      {"zoom", G::Picture, [](const Strings& t) { return t.zoom; }, nullptr, W::Surround, vr::kZoom,
       acrossTurn(vr::kZoom), tenth(vr::kZoom), false, 100, F::Percent, M::SetZoom},
      {"viewpoint", G::Place, [](const Strings& t) { return t.viewpoint.label; }, nullptr, W::Surround,
       vr::kViewpoint, acrossTurn(vr::kViewpoint), tenth(vr::kViewpoint), true, kNoMark, F::Viewpoint,
       M::SetOriginForward},
      // A centimetre a degree, a buzz each 10 cm: the whole range would be
      // 4 cm a degree.
      {"screen_distance", G::Place, [](const Strings& t) { return t.gestures.screenDistance; }, nullptr,
       W::Flat, vr::kScreenDistance, 1.0, 10, false,
       value_range::hundredths(screen.distance), F::Centimetres, M::SetScreenDistance},
      {"screen_height", G::Place, [](const Strings& t) { return t.gestures.screenHeight; }, nullptr,
       W::Flat, vr::kScreenHeight, 1.0, 10, false,
       value_range::hundredths(screen.height), F::Centimetres, M::SetScreenHeight},
      {"screen_curve", G::Place, [](const Strings& t) { return t.gestures.screenCurve; }, nullptr,
       W::Flat, vr::kScreenCurve, acrossTurn(vr::kScreenCurve), tenth(vr::kScreenCurve), false,
       value_range::hundredths(screen.curve), F::Hundredths, M::SetScreenCurve},
      // A second of the video a degree, a buzz every 10 s, as the stick's held seek.
      {"scrub", G::Playback, [](const Strings& t) { return t.gestures.scrub; }, nullptr, W::Video,
       kScrub, 1.0, 10, false, kNoMark, F::Clock, M::None},
      // Half a star each 15°.
      {"rating", G::Playback, [](const Strings& t) { return t.gestures.rating; }, nullptr, W::Video,
       kRating, 1.0 / 15, 1, false, kNoMark, F::Stars, M::None},
      {"funscript_timing", G::Funscripts, [](const Strings& t) { return t.scripts.offset; },
       [](const Strings& t) { return t.scripts.group; }, W::Anywhere, vr::kScriptOffset,
       acrossTurn(vr::kScriptOffset), tenth(vr::kScriptOffset), true, kNoMark, F::Milliseconds,
       M::SetScriptOffset},
      {"subtitle_size", G::Subtitles, [](const Strings& t) { return t.subtitleSize; },
       [](const Strings& t) { return t.subtitles; }, W::Anywhere, vr::kSubtitleSize,
       acrossTurn(vr::kSubtitleSize), tenth(vr::kSubtitleSize), false, defaults.subtitleSize,
       F::Percent, M::SetSubtitleSize},
      {"subtitle_raise", G::Subtitles, [](const Strings& t) { return t.subtitleRaise; },
       [](const Strings& t) { return t.subtitles; }, W::Anywhere, vr::kSubtitleRaise,
       acrossTurn(vr::kSubtitleRaise), tenth(vr::kSubtitleRaise), true, kNoMark, F::Signed,
       M::SetSubtitleRaise},
      {"subtitle_distance", G::Subtitles, [](const Strings& t) { return t.subtitleDistance; },
       [](const Strings& t) { return t.subtitles; }, W::Anywhere, vr::kSubtitleDistance,
       acrossTurn(vr::kSubtitleDistance), tenth(vr::kSubtitleDistance), false,
       defaults.subtitleDistance, F::Metres, M::SetSubtitleDistance},
  };
  return v;
}

const std::vector<GestureValueInfo>& gestureValues() {
  static const std::vector<GestureValueInfo> all = makeGestureValues();
  return all;
}

GestureValue gestureValueById(const std::string& id) {
  for (int v = 0; v < kGestureValues; ++v)
    if (id == gestureValues()[v].id) return static_cast<GestureValue>(v);
  return GestureValue::Count;
}

Control controlById(const std::string& id) {
  for (int c = 0; c < kControls; ++c)
    if (id == kControlTable[c].id) return static_cast<Control>(c);
  return Control::Count;
}

// The saved gestures' hands.
const char* const kGestureHands[4] = {"", "left", "right", "either"};

}  // namespace

const ControlInfo& controlInfo(Control control) { return kControlTable[static_cast<int>(control)]; }

const JobInfo& jobInfo(Job job) { return kJobTable[static_cast<int>(job)]; }

std::string jobLabel(Job job, const Strings& t, int skipSeconds, int bigSkipSeconds) {
  const JobInfo& info = jobInfo(job);
  if (info.length == JobLength::None) return info.label(t);
  char label[128];
  std::snprintf(label, sizeof(label), info.label(t),
                skipLength(info.length == JobLength::Skip ? skipSeconds : bigSkipSeconds, t).c_str());
  return label;
}

std::string skipLength(int seconds, const Strings& t) {
  char text[64];
  if (seconds >= 120 && seconds % 60 == 0)
    std::snprintf(text, sizeof(text), t.controlsPage.minutes, seconds / 60);
  else
    std::snprintf(text, sizeof(text), t.controlsPage.seconds, seconds);
  return text;
}

const char* jobTip(Job job, const Strings& t) {
  const JobStrings& j = t.jobs;
  switch (job) {
    case Job::Menu: return j.menuTip;
    case Job::Recenter: return j.recenterTip;
    case Job::Choose: return j.chooseTip;
    case Job::FromStart: return j.fromStartTip;
    case Job::Scripts: return j.scriptsTip;
    case Job::RatingUp: return j.ratingUpTip;
    case Job::RatingDown: return j.ratingDownTip;
    case Job::DeleteVideo: return j.deleteTip;
    case Job::NextAudioTrack: return j.nextAudioTrackTip;
    case Job::Passthrough: return t.passthroughHint;
    case Job::SeeThrough: return j.opacityTip;
    case Job::MatchRoom: return t.colourBalance.tipMatchRoom;
    case Job::RemoveBackground: return t.greenScreenHint;
    case Job::ToneLook: return t.toneLookHint;
    case Job::TvGamma: return t.tvGammaHint;
    case Job::ZoomIn:
    case Job::ZoomOut: return j.zoomTip;
    case Job::StereoFocus: return t.focus.enabledHint;
    case Job::ShowFocusTarget: return t.focus.showTargetHint;
    case Job::ResetScreen: return j.resetScreenTip;
    case Job::FrameBack: return j.frameBackTip;
    case Job::FrameForward: return j.frameForwardTip;
    case Job::AfterVideo: return j.afterVideoTip;
    case Job::PeekRoom: return j.peekRoomTip;
    case Job::Motion: return j.motionTip;
    case Job::ResetPicture: return t.colourBalance.tipReset;
    default: return nullptr;
  }
}

const char* jobSectionTitle(JobSection section, const Strings& t) {
  switch (section) {
    case JobSection::General: return t.general;
    case JobSection::Playback: return t.groupPlayback;
    case JobSection::Videos: return t.jobs.videos;
    case JobSection::Sound: return t.jobs.sound;
    case JobSection::Picture: return t.picture;
    case JobSection::None: break;
  }
  return "";
}

const ControllerLayout& controllerLayout(ControllerType type) {
  return layouts()[static_cast<int>(type)];
}

ControllerType controllerType(const std::string& profile) {
  for (int t = 0; t < kControllerTypes; ++t)
    if (profile == layouts()[t].profile) return static_cast<ControllerType>(t);
  return ControllerType::Count;
}

ControllerType controllerTypeById(const std::string& id) {
  for (int t = 0; t < kControllerTypes; ++t)
    if (id == layouts()[t].id) return static_cast<ControllerType>(t);
  return ControllerType::Count;
}

ControllerBindings layoutSources(ControllerType type) {
  ControllerBindings b;
  if (type == ControllerType::Count) return b;
  const ControllerLayout& layout = controllerLayout(type);
  for (int h = 0; h < 2; ++h) b.profiles[h] = layout.profile;
  for (const LayoutControl& c : layout.controls)
    b.sources[static_cast<int>(c.control == Control::Grip ? ControlAction::Grab : ControlAction::Select)]
        .push_back({c.hand, c.input});
  if (layout.stick)
    for (int h = 0; h < 2; ++h)
      b.sources[static_cast<int>(ControlAction::Stick)].push_back({h, "/input/thumbstick"});
  return b;
}

ControlMap::ControlMap() {
  std::memset(chosen_, kDefault, sizeof(chosen_));
  std::memset(grip_, kDefault, sizeof(grip_));
}

Job ControlMap::job(ControllerType type, int hand, Control control) const {
  if (type == ControllerType::Count) return control == Control::Trigger ? Job::Choose : Job::None;
  const LayoutControl* c = find(type, hand, control);
  if (!c) return Job::None;
  const uint8_t chosen = chosen_[static_cast<int>(type)][hand][static_cast<int>(control)];
  return c->fixed || chosen == kDefault ? c->job : static_cast<Job>(chosen);
}

bool ControlMap::set(ControllerType type, int hand, Control control, Job job) {
  const LayoutControl* c = find(type, hand, control);
  if (!c || c->fixed || job == Job::Count) return false;
  chosen_[static_cast<int>(type)][hand][static_cast<int>(control)] =
      job == c->job ? kDefault : static_cast<uint8_t>(job);
  if (this->job(type, hand, control) == Job::Choose)
    grip_[static_cast<int>(type)][hand][static_cast<int>(control)] = kDefault;
  return true;
}

int ControlMap::count(ControllerType type, Job job) const {
  int n = 0;
  if (type != ControllerType::Count)
    for (const LayoutControl& c : controllerLayout(type).controls)
      n += this->job(type, c.hand, c.control) == job;
  return n;
}

bool ControlMap::allowed(ControllerType type, int hand, Control control, Job job) const {
  return job == Job::Menu || this->job(type, hand, control) != Job::Menu || count(type, Job::Menu) > 1;
}

bool ControlMap::takesGrip(ControllerType type, int hand, Control control) const {
  const LayoutControl* c = type == ControllerType::Count ? nullptr : find(type, hand, control);
  return c && !c->fixed && control != Control::StickClick && job(type, hand, control) != Job::Choose;
}

bool ControlMap::hasGrip(ControllerType type, int hand, Control control) const {
  return takesGrip(type, hand, control) &&
         grip_[static_cast<int>(type)][hand][static_cast<int>(control)] != kDefault;
}

Job ControlMap::gripJob(ControllerType type, int hand, Control control) const {
  if (!hasGrip(type, hand, control)) return Job::None;
  return static_cast<Job>(grip_[static_cast<int>(type)][hand][static_cast<int>(control)]);
}

bool ControlMap::setGrip(ControllerType type, int hand, Control control, Job job) {
  if (!takesGrip(type, hand, control) || job == Job::Count) return false;
  grip_[static_cast<int>(type)][hand][static_cast<int>(control)] = static_cast<uint8_t>(job);
  return true;
}

bool ControlMap::clearGrip(ControllerType type, int hand, Control control) {
  if (!takesGrip(type, hand, control)) return false;
  grip_[static_cast<int>(type)][hand][static_cast<int>(control)] = kDefault;
  return true;
}

int ControlMap::button(ControllerType type, int hand, Control control) const {
  const int grip = hasGrip(type, hand, control) ? static_cast<int>(gripJob(type, hand, control)) : -1;
  return static_cast<int>(job(type, hand, control)) + kJobs * (grip + 1);
}

bool ControlMap::restore(ControllerType type, int hand, Control control, int button) {
  const int plain = button % kJobs, grip = button / kJobs - 1;
  if (button < 0 || grip >= kJobs || !set(type, hand, control, static_cast<Job>(plain))) return false;
  if (grip < 0) clearGrip(type, hand, control);
  else setGrip(type, hand, control, static_cast<Job>(grip));
  return true;
}

void ControlMap::keepMenu(ControllerType type) {
  if (count(type, Job::Menu) > 0) return;
  for (const LayoutControl& c : controllerLayout(type).controls)
    if (c.job == Job::Menu) chosen_[static_cast<int>(type)][c.hand][static_cast<int>(c.control)] = kDefault;
}

void ControlMap::reset(ControllerType type) {
  std::memset(chosen_[static_cast<int>(type)], kDefault, sizeof(chosen_[0]));
  std::memset(grip_[static_cast<int>(type)], kDefault, sizeof(grip_[0]));
}

std::string ControlMap::line(ControllerType type) const {
  std::string out;
  for (const LayoutControl& c : controllerLayout(type).controls) {
    const uint8_t chosen = chosen_[static_cast<int>(type)][c.hand][static_cast<int>(c.control)];
    if (c.fixed || chosen == kDefault) continue;
    if (!out.empty()) out += ',';
    out += std::string(kHands[c.hand]) + controlInfo(c.control).id + ':' +
           jobInfo(static_cast<Job>(chosen)).id;
  }
  // Read after the jobs alone: whether a button takes one depends on its own.
  for (const LayoutControl& c : controllerLayout(type).controls) {
    if (!hasGrip(type, c.hand, c.control)) continue;
    const Job grip = gripJob(type, c.hand, c.control);
    if (!out.empty()) out += ',';
    out += std::string(kGrip) + kHands[c.hand] + controlInfo(c.control).id + ':' + jobInfo(grip).id;
  }
  return out;
}

void ControlMap::read(ControllerType type, const std::string& line) {
  size_t at = 0;
  while (at < line.size()) {
    size_t end = line.find(',', at);
    if (end == std::string::npos) end = line.size();
    const std::string entry = line.substr(at, end - at);
    at = end + 1;
    const size_t colon = entry.find(':');
    if (colon == std::string::npos) continue;
    std::string where = entry.substr(0, colon);
    const bool grip = where.compare(0, std::strlen(kGrip), kGrip) == 0;
    if (grip) where.erase(0, std::strlen(kGrip));
    for (int hand = 0; hand < 2; ++hand) {
      const size_t prefix = std::strlen(kHands[hand]);
      if (where.compare(0, prefix, kHands[hand]) != 0) continue;
      for (int c = 0; c < kControls; ++c)
        if (where.compare(prefix, std::string::npos, kControlTable[c].id) == 0)
          set({type, hand, static_cast<Control>(c), grip}, jobById(entry.substr(colon + 1)));
    }
  }
  keepMenu(type);
}

bool ControlMap::operator==(const ControlMap& o) const {
  return std::memcmp(chosen_, o.chosen_, sizeof(chosen_)) == 0 &&
         std::memcmp(grip_, o.grip_, sizeof(grip_)) == 0;
}

ControllerBindings describeControls(const std::string profiles[2], const ControlMap& map,
                                    const GestureMap& gestures) {
  ControllerBindings b;
  for (int h = 0; h < 2; ++h) {
    b.profiles[h] = profiles[h];
    const ControllerType type = controllerType(profiles[h]);
    if (type == ControllerType::Count) continue;
    const ControllerLayout& layout = controllerLayout(type);
    auto actionOf = [](Job job) {
      switch (job) {
        case Job::Choose: return ControlAction::Select;
        case Job::Menu: return ControlAction::Menu;
        case Job::PlayPause: return ControlAction::PlayPause;
        case Job::Recenter: return ControlAction::Recenter;
        default: return ControlAction::Count;
      }
    };
    for (const LayoutControl& c : layout.controls) {
      if (c.hand != h) continue;
      const ControlAction action =
          c.control == Control::Grip ? ControlAction::Grab : actionOf(map.job(type, h, c.control));
      if (action != ControlAction::Count) b.sources[static_cast<int>(action)].push_back({h, c.input});
      const ControlAction grip =
          map.hasGrip(type, h, c.control) ? actionOf(map.gripJob(type, h, c.control)) : ControlAction::Count;
      if (grip != ControlAction::Count) b.gripSources[static_cast<int>(grip)].push_back({h, c.input});
    }
    if (layout.stick) b.sources[static_cast<int>(ControlAction::Stick)].push_back({h, "/input/thumbstick"});
    for (const Gesture& g : gestures.list(type)) {
      if (!(g.hands >> h & 1)) continue;
      ControllerBindings::Twist twist;
      twist.value = static_cast<int>(g.value);
      for (const Control control : {g.first, g.second})
        for (const LayoutControl& c : layout.controls)
          if (c.hand == h && c.control == control) twist.controls.push_back({h, c.input});
      if (static_cast<int>(twist.controls.size()) == g.controls()) b.twists.push_back(std::move(twist));
    }
  }
  return b;
}

void logControls(const ControllerBindings& bindings) {
  const char* const names[kControlActions] = {"select", "grab", "menu", "play_pause", "recenter", "stick"};
  for (int a = 0; a < kControlActions; ++a) {
    std::string line;
    for (const Source& s : bindings.sources[a])
      line += std::string(s.hand ? " right" : " left") + s.input;
    for (const Source& s : bindings.gripSources[a])
      line += std::string(s.hand ? " grip+right" : " grip+left") + s.input;
    std::printf("Controllers: %s:%s\n", names[a], line.empty() ? " none" : line.c_str());
  }
}

ControlInput::ControlInput() { std::memset(active_, kFree, sizeof(active_)); }

void ControlInput::cancelTap(int hand, Control control) {
  Tap& tap = tap_[hand][static_cast<int>(control)];
  if (tap == Tap::Waiting) tap = Tap::Done;
}

void ControlInput::dropFileTaps() {
  for (int h = 0; h < 2; ++h)
    for (int c = 0; c < kControls; ++c)
      if (tap_[h][c] == Tap::Waiting && active_[h][c] < kJobs &&
          jobInfo(static_cast<Job>(active_[h][c])).needs != JobNeeds::Nothing)
        tap_[h][c] = Tap::Done;
}

const JobInput& ControlInput::update(const ControllerState& raw, const ControllerType types[2],
                                     const ControlMap& map, const GestureMap* gestures, double now) {
  const bool selectWas[2] = {jobs_.state.hands[0].select, jobs_.state.hands[1].select};
  jobs_ = JobInput{};
  jobs_.state = raw;
  const bool refocused = raw.focusLosses != focusLosses_;
  focusLosses_ = raw.focusLosses;
  for (int h = 0; h < 2; ++h) {
    HandState& hand = jobs_.state.hands[h];
    // Gone, out of focus or another controller: what it held isn't let go.
    const bool changed = types_[h] != ControllerType::Count && types[h] != types_[h];
    types_[h] = types[h];
    if (!raw.focused || refocused || !hand.present || changed)
      for (int c = 0; c < kControls; ++c) {
        if (active_[h][c] == kFree) continue;
        active_[h][c] = kConsumed;
        tap_[h][c] = Tap::None;
        jobs_.lost |= 1 << h;
      }
    // The grip first: a consumed one isn't held, for the other buttons either.
    const uint32_t gripBit = controlBit(Control::Grip);
    const int grip = static_cast<int>(Control::Grip);
    if (hand.pressed & consume_[h] & gripBit) active_[h][grip] = kConsumed;
    if (((hand.controls | hand.pressed) & gripBit) && active_[h][grip] == kConsumed)
      hand.grip = hand.gripPressed = false;
    const uint32_t held = (hand.controls & ~gripBit) | (hand.grip ? gripBit : 0u);
    bool select = false, choosePressed = false;
    for (int c = 0; c < kControls; ++c) {
      const uint32_t bit = 1u << c;
      const Control control = static_cast<Control>(c);
      uint8_t& active = active_[h][c];
      Tap& tap = tap_[h][c];
      const auto fire = [&](Job job, bool pressed) {
        if (pressed) jobs_.pressed[static_cast<int>(job)] |= 1 << h;
        jobs_.held[static_cast<int>(job)] |= 1 << h;
      };
      if (!((hand.controls | hand.pressed) & bit)) {
        // Let go: a press that waited does its job now, as a tap.
        if (tap == Tap::Waiting && active != kFree && active != kConsumed &&
            static_cast<Job>(active) != Job::None && static_cast<Job>(active) != Job::Choose)
          fire(static_cast<Job>(active), true);
        active = kFree;
        tap = Tap::None;
        continue;
      }
      // Its job from when it went down; held since before the map knew the
      // type, its own.
      if (hand.pressed & bit) {
        active = consume_[h] & bit ? kConsumed
                                   : static_cast<uint8_t>(map.pressJob(types[h], h, control, hand.grip));
        tap = Tap::None;
        if (active != kConsumed && gestures && types[h] != ControllerType::Count &&
            gestures->tapOnRelease(types[h], h, control, held)) {
          tap = Tap::Waiting;
          pressedAt_[h][c] = now;
        }
      } else if (active == kFree) {
        active = static_cast<uint8_t>(map.job(types[h], h, control));
      }
      if (active == kConsumed) continue;
      const Job job = static_cast<Job>(active);
      if (job == Job::Choose) {
        select = select || (hand.controls & bit);
        choosePressed = choosePressed || (hand.pressed & bit);
        continue;
      }
      if (job == Job::None) continue;
      if (tap != Tap::None) {
        if (tap == Tap::Waiting && !(hand.controls & bit)) {  // down and up within a frame
          fire(job, true);
          tap = Tap::Done;
        } else if (tap == Tap::Waiting && jobInfo(job).held && now - pressedAt_[h][c] >= kHoldStart) {
          fire(job, true);
          tap = Tap::Holding;
        } else if (tap == Tap::Holding) {
          fire(job, false);
        }
        continue;
      }
      if (hand.controls & bit) jobs_.held[static_cast<int>(job)] |= 1 << h;
      if (hand.pressed & bit) jobs_.pressed[static_cast<int>(job)] |= 1 << h;
    }
    consume_[h] = 0;
    // A consumed press isn't held for what reads the state either (gestures).
    uint32_t consumed = 0;
    for (int c = 0; c < kControls; ++c)
      if (active_[h][c] == kConsumed) consumed |= 1u << c;
    hand.controls &= ~consumed;
    hand.pressed &= ~consumed;
    // Several buttons that choose act as one: pressed when the first goes
    // down, and only by a press.
    hand.select = select;
    hand.selectPressed = select && !selectWas[h] && choosePressed;
  }
  return jobs_;
}

const char* movementId(Movement movement) { return kMovementIds[static_cast<int>(movement)]; }

const GestureValueInfo& gestureValueInfo(GestureValue value) {
  return gestureValues()[static_cast<int>(value)];
}

const char* gestureSectionTitle(GestureSection section, const Strings& t) {
  switch (section) {
    case GestureSection::Sound: return t.jobs.sound;
    case GestureSection::Room: return t.gestures.room;
    case GestureSection::Picture: return t.picture;
    case GestureSection::Place: return t.gestures.place;
    case GestureSection::Playback: return t.groupPlayback;
    case GestureSection::Funscripts: return t.scripts.group;
    case GestureSection::Subtitles: return t.subtitles;
    case GestureSection::Count: break;
  }
  return "";
}

std::string gestureValueLabel(GestureValue value, const Strings& t) {
  const GestureValueInfo& info = gestureValueInfo(value);
  if (!info.group) return info.label(t);
  // The chooser's heading keeps the full "Remove background"; the name, shown
  // in a list column, says the shorter "Background".
  const char* group = info.section == GestureSection::Room ? t.gestures.background : info.group(t);
  char label[256];
  std::snprintf(label, sizeof(label), t.gestures.grouped, group, info.label(t));
  return label;
}

std::string gestureValueText(GestureValue value, double number) {
  const int n = static_cast<int>(std::lround(number));
  char text[64];
  switch (gestureValueInfo(value).format) {
    case GestureFormat::Number: std::snprintf(text, sizeof(text), "%d", n); break;
    case GestureFormat::Signed:
      // With a true minus, as a number centred on 0 reads.
      std::snprintf(text, sizeof(text), n > 0 ? "+%d" : n < 0 ? "\u2212%d" : "%d", std::abs(n));
      break;
    case GestureFormat::Percent: std::snprintf(text, sizeof(text), "%d %%", n); break;
    case GestureFormat::Centimetres: std::snprintf(text, sizeof(text), "%d cm", n); break;
    case GestureFormat::Metres: std::snprintf(text, sizeof(text), "%g m", n / 100.0); break;
    case GestureFormat::Hundredths: std::snprintf(text, sizeof(text), "%.2f", n / 100.0); break;
    case GestureFormat::Viewpoint: return viewpointText(n);
    case GestureFormat::Milliseconds:
      std::snprintf(text, sizeof(text), n > 0 ? "+%d ms" : n < 0 ? "\u2212%d ms" : "%d ms", std::abs(n));
      break;
    case GestureFormat::Clock: return clockTime(number);
    case GestureFormat::Stars: std::snprintf(text, sizeof(text), "%g", n / 2.0); break;
  }
  return text;
}

GestureMap::GestureMap() {
  for (int t = 0; t < kControllerTypes; ++t) lists_[t] = defaultList(static_cast<ControllerType>(t));
}

const std::vector<Gesture>& GestureMap::list(ControllerType type) const {
  static const std::vector<Gesture> none;
  return type == ControllerType::Count ? none : *lists_[static_cast<int>(type)];
}

const std::vector<Gesture>& GestureMap::defaults(ControllerType type) {
  static const std::vector<Gesture> none;
  return type == ControllerType::Count ? none : *defaultList(type);
}

namespace {

std::vector<Gesture> builtDefaults(ControllerType type) {
  using C = Control;
  using V = GestureValue;
  constexpr C kOne = C::Count;
  std::vector<Gesture> out;
  switch (type) {
    case ControllerType::Frame:
      out = {{2, C::Y, kOne, Movement::Twist, V::Opacity}, {1, C::DpadUp, kOne, Movement::Twist, V::Opacity}};
      break;
    case ControllerType::Touch: out = {{1, C::Y, kOne, Movement::Twist, V::Opacity}}; break;
    case ControllerType::Index: out = {{3, C::A, kOne, Movement::Twist, V::Opacity}}; break;
    case ControllerType::Simple:
    case ControllerType::Count: break;
  }
  if (layoutControlOf(type, 0, C::Grip) && layoutControlOf(type, 1, C::Grip))
    out.push_back({3, C::Grip, C::Trigger, Movement::Twist, V::Volume});
  return out;
}

}  // namespace

const GestureMap::List& GestureMap::defaultList(ControllerType type) {
  static const auto lists = [] {
    std::array<List, kControllerTypes> out;
    for (int t = 0; t < kControllerTypes; ++t)
      out[t] = std::make_shared<const std::vector<Gesture>>(builtDefaults(static_cast<ControllerType>(t)));
    return out;
  }();
  return lists[static_cast<int>(type)];
}

bool GestureMap::valid(ControllerType type, const Gesture& g) {
  if (type == ControllerType::Count || g.hands < 1 || g.hands > 3 || g.movement >= Movement::Count ||
      g.value >= GestureValue::Count || g.first >= Control::Count || g.first == Control::Trigger ||
      g.first == g.second)
    return false;
  for (int h = 0; h < 2; ++h) {
    if (!(g.hands >> h & 1)) continue;
    for (const Control c : {g.first, g.second}) {
      if (c == Control::Count) continue;
      const LayoutControl* lc = layoutControlOf(type, h, c);
      if (!lc || (lc->fixed && c != Control::Grip && c != Control::Trigger)) return false;
    }
  }
  return true;
}

int gestureColumn(const Gesture& g) { return g.hands == 3 ? 1 : g.hands == 1 ? 0 : 2; }

int gestureColumnRoom(ControllerType type, int column) {
  if (column != 1) return kGestureRows;
  // The fixed ones (fixedGestures() in control_rules.h): Horizon and
  // Resize; with a grip, Move the menu; with a grip and sticks, Height and
  // Clear rating. Then Add gesture.
  const bool grip = layoutControlOf(type, 0, Control::Grip) != nullptr;
  const bool stick = type != ControllerType::Count && controllerLayout(type).stick;
  return kGestureRows - 2 - grip - 2 * (grip && stick) - 1;
}

namespace {

// What stays of `o` where `g` takes its hold: nothing, or the hand `g`
// leaves it (one of either hand's replaced on one hand).
bool keepsOtherHand(const Gesture& o, const Gesture& g, Gesture& rest) {
  rest = o;
  rest.hands = static_cast<uint8_t>(o.hands & ~g.hands);
  return rest.hands != 0;
}

// Each column has room for those in it.
bool columnsFit(ControllerType type, const std::vector<Gesture>& list) {
  int rows[3] = {0, 0, 0};
  for (const Gesture& g : list) ++rows[gestureColumn(g)];
  for (int column = 0; column < 3; ++column)
    if (rows[column] > gestureColumnRoom(type, column)) return false;
  return true;
}

}  // namespace

bool GestureMap::add(ControllerType type, const Gesture& g, std::vector<Gesture>* replaced) {
  if (!valid(type, g)) return false;
  const std::vector<Gesture>& list = this->list(type);
  std::vector<Gesture> kept, gone;
  // In the place of the first it replaces, else at the end.
  size_t at = list.size();
  for (const Gesture& o : list) {
    Gesture rest;
    if (!o.sameHold(g)) {
      kept.push_back(o);
      continue;
    }
    if (gone.empty()) at = kept.size();
    gone.push_back(o);
    if (keepsOtherHand(o, g, rest)) kept.push_back(rest);
  }
  if (kept.size() >= kMaxGestures) return false;
  kept.insert(kept.begin() + static_cast<std::ptrdiff_t>(std::min(at, kept.size())), g);
  if (!columnsFit(type, kept)) return false;
  lists_[static_cast<int>(type)] = std::make_shared<const std::vector<Gesture>>(std::move(kept));
  if (replaced) *replaced = std::move(gone);
  return true;
}

bool GestureMap::replace(ControllerType type, size_t index, const Gesture& g,
                         std::vector<Gesture>* replaced) {
  if (!valid(type, g) || index >= list(type).size()) return false;
  const std::vector<Gesture>& list = this->list(type);
  const Gesture old = list[index];
  std::vector<Gesture> kept, gone{old};
  for (size_t i = 0; i < list.size(); ++i) {
    Gesture rest;
    if (i == index) {
      kept.push_back(g);
    } else if (list[i].sameHold(g)) {
      gone.push_back(list[i]);
      if (keepsOtherHand(list[i], g, rest)) kept.push_back(rest);
    } else {
      kept.push_back(list[i]);
    }
  }
  if (kept.size() > kMaxGestures || !columnsFit(type, kept)) return false;
  lists_[static_cast<int>(type)] = std::make_shared<const std::vector<Gesture>>(std::move(kept));
  if (replaced) *replaced = std::move(gone);
  return true;
}

bool GestureMap::remove(ControllerType type, size_t index) {
  if (index >= list(type).size()) return false;
  std::vector<Gesture> list = this->list(type);
  list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
  lists_[static_cast<int>(type)] = std::make_shared<const std::vector<Gesture>>(std::move(list));
  return true;
}

void GestureMap::restore(ControllerType type, const std::vector<Gesture>& gestures) {
  if (type == ControllerType::Count) return;
  lists_[static_cast<int>(type)] = std::make_shared<const std::vector<Gesture>>();
  for (const Gesture& g : gestures) add(type, g);
}

void GestureMap::reset(ControllerType type) {
  if (type != ControllerType::Count) lists_[static_cast<int>(type)] = defaultList(type);
}

const Gesture* GestureMap::find(ControllerType type, int hand, Control a, Control b,
                                Movement movement) const {
  const uint32_t bits = controlBit(a) | (b == Control::Count ? 0u : controlBit(b));
  for (const Gesture& g : list(type))
    if ((g.hands >> hand & 1) && g.bits() == bits && g.movement == movement) return &g;
  return nullptr;
}

bool GestureMap::tapOnRelease(ControllerType type, int hand, Control control, uint32_t held) const {
  if (control == Control::Trigger) return false;
  for (const Gesture& g : list(type)) {
    if (!(g.hands >> hand & 1) || !g.has(control)) continue;
    // With the grip, a press of the other button is the gesture's only with
    // grip held (GestureInput starts it so); two buttons, in either order.
    if (control != Control::Grip && g.has(Control::Grip) && !(held & controlBit(Control::Grip))) continue;
    return true;
  }
  return false;
}

Control choosingButton(const ControlMap& map, ControllerType type, int hand, const Gesture& g) {
  for (const Control c : {g.first, g.second})
    if (c != Control::Count && c != Control::Trigger && c != Control::Grip &&
        map.pressJob(type, hand, c, g.has(Control::Grip)) == Job::Choose)
      return c;
  return Control::Count;
}

std::string GestureMap::line(ControllerType type) const {
  if (type == ControllerType::Count) return std::string();
  const List& now = lists_[static_cast<int>(type)];
  return now == defaultList(type) || *now == *defaultList(type) ? std::string() : listLine(type);
}

std::string GestureMap::listLine(ControllerType type) const {
  std::string out;
  for (const Gesture& g : list(type)) {
    if (!out.empty()) out += ',';
    out += std::string(kGestureHands[g.hands]) + ':' + controlInfo(g.first).id;
    if (g.second != Control::Count) out += std::string("+") + controlInfo(g.second).id;
    out += std::string(":") + movementId(g.movement) + ':' + gestureValueInfo(g.value).id;
  }
  return out.empty() ? "none" : out;
}

void GestureMap::read(ControllerType type, const std::string& line) {
  if (type == ControllerType::Count) return;
  if (line.empty()) {
    reset(type);
    return;
  }
  std::vector<Gesture> read;
  size_t at = 0;
  while (at < line.size()) {
    size_t end = line.find(',', at);
    if (end == std::string::npos) end = line.size();
    const std::string entry = line.substr(at, end - at);
    at = end + 1;
    std::string fields[4];
    size_t from = 0;
    int n = 0;
    for (; n < 4 && from <= entry.size(); ++n) {
      size_t colon = entry.find(':', from);
      if (colon == std::string::npos) colon = entry.size();
      fields[n] = entry.substr(from, colon - from);
      from = colon + 1;
    }
    if (n < 4 || from <= entry.size()) continue;  // not four fields
    Gesture g;
    for (uint8_t h = 1; h <= 3; ++h)
      if (fields[0] == kGestureHands[h]) g.hands = h;
    const size_t plus = fields[1].find('+');
    g.first = controlById(fields[1].substr(0, plus));
    if (plus != std::string::npos) {
      g.second = controlById(fields[1].substr(plus + 1));
      if (g.second == Control::Count) continue;
    }
    g.movement = Movement::Count;
    for (int m = 0; m < kMovements; ++m)
      if (fields[2] == kMovementIds[m]) g.movement = static_cast<Movement>(m);
    g.value = gestureValueById(fields[3]);
    read.push_back(g);
  }
  restore(type, read);
}

bool GestureMap::operator==(const GestureMap& o) const {
  for (int t = 0; t < kControllerTypes; ++t)
    if (lists_[t] != o.lists_[t] && *lists_[t] != *o.lists_[t]) return false;
  return true;
}
