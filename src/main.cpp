#include "anti_shimmer.h"
#include "app_dirs.h"
#include "autofocus.h"
#include "bluray.h"
#include "button_jobs.h"
#include "direct_depth.h"
#include "direct_video.h"
#include "eye_resolution.h"
#include "eye_targets.h"
#include "file_util.h"
#include "first_run.h"
#include "focus_marker.h"
#include "focus_target.h"
#include "gaze_correction.h"
#include "gesture_input.h"
#include "foveation.h"
#include "frame_reuse.h"
#include "frame_timer.h"
#include "hints.h"
#include "video_check.h"
#include "removable_drives.h"
#include "gl_interop.h"
#include "gl_util.h"
#include "gpu_context.h"
#include "key_picker.h"
#include "library.h"
#include "library_index.h"
#include "mask_probe.h"
#include "menu.h"
#include "mirror_window.h"
#include "motion.h"
#include "mpv_gl_context.h"
#include "notices.h"
#include "offline.h"
#include "options.h"
#include "passthrough.h"
#include "performance_readout.h"
#include "photo_preloader.h"
#include "pixel_path.h"
#include "platform.h"
#include "presets.h"
#include "refresh_rate.h"
#include "decoder_holders.h"
#include "deletion.h"
#include "diagnostics.h"
#include "release_update.h"
#include "run_log.h"
#include "running_video.h"
#include "scene_renderer.h"
#include "screen_control.h"
#include "see_through.h"
#include "seek_preview.h"
#include "setting_commands.h"
#include "settings.h"
#include "setup_intro.h"
#include "setup_placement.h"
#include "remote.h"
#include "stash.h"
#include "media_server.h"
#include "stash_http.h"
#include "stash_stream.h"
#include "stick_seek.h"
#include "strings.h"
#include "subtitle_overlay.h"
#include "thumbnailer.h"
#include "uploader.h"
#include "video_format.h"
#include "video_memory.h"
#include "video_player.h"
#include "value_adjust.h"
#include "vr_input.h"
#include "vr_space.h"
#include "whats_new.h"
#include "xmath.h"
#include "xr_context.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <malloc.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

// The pointer beam starts 1 m long; pointing at no panel, it
// keeps the length of its last hit. Its dot is 20 menu layout units wide.
constexpr float kLaserStartLength = 1.0f;  // meters
constexpr float kLaserDotRadius = 10 * menu_geometry::kUnit;
constexpr float kSurroundSubtitleDistance = 2.5f;
constexpr float kSurroundSubtitleWidth = 2.0f;
constexpr float kSurroundSubtitleDrop = 0.55f;
constexpr float kTurnStep = kPi / 6;  // 30 degrees per A or D press
// The viewpoint's and opacity's overlay stays this long after its gesture lets go.
constexpr double kValueShown = 1.5;
// A microSD card or USB drive put in says so this long.
constexpr double kDriveNoticeShown = 5.0;
// Dragging the picture with the trigger: a smoothed copy of the hand's turn
// catches up with the real one at this rate (per second, about a 250 ms lag),
// which calms hand jitter. Frame gaps longer than kDragMaxStep count as that long.
constexpr float kDragResponse = 4.0f;
constexpr double kDragMaxStep = 0.1;  // seconds

struct App {
  VideoPlayer* player = nullptr;
  int slideshowSeconds = 0;  // photos: show the next one after this long; 0 = off
  double nextSlideAt = 0;    // glfwGetTime() when it does
  // Picking the background key's colour (MenuCommand::PickKeyColour): the
  // video it is for (empty: not picking); a press to read it at keyPickUv
  // within keyPickPart (KeyPicker::pointOf()); why the last one gave none.
  std::string keyPickFor;
  bool keyPickWanted = false;
  float keyPickUv[2] = {}, keyPickPart[4] = {};
  KeyPickNote keyPickNote = KeyPickNote::None;
  double recenterAt = 0;  // glfwGetTime() of a recenter counting down; 0 = none
};

std::string utf8(unsigned int codepoint) {
  std::string s;
  if (codepoint < 0x80) {
    s += static_cast<char>(codepoint);
  } else if (codepoint < 0x800) {
    s += static_cast<char>(0xC0 | (codepoint >> 6));
    s += static_cast<char>(0x80 | (codepoint & 0x3F));
  } else if (codepoint < 0x10000) {
    s += static_cast<char>(0xE0 | (codepoint >> 12));
    s += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    s += static_cast<char>(0x80 | (codepoint & 0x3F));
  } else {
    s += static_cast<char>(0xF0 | (codepoint >> 18));
    s += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
    s += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    s += static_cast<char>(0x80 | (codepoint & 0x3F));
  }
  return s;
}

void printControls() {
  std::printf(
      "\nControllers:\n"
      "  Trigger        click in menu, hold on a list to drag it; elsewhere hold and\n"
      "                 move to drag the picture (sideways, up/down; twist the wrist to\n"
      "                 level the horizon), or the flat screen; on its corner: resize it\n"
      "  Menu / B       open or close menu (Frame: right Menu and B, left View and\n"
      "                 D-pad right; Touch: left Menu, right B; Index: B)\n"
      "  A              play / pause (Touch: also left X; Frame: also left D-pad down)\n"
      "  X              Frame: stop (also left D-pad left)\n"
      "  Y              Frame: next video (also left D-pad up); Touch: left Y, passthrough\n"
      "  Stick click    recenter\n"
      "                 (change what each button does in Settings > Controls)\n"
      "  Stick          left/right: seek (hold to go faster)\n"
      "  Stick up/down  scroll lists; flat screen: distance (with a grip held: height);\n"
      "                 180°/360°: move toward or away from the picture (also while dragging)\n"
      "  Grip           hold on the menu to move it\n"
      "  Gestures       hold and twist the wrist (change them in Settings > Controls):\n"
      "                 opacity: Frame right Y or left D-pad up, Touch left Y, Index A;\n"
      "                 volume: grip, then the trigger\n"
      "\nKeyboard (the desktop window must have focus):\n"
      "  Tab          open or close menu\n"
      "  Space        play / pause\n"
      "  Left/Right   seek (hold to go faster)\n"
      "  A / D        turn the view left / right\n"
      "  Up/Down      next / previous video in the folder\n"
      "  + / -        volume\n"
      "  M            mute\n"
      "  P            projection: flat / 180 / 360 / fisheye 180 / 190 / 200 / 360 cubemap\n"
      "  L            stereo layout: mono / side-by-side / top-bottom\n"
      "  C            curved / flat screen (the curve itself: video settings)\n"
      "  T            passthrough on / off (the headset's cameras; in green: Settings)\n"
      "  W/S          screen closer / farther\n"
      "  Q/E          screen lower / higher (its height; the width follows)\n"
      "  R            recenter in front of you\n"
      "  F            flip video vertically\n"
      "  Esc          leave the search keyboard\n"
      "  Esc Esc      quit (press twice)\n\n");
}

// Text from the desktop clipboard, on one line.
std::string clipboardText(GLFWwindow* window) {
  const char* clip = glfwGetClipboardString(window);
  std::string text = clip ? clip : "";
  std::replace(text.begin(), text.end(), '\n', ' ');
  std::replace(text.begin(), text.end(), '\r', ' ');
  return text;
}

// The desktop window's keys, from GLFW's callbacks (on the GL thread, inside
// glfwPollEvents): what they ask for, until the frame loop takes it. Keys for
// the running video change it at once.
struct KeyboardInput {
  App& app;
  RunningVideo& running;
  bool recenter = false;
  bool togglePassthrough = false;  // T key; handled where the settings are
  bool toggleMenu = false;
  bool seekBackKey = false;     // arrow keys held down
  bool seekForwardKey = false;
  bool turnLeftKey = false;     // A / D held down
  bool turnRightKey = false;
  int videoStep = 0;            // +1 / -1: switch to the next / previous video
  // While the menu's search field is open, the keyboard types into it.
  bool textInput = false;
  std::string typed;
  int backspaces = 0;
  int cursorSteps = 0;  // ← → (Home and End: far enough to the start or the end)
  bool submit = false;
  bool stopTyping = false;  // Esc while typing: leave the search keyboard
  bool quit = false;
  double lastEscape = -10;  // glfwGetTime() of the last Esc; a second one soon after quits

  void onKey(GLFWwindow* window, int key, int action, int mods);
  void onChar(unsigned int codepoint);
};

void KeyboardInput::onKey(GLFWwindow* window, int key, int action, int mods) {
  if (textInput) {
    // Letters arrive through onChar; only editing keys matter here.
    if (action == GLFW_RELEASE) return;
    if (key == GLFW_KEY_V && (mods & GLFW_MOD_CONTROL)) typed += clipboardText(window);
    else if (key == GLFW_KEY_BACKSPACE) ++backspaces;
    else if (key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT) cursorSteps += key == GLFW_KEY_LEFT ? -1 : 1;
    else if (key == GLFW_KEY_HOME || key == GLFW_KEY_END) cursorSteps = key == GLFW_KEY_HOME ? -(1 << 20) : 1 << 20;
    else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) submit = true;
    else if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) stopTyping = true;
    else if (key == GLFW_KEY_TAB && action == GLFW_PRESS) toggleMenu = true;
    return;
  }
  // Seeking repeats on its own while the key is held, so track press/release.
  if (key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT) {
    const bool down = action != GLFW_RELEASE;
    (key == GLFW_KEY_LEFT ? seekBackKey : seekForwardKey) = down;
    return;
  }
  if (key == GLFW_KEY_A || key == GLFW_KEY_D) {
    const bool down = action != GLFW_RELEASE;
    (key == GLFW_KEY_A ? turnLeftKey : turnRightKey) = down;
    return;
  }
  if (action != GLFW_PRESS) return;

  VideoPlayer& player = *app.player;
  ScreenPlacement& screen = running.screen;
  switch (key) {
    case GLFW_KEY_ESCAPE: {
      // Quitting takes two presses, so a stray Esc doesn't end the video.
      const double now = glfwGetTime();
      if (now - lastEscape < 1.5) {
        quit = true;
      } else {
        lastEscape = now;
        std::printf("Press Esc again to quit\n");
      }
      break;
    }
    case GLFW_KEY_TAB: toggleMenu = true; break;
    case GLFW_KEY_SPACE: player.command("cycle pause"); break;
    case GLFW_KEY_UP: videoStep = 1; break;
    case GLFW_KEY_DOWN: videoStep = -1; break;
    case GLFW_KEY_M: player.command("cycle mute"); break;
    case GLFW_KEY_P:
      running.format.projection = nextProjection(running.format.projection);
      running.rememberFormat = true;
      printFormat(running.format, "manual");
      break;
    case GLFW_KEY_L:
      running.format.stereo = nextStereo(running.format.stereo);
      running.rememberFormat = true;
      printFormat(running.format, "manual");
      break;
    case GLFW_KEY_C:
      screen.size.curve = screen.size.curve > 0 ? 0.0f : ScreenSize::kMaxCurve;
      running.screenChanged = true;
      break;
    case GLFW_KEY_T: togglePassthrough = true; break;
    case GLFW_KEY_R: recenter = true; break;
    case GLFW_KEY_F:
      running.flipVideo = !running.flipVideo;
      std::printf("Video flip: %s\n", running.flipVideo ? "on" : "off");
      break;
    case GLFW_KEY_W:
    case GLFW_KEY_S:
      screen.size.distance += key == GLFW_KEY_W ? -0.25f : 0.25f;
      running.screenChanged = true;
      break;
    case GLFW_KEY_Q:
    case GLFW_KEY_E:
      screen.size.height *= key == GLFW_KEY_Q ? 1 / 1.1f : 1.1f;
      running.screenChanged = true;
      break;
    default: break;
  }
}

// + and - by the typed character, so they work on any keyboard layout and
// on the number pad. Held keys repeat through the system's key repeat.
void KeyboardInput::onChar(unsigned int codepoint) {
  if (textInput) {
    typed += utf8(codepoint);
    return;
  }
  if (codepoint == '+') app.player->command("add volume 5");
  else if (codepoint == '-') app.player->command("add volume -5");
}

// A link: a scene from Stash plays as its file does.
bool isStream(const std::string& location) {
  return location.find("://") != std::string::npos && !remote::isRemote(location) &&
         !isStashScene(location);
}

// Ctrl+C or a polite kill: the main loop ends as if quit from the menu, so
// what is unsaved is saved. The same signal again ends the program at once.
volatile std::sig_atomic_t quitSignal = 0;

void onQuitSignal(int signal) { quitSignal = signal; }

void catchQuitSignals() {
  struct sigaction action = {};
  action.sa_handler = onQuitSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESETHAND | SA_RESTART;
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
}

// Where floating subtitles go: on the bottom edge of the flat screen, or for
// surround video below the line of sight, moving with the head's heading.
// The user's
// subtitle size, raise (percent of the picture) and distance apply.
class SubtitlePlacement {
 public:
  Mat4 update(const RunningVideo& running, float frameAspect, Vec3 headPos, float headYaw,
              const Settings& settings) {
    const ScreenPlacement& s = running.screen;
    const float scale = settings.subtitleSize / 100.0f;
    // 0 is where the user found them best: 30% of the picture lower than the
    // first placement (on the flat screen: below the picture).
    const float raise = (settings.subtitleRaise - 30) / 100.0f;
    if (running.format.projection == Projection::Flat) {
      const ScreenSurface screen(s, eyeDisplayAspect(running.format, frameAspect));
      const float screenH = screen.height();
      const float w = screen.width() * 0.8f * scale, h = w / SubtitleOverlay::kAspect;
      const Vec3 local = {0.0f, -screenH / 2 + h / 2 + screenH * (0.04f + raise),
                          -(screen.distance() - 0.05f)};
      return screen.pose() * translation(local) * scaling(w, h, 1);
    }
    const float distance = settings.subtitleDistance / 100.0f;
    const float k = distance / kSurroundSubtitleDistance;  // keep the size as seen
    const float w = kSurroundSubtitleWidth * scale * k, h = w / SubtitleOverlay::kAspect;
    const float drop = (kSurroundSubtitleDrop - raise * kSurroundSubtitleDistance) * k;
    // Right along with the head's heading, at once.
    Vec3 center = headPos + forwardFromYaw(headYaw) * distance;
    center.y -= drop;
    return translation(center) * rotationY(headYaw) * scaling(w, h, 1);
  }
};

// The strip or the browser fading in as it starts being shown and out as it
// stops (menu_feel.h): drawn as it was last shown until it has faded.
struct PanelFade {
  ShownFade fade;
  std::array<PanelItem, 3> drawn;  // the panel, then its list's faded edges
  int count = 0;
  bool shown = false;    // last frame
  bool waiting = false;  // shown, not yet fading in: its page isn't drawn yet
};

// The menu panel (Menu::PanelId) a ray hits first, -1 if none; u/v in its
// texture (v = 0 at the top) and the distance along the ray.
int menuHitTest(const MenuPanels& panel, const Menu& menu, Vec3 origin, Vec3 dir, float& u,
                float& v, float& distance) {
  if (!panel.open) return -1;
  int hit = -1;
  for (const Menu::PanelId id : {Menu::kMain, Menu::kBrowser, Menu::kSetup}) {
    float pu, pv, d;
    if (menu.panelShown(id) && panel.panel(menu, id).hit(origin, dir, pu, pv, d) &&
        (hit < 0 || d < distance)) {
      hit = id;
      u = pu, v = pv, distance = d;
    }
  }
  return hit;
}

// The desktop mirror's window (docs/vulkan-renderer.md, 6): without a client
// API, so it can have a Vulkan surface; the GL context's window stays hidden.
using WindowPtr = std::unique_ptr<GLFWwindow, decltype(&glfwDestroyWindow)>;
WindowPtr createMirrorWindow() {
  glfwDefaultWindowHints();
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  return WindowPtr(glfwCreateWindow(1280, 720, kAppTitle, nullptr, nullptr), glfwDestroyWindow);
}

// Quitting waits for mpv to close its file, which a hardware decoder stuck
// in the kernel can keep from ever finishing. Armed as the player's teardown
// starts, once the library has written what it holds; if the teardown hasn't
// finished in kQuitLimit, the app exits without it, keeping what the Stash
// outbox and the config queue still had to write.
constexpr double kQuitLimit = 5.0;

struct QuitWatch {
  std::mutex mutex;
  std::condition_variable wake;
  bool done = false;
};
std::shared_ptr<QuitWatch> quitWatch;

void armQuitWatchdog() {
  quitWatch = std::make_shared<QuitWatch>();
  try {
    std::thread([watch = quitWatch] {
      {
        std::unique_lock<std::mutex> lock(watch->mutex);
        if (watch->wake.wait_for(lock, std::chrono::duration<double>(kQuitLimit),
                                 [&] { return watch->done; }))
          return;
      }
      Stash::finishStopping();  // a sync under way gives up within about a second
      drainConfigWork(kQuitLimit);
      std::fprintf(stderr, "mpv did not shut down in %.0f s: exiting without it\n", kQuitLimit);
      endRunLog();
      _exit(1);
    }).detach();
  } catch (const std::system_error&) {
  }
}

void disarmQuitWatchdog() {
  if (!quitWatch) return;
  {
    std::lock_guard<std::mutex> lock(quitWatch->mutex);
    quitWatch->done = true;
  }
  quitWatch->wake.notify_one();
}

// Destroyed just before the player: everything declared after it is gone.
struct QuitWatchdogArm {
  ~QuitWatchdogArm() { armQuitWatchdog(); }
};

int run(Options opts) {
  WindowPtr mirrorWindow(nullptr, glfwDestroyWindow);
  GpuRequirements requirements;
  if (opts.mirror) {
    requirements.mirrorInstanceExtensions = MirrorWindow::instanceExtensions();
    if (requirements.mirrorInstanceExtensions.empty())
      std::printf("Mirror: off, GLFW finds no Vulkan window support\n");
    else
      mirrorWindow = createMirrorWindow();
    requirements.mirror = mirrorWindow != nullptr;
  }
  // Made in this order and destroyed in reverse: whatever uses the Vulkan
  // device goes before XrContext, which owns it (docs/vulkan-renderer.md, 2.10).
  XrContext xr;
  xr.setHeadLocked(opts.fixedView);
  if (opts.timing || opts.refreshRate >= 0) xr.setRefreshRateDiagnostic(opts.refreshRate);
  if (opts.fixedView) std::printf("Fixed view: the scene stays in front of the view\n");
  // Foveation on the Frame, or where it is asked for (and then must work).
  const bool foveationAsked = opts.foveationSet && opts.foveation != FoveationLevel::Off;
  requirements.foveation = kStandaloneHeadset || foveationAsked;
  // The Frame's decoder frames, sampled by the eyes (docs/vulkan-renderer.md 11).
  requirements.directVideo = opts.hwdec == "v4l2m2m";

  // First-run setup (first_run.h), decided before anything below writes
  // the files it looks for. A video to play puts it off to the next start;
  // --first-run shows it on any profile, which it leaves alone until it ends.
  const bool setupForced = opts.firstRun;
  const bool freshProfile = newProfile();  // for What's new below
  SetupChoices setupChoices;
  bool showSetup = setupForced;
  if (!setupForced && setupDue()) {
    setupChoices = loadSetupChoices();
    markSetupStarted(setupChoices);
    showSetup = opts.file.empty() && opts.startMenu == StartMenu::None;
  }
  if (setupForced) holdSettings(true);
  Settings settings = loadSettings();
  setLanguage(showSetup && !setupChoices.language.empty() ? setupChoices.language
                                                          : settings.language);
  // The media server's name in the strings: the kind's in use; every
  // kind's ("Stash or XBVR") while none is, and in first-run setup, which
  // connects to any.
  auto serverNameNow = [&settings](bool inSetup) {
    return inSetup || !settings.stash.enabled
               ? mediaServerNames(stashOwnStrings().stashSetup.orKind)
               : std::string(mediaServer(settings.stash.kind).name());
  };
  std::string serverNamed = serverNameNow(showSetup);
  setServerName(serverNamed);
  // SteamVR's refresh rate for this app (refresh_rate.h), read before the
  // session starts; if it has none, the preset's is written first, so the
  // session starts at it (settle() below gives the same rate without the
  // resolution stops). A profile in first-run setup starts on its preset's
  // rate, whatever an earlier install left there.
  const bool refreshRateAvailable = appRefreshRateAvailable();
  const int steamVrRate =
      refreshRateAvailable && !(showSetup && !setupForced) ? readAppRefreshRate() : 0;
  if (refreshRateAvailable && steamVrRate == 0)
    writeAppRefreshRate(Presets(std::vector<int>{}).with(settings.performancePreset, settings).refreshRate);

  if (!xr.init(requirements)) return 1;
  const bool amd = xr.gpuDevice().caps.vendorId == 0x1002;
  if (opts.hwdec.empty()) opts.hwdec = amd ? "vaapi,vaapi-copy" : "nvdec,nvdec-copy";
  if (opts.previewHwdec.empty()) opts.previewHwdec = amd ? "vaapi-copy" : "nvdec-copy";
  if (opts.hwdec == "no") opts.previewHwdec = "no";
  if (amd)
    std::printf("AMD support is experimental: use Mesa 26.1+ radeonsi and RADV\n");
  std::printf("Video decoding requested: %s; previews: %s\n",
              opts.hwdec.c_str(), opts.previewHwdec.c_str());
  // OpenXR chooses the GPU before GL is created, so AMD can use EGL without
  // changing NVIDIA's context API or creating a temporary GLX context.
  MpvGlContext glContext;
  if (!glContext.init(amd || opts.hwdec == "v4l2m2m", amd && opts.hwdec != "no")) return 1;
  GpuContext gpu;
  if (!gpu.init(xr.gpuDevice())) return 1;
  GlInterop interop;  // what GL renders, the scene samples
  if (!interop.init(gpu)) return 1;
  Uploader uploader;  // what canvases draw goes to the GPU through it
  if (!uploader.init(gpu)) return 1;
  // The eye images' size counts from SteamVR's recommendation, on the Frame
  // from its panel (eye_resolution.h); a profile from before is settled here.
  const auto resolutionStops = eyeResolutionStops(gpu.caps());
  if (settleEyeResolution(settings, xr.recommendedEyeWidth(), resolutionStops)) saveSettings(settings);
  const Presets presets(resolutionStops);
  if (presets.settle(settings)) saveSettings(settings);
  // A rate chosen in SteamVR's per-app page shows here; another than the
  // menu's choices too, none of them lit.
  if (steamVrRate > 0 && steamVrRate != settings.refreshRate) {
    settings.refreshRate = steamVrRate;
    settings.performancePreset = presets.matching(settings);
    saveSettings(settings);
  }
  if (refreshRateAvailable)
    std::printf(steamVrRate > 0 ? "Refresh rate: %d Hz, SteamVR's for this app\n"
                                : "Refresh rate: %d Hz, set in SteamVR for this app (it had none)\n",
                settings.refreshRate);
  else if (kStandaloneHeadset)
    std::printf("Refresh rate: not started from Steam, left to SteamVR\n");
  // What's new (whats_new.h): the notes since the version last seen, shown
  // as the menu first opens; closing them counts this version as seen
  // (markVersionSeen, then saveSettings). With none to show, it counts now.
  WhatsNewLaunch whatsNew = whatsNewAtLaunch(freshProfile, settings.versionSeen, settings.showWhatsNew);
  if (whatsNew.seenNow && markVersionSeen(settings.versionSeen)) saveSettings(settings);
  if (!whatsNew.releases.empty())
    std::printf("What's new: %zu release(s) to show, newest %s\n", whatsNew.releases.size(),
                whatsNew.releases.front().version.c_str());
  auto chosenResolution = [&] {
    return resolutionStops.empty() ? settings.videoResolution : settings.videoResolutionPixels;
  };
  // What this run uses; the command line's overrides stay out of settings.
  FoveationLevel chosenFoveation = opts.foveationSet ? opts.foveation : settings.foveation;
  const int resolutionBase = kPanelWidth > 0 ? kPanelWidth : xr.recommendedEyeWidth();
  // Half a pixel more, so the size is not floored to one less.
  auto pixelScale = [&](int pixels) { return (pixels + 0.5f) / resolutionBase; };
  // The eye size asked for, as a scale of 1x: the command line's for this
  // run (--resolution, --resolution-cycle) until the menu's Resolution is
  // changed, else the setting. Applied while running (XrContext::requestEyeScale).
  float overrideScale = 0;
  if (opts.resolutionPixels > 0) overrideScale = pixelScale(opts.resolutionPixels);
  if (opts.resolutionPercent > 0) overrideScale = static_cast<float>(opts.resolutionPercent / 100);
  if (overrideScale > 0)
    std::printf("Eye resolution: %d pixels across for this run (--resolution)\n",
                static_cast<int>(std::floor(resolutionBase * overrideScale)));
  auto askedScale = [&] {
    return overrideScale > 0 ? overrideScale
           : resolutionStops.empty() ? settings.videoResolution / 100.0f
                                     : pixelScale(settings.videoResolutionPixels);
  };
  if (kPanelWidth > 0)
    std::printf("Eye resolution: 1x is the panel's %dx%d per eye\n", kPanelWidth, kPanelHeight);
  else
    std::printf("Eye resolution: 1x is the runtime's recommended %dx%d per eye\n",
                xr.recommendedEyeWidth(), xr.recommendedEyeHeight());
  xr.preferAppDensityMaps(!opts.runtimeDensityMaps);
  if (!xr.createEyeImages(gpu, kPanelWidth, kPanelHeight, askedScale(), kInstantResolution / 100.0f))
    return 1;
  if (foveationAsked && !xr.foveated()) {
    std::fprintf(stderr, "--foveation=%s: this headset, runtime or GPU cannot foveate\n",
                 foveationLevelName(opts.foveation));
    return 1;
  }
  Foveation foveation;
  int sharpBins = settings.sharpBins, foveationOutside = settings.foveationOutside;
  bool foveationRing = settings.foveationRing;
  if (opts.foveationSet) foveationPreset(chosenFoveation, sharpBins, foveationRing, foveationOutside);
  if (opts.sharpBins) sharpBins = opts.sharpBins;
  if (opts.ring >= 0) foveationRing = opts.ring != 0;
  chosenFoveation = foveationLevel(chosenFoveation != FoveationLevel::Off, sharpBins, foveationRing, foveationOutside);
  foveation.init(xr, chosenFoveation, opts.foveationGaze, sharpBins, foveationRing, foveationOutside);
  bool showFoveation = foveation.active() && (opts.showFoveation || settings.showFoveation);
  if (opts.showFoveation && !foveation.active()) std::printf("Show foveation: Steam Frame only\n");
  EyeTargets eyeTargets;
  if (!eyeTargets.init(gpu, xr)) return 1;
  SceneRenderer renderer;
  if (!renderer.init(gpu, interop, uploader, eyeTargets)) return 1;
  renderer.setTestPattern(opts.xrTest);
  renderer.setMenuLodBias(opts.menuLodBias);
  if (opts.menuLodBias != 0.0f) std::printf("Menu sharpness: mip bias %g\n", opts.menuLodBias);
  if (opts.xrTest) std::printf("XR test: the eyes show a pulsing colour instead of the scene\n");
  MirrorWindow mirror;
  if (mirrorWindow) {
    if (mirror.init(gpu, mirrorWindow.get(), xr.eyeFormat()))
      renderer.setMirror(&mirror);
    else
      mirrorWindow.reset();
  }
  // The keyboard and the clipboard go through the mirror's window; without
  // it the hidden one gets no keys.
  GLFWwindow* input = mirrorWindow ? mirrorWindow.get() : glContext.window();
  DirectVideo direct;  // before the player, which hands it frames
  if (requirements.directVideo && direct.init(gpu)) renderer.setDirectVideo(&direct);
  VideoPlayer player;
  QuitWatchdogArm quitWatchdogArm;
  if (!player.init(opts.hwdec, interop, glContext, opts.timing, opts.decoderQueue)) return 1;
  player.command(("set volume " + std::to_string(settings.volume)).c_str());
  if (direct.available()) player.setDirectVideo(&direct);
  DirectDepth directDepth;  // automatic focus on frames handed out, which stay out of GL
  if (direct.available() && !directDepth.init(gpu, direct))
    std::printf("Focus: no automatic focus on decoded frames mpv hands out\n");
  Menu menu;
  if (!menu.init(uploader)) return 1;
  if (showSetup && !menu.startSetup(setupForced, setupChoices.places)) return 1;
  ReleaseUpdate releaseUpdate;  // inert unless the GitHub install launcher opts in
  bool updateOffered = false, menuWasOpen = false;  // its dialog, once a run as the menu opens
  bool whatsNewWasOpen = false;
  // The Frame's decoder refused a video while Steam holds part of it
  // (decoder_holders.h): the menu asks whether to end Steam's holders; if
  // so, the video opens again where it was, once.
  struct {
    std::string path;      // the video refused
    double at = 0;         // ... where
    uint64_t handled = 0;  // the opening (VideoPlayer::loadCount()) whose refusal was handled
    uint64_t retry = 0;    // ... the one after freeing the decoder
    bool ask = false;      // to ask once no other dialog is open
    bool openedMenu = false;  // the menu was opened to ask
    decoder_holders::Busy busy = decoder_holders::Busy::No;
    std::future<std::vector<decoder_holders::Holder>> holders;
    std::future<bool> freed;
  } decoderRescue;
  DiagnosticsExport diagnostics;
  SubtitleOverlay subtitles;
  if (!subtitles.init(uploader)) return 1;
  // "Loading …" shown instead of the video while a file opens, or why it
  // doesn't play. Drawn once before playback starts.
  NoticeCanvas loadingText(1);
  if (!loadingText.init(uploader)) return 1;
  loadingText.set(menu, NoticeIcon::None, strings().loading);
  SeekPreview seekPreview;
  // Not on the Frame: a second full-size decoder beside playback fails there
  // (docs/frame-plan.md, W4). Without init() it never shows.
  if (!kStandaloneHeadset &&
      !seekPreview.init(interop, opts.previewHwdec, uploader, glContext))
    return 1;
  // "Recentering in 2 …"; "Viewpoint: 15 % closer" while the stick moves it,
  // a gesture's value in its ring while a twist changes it, or what a
  // button's job said, whichever changed last;
  // "Passthrough on: green screen video"; "microSD card: HOLIDAY", "Now in
  // the file explorer".
  Notices notices;
  if (!notices.init(uploader)) return 1;
  // microSD cards and USB drives; on the Frame the app mounts those SteamOS
  // leaves alone.
  RemovableDrives drives;
  drives.start(kStandaloneHeadset);
  std::string driveText;
  NoticeIcon driveIcon = NoticeIcon::Drive;
  double driveShownUntil = 0;
  double viewpointShownUntil = 0;  // glfwGetTime() it goes away
  double opacityShownUntil = 0;    // ... and the opacity See-through on or off set
  // A button's job saying what it did, where that doesn't show by itself, or
  // why it did nothing; the newest of it, the viewpoint and the opacity shows.
  struct JobSaid {
    std::string text;
    double until = 0;
  } jobSaid;
  auto say = [&](std::string text) { jobSaid = {std::move(text), glfwGetTime() + kValueShown}; };
  auto sayf = [&](const char* format, auto... args) {
    std::string text(std::max(std::snprintf(nullptr, 0, format, args...), 0), '\0');
    std::snprintf(text.data(), text.size() + 1, format, args...);
    say(std::move(text));
  };
  bool peeking = false;  // Peek at the room is held: the room shows instead of the video
  double softwareSince = -1;       // the video has played in software decoding since

  // Frame rates and drops over the video: the setting's, or the command
  // line's for this run until the menu switches it.
  PerformanceReadout readout;
  if (!readout.init(uploader)) return 1;
  bool showPerformance = opts.showPerformance || settings.showPerformance;
  auto switchReadout = [&](bool on) {
    std::printf("Performance readout: %s\n", on ? "on" : "off");
    showPerformance = on;
    player.setCounting(on);
    if (on) readout.restart(glfwGetTime());
  };
  switchReadout(showPerformance);
  Autofocus autofocus;  // without it, only manual stereo focus
  autofocus.init(opts.timing);
  // A packed mask's second layer showing late (mask_probe.h), from GL or
  // from frames handed out.
  MaskProbe maskProbe;
  const bool maskProbeReady = maskProbe.init();
  // ... also the key picker's for those frames: the two never read at once
  // (a key is used only without the mask).
  DirectDepth maskProbeDirect;
  const bool maskProbeDirectReady =
      direct.available() && maskProbeDirect.init(gpu, direct, MaskProbe::kGrid, 2 * MaskProbe::kGrid);
  KeyPicker keyPicker;  // the background key's colour, pointed at
  {
    const auto start = std::chrono::steady_clock::now();
    const bool ready = keyPicker.init();
    std::printf("Key picker: %s (%.1f ms)\n", ready ? "ready" : "GL read unavailable",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count());
  }
  FocusTargetSelector focusTargets;  // head direction or eye gaze
  GazeCorrection gazeCorrection;     // what the eye tracker is off by, from the menu's clicks
  gazeCorrection.load(settings.gazeLearned, xr.runtimeVersion());
  bool gazeSitting = false;          // worn with input focus
  // The headset came off or the input focus went: what the sitting's clicks
  // said is remembered.
  auto endGazeSitting = [&] {
    if (!gazeSitting) return;
    gazeSitting = false;
    gazeCorrection.endSitting();
    if (gazeCorrection.takeChanged()) {
      settings.gazeLearned = gazeCorrection.saved();
      saveSettings(settings);
    }
  };
  FocusMarker focusMarker;
  if (!focusMarker.init(uploader)) return 1;
  Thumbnailer thumbnailer;
  menu.setThumbnailer(&thumbnailer);
  thumbnailer.start();
  PhotoPreloader photoPreloader;  // the next photo, decoded ahead
  photoPreloader.start(interop);
  VideoMemory memory;  // settings kept before the library, carried over into it
  memory.load();
  Library library;
  library.load();
  menu.setLibrary(&library);
  LibraryIndex libraryIndex;  // the videos in the pinned folders (library_index.h)
  if (!setupForced) libraryIndex.start();  // it writes its own file
  menu.setLibraryIndex(&libraryIndex);
  Offline offline;  // offline downloads (offline.h)
  offline.start();
  menu.setOffline(&offline);
  // Stash (optional, docs/stash.md): none while it is off or not set up.
  std::unique_ptr<Stash> stash;
  // Funscripts on a device (script_player.h): nothing of it runs while off.
  ScriptPlayer scripts;
  scripts.configure(settings.scripts);
  offline.setScripts(settings.scripts.enabled);
  if (settings.stash.enabled && !settings.stash.url.empty() && !setupForced)
    stash = std::make_unique<Stash>(settings.stash, library);
  menu.setStash(stash.get());
  // Stash switched on or off, or its server changed: made anew (the old one
  // finishes stopping in the background).
  auto restartStash = [&] {
    menu.setStash(nullptr);
    stash.reset();
    if (settings.stash.enabled && !settings.stash.url.empty())
      stash = std::make_unique<Stash>(settings.stash, library);
    menu.setStash(stash.get());
  };
  player.setSubtitleStyle(settings.subtitleSize / 100.0, settings.subtitleRaise);
  player.setMipmaps(settings.videoMipmaps);
  player.setSharpened(settings.sharpen > 0);
  // The Anti-shimmer this run draws with: the command line's until the menu
  // changes it. Pushed with each video draw, so a change applies from the next frame.
  AntiShimmer antiShimmer = opts.antiShimmerSet ? opts.antiShimmer : settings.antiShimmer;
  auto reportAntiShimmer = [&](const char* from) {
    const int taps = antiShimmerTaps(antiShimmer);
    std::printf("Anti-shimmer: %s, at most %dx%d taps per pixel with mipmaps off (%s)\n",
                antiShimmerName(antiShimmer), taps, taps, from);
  };
  reportAntiShimmer(opts.antiShimmerSet ? "command line" : "settings");
  // Frame: a new video frame is shown from the next headset frame on, so the
  // eyes never wait for its V4L2 decoding, Main 10 conversion and mpv render
  // (--video-pipeline). Set up once the runtime gives its frame period.
  const bool pipelineVideo =
      opts.videoPipeline < 0 ? opts.hwdec == "v4l2m2m" : opts.videoPipeline == 1;
  double pipelinePeriod = 0;
  std::printf("Video pipeline: %s\n", pipelineVideo ? "on, one headset frame ahead" : "off");
  // A frame that would look like the last one drawn is not drawn: the
  // runtime shows that one again (frame_reuse.h). The setting decides on
  // the Steam Frame, --frame-reuse at start anywhere.
  FrameReuse frameReuse;
  frameReuse.setEnabled(opts.frameReuse < 0 ? kStandaloneHeadset && settings.frameReuse
                                             : opts.frameReuse == 1);
  std::printf("Frame reuse: %s\n", frameReuse.enabled() ? "on" : "off");
  seekPreview.setEnabled(settings.seekPreview);
  FolderMemory folders;
  folders.load();
  menu.setFolderMemory(&folders);
  menu.setStartDirectory(settings.lastFolder);
  menu.setGallery(settings.gallery);
  menu.setVideoSettingsThisVideo(settings.videoSettingsThisVideo);
  menu.setKeyboardLayout(settings.keyboardLayout, settings.keyboardOther);

  App app;
  app.player = &player;
  RunningVideo running{player, settings, library};
  KeyboardInput keys{app, running};
  glfwSetWindowUserPointer(input, &keys);
  glfwSetKeyCallback(input, [](GLFWwindow* window, int key, int /*scancode*/, int action, int mods) {
    static_cast<KeyboardInput*>(glfwGetWindowUserPointer(window))->onKey(window, key, action, mods);
  });
  glfwSetCharCallback(input, [](GLFWwindow* window, unsigned int codepoint) {
    static_cast<KeyboardInput*>(glfwGetWindowUserPointer(window))->onChar(codepoint);
  });
  printControls();

  Deletion deletion{menu, player, settings, library, libraryIndex, offline, stash};
  Passthrough passthrough{settings, library, xr, running.video, running.currentFile, running.format};
  // Holding the grip on the menu moves it with the hand; the trigger off it
  // holds the flat screen (screenControl).
  struct MenuGrab {
    int hand = -1;  // -1 = not held
    Mat4 startInverse;  // inverse of the hand pose at the grab
    Vec3 startPoint;    // the menu's front at the grab
  } menuGrab;
  ScreenControl screenControl;
  // The gestures (gesture_input.h), and what the app does for them (set up
  // below, before the frames).
  GestureInput gestureInput;
  GestureInput::Host gestureHost;
  // Holding the trigger away from the menu turns the picture with the hand
  // (sideways, up/down and roll), for fine centring.
  struct TurnDrag {
    int hand = -1;         // -1 = not dragging
    bool tracking = false;  // false: restart smoothing from the hand's current pose
    Quat smoothHand;       // the smoothed hand orientation the picture has followed
    double lastTime = 0;   // display time of the last update, seconds
  } turnDrag;
  bool photoSized = false;  // the current photo's size is known (its format guessed)
  ForwardStick forwardStick[2];
  bool initialRecenter = true;  // establish the head anchor without clearing saved placement
  double lastFrameTime = glfwGetTime();
  SettledLine samplingLog;  // the pixel path per eye, for the console

  MenuPanels panel;
  SubtitlePlacement subtitlePlacement;
  // Without a video, start in the file browser as soon as the head is tracked.
  bool showBrowserAtStart = opts.file.empty() && !showSetup;
  // At the end of a video, the menu opens on the list it was opened from.
  bool wasAtEnd = false, menuAtEnd = false;
  SetupPlacement setupPlacement;
  SetupIntro setupIntro;
  bool setupIntroDropped = false;  // its layers, once it has played
  // The hints after setup (hints.h): this profile's, or (--first-run) the
  // run's once its setup ends.
  Hints hints;
  hints.load(setupForced ? std::string() : settings.hints, !setupForced);
  uint64_t hintBindingsSeen = 0;  // controlsVersion the menu's hints have
  const Texture* hintTexture = nullptr;
  int hintW = 0, hintH = 0;
  bool stashInSetup = false;  // Stash was connected in setup (MenuCommand::SetupStash)
  // The Library's "You're all set" once setup is finished, until dismissed
  // or a video starts; kept for the next start, but not by --first-run.
  bool setupBanner = settings.setupBanner;
  auto hideSetupBanner = [&] {
    setupBanner = false;
    if (!settings.setupBanner) return;
    settings.setupBanner = false;
    saveSettings(settings);
  };
  double controllerSeen = -10;  // glfwGetTime() a controller was last tracked
  uint64_t bindingsSeen = 0;    // controlsVersion setup has
  StartMenu startMenu = opts.startMenu;  // ... or on this page, then
  int activeHand = 1;  // the hand whose pointer drives the menu
  float laserLength = kLaserStartLength;
  ShownFade menuFade;  // the laser's, as the menu opens and closes
  PanelFade panelFades[2];  // the strip's and the browser's (Menu::kMain, Menu::kBrowser)
  Ease dotTight;  // how far the pointer's dot has tightened
  int knownWidth = 0, knownHeight = 0;
  TwoViews knownViews = TwoViews::No;
  bool formatDetected = false;  // for this file: logged once, then as it changes
  StickSeek stickSeek{player, panel};
  ButtonJobs buttonJobs{player, running, settings, library};
  HoldRepeat turnLeft(0.5, 0.35), turnRight(0.5, 0.35);  // A and D
  // The controllers' buttons as jobs (controls.h), by each hand's type; the
  // bindings Your controllers and the hints show, made again when the
  // profiles change (controlsVersion).
  ControlInput controlInput;
  ControllerType controllerTypes[2] = {ControllerType::Count, ControllerType::Count};
  uint64_t profilesSeen = 0, controlsVersion = 1;
  ControllerBindings shownBindings;
  // The type Settings' Controls shows: the right hand's, else the left's;
  // with neither known, the Frame's on the Frame, else the last one seen.
  auto shownControllerType = [&] {
    for (int h : {1, 0})
      if (controllerTypes[h] != ControllerType::Count) return controllerTypes[h];
    if (kStandaloneHeadset) return ControllerType::Frame;
    return settings.controllersSeen != ControllerType::Count ? settings.controllersSeen : ControllerType::Touch;
  };
  auto describeBindings = [&] {
    std::string profiles[2];
    xr.pairedProfiles(profiles);
    ControllerBindings bindings = describeControls(profiles, settings.controls, settings.gestures);
    if (bindings == shownBindings) return;
    shownBindings = std::move(bindings);
    ++controlsVersion;
    logControls(shownBindings);
  };
  HoldRepeat cursorLeft(0.5, 0.07), cursorRight(0.5, 0.07);  // as the keyboard's keys repeat
  // A pointed value stepped by either stick (value_adjust.h), which is its
  // until both sticks are let go.
  HoldRepeat valueLess(adjust::kStickDelay, adjust::kStickRepeat);
  HoldRepeat valueMore(adjust::kStickDelay, adjust::kStickRepeat);
  bool valueStick = false;
  // A short pulse on a controller (0 left, 1 right, -1 both), on events
  // only, unless Settings has Controller vibration off. Pulses asked for in
  // one frame go out together at its end (sendBuzzes()), the strongest per
  // hand: a later request would cut an earlier one short.
  struct Buzz {
    float amplitude = 0;
    double seconds = 0;
    float frequency = 0;
  } buzzes[2];
  auto buzz = [&](int hand, float amplitude, double seconds, float frequency = 0) {
    for (int h = 0; h < 2; ++h)
      if ((hand < 0 || hand == h) && amplitude > buzzes[h].amplitude)
        buzzes[h] = {amplitude, seconds, frequency};
  };
  auto sendBuzzes = [&] {
    for (int h = 0; h < 2; ++h) {
      if (buzzes[h].amplitude > 0 && settings.vibration)
        xr.vibrate(h, buzzes[h].amplitude, buzzes[h].seconds, buzzes[h].frequency);
      buzzes[h] = {};
    }
  };
  double listPushSeconds = 0;  // a list scrolled with the stick pushed well over, for its speed-up
  float listPushSign = 0;      // ... and which way
  // The playing file's content record is still being found: its settings
  // apply when it is (they are newer than any videos.conf has).
  bool awaitingRecord = false;
  int64_t openingRecord = 0;
  bool awaitingResume = false;
  double resolvedResume = -1, videoOpenedAt = 0;
  std::future<std::string> neighbor, nextPhoto;
  // What the playing video was opened as, before a copy or Stash's stream
  // took its place: Previous and Next go on from there through a list.
  std::string openedAs;
  int neighborStep = 0;
  bool neighborWrap = false;
  // What happens when a video ends: the setting, or Repeat or Shuffle once
  // chosen in this run. Stash's Shuffle play makes it Shuffle until a video
  // is opened by hand or it is chosen again. Shuffle goes round a list once
  // from the video opened by hand (as it was opened, and its file), in a
  // new order each time it starts.
  AfterVideo afterChosen = settings.afterVideo;
  bool listShuffle = false;
  std::string playStart, playStartFile;
  std::random_device seedSource;
  uint64_t shuffleSeed = 0;
  bool endStep = false;  // the step under way is a video's end going on: with no next one, the menu opens
  uint64_t endStepRestarts = 0;  // ... and the player's seeks sent when it began
  auto afterVideo = [&] { return listShuffle ? AfterVideo::Shuffle : afterChosen; };
  auto startShuffle = [&] {
    shuffleSeed = (static_cast<uint64_t>(seedSource()) << 32 | seedSource()) | 1;
    playStart = openedAs;
    playStartFile = running.currentFile;
  };
  // mpv starts a repeated video over by itself; it never reaches its end.
  auto applyAfterVideo = [&] {
    player.command(afterVideo() == AfterVideo::Repeat ? "set loop-file inf" : "set loop-file no");
  };
  applyAfterVideo();
  // The playing video file was opened (its play counted): where it stops is
  // kept when it is left (and every minute meanwhile), and its duration
  // once known.
  bool videoOpened = false, durationKept = false;
  double resumeSaveAt = 0;  // glfwGetTime() to keep the position next
  double resumeSaved = -2;  // ... and the one kept last

  // Keeps where the running video is (none near either end), once mpv has
  // said where it is: leaving it sooner keeps the position it had.
  auto saveResume = [&] {
    if (!videoOpened || !player.positionKnown()) return;
    const PlaybackState& p = player.playback();
    const double point = resumePoint(p.position, p.duration);
    resumeSaveAt = glfwGetTime() + 60;
    if (point == resumeSaved) return;
    resumeSaved = point;
    library.setResume(running.currentFile, point);
  };
  auto keepResume = [&] {
    saveResume();
    videoOpened = false;
  };
  // Opens a video with what is remembered about it. True if its format was
  // remembered.
  auto openVideo = [&](const std::string& requested) {
    // A video downloaded to this device plays from its copy, however it was chosen.
    std::string oshash = library.oshashOf(requested);
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
    neighbor = {};
    nextPhoto = {};
    neighborStep = 0;
    running.saveUnsaved();
    keepResume();
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
    if (!isPhotoFile(path)) library.open(path);
    awaitingRecord = library.available() && !library.known(path);
    if (!library.settings(path, running.video)) {
      VideoSettings legacy;
      if (memory.get(path, legacy)) {
        // Queue the fallback before user edits can create settings. A known
        // path must still resolve to the same identity before it can use it.
        library.carryOverSettings(path, legacy, awaitingRecord ? openingRecord : 0);
        if (!openingRecord || !awaitingRecord) running.video = legacy;
      }
    }
    awaitingResume = awaitingRecord && settings.resume && !isStream(path) && !isPhotoFile(path);
    resolvedResume = -1;
    passthrough.videoChanged(isStream(path));
    gestureInput.cancel(gestureHost);  // a twist under way was the last video's
    gestureInput.hide();
    controlInput.dropFileTaps();  // ... and a press waiting to skip or stop it
    opacityShownUntil = 0;
    running.applyVideoSettings();
    knownWidth = knownHeight = 0;  // re-detect the format once the size is known
    knownViews = TwoViews::No;
    formatDetected = false;
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
      // from where it was left if the library knows (streams start over).
      double start = -1;
      if (settings.resume && !isStream(path)) {
        const VideoRecord r = library.record(path);
        start = resumePoint(r.resumeSeconds, r.durationSeconds);
      }
      player.command("set pause no");
      stickSeek.cancel();
      player.load(path, std::max(0.0, start));
      if (start > 0) std::printf("Resuming at %s\n", clockTime(start).c_str());
    }
    durationKept = false;
    // As for links, Stash's stream opens its preview at once: the first one
    // pointed at then comes about 0.4 s sooner over a LAN.
    seekPreview.videoChanged(path, isStream(path) || isStashScene(path));
    app.nextSlideAt = glfwGetTime() + app.slideshowSeconds;  // each photo gets its full time
    return running.video.hasFormat;
  };
  // Closes the running video or photo and leaves the scene empty, keeping
  // where it was left as switching to another would.
  auto closeVideo = [&] {
    if (running.currentFile.empty()) return;
    deletion.dropDialog();
    running.saveUnsaved();
    keepResume();
    player.stop();
    stickSeek.cancel();
    seekPreview.close();
    running.currentFile.clear();
    running.streamOshash.clear();
    scripts.open({});
    running.video.passthrough = false;
    running.video.opacity = 100;  // the empty scene isn't blended; the video keeps its saved value
    gestureInput.cancel(gestureHost);
    gestureInput.hide();
    controlInput.dropFileTaps();
    opacityShownUntil = 0;
    neighbor = {};
    nextPhoto = {};
    neighborStep = 0;
    awaitingRecord = false;
    awaitingResume = false;
  };
  deletion.closeVideo = closeVideo;
  // Stores the current format for this video (after the user chose it).
  auto rememberFormat = [&] {
    running.formatChosenByUser = true;
    running.video.hasFormat = true;
    running.video.format = running.format;
    running.storeVideo(VideoSettings::kFormat);
  };
  // A Stash scene given on the command line opens once Stash's catalog is there.
  std::string startScene = isStashScene(opts.file) ? opts.file : "";
  if (!opts.file.empty() && startScene.empty()) {
    openVideo(opts.file);
    startShuffle();
  }
  // `atEnd`: a video's end going on with the next one, not Previous or Next.
  auto stepVideo = [&](int step, bool atEnd = false) {
    const bool replaced = endStep;  // Previous or Next pressed while an end's step is under way
    endStep = false;
    if (running.currentFile.empty()) return;
    if (isStream(running.currentFile)) {
      say(strings().jobs.notForStreams);
      return;
    }
    const bool shuffle = afterVideo() == AfterVideo::Shuffle && !isPhotoFile(running.currentFile);
    // Opened from a list in the menu (Stash's scenes): through that list.
    std::string next;
    if (!menu.listNeighbor(openedAs, step, next, {shuffle ? shuffleSeed : 0, playStart})) {
      neighborStep = neighbor.valid() && !neighborWrap && !replaced ? neighborStep + step : step;
      neighborWrap = false;
      neighbor = mediaNeighbor(running.currentFile, neighborStep, false,
                               {shuffle ? shuffleSeed : 0, playStartFile});
      endStep = atEnd;
      endStepRestarts = player.counts().restarts;
      return;
    }
    if (!next.empty()) {
      openVideo(next);
    } else {
      std::printf("No %s video in this list\n", step > 0 ? "next" : "previous");
      if (atEnd) menuAtEnd = true;
      else say(step > 0 ? strings().jobs.lastVideo : strings().jobs.firstVideo);
    }
  };

  // The Resolution row's choice: shown at once, switched to (and saved)
  // once it settles: a pause after its last change, also while its slider
  // is held, so a drag across several stops or several clicks make one
  // switch; at once on letting go. 0 = none.
  constexpr double kResolutionSettle = 0.35;
  int pendingResolution = 0;
  double pendingResolutionAt = 0;
  bool resolutionHeld = false;
  bool pendingFoveation = false;
  // The user's named presets as last listed, and the listing on its way;
  // the one picked last (its name), and the one shown as chosen.
  std::vector<UserPreset> userPresets;
  std::future<std::vector<UserPreset>> userPresetsListing = listUserPresets();
  int userPresetsListed = 0;
  std::string pickedUserPreset;
  int userPresetShown = -1;
  // After a covered setting changes: the preset they now match, else Custom.
  auto matchPreset = [&] {
    settings.performancePreset = presets.matching(settings);
    userPresetShown = presets.shownUserPreset(userPresets, settings, pickedUserPreset);
  };
  running.matchPreset = matchPreset;
  auto applyPendingResolution = [&] {
    const int value = pendingResolution;
    pendingResolution = 0;
    if (value == chosenResolution() && overrideScale == 0) return;
    settings.videoResolutionPixels = resolutionStops.empty() ? 0 : value;
    settings.videoResolution = resolutionStops.empty()
                                   ? value : static_cast<int>(std::lround(value * 100.0 / resolutionBase));
    settings.resolutionNotice = false;
    matchPreset();
    saveSettings(settings);
    overrideScale = 0;  // the command line's size gives way to the setting
    xr.requestEyeScale(askedScale());
  };
  // Off the frame loop: vrcmd runs on the config queue; SteamVR switches
  // within about 2 s.
  auto setRefreshRate = [&](int hz) {
    if (hz == settings.refreshRate) return;
    settings.refreshRate = hz;
    if (refreshRateAvailable) queueConfigWork([hz] { writeAppRefreshRate(hz); });
  };
  // A preset's settings, live as their own rows apply them.
  auto applyPreset = [&](const Settings& p) {
    if (p.foveation != chosenFoveation || p.sharpBins != sharpBins ||
        p.foveationRing != foveationRing || p.foveationOutside != foveationOutside) {
      chosenFoveation = p.foveation;
      sharpBins = p.sharpBins;
      foveationRing = p.foveationRing;
      foveationOutside = p.foveationOutside;
      pendingFoveation = true;  // applied before xrWaitFrame
    }
    settings.videoMipmaps = p.videoMipmaps;
    player.setMipmaps(settings.videoMipmaps);
    settings.antiShimmer = p.antiShimmer;
    if (antiShimmer != settings.antiShimmer) {
      antiShimmer = settings.antiShimmer;
      reportAntiShimmer("preset");
    }
    settings.sharpen = p.sharpen;
    player.setSharpened(settings.sharpen > 0);
    settings.toneLook = p.toneLook;
    if (settings.tvGamma != p.tvGamma) {
      settings.tvGamma = p.tvGamma;
      player.setTvGamma(settings.tvGamma);
    }
    settings.focus = p.focus;
    setRefreshRate(p.refreshRate);
    pendingResolution = resolutionStops.empty() ? p.videoResolution : p.videoResolutionPixels;
    applyPendingResolution();
    settings.performancePreset = presets.matching(p);
    userPresetShown = presets.shownUserPreset(userPresets, p, pickedUserPreset);
    saveSettings(settings);
  };

  // Setup ended: the language chosen and Stash connected are kept (with
  // --first-run only if it was finished: Set up later leaves the profile as
  // it was), Stash not set up leaves the menu's "Use Stash?" card, and the
  // Library opens, recentred (once finished, with its "You're all set").
  // Stash's folders are looked for in the places ticked too, and saved as
  // found.
  auto finishSetup = [&](bool finished) {
    menu.endSetup(finished);
    hints.begin(!setupForced);
    hints.takeChanged();
    mirror.setCover(nullptr);
    setupPlacement = {};
    const std::string& language = setupChoices.language;
    const bool keepStash = !setupForced || (finished && stashInSetup);
    setupBanner = finished;
    if (setupForced) {
      holdSettings(false);
      Settings saved = loadSettings();
      if (!keepStash) {  // the profile's own, held back while setup ran
        settings.stash = saved.stash;
        settings.otherServers = saved.otherServers;
        restartStash();
      }
      bool changed = false;
      if (finished && !language.empty()) {
        saved.language = settings.language = language;
        changed = true;
      } else {
        setLanguage(settings.language);
        menu.languageChanged();
      }
      if (finished && keepStash) {
        saved.stash = settings.stash;
        saved.otherServers = settings.otherServers;
        changed = true;
      } else if (finished && !stash) {
        saved.stash.card = settings.stash.card = true;
        changed = true;
      }
      if (changed) saveSettings(saved);
      libraryIndex.start();
    } else {
      if (!language.empty()) settings.language = language;
      if (!stash) settings.stash.card = true;
      settings.setupBanner = finished;
      settings.hints = hints.line();
      saveSettings(settings);
      endSetup();
    }
    if (stash && keepStash)
      menu.findStashFolders(true, finished ? setupChoices.places : std::vector<std::string>());
    else
      menu.findStashFolders(false);
    std::printf("Setup: %s, quality preset %s\n", finished ? "finished" : "set up later",
                performancePresetName(settings.performancePreset));
    menu.showLibrary();
    panel.open = true;
    initialRecenter = true;
  };

  // Picking the background key's colour ends; a read on its way is forgotten.
  auto endKeyPick = [&] {
    keyPicker.cancel();
    app.keyPickFor.clear();
    app.keyPickWanted = false;
    app.keyPickNote = KeyPickNote::None;
  };

  // A PC whose runtime doesn't blend, with the setting on: passthrough draws
  // the room in key green, for a streamer that keys it out on the headset.
  auto roomKeyed = [&] {
    return !kStandaloneHeadset && settings.keyGreen && !xr.alphaBlendSupported();
  };

  auto execute = [&](const MenuCommand& c) {
    if (applySettingCommand(c, settings)) return;
    switch (c.type) {
      case MenuCommand::TogglePause: player.command("cycle pause"); break;
      case MenuCommand::Stop:
        closeVideo();
        menu.showPlayedFrom();
        break;
      case MenuCommand::Seek:
        awaitingResume = false;
        player.seekBy(c.value);
        break;
      case MenuCommand::SeekTo: {
        awaitingResume = false;
        // Chapters, and short jumps along the seek bar, land exactly; a long
        // jump lands on the keyframe before the time: much faster (exact
        // seeks decode from it, up to seconds on the Frame), and closer than
        // a pointer on the bar can aim.
        constexpr double kExactJump = 30;  // seconds
        const bool fromBar = c.option == 1;
        player.seekTo(c.value, !fromBar || std::abs(c.value - player.playback().position) <= kExactJump);
        break;
      }
      case MenuCommand::ToggleMute: player.command("cycle mute"); break;
      case MenuCommand::SetVolume:
        player.command(("set volume " + std::to_string(static_cast<int>(c.value))).c_str());
        if (player.playback().muted) player.command("set mute no");  // a volume set is heard
        break;
      case MenuCommand::PreviousVideo: stepVideo(-1); break;
      case MenuCommand::NextVideo: stepVideo(1); break;
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
      case MenuCommand::SetBrightness:
        running.video.brightness = static_cast<int>(c.value);
        running.applyPicture(VideoSettings::kBrightness);
        break;
      case MenuCommand::SetContrast:
        running.video.contrast = static_cast<int>(c.value);
        running.applyPicture(VideoSettings::kContrast);
        break;
      case MenuCommand::SetSaturation:
        running.video.saturation = static_cast<int>(c.value);
        running.applyPicture(VideoSettings::kSaturation);
        break;
      case MenuCommand::SetWarmth:  // the colour balance: applied while drawing
      case MenuCommand::SetTint:
      case MenuCommand::SetExposure:
        if (const unsigned part = passthrough.setBalance(c)) running.storeVideoSoon(part);
        break;
      case MenuCommand::ToggleMatchRoom:  // off: the video's own balance again
        passthrough.toggle();
        break;
      case MenuCommand::ReadRoomAgain:
        passthrough.readAgain();
        break;
      case MenuCommand::SetMatchColour:
        settings.matchColour = std::clamp(static_cast<int>(c.value), 0, 100);
        saveSettings(settings);
        passthrough.applyRoomMatch();
        break;
      case MenuCommand::SetMatchBrightness:
        settings.matchBrightness = std::clamp(static_cast<int>(c.value), 0, 100);
        saveSettings(settings);
        passthrough.applyRoomMatch();
        break;
      case MenuCommand::SetOpacity:  // applied while drawing
        running.video.opacity = static_cast<int>(c.value);
        running.storeVideoSoon(VideoSettings::kOpacity);
        break;
      case MenuCommand::SetZoom:
        running.video.zoom = static_cast<float>(c.value / 100.0);
        running.applyPicture(VideoSettings::kZoom);
        break;
      case MenuCommand::ToggleTvGamma:
        settings.tvGamma = !settings.tvGamma;
        player.setTvGamma(settings.tvGamma);
        matchPreset();
        saveSettings(settings);
        break;
      case MenuCommand::ResetPicture:
        running.video.brightness = running.video.contrast = running.video.saturation = 0;
        running.video.balance = {};
        passthrough.pictureReset();
        running.video.zoom = 1.0f;
        running.video.originForward = 0;
        running.applyPicture(VideoSettings::kPicture | VideoSettings::kOrigin);
        break;
      case MenuCommand::SetOriginForward:
        running.video.originForward = std::clamp(static_cast<float>(c.value / 100.0),
                                             -VideoSettings::kMaxOriginForward,
                                             VideoSettings::kMaxOriginForward);
        running.storeVideoSoon(VideoSettings::kOrigin);
        break;
      case MenuCommand::Paste: menu.typeText(clipboardText(input)); break;
      case MenuCommand::SetSlideshow:
        app.slideshowSeconds = static_cast<int>(c.value);
        if (!app.slideshowSeconds && neighborWrap) neighbor = {};
        app.nextSlideAt = glfwGetTime() + app.slideshowSeconds;
        break;
      case MenuCommand::SetScreenDistance:
      case MenuCommand::SetScreenHeight:
      case MenuCommand::SetScreenCurve: {
        ScreenSize& size = running.screen.size;
        (c.type == MenuCommand::SetScreenDistance ? size.distance
         : c.type == MenuCommand::SetScreenHeight ? size.height
                                                  : size.curve) = static_cast<float>(c.value / 100);
        running.keepScreenSize();
        break;
      }
      case MenuCommand::ResetScreen:
        running.screen.size = settings.screen = ScreenSize{};
        running.storeVideoSoon(VideoSettings::kScreen);
        break;
      case MenuCommand::Recenter:
        // From the menu: two seconds to look where "front" should be.
        app.recenterAt = glfwGetTime() + 2.0;
        break;
      case MenuCommand::ToggleAutoPassthrough:
        settings.autoPassthrough = !settings.autoPassthrough;
        saveSettings(settings);
        passthrough.resettle();  // for the video playing too
        break;
      case MenuCommand::ToggleKeyGreen:
        settings.keyGreen = !settings.keyGreen;
        saveSettings(settings);
        passthrough.resettle();  // Automatic passthrough, for the video playing
        break;
      case MenuCommand::ToggleVideoMipmaps:
        settings.videoMipmaps = !settings.videoMipmaps;
        matchPreset();
        saveSettings(settings);
        player.setMipmaps(settings.videoMipmaps);
        break;
      case MenuCommand::SetAntiShimmer:
        settings.antiShimmer = static_cast<AntiShimmer>(
            std::clamp(static_cast<int>(c.value), 0, kAntiShimmerCount - 1));
        matchPreset();
        saveSettings(settings);
        if (settings.antiShimmer == antiShimmer) break;
        antiShimmer = settings.antiShimmer;
        reportAntiShimmer("menu");
        break;
      case MenuCommand::SetVideoResolution: {  // applied once it settles (applyPendingResolution)
        // Frame sends a stop index, desktop a percent; the held slider repeats it.
        const int choice = static_cast<int>(c.value);
        const int value = resolutionStops.empty()
                              ? std::clamp(choice, kMinResolution, kMaxResolution)
                              : resolutionStops[std::clamp(choice, 0, static_cast<int>(resolutionStops.size()) - 1)];
        if (value == (pendingResolution > 0 ? pendingResolution : chosenResolution()) &&
            (pendingResolution > 0 || overrideScale == 0))
          break;
        pendingResolution = value;
        pendingResolutionAt = glfwGetTime() + kResolutionSettle;
        break;
      }
      case MenuCommand::SetFoveation:  // applied before xrWaitFrame
        chosenFoveation = static_cast<FoveationLevel>(std::clamp(static_cast<int>(c.value), 0, 3));
        foveationPreset(chosenFoveation, sharpBins, foveationRing, foveationOutside);
        pendingFoveation = true;
        break;
      case MenuCommand::SetRefreshRate:  // value: Hz
        setRefreshRate(static_cast<int>(c.value));
        matchPreset();
        saveSettings(settings);
        break;
      case MenuCommand::ApplyPreset: {  // value: the PerformancePreset
        const auto preset = static_cast<PerformancePreset>(static_cast<int>(c.value));
        if (!presets.offered(preset)) break;
        pickedUserPreset.clear();
        std::printf("Performance preset: %s\n", performancePresetName(preset));
        applyPreset(presets.with(preset, settings));
        break;
      }
      case MenuCommand::ListUserPresets: userPresetsListing = listUserPresets(); break;
      case MenuCommand::SaveUserPreset:
        pickedUserPreset = presetName(c.path);
        userPresetsListing = saveUserPreset(c.path, settings);
        break;
      case MenuCommand::ApplyUserPreset:
        for (const UserPreset& preset : userPresets) {
          if (preset.file != c.path) continue;
          pickedUserPreset = preset.name;
          applyPreset(presets.with(preset, settings));
          break;
        }
        break;
      case MenuCommand::RenameUserPreset: {
        const size_t line = c.path.find('\n');
        if (line == std::string::npos) break;
        for (const UserPreset& preset : userPresets)
          if (preset.file == c.path.substr(0, line) && preset.name == pickedUserPreset)
            pickedUserPreset = presetName(c.path.substr(line + 1));
        userPresetsListing = renameUserPreset(c.path.substr(0, line), c.path.substr(line + 1));
        break;
      }
      case MenuCommand::DeleteUserPreset: userPresetsListing = deleteUserPreset(c.path); break;
      case MenuCommand::MoveUserPreset:
        userPresetsListing = moveUserPreset(c.path, static_cast<int>(c.value));
        break;
      case MenuCommand::SetLanguage:
        if (menu.setupRunning()) {  // shown at once, kept when setup ends
          setupChoices.language = languageCode(static_cast<int>(c.value));
          setLanguage(setupChoices.language);
          menu.languageChanged();
          if (!setupForced) saveSetupChoices(setupChoices);
          break;
        }
        settings.language = languageCode(static_cast<int>(c.value));
        setLanguage(settings.language);
        menu.languageChanged();
        saveSettings(settings);
        break;
      case MenuCommand::EndSetup: finishSetup(c.value == 1); break;
      case MenuCommand::SetupPlaces: {
        setupChoices.places.clear();
        for (size_t at = 0; at < c.path.size();) {
          const size_t line = std::min(c.path.find('\n', at), c.path.size());
          if (line > at) setupChoices.places.push_back(c.path.substr(at, line - at));
          at = line + 1;
        }
        if (!setupForced) saveSetupChoices(setupChoices);
        break;
      }
      case MenuCommand::SetupStash: {
        // Connected in setup, as Settings sets it (the kind that answered
        // in its value), and setup ends there: its first sync starts at
        // once, and its folders are looked for once it is done, in the
        // places ticked too.
        const ServerKind kind = static_cast<ServerKind>(static_cast<int>(c.value));
        const size_t line = c.path.find('\n');
        const std::string url = c.path.substr(0, line);
        const std::string key = line == std::string::npos ? "" : c.path.substr(line + 1);
        const std::string unverifiedHost = c.option ? stashHost(url) : std::string();
        const bool same = stash && settings.stash.kind == kind && settings.stash.url == url &&
                          settings.stash.apiKey == key &&
                          settings.stash.unverifiedHost == unverifiedHost;
        useServer(settings.stash, settings.otherServers, kind);
        // Another server than this kind had: nothing is sent to it unasked.
        if (StashAddress(url).server != StashAddress(settings.stash.url).server)
          settings.stash.writeBack = false;
        settings.stash.enabled = true;
        settings.stash.url = url;
        settings.stash.apiKey = key;
        settings.stash.unverifiedHost = unverifiedHost;
        settings.stash.card = false;
        stashInSetup = true;
        saveSettings(settings);
        if (!same) restartStash();
        std::printf("Setup: %s connected\n", mediaServer(kind).name());
        finishSetup(true);
        break;
      }
      case MenuCommand::DismissSetupBanner:
        hideSetupBanner();
        break;
      case MenuCommand::ToggleShowFoveation:
        if (!foveation.active()) break;
        settings.showFoveation = showFoveation = !showFoveation;
        saveSettings(settings);
        break;
      case MenuCommand::ToggleFoveationLearned:
        if (!foveation.active()) break;
        settings.foveationLearned = !settings.foveationLearned;
        saveSettings(settings);
        break;
      case MenuCommand::SetSharpBins: {
        const int bins = c.value == 1 ? 1 : 3;
        if (bins == sharpBins) break;
        settings.sharpBins = sharpBins = bins;
        pendingFoveation = true;
        break;
      }
      case MenuCommand::ToggleFoveationRing:
        settings.foveationRing = foveationRing = !foveationRing;
        pendingFoveation = true;
        break;
      case MenuCommand::SetFoveationOutside:
        foveationOutside = c.value == 2 ? 2 : 4;
        pendingFoveation = true;
        break;
      case MenuCommand::SetSharpen:
        settings.sharpen = std::clamp(static_cast<int>(c.value), 0, 300);
        player.setSharpened(settings.sharpen > 0);
        running.keepSharpenSoon();
        break;
      case MenuCommand::ToggleToneLook:
        settings.toneLook = !settings.toneLook;
        matchPreset();
        saveSettings(settings);
        break;
      case MenuCommand::ToggleFrameReuse:
        settings.frameReuse = !frameReuse.enabled();
        saveSettings(settings);
        frameReuse.setEnabled(kStandaloneHeadset && settings.frameReuse);
        frameReuse.forget();  // nothing drawn before the switch is shown again
        std::printf("Frame reuse: %s\n", frameReuse.enabled() ? "on" : "off");
        break;
      case MenuCommand::ToggleSeekPreview:
        settings.seekPreview = !settings.seekPreview;
        saveSettings(settings);
        seekPreview.setEnabled(settings.seekPreview);
        break;
      case MenuCommand::ToggleShowPerformance:
        settings.showPerformance = !showPerformance;
        saveSettings(settings);
        switchReadout(settings.showPerformance);
        break;
      case MenuCommand::SetButtonJob: {
        const ControlSlot slot = ControlSlot::unpack(c.option);
        const int job = static_cast<int>(std::lround(c.value));  // -1 with grip held: none of its own
        if (slot.type == ControllerType::Count || slot.hand > 1 || job >= kJobs) break;
        ControlMap& map = settings.controls;
        const bool done =
            job < 0 ? slot.grip && map.clearGrip(slot.type, slot.hand, slot.control)
                    : (slot.grip || map.allowed(slot.type, slot.hand, slot.control, static_cast<Job>(job))) &&
                          map.set(slot, static_cast<Job>(job));
        if (!done) break;
        saveSettings(settings);
        describeBindings();
        break;
      }
      case MenuCommand::RestoreButton: {
        const ControlSlot slot = ControlSlot::unpack(c.option);
        const int button = static_cast<int>(std::lround(c.value));
        ControlMap& map = settings.controls;
        if (slot.type == ControllerType::Count || slot.hand > 1 || button < 0 ||
            !map.allowed(slot.type, slot.hand, slot.control, static_cast<Job>(button % kJobs)) ||
            !map.restore(slot.type, slot.hand, slot.control, button))
          break;
        saveSettings(settings);
        describeBindings();
        break;
      }
      case MenuCommand::ResetButtons: {
        const int type = static_cast<int>(std::lround(c.value));
        if (type < 0 || type >= kControllerTypes) break;
        settings.controls.reset(static_cast<ControllerType>(type));
        settings.gestures.reset(static_cast<ControllerType>(type));
        saveSettings(settings);
        describeBindings();
        break;
      }
      case MenuCommand::AddGesture:
      case MenuCommand::ChangeGesture:
      case MenuCommand::RemoveGesture:
      case MenuCommand::RestoreGestures: {
        const bool packed = c.type == MenuCommand::AddGesture || c.type == MenuCommand::ChangeGesture;
        const int type = packed ? c.option >> 17 : c.option;
        if (type < 0 || type >= kControllerTypes) break;
        const ControllerType t = static_cast<ControllerType>(type);
        const size_t index = static_cast<size_t>(std::max(0L, std::lround(c.value)));
        GestureMap& map = settings.gestures;
        bool done = true;
        if (c.type == MenuCommand::AddGesture) done = map.add(t, Gesture::unpack(c.option & 0x1ffff));
        else if (c.type == MenuCommand::ChangeGesture) done = map.replace(t, index, Gesture::unpack(c.option & 0x1ffff));
        else if (c.type == MenuCommand::RemoveGesture) done = map.remove(t, index);
        else map.read(t, c.path);
        if (!done) break;
        saveSettings(settings);
        describeBindings();
        break;
      }
      case MenuCommand::ToggleScripts:
      case MenuCommand::SetScriptAddress:
      case MenuCommand::SetScriptOffset:
      case MenuCommand::SetScriptRangeLow:
      case MenuCommand::SetScriptRangeHigh: {
        ScriptSettings& s = settings.scripts;
        const int value = static_cast<int>(std::lround(c.value));
        if (c.type == MenuCommand::ToggleScripts) {
          s.enabled = !s.enabled;
        } else if (c.type == MenuCommand::SetScriptAddress) {
          const size_t first = c.path.find_first_not_of(" \t");
          s.address = first == std::string::npos
                          ? std::string()
                          : c.path.substr(first, c.path.find_last_not_of(" \t") - first + 1);
        } else if (c.type == MenuCommand::SetScriptOffset) {
          s.offsetMs = std::clamp(value, -kScriptOffsetLimit, kScriptOffsetLimit);
        } else if (c.type == MenuCommand::SetScriptRangeLow) {
          // The other end keeps its distance.
          s.rangeLow = std::clamp(value, 0, 100 - kScriptRangeGap);
          s.rangeHigh = std::max(s.rangeHigh, s.rangeLow + kScriptRangeGap);
        } else {
          s.rangeHigh = std::clamp(value, kScriptRangeGap, 100);
          s.rangeLow = std::min(s.rangeLow, s.rangeHigh - kScriptRangeGap);
        }
        saveSettings(settings);
        scripts.configure(s);
        offline.setScripts(s.enabled);
        break;
      }
      case MenuCommand::TogglePassthrough:
        keys.togglePassthrough = true;
        break;
      case MenuCommand::TogglePackedMask: {
        // Nothing is stored when the choice is what the name says.
        const bool on = !packedMask(running.currentFile, running.video);
        running.video.packedMask = on == hasPackedAlpha(running.currentFile) ? std::nullopt
                                                                     : std::optional<bool>(on);
        running.storeVideo(VideoSettings::kPackedMask);
        std::printf("Passthrough: mask in the video %s\n", on ? "on" : "off");
        break;
      }
      case MenuCommand::ToggleSecondLayer:
        running.video.secondLayerHidden = !running.video.secondLayerHidden;
        running.storeVideo(VideoSettings::kSecondLayer);
        std::printf("Passthrough: the mask's second layer %s\n",
                    running.video.secondLayerHidden ? "hidden" : "shown");
        break;
      case MenuCommand::ToggleGreenScreen:
      case MenuCommand::SetKeyStrength:
      case MenuCommand::SetKeySoftness:
      case MenuCommand::SetKeySpill:
      case MenuCommand::SetKeyShrink:
      case MenuCommand::SetKeyBackground: {
        GreenScreen& key = running.video.greenScreen;
        const int value = static_cast<int>(c.value);
        KeySliders& sliders = running.video.keySliders();
        if (c.type == MenuCommand::ToggleGreenScreen) key.on = !key.on;
        else if (c.type == MenuCommand::SetKeyBackground)
          key.source = value == static_cast<int>(KeySource::GreenScreen) ? KeySource::GreenScreen
                                                                         : KeySource::Automatic;
        else if (c.type == MenuCommand::SetKeyStrength) sliders.strength = value;
        else if (c.type == MenuCommand::SetKeySoftness) sliders.softness = value;
        else if (c.type == MenuCommand::SetKeyShrink) sliders.shrink = value;
        else sliders.spill = value;
        running.storeVideoSoon(VideoSettings::kGreenScreen);
        break;
      }
      case MenuCommand::PickKeyColour:
        // Where the key is used: passthrough on a surround video, without its own mask.
        if (running.video.passthrough && running.video.greenScreen.on && !running.currentFile.empty() &&
            running.format.projection != Projection::Flat &&
            !(packedMask(running.currentFile, running.video) && maskFormat(running.format))) {
          app.keyPickFor = running.currentFile;
          app.keyPickNote = KeyPickNote::None;
          std::printf("Background key: pick a colour\n");
        }
        break;
      case MenuCommand::CancelKeyPick:
        endKeyPick();
        break;
      case MenuCommand::ResetPassthrough: {
        // What the group shows: the key's sliders only while the green
        // screen is on (passthrough on, not the video's own mask) and removes
        // something, Match the room's strengths only while it is on.
        passthrough.resetMatch();
        unsigned parts = VideoSettings::kOpacity;
        const bool own = packedMask(running.currentFile, running.video) && maskFormat(running.format);
        if (running.video.passthrough && running.video.greenScreen.on && !own && running.video.keyRemoves()) {
          running.video.keySliders() = GreenScreen().sliders(running.video.colourKeyed());
          parts |= VideoSettings::kGreenScreen;
        }
        running.video.opacity = 100;
        running.storeVideoSoon(parts);
        break;
      }
      case MenuCommand::SetSubtitleSize:
      case MenuCommand::SetSubtitleRaise:
      case MenuCommand::SetSubtitleDistance:
        (c.type == MenuCommand::SetSubtitleSize    ? settings.subtitleSize
         : c.type == MenuCommand::SetSubtitleRaise ? settings.subtitleRaise
                                                   : settings.subtitleDistance) =
            static_cast<int>(c.value);
        saveSettings(settings);
        player.setSubtitleStyle(settings.subtitleSize / 100.0, settings.subtitleRaise);
        break;
      case MenuCommand::SetFocus:
        if (applyFocusControl(static_cast<FocusControl>(c.option), c.value, running.video.hasFocus,
                              running.video.focus, settings.focus, settings.focusPrefs)) {
          matchPreset();  // a user's preset keeps the focus defaults
          saveSettings(settings);
        }
        running.storeVideo(VideoSettings::kFocus);
        break;
      case MenuCommand::Quit: keys.quit = true; break;
      case MenuCommand::OpenFile:
        openVideo(c.path);
        panel.open = false;
        if (listShuffle != (c.option == 1)) {
          listShuffle = c.option == 1;
          applyAfterVideo();
        }
        startShuffle();
        break;
      case MenuCommand::SetAfterVideo:
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
        break;
      case MenuCommand::SelectAudio:
        player.command(("set aid " + std::to_string(static_cast<int64_t>(c.value))).c_str());
        break;
      case MenuCommand::SelectSubtitle:
        player.command(c.value > 0
                           ? ("set sid " + std::to_string(static_cast<int64_t>(c.value))).c_str()
                           : "set sid no");
        break;
      case MenuCommand::LoadSubtitle: player.addSubtitle(c.path); break;
      case MenuCommand::RequestDelete:
        if (deletion.request(c, running.currentFile, running.streamOshash)) app.slideshowSeconds = 0;
        break;
      case MenuCommand::CancelDelete: deletion.cancel(running.currentFile); break;
      case MenuCommand::DeleteFile: deletion.confirm(c, running.currentFile); break;
      case MenuCommand::UseServer:
        // None (-1), or a kind's own setup in use and on: the one it
        // replaces is kept as it was left.
        if (c.value < 0) {
          settings.stash.enabled = false;
        } else {
          useServer(settings.stash, settings.otherServers,
                    static_cast<ServerKind>(static_cast<int>(c.value)));
          settings.stash.enabled = true;
        }
        menu.findStashFolders(false);  // its folders are as they were left
        saveSettings(settings);
        restartStash();
        break;
      case MenuCommand::SetStashServer: {
        // The URL, then the API key; its value, the kind of server it is,
        // whose setup becomes the one in use.
        StashSettings changed = settings.stash;
        std::vector<StashSettings> others = settings.otherServers;
        useServer(changed, others, static_cast<ServerKind>(static_cast<int>(c.value)));
        const size_t line = c.path.find('\n');
        std::string url = c.path.substr(0, line);
        std::string key = line == std::string::npos ? std::string() : c.path.substr(line + 1);
        const size_t first = url.find_first_not_of(" \t");
        url = first == std::string::npos
                  ? std::string()
                  : url.substr(first, url.find_last_not_of(" \t") - first + 1);
        while (url.size() > 1 && url.back() == '/') url.pop_back();
        // A kind not set up yet, saved without an address: the setup in use
        // stays.
        if (url.empty() && changed.kind != settings.stash.kind) break;
        // The server this kind had, however its address was typed.
        const bool sameServer =
            !url.empty() && StashAddress(url).server == StashAddress(changed.url).server;
        // Its certificate accepted unverified (option 1) for its host alone.
        std::string unverifiedHost = (c.option & 1) && !url.empty() ? stashHost(url) : std::string();
        // Found to be another kind than its fields were for (option 2):
        // nothing was typed for it. At the server it had it keeps its own
        // key and whether its certificate is verified; at another, nothing
        // is sent to it unasked.
        if (c.option & 2) {
          if (sameServer) {
            key = changed.apiKey;
            unverifiedHost = changed.unverifiedHost;
          } else {
            changed.writeBack = false;
          }
        }
        changed.url = url;
        changed.apiKey = key;
        if (!url.empty()) changed.enabled = true;
        changed.card = changed.card && url.empty();  // set up: no "Use Stash?" card
        changed.unverifiedHost = std::move(unverifiedHost);
        if (changed.url != settings.stash.url || changed.apiKey != settings.stash.apiKey ||
            changed.kind != settings.stash.kind || changed.enabled != settings.stash.enabled ||
            changed.unverifiedHost != settings.stash.unverifiedHost) {
          // Another server than this kind had: where its folders are here
          // is looked for after its first sync, and one found in a single
          // place is used without asking. A setup gone back to keeps its
          // folders as they were left.
          if (!changed.url.empty() && !sameServer) menu.findStashFolders(true);
          else if (c.option & 2) menu.findStashFolders(false);
          settings.stash = std::move(changed);
          settings.otherServers = std::move(others);
          saveSettings(settings);
          restartStash();
        } else if (stash) {
          stash->syncNow();
        }
        break;
      }
      case MenuCommand::SetStashFolders: {
        // Each mapping's folder on the server and the folder here, one per
        // line.
        std::vector<std::string> lines;
        for (size_t start = 0; start < c.path.size();) {
          const size_t end = std::min(c.path.find('\n', start), c.path.size());
          lines.push_back(c.path.substr(start, end - start));
          start = end + 1;
        }
        std::vector<StashFolderMap> folders;
        for (size_t i = 0; i + 1 < lines.size(); i += 2)
          if (!lines[i].empty()) folders.push_back({lines[i], lines[i + 1]});
        if (folders != settings.stash.folders) {
          settings.stash.folders = std::move(folders);
          saveSettings(settings);
          restartStash();
        }
        break;
      }
      case MenuCommand::ToggleStashCovers:
        settings.stash.covers = !settings.stash.covers;
        saveSettings(settings);
        if (stash) stash->setCovers(settings.stash.covers);
        break;
      case MenuCommand::ToggleStashTitles:
        settings.stash.titles = !settings.stash.titles;
        saveSettings(settings);
        if (stash) stash->setTitles(settings.stash.titles);
        break;
      case MenuCommand::ToggleStashMalePerformers:
        settings.stash.malePerformers = !settings.stash.malePerformers;
        saveSettings(settings);
        if (stash) stash->setMalePerformers(settings.stash.malePerformers);
        break;
      case MenuCommand::SyncStash:
        if (stash) stash->syncNow();
        break;
      case MenuCommand::ToggleStashWriteBack:
        settings.stash.writeBack = !settings.stash.writeBack;
        saveSettings(settings);
        if (stash) stash->setWriteBack(settings.stash.writeBack);
        break;
      case MenuCommand::DownloadReleaseUpdate: releaseUpdate.download(); break;
      case MenuCommand::FreeDecoder:
        if (c.value != 0 && !settings.freeDecoder) {
          settings.freeDecoder = true;
          saveSettings(settings);
        }
        if (decoderRescue.busy == decoder_holders::Busy::Steam && !decoderRescue.freed.valid()) {
          decoderRescue.busy = decoder_holders::Busy::Freeing;
          decoderRescue.freed = std::async(std::launch::async, decoder_holders::freeFromSteam);
          if (decoderRescue.openedMenu) panel.open = false;
        }
        break;
      case MenuCommand::SaveDiagnostics: diagnostics.start(); break;
      case MenuCommand::None: break;
      default: break;  // applySettingCommand's
    }
  };

  // The gestures' values: read and set as the menu's sliders set them (their
  // commands), and why one can't change now, as the buttons' jobs say it.
  gestureHost.why = [&](GestureValue v) -> const char* {
    using V = GestureValue;
    const JobStrings& said = strings().jobs;
    const bool open = !running.currentFile.empty(), photo = isPhotoFile(running.currentFile);
    switch (gestureValueInfo(v).where) {
      case GestureWhere::Anywhere: break;
      case GestureWhere::File:
        if (!open) return said.nothingOpen;
        break;
      case GestureWhere::Video:
        if (!open) return said.nothingOpen;
        if (photo) return said.notForPhotos;
        break;
      case GestureWhere::Surround:
        if (!open) return said.nothingOpen;
        if (running.format.projection == Projection::Flat) return said.notForFlat;
        break;
      case GestureWhere::Flat:
        if (!open) return said.nothingOpen;
        if (running.format.projection != Projection::Flat) return said.flatOnly;
        break;
    }
    switch (v) {
      case V::Opacity:
        if (!xr.alphaBlendSupported()) return said.noRoom;
        if (peeking) return "";  // the video isn't seen
        break;
      case V::MatchBrightness:
      case V::MatchColour:
        // Where the menu shows them: Match the room on, the room shown.
        if (!passthrough.roomReadable) return said.notOnHeadset;
        if (!passthrough.roomShown()) return said.needsPassthrough;
        if (!passthrough.matched()) return strings().gestures.needsMatchRoom;
        break;
      case V::KeyStrength:
      case V::KeyEdges:
      case V::KeySpill:
      case V::KeyShrink:
        if (!xr.alphaBlendSupported() && !roomKeyed()) return said.noRoom;
        if (!running.video.passthrough) return said.needsPassthrough;
        if (packedMask(running.currentFile, running.video) && maskFormat(running.format)) return said.ownMask;
        if (!running.video.greenScreen.on) return strings().gestures.needsRemoveBackground;
        // As the menu hides their sliders: Automatic found nothing to remove.
        if (!running.video.keyRemoves()) return strings().gestures.nothingRemoved;
        break;
      case V::Scrub:
        if (player.loading() || player.playback().duration <= kSeekEndMargin || stickSeek.active()) return "";
        break;
      case V::Rating:
        if (isStream(running.currentFile)) return said.notForStreams;
        if (!library.available()) return said.ratingsLocked;
        if (!library.known(running.currentFile)) return "";  // its rating isn't read yet
        break;
      default: break;
    }
    return nullptr;
  };
  gestureHost.read = [&](GestureValue v) -> double {
    using V = GestureValue;
    const ColourBalance& balance =
        passthrough.balance(passthrough.matched() && passthrough.roomShown());
    const KeySliders& key = running.video.keySliders();
    switch (v) {
      case V::Volume: return player.playback().volume;
      case V::Opacity: return running.video.opacity;
      case V::MatchBrightness: return settings.matchBrightness;
      case V::MatchColour: return settings.matchColour;
      case V::KeyStrength: return key.strength;
      case V::KeyEdges: return key.softness;
      case V::KeySpill: return key.spill;
      case V::KeyShrink: return key.shrink;
      case V::Brightness: return running.video.brightness;
      case V::Contrast: return running.video.contrast;
      case V::Saturation: return running.video.saturation;
      case V::Warmth: return balance.warmth;
      case V::Tint: return balance.tint;
      case V::Exposure: return balance.exposure;
      case V::Sharpen: return settings.sharpen;
      case V::Zoom: return running.video.zoom * 100.0;
      case V::Viewpoint: return running.video.originForward * 100.0;
      case V::ScreenDistance: return running.screen.size.distance * 100.0;
      case V::ScreenHeight: return running.screen.size.height * 100.0;
      case V::ScreenCurve: return running.screen.size.curve * 100.0;
      case V::Scrub: return stickSeek.scrubValue();
      case V::Rating: return library.record(running.currentFile).rating * 2.0;
      case V::ScriptTiming: return settings.scripts.offsetMs;
      case V::SubtitleSize: return settings.subtitleSize;
      case V::SubtitleRaise: return settings.subtitleRaise;
      case V::SubtitleDistance: return settings.subtitleDistance;
      case V::Count: break;
    }
    return 0;
  };
  gestureHost.write = [&](GestureValue v, double value) {
    if (v == GestureValue::Scrub) {
      stickSeek.scrubTo(value);
    } else if (v == GestureValue::Rating) {
      // As Rating up and down do, by half stars.
      const int now = static_cast<int>(std::lround(library.record(running.currentFile).rating * 2));
      menu.stepRating(running.currentFile, static_cast<int>(std::lround(value)) - now);
    } else {
      execute({gestureValueInfo(v).command, value});
    }
  };
  gestureHost.range = [&](GestureValue v, double& least, double& most) {
    if (v != GestureValue::Scrub) return;
    least = 0;
    most = std::max(0.0, player.playback().duration - kSeekEndMargin);
  };
  gestureHost.began = [&](GestureValue v) {
    if (v != GestureValue::Scrub) return;
    stickSeek.beginScrub(glfwGetTime());
    awaitingResume = false;  // a seek of the user's
  };
  gestureHost.ended = [&](GestureValue v, bool released) {
    if (v != GestureValue::Scrub) return;
    stickSeek.endScrub(released);
  };
  gestureHost.say = [&](const char* text) { say(text); };
  gestureHost.buzz = [&](int hand, float amplitude, double seconds) { buzz(hand, amplitude, seconds); };
  stickSeek.buzz = [&](int hand, float amplitude, double seconds) { buzz(hand, amplitude, seconds); };

  bool xrQuit = false;
  FrameTimer timer(opts.timing);
  // Frames drawn per second, for the strip: counted over one-second spans of
  // the runtime's display times, which fall on the headset's refresh, so a
  // frame drawn at every refresh counts as exactly the refresh rate.
  int appFps = 0, framesCounted = 0;
  double fpsSince = -1;
  double resolutionCycleAt = glfwGetTime() + opts.resolutionCycleSeconds;
  size_t resolutionCycleStep = 0;
  FrameScene scene;  // what each frame draws
  // Frames shown again and drawn, by FrameReuse::Reason: for --timing's
  // lines and its totals at the end.
  unsigned long long framesBy[FrameReuse::kReasons] = {}, framesReported[FrameReuse::kReasons] = {};
  const auto reuseReport = [&](const unsigned long long* since) {
    std::string text;
    for (int i = 0; i < FrameReuse::kReasons; ++i)
      text += std::string(i ? " " : "") + FrameReuse::reasonName(static_cast<FrameReuse::Reason>(i)) +
              "=" + std::to_string(framesBy[i] - (since ? since[i] : 0));
    return text;
  };
  EyeCorrection shownCorrection;  // the left eye's stereo correction, as last drawn
  while (!xrQuit && !keys.quit && !quitSignal && !glfwWindowShouldClose(input)) {
    timer.start();
    glfwPollEvents();
    xr.pollEvents(xrQuit);
    player.pollEvents();
    if (settings.scripts.enabled) {
      // Held too while the headset is taken off, though the video plays on.
      const PlaybackState& p = player.playback();
      scripts.sync(p.position, !running.currentFile.empty() && !p.paused && !p.endReached &&
                                   !player.seeking() && !player.loading() && !player.failed() &&
                                   xr.userPresence() != 0);
    }
    if (kStandaloneHeadset && opts.hwdec == "v4l2m2m") {
      if (!xr.isVisible() && stickSeek.active()) {
        if (stickSeek.pausedVideo()) player.command("set pause no");
        stickSeek.cancel();
      }
      player.setPresentationActive(xr.isVisible());
    }
    timer.lap(FrameTimer::Events);
    if (player.shutdownRequested()) break;
    // Also while nothing is drawn. The input focus here is the last drawn
    // frame's: a frame checks its own before the gaze correction sees its input.
    if (!xr.isRunning() || !xr.controllers().focused || xr.userPresence() == 0) endGazeSitting();

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
          openVideo(stashStream(scene));
          openedAs = listed;
        }
      }
    // A play counts each time a video file opens (a resumed
    // one too; pausing, looping and seeking don't). Streams and photos don't.
    const bool opened = player.takeOpened();
    // Played after freeing the decoder: a later refusal (after a sleep) is asked about again.
    if (opened && player.loadCount() == decoderRescue.retry) decoderRescue.retry = 0;
    if (opened && setupBanner && !isPhotoFile(running.currentFile)) hideSetupBanner();
    if (opened && !running.currentFile.empty() && !isStream(running.currentFile) &&
        !isPhotoFile(running.currentFile)) {
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

    if (keys.videoStep != 0) {
      stepVideo(keys.videoStep);
      keys.videoStep = 0;
    }

    running.saveDue(buttonJobs.zoomHeld());
    // The library's background results; the playing file's record may be
    // among them.
    if (library.poll() && awaitingRecord && library.known(running.currentFile)) {
      awaitingRecord = false;
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
    if (stash) stash->poll();
    if (!startScene.empty() && stash && stash->catalog()) {
      openVideo(startScene);
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
        openVideo(next);
      else if (!neighborWrap)
        std::printf("No %s video in this folder\n", neighborStep > 0 ? "next" : "previous");
      if (next.empty() && !neighborWrap && !endStep)
        say(neighborStep > 0 ? strings().jobs.lastVideo : strings().jobs.firstVideo);
      if (next.empty() && endStep) menuAtEnd = true;
      endStep = false;
      neighborStep = 0;
      app.nextSlideAt = glfwGetTime() + app.slideshowSeconds;
    }
    // mpv keeps a video that reached its end paused on its last frame. Play
    // the next video and Shuffle go on from there; with no next one, as for
    // Stop, the menu opens.
    const bool atEnd = player.playback().endReached && !running.currentFile.empty() &&
                       !isPhotoFile(running.currentFile);
    if (atEnd != wasAtEnd) {
      const AfterVideo after = afterVideo();
      if (atEnd && (after == AfterVideo::Next || after == AfterVideo::Shuffle) &&
          !isStream(running.currentFile))
        stepVideo(1, true);
      else
        menuAtEnd = atEnd;
    }
    wasAtEnd = atEnd;
    // Slideshow: the next photo in the folder, after the last one the first.
    if (app.slideshowSeconds > 0 && isPhotoFile(running.currentFile) &&
        glfwGetTime() >= app.nextSlideAt && !neighbor.valid()) {
      neighborStep = 1;
      neighborWrap = true;
      neighbor = mediaNeighbor(running.currentFile, 1, true);
    }

    deletion.poll();

    // Re-guess the format once the real frame size is known.
    if (running.rememberFormat) {  // changed by key
      rememberFormat();
      running.rememberFormat = false;
    }
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

    // Passthrough for videos made for it, once their size (so their format)
    // and their settings are known. Never where it couldn't show the room:
    // the runtime must blend, or key green stand in for it.
    const bool passthroughAvailable = xr.alphaBlendSupported() || roomKeyed();
    const bool autoPassthroughOn = settings.autoPassthrough && passthroughAvailable;
    passthrough.settle(autoPassthroughOn, knownWidth > 0 && !awaitingRecord);
    if (RemovableDrives::Change change; drives.poll(change)) {
      menu.drivesChanged(change.removed);
      offline.drivesChanged();
      const DriveStrings& t = strings().drives;
      using Kind = RemovableDrives::Kind;
      if (!change.failures.empty()) {
        // udisks says "Error mounting /dev/… at /run/media/…: why": just why.
        std::string why = change.failures.back();
        const size_t colon = why.rfind(": ");
        if (why.rfind("Error mounting", 0) == 0 && colon != std::string::npos)
          why = why.substr(colon + 2);
        if (why.size() > 80) why = why.substr(0, 77) + "...";
        const bool usb = change.failedKinds.back() == Kind::Usb;
        driveText = std::string(usb ? t.usbFailed : t.cardFailed) + "\n" + why;
        driveIcon = NoticeIcon::Warning;
        driveShownUntil = glfwGetTime() + kDriveNoticeShown;
      } else if (!change.added.empty() && !menu.setupRunning()) {  // setup shows it as a card
        const RemovableDrives::Drive& d = change.added.back();
        const char* kind = d.kind == Kind::Card  ? t.card
                           : d.kind == Kind::Usb ? t.usbDrive
                                                 : t.drive;
        driveText = kind + (d.label.empty() ? "" : ": " + d.label) + "\n" + t.inExplorer;
        driveIcon = d.kind == Kind::Card  ? NoticeIcon::Card
                    : d.kind == Kind::Usb ? NoticeIcon::Usb
                                          : NoticeIcon::Drive;
        driveShownUntil = glfwGetTime() + kDriveNoticeShown;
      }
    }
    // Looks for new videos in the pinned folders wait while a video is open.
    libraryIndex.setVideoOpen(!running.currentFile.empty() && !isPhotoFile(running.currentFile));
    // ... and so does looking for Stash's folders.
    if (stash) stash->setVideoOpen(!running.currentFile.empty() && !isPhotoFile(running.currentFile));
    // Downloads and new thumbnails wait while a video plays.
    const bool videoPlaying =
        !running.currentFile.empty() && !isPhotoFile(running.currentFile) && !player.playback().paused;
    offline.setPlaying(videoPlaying);
    thumbnailer.setPlaying(videoPlaying);
    std::vector<Offline::Download> downloaded;
    offline.poll(downloaded);
    // Stash is asked whether it answers while a scene may play from its stream.
    offline.watchStash(stash && stash->catalog() &&
                       (stash->catalog()->stats().streamed > 0 || !offline.downServers().empty()));
    for (const Offline::Download& d : downloaded) library.addCopy(d.path, d.oshash, d.size, d.mtime);
    passthrough.poll(autoPassthroughOn);

    // 3D and surround video always get floating subtitles (mpv's would be cut
    // in half or end up behind you); for 2D video the setting decides.
    const bool floatingSubtitles =
        settings.floatingSubtitles ||
        running.format.stereo != StereoLayout::Mono || running.format.projection != Projection::Flat;
    player.setSubtitlesInFrame(!floatingSubtitles);
    subtitles.setText(floatingSubtitles ? player.playback().subtitleText : "");
    // Until the new file's first frame arrives, the texture still holds the old
    // video, which would show cut apart in the new format; show nothing instead.
    bool photo = isPhotoFile(running.currentFile);
    bool loading = !photo && player.loading() && !running.currentFile.empty();
    if (keys.togglePassthrough && !passthroughAvailable) {
      // The video keeps its choice for a runtime that shows the room.
      keys.togglePassthrough = false;
      std::printf("Passthrough: the runtime offers no alpha blending, so the camera cannot "
                  "show\n");
    }
    if (keys.togglePassthrough) {
      keys.togglePassthrough = false;
      running.video.passthrough = !running.video.passthrough;
      running.video.passthroughChosen = true;  // never switched by itself again
      running.storeVideo(VideoSettings::kPassthrough);
      std::printf("Passthrough: %s\n", running.video.passthrough ? "on" : "off");
      passthrough.switchedByHand();
    }
    // Passthrough, where the runtime blends (the Frame itself, or a PC whose
    // runtime shows the headset's camera): the eye images are blended over
    // the camera by their alpha. Where it doesn't there is none, unless a PC
    // is set to draw the room in key green.
    const bool canBlend = xr.alphaBlendSupported();
    const bool alphaPassthrough = running.video.passthrough && canBlend;
    const bool keyPassthrough = running.video.passthrough && roomKeyed();
    const bool blended = passthrough.roomShown();
    // Peek at the room, held: the video at no opacity over the room, none of
    // its settings changed.
    const bool peek = peeking && canBlend;
    // The room's balance, while the switch is on and the room is shown.
    const bool roomMatching = passthrough.matched() && blended;
    passthrough.pollRoom();
    // Saved on every change of folder, so even a killed player reopens there.
    if (!menu.directory().empty() && menu.directory() != settings.lastFolder) {
      settings.lastFolder = menu.directory();
      saveSettings(settings);
    }
    if (menu.gallery() != settings.gallery) {
      settings.gallery = menu.gallery();
      saveSettings(settings);
    }
    if (menu.videoSettingsThisVideo() != settings.videoSettingsThisVideo) {
      settings.videoSettingsThisVideo = menu.videoSettingsThisVideo();
      saveSettings(settings);
    }

    if (!xr.isRunning()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    // Pipelined (--video-pipeline), the video frame rendered in the last
    // iteration is shown from this headset frame on.
    player.showRendered();
    // Frame's Q10C conversion is asynchronous GPU work, independent of the
    // head pose. Submit it before the runtime wait so it can finish while
    // SteamVR paces the frame, rather than consuming the eye-render deadline.
    const bool earlyVideo = opts.hwdec == "v4l2m2m";
    if (earlyVideo) {
      player.render();
      glFlush();
      timer.lap(FrameTimer::Mpv);
    }
    // Let xrWaitFrame absorb the user-triggered map replacement.
    if (pendingFoveation) {
      pendingFoveation = false;
      settings.foveation = chosenFoveation = foveationLevel(chosenFoveation != FoveationLevel::Off,
                                                           sharpBins, foveationRing, foveationOutside);
      settings.sharpBins = sharpBins;
      settings.foveationRing = foveationRing;
      settings.foveationOutside = foveationOutside;
      foveation.setLevel(chosenFoveation);
      foveation.setShape(sharpBins, foveationRing, foveationOutside);
      matchPreset();
      saveSettings(settings);
      foveation.prepare();
      if (!eyeTargets.update()) break;
    }
    const bool shouldRender = xr.beginFrame();
    if (pipelineVideo && xr.displayPeriod() > 0 &&
        std::abs(xr.displayPeriod() - pipelinePeriod) > 0.0005) {
      pipelinePeriod = xr.displayPeriod();
      player.setPipelined(true, pipelinePeriod);
    }
    // Emptied, keeping its overlay list's storage: no allocation per frame.
    scene.clear = ClearMode::Dark;
    scene.sky = false;
    scene.video.reset();
    scene.overlays.clear();
    timer.lap(FrameTimer::XrWait);
    if (!earlyVideo) player.render();
    seekPreview.poll();
    photoPreloader.update();
    timer.lap(FrameTimer::Mpv);
    // The picture for the scene, and for GL (depth estimation): the video's
    // frame just rendered, if any, or the photo once it is all uploaded.
    const Texture* videoImage = nullptr;
    GLuint videoTexture = 0;
    SharedImage* photoImage = nullptr;
    float frameAspect = 1.0f;
    int photoWidth = 0, photoHeight = 0;
    bool stillLoading = false;
    auto choosePicture = [&] {
      photo = isPhotoFile(running.currentFile);
      loading = !photo && player.loading() && !running.currentFile.empty();
      videoImage = nullptr;
      videoTexture = 0;
      photoImage = nullptr;
      frameAspect = player.aspect();
      photoWidth = photoHeight = 0;
      if (photo) {
        photoImage = photoPreloader.image(running.currentFile, photoWidth, photoHeight);
        if (photoImage) {
          videoImage = &photoImage->texture();
          videoTexture = photoImage->glTexture();
          frameAspect = static_cast<float>(photoWidth) / photoHeight;
          if (!photoSized) {  // the size is known now: guess the format
            photoSized = true;
            std::printf("Photo size: %dx%d\n", photoWidth, photoHeight);
            if (!running.formatChosenByUser) {
              running.format = guessVideoFormat(running.currentFile, photoWidth, photoHeight);
              printFormat(running.format, "detected");
            }
          }
        }
      } else if (!loading && !running.currentFile.empty()) {
        videoImage = player.texture();
        videoTexture = player.glTexture();
      }
      stillLoading = !running.currentFile.empty() && !videoImage && (photo || loading);
    };
    choosePicture();

    Quat headRot;
    bool headValid = false;
    if (shouldRender) {
      const double now = glfwGetTime();
      const double shownAt = xr.displayTime();
      ++framesCounted;
      if (fpsSince < 0 || shownAt - fpsSince > 2.0) {
        // The first frame, or the first after the session paused: a new span.
        framesCounted = 0;
        fpsSince = shownAt;
      } else if (shownAt - fpsSince >= 1.0) {
        appFps = static_cast<int>(std::lround(framesCounted / (shownAt - fpsSince)));
        framesCounted = 0;
        fpsSince = shownAt;
      }
      if (showPerformance) readout.frameDrawn(shownAt);
      Vec3 headPos;
      headValid = xr.headPose(headPos, headRot);
      const Mat4 frame = headFrame(headRot);
      const Mat4 menuAt = menuFrame(headRot);
      if (app.recenterAt > 0 && now >= app.recenterAt) {  // the menu's countdown is over
        keys.recenter = true;
        app.recenterAt = 0;
        buzz(-1, 0.3f, 0.020);
      }
      if ((initialRecenter || (keys.recenter && !menu.setupRunning())) && headValid) {
        if (keys.recenter) {
          if (running.video.originForward != 0)
            viewpointShownUntil = now + kValueShown;  // "Centered": it did go back
          running.video.originForward = 0;
          // Also mark a provisional zero: a late identity restore must not
          // bring back a remembered origin after an explicit recenter.
          running.storeVideoSoon(VideoSettings::kOrigin);
        }
        running.screen.anchorPosition = headPos;
        running.screen.anchorRotation = frame;
        running.screen.turn = 0.0f;
        turnDrag.tracking = false;  // a drag goes on from here, without catching up first
        if (panel.open) panel.placeInFrontOf(headPos, menuAt);  // the menu comes along
        keys.recenter = false;
        initialRecenter = false;
      }

      // --- Controller and keyboard input ---
      if (xr.profilesVersion() != profilesSeen) {
        profilesSeen = xr.profilesVersion();
        for (int h = 0; h < 2; ++h) controllerTypes[h] = controllerType(xr.interactionProfile(h));
        describeBindings();
        const ControllerType seen =
            controllerTypes[1] != ControllerType::Count ? controllerTypes[1] : controllerTypes[0];
        if (seen != ControllerType::Count && seen != settings.controllersSeen) {
          settings.controllersSeen = seen;
          saveSettings(settings);
        }
      }
      const ControllerState& raw = xr.controllers();
      // Presses that do something else, and nothing more: grip held and a
      // stick's click on the stars clears the rating; any press skips setup's
      // intro (below).
      if (panel.open && !menu.setupRunning() && (raw.hands[0].grip || raw.hands[1].grip))
        for (int h = 0; h < 2; ++h)
          if ((raw.hands[h].pressed & controlBit(Control::StickClick)) && menu.clearRatingAtPointer())
            controlInput.consume(h, Control::StickClick);
      bool introPressed = false;
      if (menu.setupRunning() && setupIntro.playing(now))
        for (int h = 0; h < 2; ++h)
          for (int c = 0; c < kControls; ++c)
            if (raw.hands[h].pressed & (1u << c)) {
              controlInput.consume(h, static_cast<Control>(c));
              introPressed = true;
            }
      // While Settings' Controls waits for a new gesture's buttons, a press is
      // only for that; the trigger alone still chooses on the menu (Cancel),
      // and does nothing off it. Closing the menu stops waiting.
      if (!panel.open) menu.stopRecordingGesture();
      if (panel.open && menu.recordingGesture())
        for (int h = 0; h < 2; ++h)
          for (int c = 0; c < kControls; ++c) {
            const uint32_t bit = 1u << c;
            const HandState& g = raw.hands[h];
            if (!(g.pressed & bit)) continue;
            float u, v, d;
            if (static_cast<Control>(c) == Control::Trigger && !(g.controls & ~bit) && g.poseValid &&
                menuHitTest(panel, menu, g.aimPosition, aimDirection(g), u, v, d) >= 0)
              continue;
            controlInput.consume(h, static_cast<Control>(c));
          }
      // While setup shows what each button does, a press shows at once.
      const JobInput& jobs = controlInput.update(raw, controllerTypes, settings.controls,
                                                 menu.setupShowsControls() ? nullptr : &settings.gestures, now);
      const ControllerState& input = jobs.state;
      {
        // As held, also those consumed: recording a gesture consumes them.
        const uint32_t held[2] = {raw.hands[0].controls, raw.hands[1].controls};
        menu.setControlsHeld(held, jobs.lost);
      }
      const bool menuPressed = jobs.wasPressed(Job::Menu);
      for (int h = 0; h < 2; ++h)
        if (input.hands[h].selectPressed && activeHand != h) {
          activeHand = h;
          laserLength = kLaserStartLength;
        }

      bool openedNow = false;
      bool menuBroughtBack = false;  // by the menu button (the hints)
      bool stickScrolled = false, recentered = false, volumeTwisted = false;
      if (menu.setupRunning()) {
        // Its panel takes B, Tab and recenter (setup_placement.h).
        SetupPlacement::Frame s;
        s.now = now;
        s.headValid = headValid;
        s.headPos = headPos;
        s.headRot = headRot;
        s.presence = xr.userPresence();
        s.bring = menuPressed || jobs.wasPressed(Job::Recenter) || keys.toggleMenu || keys.recenter;
        s.pressed = s.bring;
        for (const uint8_t hands : jobs.pressed) s.pressed |= hands != 0;
        for (const HandState& h : input.hands) {
          s.pressed |= h.selectPressed || h.gripPressed;
          if (h.poseValid) controllerSeen = now;
        }
        // Welcome's intro plays from when the panel is placed; a press skips
        // it, and does nothing else (consumed above).
        const bool skipped = setupIntro.playing(now) && (s.pressed || introPressed);
        if (skipped) {
          setupIntro.skip(now);
          s.pressed = s.bring = false;
        }
        const bool waited = setupPlacement.waiting();
        setupPlacement.update(s, panel);
        openedNow = (waited && !setupPlacement.waiting()) || skipped;  // that press doesn't click
        if (!setupPlacement.waiting() && menu.setupIntroDue()) {
          setupIntro.start(now);
          menu.startSetupIntro();
        }
        if (setupIntro.started() && !setupIntro.playing(now) && !setupIntroDropped) {
          menu.dropSetupIntro();
          setupIntroDropped = true;
        }
        keys.recenter = false;
        menu.setSetupController(now - controllerSeen < 1.0);
        // Your controllers: the bindings when they changed, and what is held.
        if (controlsVersion != bindingsSeen) {
          bindingsSeen = controlsVersion;
          menu.setSetupControls(shownBindings);
        }
        uint32_t held = 0;
        for (int h = 0; h < 2; ++h) {
          const HandState& g = input.hands[h];
          const bool sideways = std::abs(g.stickX) > 0.5f && std::abs(g.stickX) >= std::abs(g.stickY);
          const bool upDown = std::abs(g.stickY) > 0.5f && std::abs(g.stickY) > std::abs(g.stickX);
          const bool on[] = {g.select, g.grip, jobs.isHeld(Job::Menu, h),
                             jobs.isHeld(Job::PlayPause, h), jobs.isHeld(Job::Recenter, h),
                             sideways, upDown};
          for (int c = 0; c <= kStickUpDown; ++c)
            if (on[c]) held |= heldBit(h, c);
        }
        menu.setSetupHeld(held);
        if (!kStandaloneHeadset) {
          int w = 0, h = 0;
          mirror.setCover(setupPlacement.waiting() ? menu.setupCue(Menu::SetupCue::Mirror, w, h)
                                                   : nullptr);
        }
      } else if (startMenu != StartMenu::None && headValid) {
        panel.open = true;
        panel.placeInFrontOf(headPos, menuAt);
        if (startMenu == StartMenu::Browser) menu.showFileBrowser();
        else if (startMenu == StartMenu::Settings) menu.showSettings();
        else menu.showMainPage();
        startMenu = StartMenu::None;
        showBrowserAtStart = false;
      } else if (showBrowserAtStart && headValid) {
        showBrowserAtStart = false;
        panel.open = true;
        panel.placeInFrontOf(headPos, menuAt);
        menu.showFileBrowser();
      } else if (menuAtEnd && headValid) {
        menuAtEnd = false;
        if (!panel.open) panel.placeInFrontOf(headPos, menuAt);
        panel.open = true;
        menu.showPlayedFrom();
      } else if ((menuPressed || keys.toggleMenu) && !app.keyPickFor.empty()) {
        endKeyPick();  // the menu button cancels picking the key's colour, the menu stays
      } else if (menuPressed || keys.toggleMenu) {
        panel.open = !panel.open;
        openedNow = panel.open;
        menuBroughtBack = panel.open && menuPressed;
        if (panel.open) {
          // Always open on the player page; sub pages (search, files) keep their state.
          menu.showMainPage();
          if (headValid) panel.placeInFrontOf(headPos, menuAt);
        }
      }
      keys.toggleMenu = false;
      buttonJobs.keepSeeThrough();
      // The buttons' other jobs, as the menu's or the strip's controls do
      // them. Menu and Recenter are above and below, Choose is the trigger's.
      // Not while setup's Your controllers shows what each does.
      const bool practising = menu.setupShowsControls();
      for (int j = 0; j < kJobs && !practising; ++j) {
        const Job job = static_cast<Job>(j);
        const int hand = jobs.pressedBy(job);
        if (hand < 0 || job == Job::Recenter || job == Job::Choose) continue;
        buzz(hand, 0.2f, 0.014);
        const JobInfo& info = jobInfo(job);
        const JobStrings& said = strings().jobs;
        const bool open = !running.currentFile.empty();
        if ((info.needs == JobNeeds::File || info.needs == JobNeeds::Video) && !open) {
          say(said.nothingOpen);
          continue;
        }
        if (info.needs == JobNeeds::Video && photo) {
          say(said.notForPhotos);
          continue;
        }
        ButtonJobs::Context context;
        context.now = now;
        context.photo = photo;
        context.slideshow = app.slideshowSeconds > 0;
        context.passthroughAvailable = passthroughAvailable;
        context.alphaBlend = xr.alphaBlendSupported();
        context.roomReadable = passthrough.roomReadable;
        context.roomShown = passthrough.roomShown();
        context.roomMatched = passthrough.matched();
        context.afterVideo = afterVideo();
        JobAction action = buttonJobs.press(job, hand, context);
        MenuCommand& c = action.command;
        // These need the stick's seek or the menu: done here.
        switch (job) {
          case Job::FrameBack:
          case Job::FrameForward:
            // Not while the stick seeks (its release may play on).
            if (!stickSeek.held() && player.frameStep(job == Job::FrameBack))
              awaitingResume = false;  // a seek of the user's
            break;
          case Job::RatingUp:
          case Job::RatingDown:
            if (isStream(running.currentFile)) {
              say(said.notForStreams);
              break;
            }
            if (!library.available()) {
              say(said.ratingsLocked);
              break;
            }
            menu.stepRating(running.currentFile, job == Job::RatingUp ? 1 : -1);
            if (const float rating = library.record(running.currentFile).rating; rating > 0) {
              char stars[16];
              std::snprintf(stars, sizeof(stars), "%g", rating);
              sayf(said.rating, stars);
            } else {
              say(said.notRated);
            }
            break;
          case Job::DeleteVideo:
            // The file the strip shows (not one opened this frame); the
            // dialog is the menu's, its page shown first (showing a page
            // closes dialogs). Setup shows only its own panel; picking the
            // key's colour hides the dialog's, so it ends.
            if (menu.setupRunning()) {
              say(said.notInSetup);
              break;
            }
            if (!menu.canRequestDelete(running.currentFile)) {
              using D = MenuView::Deletable;
              const D d = menu.deletable();
              if (isStream(running.currentFile)) say(said.notForStreams);
              else if (d == D::NeedsSendToStash) say(said.deleteNeedsStash);
              else if (d == D::StashDown) say(strings().deletion.tipStashDown);
              else if (d == D::No) say(said.serverKeeps);
              break;
            }
            if (!panel.open) {
              panel.open = true;
              menu.showMainPage();
              if (headValid) panel.placeInFrontOf(headPos, menuAt);
            }
            if (!app.keyPickFor.empty()) endKeyPick();
            c = menu.requestDelete();
            break;
          default: break;
        }
        if (!action.said.empty()) say(std::move(action.said));
        if (action.showOpacity) opacityShownUntil = now + kValueShown;
        if (action.showRing != GestureValue::Count) gestureInput.show(action.showRing, now);
        if (c.type != MenuCommand::None) execute(c);
        // The switches whose change doesn't show by itself, as they are now.
        if (std::string text = buttonJobs.saidAfter(job, afterVideo()); !text.empty()) say(std::move(text));
      }
      peeking = !practising && jobs.held[static_cast<int>(Job::PeekRoom)] != 0;
      if (buttonJobs.holdZoom(jobs, now)) gestureInput.show(GestureValue::Zoom, now);
      if (jobs.wasPressed(Job::Recenter) && !menu.setupRunning()) {
        keys.recenter = true;
        recentered = true;
        buzz(-1, 0.3f, 0.020);
      }

      const float dt = static_cast<float>(std::min(0.1, now - lastFrameTime));
      lastFrameTime = now;
      const Vec3 up = transformDir(frame, {0.0f, 1.0f, 0.0f});
      const bool listMode =
          panel.open && (menu.setupRunning() ? menu.setupScrolls() : menu.listOpen());
      bool onMenu[2] = {false, false};  // each hand's pointer is on a menu panel
      for (int h = 0; h < 2; ++h) {
        const HandState& g = input.hands[h];
        float u, v, d;
        onMenu[h] = g.poseValid && menuHitTest(panel, menu, g.aimPosition, aimDirection(g), u, v,
                                               d) >= 0;
      }

      // Grip on the menu: it follows the hand and keeps facing the viewer.
      if (menuGrab.hand >= 0 && !input.hands[menuGrab.hand].grip) menuGrab.hand = -1;
      for (int h = 0; h < 2 && menuGrab.hand < 0 && !screenControl.holding() && headValid; ++h) {
        const HandState& g = input.hands[h];
        if (!g.gripPressed || !onMenu[h]) continue;
        menuGrab.hand = h;
        menuGrab.startPoint = panel.front();
        menuGrab.startInverse = poseInverse(g.aimPosition, g.aimOrientation);
        buzz(h, 0.25f, 0.012);
      }
      if (menuGrab.hand >= 0 && input.hands[menuGrab.hand].poseValid && headValid) {
        const HandState& g = input.hands[menuGrab.hand];
        panel.moveFront(transformPoint(poseMatrix(g.aimPosition, g.aimOrientation) *
                                           menuGrab.startInverse,
                                       menuGrab.startPoint),
                        headPos, up);
      }

      // The gestures: buttons held and a twist of that wrist change a value.
      {
        GestureInput::Frame g;
        g.now = now;
        g.blocked = practising || (panel.open && menu.recordingGesture());
        g.lost = jobs.lost;
        for (int h = 0; h < 2; ++h) {
          g.types[h] = controllerTypes[h];
          g.onMenu[h] = onMenu[h] || menuGrab.hand == h;
        }
        gestureInput.update(input, settings.gestures, settings.controls, g, controlInput, gestureHost);
        if (gestureInput.changing() && gestureInput.gesture().value == GestureValue::Volume)
          volumeTwisted = true;
        stickSeek.followScrub(now);
      }

      // The trigger off the menu, a corner's handle and stick up/down: the flat screen.
      if (running.format.projection == Projection::Flat && videoImage && headValid) {
        ScreenControl::Frame screenFrame;
        screenFrame.up = up;
        screenFrame.aspect = eyeDisplayAspect(running.format, frameAspect);
        screenFrame.dt = dt;
        screenFrame.now = now;
        screenFrame.onMenu[0] = onMenu[0];
        screenFrame.onMenu[1] = onMenu[1];
        screenFrame.menuHeld = menuGrab.hand >= 0;
        screenFrame.sticksFree = !listMode;
        for (int h = 0; h < 2; ++h) screenFrame.triggerTaken[h] = gestureInput.claims(h, Control::Trigger);
        const bool wasHolding = screenControl.holding();
        if (screenControl.update(input, screenFrame, running.screen)) running.keepScreenSize();
        if (!wasHolding && screenControl.holding()) buzz(screenControl.hand(), 0.25f, 0.012);
      } else {
        screenControl.reset();
      }
      if (running.screenChanged) {
        running.screenChanged = false;
        running.keepScreenSize();
      }

      // Picking the key's colour: the trigger away from the menu reads it
      // where that hand's laser dot is seen, as the scene shows the left
      // eye's picture there (from that eye, through the zoom, into the
      // picture's axes, and onto its stereo correction's sphere, as
      // surround.frag). The dot is where the laser below draws it.
      if (!app.keyPickFor.empty() &&
          (app.keyPickFor != running.currentFile || !panel.open || !running.video.passthrough ||
           !running.video.greenScreen.on || running.format.projection == Projection::Flat ||
           (packedMask(running.currentFile, running.video) && maskFormat(running.format))))
        endKeyPick();
      const bool keyPicking = !app.keyPickFor.empty();
      // Not while the room is peeked at: the video isn't seen.
      for (int h = 0; h < 2 && keyPicking && !app.keyPickWanted && !keyPicker.busy() && headValid &&
                      !peeking;
           ++h) {
        const HandState& t = input.hands[h];
        if (h != activeHand || !t.selectPressed || !t.poseValid || onMenu[h]) continue;
        const EyeMatrices& eye = xr.eye(0);
        const Mat4 view = rotation(eye.orientation);
        const Vec3 dotAt = t.aimPosition + aimDirection(t) * laserLength;
        Vec3 d = transformDir(transposedRotation(view), normalize(dotAt - eye.position));
        if (d.z < 0) d = normalize(Vec3{d.x / running.screen.zoom, d.y / running.screen.zoom, d.z});
        Vec3 inPicture =
            transformDir(transposedRotation(running.screen.rotation()), transformDir(view, d));
        if (shownCorrection.on && shownCorrection.distance > 0) {
          const Vec3 viewer = shownCorrection.viewer - shownCorrection.camera;
          const float along = dot(viewer, inPicture);
          const float reach = along * along - dot(viewer, viewer) +
                              shownCorrection.distance * shownCorrection.distance;
          inPicture = normalize(viewer + inPicture * (std::sqrt(std::max(reach, 0.0f)) - along));
        }
        app.keyPickWanted = KeyPicker::pointOf(running.format, frameAspect, running.flipVideo, inPicture,
                                               app.keyPickUv, app.keyPickPart);
        break;
      }

      // Trigger away from the menu: drag the 180°/360° picture around you (the
      // flat screen's is ScreenControl's). Releasing stops it where it is
      // shown; what the smoothing had not caught up is dropped.
      const bool noDrag = screenControl.holding() || running.format.projection == Projection::Flat;
      if (turnDrag.hand >= 0 && (!input.hands[turnDrag.hand].select || noDrag ||
                                 menuGrab.hand == turnDrag.hand))
        turnDrag.hand = -1;
      for (int h = 0; h < 2 && turnDrag.hand < 0 && headValid && !noDrag; ++h) {
        const HandState& t = input.hands[h];
        if (!t.selectPressed || !t.poseValid || menuGrab.hand == h || onMenu[h] ||
            gestureInput.claims(h, Control::Trigger) || keyPicking)
          continue;
        turnDrag.hand = h;
        turnDrag.tracking = false;
        buzz(h, 0.25f, 0.012);
      }
      if (turnDrag.hand >= 0) {
        const HandState& t = input.hands[turnDrag.hand];
        const double time = xr.displayTime();
        if (!t.poseValid) {
          turnDrag.tracking = false;  // hold still until the hand is tracked again
        } else if (!turnDrag.tracking) {
          turnDrag.tracking = true;
          turnDrag.smoothHand = normalize(t.aimOrientation);
        } else {
          // The picture turns as the smoothed hand did this frame: sideways,
          // up/down, and twisting the wrist rolls it (to level a tilted horizon).
          const double step = std::clamp(time - turnDrag.lastTime, 0.0, kDragMaxStep);
          const float alpha = 1.0f - std::exp(-kDragResponse * static_cast<float>(step));
          const Quat next = slerpShortest(turnDrag.smoothHand, t.aimOrientation, alpha);
          running.screen.anchorRotation =
              rotation(next * conjugate(turnDrag.smoothHand)) * running.screen.rotation();
          running.screen.turn = 0.0f;
          turnDrag.smoothHand = next;
        }
        turnDrag.lastTime = time;
      }

      // Either stick sideways seeks; up/down scrolls lists, else moves the
      // viewpoint or flat screen. Only the dominant axis counts. While the
      // keyboard is up, sideways moves the text cursor instead.
      const bool typing = panel.open && menu.textInputActive();
      const StickDirections left = stickDirections(input.hands[0]);
      const StickDirections right = stickDirections(input.hands[1]);
      if (cursorLeft.update(typing && (left.left || right.left), now)) menu.moveCursor(-1);
      if (cursorRight.update(typing && (left.right || right.right), now)) menu.moveCursor(1);
      for (int i = 0; i < 2; ++i) forwardStick[i].update(input.hands[i], now);

      // A value row pointed (its field, − or +): either stick sideways steps
      // it, as − and + do, and holding repeats. Until both sticks are let go
      // the stick is the value's: it doesn't seek.
      {
        float x = 0.0f;
        bool centred = true;
        for (const HandState& h : input.hands) {
          if (std::abs(h.stickX) > std::abs(h.stickY) && std::abs(h.stickX) > std::abs(x)) x = h.stickX;
          centred = centred && std::abs(h.stickX) < adjust::kStickRelease &&
                    std::abs(h.stickY) < adjust::kStickRelease;
        }
        if (!panel.open || typing || centred) valueStick = false;
        if (!valueStick && panel.open && !typing && !stickSeek.active() &&
            std::abs(x) >= adjust::kStickPress && menu.pointedValue())
          valueStick = true;
        if (valueLess.update(valueStick && x <= -adjust::kStickPress, now)) execute(menu.stepValue(-1));
        if (valueMore.update(valueStick && x >= adjust::kStickPress, now)) execute(menu.stepValue(1));
      }

      {
        StickSeek::Frame seekFrame;
        seekFrame.now = now;
        seekFrame.seekable = !running.currentFile.empty() && !photo;
        seekFrame.sticksTaken = typing || valueStick;
        seekFrame.backKey = keys.seekBackKey;
        seekFrame.forwardKey = keys.seekForwardKey;
        seekFrame.skipSeconds = settings.skipSeconds;
        seekFrame.headValid = headValid;
        seekFrame.headPos = headPos;
        seekFrame.menuAt = menuAt;
        if (stickSeek.update(input, seekFrame)) awaitingResume = false;
      }
      if (turnLeft.update(keys.turnLeftKey, now)) running.screen.turn = wrapAngle(running.screen.turn - kTurnStep);
      if (turnRight.update(keys.turnRightKey, now)) running.screen.turn = wrapAngle(running.screen.turn + kTurnStep);
      if (listMode) {
        // Lists glide: the further the stick is tilted, the faster (up to
        // ~14 rows a second), with a small dead zone around the middle.
        // Kept pushed well over for more than a second, a long list speeds
        // up smoothly to 4 times that by 3 s. Easing off loses the speed-up
        // within half a second; letting go or reversing loses it at once.
        float tilt = 0.0f;
        for (int i = 0; i < 2; ++i) {
          const HandState& h = input.hands[i];
          if (valueStick || menuGrab.hand == i || screenControl.hand() == i ||
              std::abs(h.stickY) <= std::abs(h.stickX))
            continue;
          if (std::abs(h.stickY) > std::abs(tilt)) tilt = h.stickY;
        }
        const float amount = std::max(0.0f, (std::abs(tilt) - 0.15f) / 0.85f);
        const float sign = tilt > 0 ? -1.0f : 1.0f;
        if (amount == 0 || sign != listPushSign)
          listPushSeconds = 0;
        else if (amount > 0.7f)
          listPushSeconds = std::min(listPushSeconds + dt, 3.0);
        else
          listPushSeconds = std::max(0.0, listPushSeconds - 4.0 * dt);
        listPushSign = amount > 0 ? sign : 0.0f;
        const double ramp = std::clamp((listPushSeconds - 1.0) / 2.0, 0.0, 1.0);
        const double boost = 1.0 + 3.0 * ramp * ramp * (3.0 - 2.0 * ramp);
        if (amount > 0) {
          menu.scroll(sign * amount * amount * 14.0 * boost * dt);
          stickScrolled = true;
        }
      } else {
        listPushSeconds = 0;
        listPushSign = 0;
        if (running.format.projection != Projection::Flat && videoImage &&
            !input.hands[0].grip && !input.hands[1].grip) {
          int direction = 0;
          for (int i = 0; i < 2; ++i)
            if (!onMenu[i] && forwardStick[i].held(now))
              direction += input.hands[i].stickY > 0 ? 1 : -1;
          if (direction) {
            constexpr float kMax = VideoSettings::kMaxOriginForward;
            running.video.originForward =
                std::clamp(running.video.originForward + direction * 0.05f * dt, -kMax, kMax);
            running.storeVideoSoon(VideoSettings::kOrigin);
            viewpointShownUntil = now + kValueShown;
          }
        }
      }

      // Pointer ray against the menu panels; the nearer hit wins.
      const HandState& hand = input.hands[activeHand];
      Vec3 rayOrigin, rayDir, hitPoint;
      float hitU = 0, hitV = 0;
      int hitPanelId = -1;
      const bool pointing = panel.open && hand.poseValid && !setupPlacement.fading();
      if (pointing) {
        rayOrigin = hand.aimPosition;
        rayDir = aimDirection(hand);
        float distance = 0;
        hitPanelId = menuHitTest(panel, menu, rayOrigin, rayDir, hitU, hitV, distance);
        if (hitPanelId >= 0) {
          hitPoint = rayOrigin + rayDir * distance;
          laserLength = distance;
        }
      }
      const bool hitPanel = hitPanelId >= 0;

      // Stereo focus: the video's own settings, else the defaults; none while
      // it is off for all videos.
      const FocusSettings& focusSettings = running.video.hasFocus ? running.video.focus : settings.focus;
      FocusInput focusIn;
      focusIn.settings = focusSettings;
      focusIn.format = running.format;
      focusIn.video = running.currentFile;
      focusIn.shown = videoImage != nullptr;
      focusIn.texture = videoTexture;
      if (photoImage)
        focusIn.prepareRead = [&interop, photoImage] { interop.acquireForGl(*photoImage); return true; };
      else if (!photo)
        focusIn.prepareRead = [&player] { return player.prepareRead(); };
      if (!photo && !videoTexture) focusIn.drawer = directDepth.source(player.directCurrent());
      focusIn.flip = running.flipVideo;
      focusIn.position = photo ? 0.0 : player.playback().position;
      focusIn.paused = photo || player.playback().paused;
      focusIn.headValid = headValid && xr.eyeCount() == 2;
      if (focusIn.headValid) {
        focusIn.leftEye = xr.eye(0).position;
        focusIn.rightEye = xr.eye(1).position;
      }
      if (!focusIn.headValid || !settings.focusPrefs.enabled) focusIn.settings.mode = FocusMode::Off;
      if (!settings.focusPrefs.autoIpd) focusIn.viewerIpd = settings.focusPrefs.ipdTenthMm / 1e4f;
      if (headValid)
        focusIn.motion = emulatedMotion(settings.motionEmulation, settings.motionDistance, headPos,
                                        running.screen.anchorPosition);
      focusIn.head = headRot;
      focusIn.picture = running.screen.rotation();
      // Static placement and head motion share the correction's sphere limit.
      if (running.format.projection != Projection::Flat && running.video.originForward != 0)
        focusIn.motion = focusIn.motion +
                         transformDir(focusIn.picture, {0, 0, -running.video.originForward});
      focusIn.zoom = running.screen.zoom;
      // Only passthrough the eye images show: without alpha blending or
      // key green the video is drawn whole.
      focusIn.seeThrough = seeThrough(alphaPassthrough || keyPassthrough,
                                      packedMask(running.currentFile, running.video), running.format,
                                      running.video, !photo && player.tvGammaImage(), keyPicking);
      // A second layer not found yet (by the keyframe check, which may not
      // have run or answered): looked for while the video plays.
      const bool probeWanted =
          maskProbeReady && !photo && focusIn.seeThrough.packedAlpha &&
          !running.video.maskLayersFound && !player.playback().paused && !player.seeking() &&
          !player.loading();
      DepthPatchDrawer* probeDrawer =
          !photo && !videoTexture && maskProbeDirectReady && !keyPicker.busy()
              ? maskProbeDirect.source(player.directCurrent())
              : nullptr;
      if (maskProbe.update(now, running.currentFile, probeWanted, videoTexture,
                           [&player] { return player.prepareFullSizeRead(); }, probeDrawer)) {
        running.video.maskLayersVersion = VideoCheck::kLayersVersion;
        running.video.maskLayersFound = true;
        passthrough.layersFound();
        running.storeVideo(VideoSettings::kMaskLayers);
      }
      // The key's colour where it was pointed at: read once, then taken as
      // the video's background, and the menu comes back. A frame that stays
      // out of GL is read by Vulkan, if its format can be.
      if (app.keyPickWanted && !maskProbe.drawing()) {
        DirectFrame* direct = !photo && !videoTexture ? player.directCurrent() : nullptr;
        DepthPatchDrawer* drawer =
            direct && maskProbeDirectReady ? maskProbeDirect.source(direct) : nullptr;
        const std::function<bool()> prepare =
            photo ? focusIn.prepareRead : [&player] { return player.prepareFullSizeRead(); };
        const KeyPicker::Read read =
            direct && !drawer
                ? KeyPicker::Read::Unavailable
                : keyPicker.read(app.keyPickUv, app.keyPickPart, videoTexture,
                                 !photo && player.tvGammaImage(), prepare, drawer);
        if (read != KeyPicker::Read::Later) app.keyPickWanted = false;
        if (read == KeyPicker::Read::Unavailable) {
          app.keyPickNote = KeyPickNote::Unreadable;
          std::printf("Background key: this picture can't be read\n");
        }
      }
      // A read that crossed an edge or failed: picking goes on, for another press.
      KeyColour picked;
      switch (keyPicker.result(picked)) {
        case KeyPicker::Result::Picked:
          running.video.greenScreen.source = KeySource::Picked;
          running.video.greenScreen.picked = picked;
          running.storeVideoSoon(VideoSettings::kGreenScreen);
          endKeyPick();
          std::printf("Background key: picked %d, %d, %d\n", picked.r, picked.g, picked.b);
          break;
        case KeyPicker::Result::Uneven:
          app.keyPickNote = KeyPickNote::Uneven;
          std::printf("Background key: not one colour there\n");
          break;
        case KeyPicker::Result::Failed:
          std::printf("Background key: the colour wasn't read back\n");
          break;
        case KeyPicker::Result::None: break;
      }
      focusIn.now = now;
      focusIn.target = focusTargets.update(
          settings.focusPrefs.eyeTracking, xr.gaze(), headRot, now,
          settings.focusPrefs.gazeCorrect ? gazeCorrection.fit() : GazeFit{});
      const FocusOutput focusOut = autofocus.update(focusIn);
      focusMarker.update(settings.focusPrefs.showTarget,
                         (focusIn.leftEye + focusIn.rightEye) * 0.5f, headRot, focusOut.marker,
                         focusOut.markerDistance, focusOut.distanceText);

      if (kStandaloneHeadset) {
        using decoder_holders::Busy;
        auto& r = decoderRescue;
        auto ready = [](const auto& f) {
          return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        };
        const bool refused = player.decoderRefused() && !running.currentFile.empty();
        const uint64_t opening = player.loadCount();
        if (refused && r.handled != opening && !r.holders.valid() && !r.freed.valid()) {
          r.handled = opening;
          r.path = running.currentFile;
          r.at = player.refusedAt();
          r.ask = false;
          // Refused again once freed: not Steam's doing, or not only.
          if (opening == r.retry) {
            r.busy = Busy::StillBusy;
          } else {
            r.busy = Busy::Checking;
            r.holders = std::async(std::launch::async, decoder_holders::find);
          }
        }
        const bool current = refused && r.handled == opening;
        if (ready(r.holders)) {
          bool steam = false, others = false;
          for (const decoder_holders::Holder& h : r.holders.get()) {
            std::printf("Decoder: also held by %s (%d)\n", h.name.c_str(), static_cast<int>(h.pid));
            (h.steam ? steam : others) = true;
          }
          r.busy = !current ? Busy::No : steam ? Busy::Steam : others ? Busy::StillBusy : Busy::No;
          // Freed without asking once the user chose Always free.
          if (r.busy == Busy::Steam && settings.freeDecoder) {
            r.busy = Busy::Freeing;
            r.freed = std::async(std::launch::async, decoder_holders::freeFromSteam);
          }
          r.ask = r.busy == Busy::Steam;
        }
        // Not over another question, nor first-run setup. A dialog left in a
        // closed menu is gone once it opens, as with the menu button.
        if (r.ask && current && !(panel.open && menu.dialogOpen()) && !menu.setupRunning()) {
          r.ask = false;
          r.openedMenu = !panel.open;
          if (!panel.open) {
            panel.open = true;
            menu.showMainPage();
            if (headValid) panel.placeInFrontOf(headPos, menuAt);
          }
          menu.showDecoderBusy();
        }
        if (ready(r.freed)) {
          r.freed.get();
          r.busy = Busy::No;
          if (current && running.currentFile == r.path) {
            r.retry = opening + 1;  // load() counts one more
            player.load(r.path, r.at);
          }
        }
        // Another video, or none: the question is gone with it.
        if (r.busy == Busy::Steam && !current) {
          r.ask = false;
          menu.closeDecoderBusy();
          r.busy = Busy::No;
        }
      }
      if (panel.open && !loading) releaseUpdate.menuOpened();
      releaseUpdate.poll();
      // Never over playback: a newer release is offered as the menu opens,
      // and an installed one restarts the app while the menu is open.
      if (panel.open && !menuWasOpen && !updateOffered && !menu.setupRunning() &&
          !menu.dialogOpen() && releaseUpdate.state() == ReleaseUpdate::State::Available) {
        updateOffered = true;
        menu.showUpdate();
      }
      // A development build's feel.conf is read again as the menu opens; as
      // it closes, nothing stays pointed.
      if (panel.open && !menuWasOpen) {
        reloadMenuFeel(configDir());
        dotTight.set(0);
      }
      if (!panel.open && menuWasOpen) menu.resetHover();
      menuWasOpen = panel.open;
      // What's new, the first time the menu shows: not during setup, nor
      // over another dialog (the update's comes first). Closed, however that
      // happens, this version counts as seen.
      if (!whatsNew.releases.empty() && panel.open && !menu.setupRunning() && !menu.dialogOpen()) {
        menu.showWhatsNew(std::move(whatsNew.releases), settings.versionSeen, VIDEOFRAMEVR_VERSION);
        whatsNew.releases.clear();
      }
      const bool whatsNewOpen = menu.whatsNewOpen();
      if (whatsNewWasOpen && !whatsNewOpen && markVersionSeen(settings.versionSeen))
        saveSettings(settings);
      whatsNewWasOpen = whatsNewOpen;
      if (panel.open && releaseUpdate.state() == ReleaseUpdate::State::Restarting && !keys.quit) {
        std::printf("Update: restarting into %s\n", releaseUpdate.version().c_str());
        menu.showUpdate();
        releaseUpdate.restartAtExit();
        keys.quit = true;
      }
      if (std::string name = serverNameNow(menu.setupRunning()); name != serverNamed) {
        serverNamed = std::move(name);
        setServerName(serverNamed);
        menu.languageChanged();
      }
      MenuView view;
      view.releaseUpdate = releaseUpdate.state();
      view.releaseVersion = releaseUpdate.version();
      diagnostics.poll();
      view.diagnostics = diagnostics.state();
      view.diagnosticsFile = diagnostics.fileName();
      view.crashedLastRun =
          previousRunCrashed() && diagnostics.state() != DiagnosticsExport::State::Saved;
      view.playback = player.playback();
      view.hardwareAv1 = player.hardwareAv1();
      stickSeek.fill(view.playback);
      // Software decoding the strip warns of, once it has lasted a second
      // (not a decoder restarting after a seek).
      if (photo || loading || !videoImage || player.counts().decoder != "no")
        softwareSince = -1;
      else if (softwareSince < 0)
        softwareSince = now;
      view.softwareDecoding = softwareSince >= 0 && now - softwareSince >= 1.0;
      view.scripts = settings.scripts;
      view.scriptStatus = scripts.status();
      view.format = running.format;
      view.halfPacked = isHalfPacked(running.format);
      view.screen = ScreenView::of(running.screen.size);
      view.currentFile = running.currentFile;
      view.playbackFailed = player.failed();
      view.discEncrypted = player.failed() && bdmv::isEncrypted(running.currentFile);
      view.decoderBusy = player.decoderRefused() && decoderRescue.path == running.currentFile &&
                         (decoderRescue.busy == decoder_holders::Busy::Steam ||
                          decoderRescue.busy == decoder_holders::Busy::StillBusy);
      view.accent = settings.accent;
      view.floatingSubtitles = settings.floatingSubtitles;
      view.streaming = isStream(running.currentFile);
      view.videoWidth = photo ? photoWidth : player.width();
      view.videoHeight = photo ? photoHeight : player.height();
      if (photo) view.playback.title = std::filesystem::path(running.currentFile).filename().string();
      view.background = settings.background;
      view.autoPassthrough = settings.autoPassthrough;
      view.keyGreen = settings.keyGreen;
      view.passthroughAvailable = passthroughAvailable;
      view.passthrough = alphaPassthrough || keyPassthrough;
      view.greenScreen = running.video.greenScreen;
      view.colourKey = running.video.colourKeyed();
      view.keyColour = running.video.keyColour();
      view.keyPicking = !app.keyPickFor.empty();
      view.keyPickNote = app.keyPickNote;
      view.fillChecked = running.video.fillCheckVersion > 0;
      view.greenScreenFound = running.video.greenCheckVersion > 0 && running.video.greenScreenFound;
      view.packedAlpha = packedMask(running.currentFile, running.video);
      view.maskFormat = maskFormat(running.format);
      view.maskLayers = running.video.maskLayersFound;
      view.secondLayerHidden = running.video.secondLayerHidden;
      view.opacity = running.video.opacity;
      view.opacityApplies = canBlend;
      view.subtitleSize = settings.subtitleSize;
      view.subtitleRaise = settings.subtitleRaise;
      view.subtitleDistance = settings.subtitleDistance;
      view.photo = !running.currentFile.empty() && isPhotoFile(running.currentFile);
      // A scene from Stash's stream only through Stash (docs/alpha/delete-file.md).
      // A scene played from its server: only through a server that deletes.
      view.deletable = running.currentFile.empty() || view.streaming ? MenuView::Deletable::No
                       : !isStashScene(running.currentFile)         ? MenuView::Deletable::Yes
                       : !mediaServer(settings.stash.kind).abilities().deleteFiles
                           ? MenuView::Deletable::No
                       : !settings.stash.writeBack              ? MenuView::Deletable::NeedsSendToStash
                       : offline.stashDown()                    ? MenuView::Deletable::StashDown
                                                                : MenuView::Deletable::Yes;
      view.slideshowSeconds = app.slideshowSeconds;
      view.afterVideo = afterVideo();
      view.brightness = running.video.brightness;
      view.contrast = running.video.contrast;
      view.saturation = running.video.saturation;
      passthrough.fill(view, blended, roomMatching, now);
      view.matchColour = settings.matchColour;
      view.matchBrightness = settings.matchBrightness;
      view.tvGamma = settings.tvGamma;
      view.zoom = running.video.zoom;
      view.originForward = static_cast<int>(std::lround(running.video.originForward * 100));
      view.videoMipmaps = settings.videoMipmaps;
      view.antiShimmer = antiShimmer;
      view.sharpen = settings.sharpen;
      view.toneLook = settings.toneLook;
      view.videoResolution = pendingResolution > 0 ? pendingResolution : chosenResolution();
      view.resolutionStops = resolutionStops.empty() ? nullptr : &resolutionStops;
      view.videoResolutionActive = static_cast<int>(std::lround(askedScale() * 100));
      view.videoResolutionEffective = static_cast<int>(std::lround(xr.requestedEyeScale() * 100));
      view.resolutionOverride =
          overrideScale > 0 ? static_cast<int>(std::floor(resolutionBase * overrideScale)) : 0;
      view.resolutionBase = resolutionBase;
      view.foveationAvailable = foveation.active();
      view.foveation = foveation.level();
      view.showFoveation = showFoveation;
      view.foveationLearned = settings.foveationLearned;
      view.sharpBins = sharpBins;
      view.foveationRing = foveationRing;
      view.foveationOutside = foveationOutside;
      view.frameReuse = frameReuse.enabled();
      view.advancedSettings = settings.advancedSettings;
      view.refreshRate = refreshRateAvailable ? settings.refreshRate : 0;
      view.performancePreset = settings.performancePreset;
      if (userPresetsListing.valid() &&
          userPresetsListing.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        std::vector<UserPreset> listed = userPresetsListing.get();
        // The menu redraws for a listing only if something in it changed.
        if (!userPresetsListed || !sameUserPresets(listed, userPresets)) {
          userPresets = std::move(listed);
          ++userPresetsListed;
        }
        userPresetShown = presets.shownUserPreset(userPresets, settings, pickedUserPreset);
      }
      view.userPresets = &userPresets;
      view.userPresetsListed = userPresetsListed;
      view.userPresetShown = userPresetShown;
      view.presets = &presets;
      view.foveationMaps = foveation.equivalentMaps();
      view.sharpDegrees = foveation.binDegrees();
      view.resolutionNotice = settings.resolutionNotice;
      view.focus.settings = focusSettings;
      view.focus.preferences = settings.focusPrefs;
      view.focus.own = running.video.hasFocus;
      view.focus.supported = stereoCorrectionSupported(running.format) && xr.eyeCount() == 2;
      view.focus.unavailable = !autofocus.available();
      view.focus.targetNotice = targetNotice(focusTargets.status());
      view.motionEmulation = settings.motionEmulation;
      view.motionDistance = settings.motionDistance;
      view.resume = settings.resume;
      view.freeDecoder = settings.freeDecoder;
      view.downloadsTo = settings.downloadsTo;
      view.seekPreview = settings.seekPreview;
      view.seekPreviewAvailable = !kStandaloneHeadset;
      view.thumbnails = settings.thumbnails;
      view.showPerformance = showPerformance;
      view.vibration = settings.vibration;
      view.controls.type = shownControllerType();
      view.controls.map = settings.controls;
      view.controls.gestures = settings.gestures;
      view.controls.skipSeconds = settings.skipSeconds;
      view.controls.bigSkipSeconds = settings.bigSkipSeconds;
      view.showWhatsNew = settings.showWhatsNew;
      view.stash = settings.stash;
      for (const MediaServer* server : mediaServers())
        if (serverSetUp(settings.stash, settings.otherServers, server->kind()))
          view.serversSetUp |= 1u << static_cast<int>(server->kind());
      view.setupBanner = setupBanner;
      view.appFps = appFps;
      view.focus.headsetIpdTenthMm =
          focusIn.headValid
              ? 5 * static_cast<int>(std::lround(2000.0f * length(focusIn.rightEye - focusIn.leftEye)))
              : 0;
      menu.update(view);
      execute(menu.takeCommand());
      if (opts.startMenu == StartMenu::Seek && panel.open && !hitPanel) {
        // Back and forth along the seek bar every 20 seconds, as a hand looking for a scene would.
        const double fraction = 0.5 - 0.4 * std::cos(now * 2 * kPi / 20);
        menu.seekBarPoint(fraction, hitU, hitV);
        hitPanelId = Menu::kMain;
      }
      menu.setPointer(hitPanelId, hitU, hitV);
      keys.textInput = panel.open && menu.textInputActive();
      menu.typeText(keys.typed);
      for (; keys.backspaces > 0; --keys.backspaces) menu.backspace();
      if (keys.cursorSteps) menu.moveCursor(std::exchange(keys.cursorSteps, 0));
      if (keys.submit) execute(menu.submitText());
      if (keys.stopTyping) menu.stopEditing();
      keys.typed.clear();
      keys.submit = false;
      keys.stopTyping = false;
      // Trigger on the panel clicks it (anywhere else it drags the picture).
      const bool clicked = panel.open && hand.selectPressed && !openedNow && hitPanel;
      {
        // The eye tracker's error, learned from where the eyes are at a
        // click; before the click, which may change the page.
        GazeCorrection::Input gaze;
        gaze.now = now;
        gaze.gazeGood = focusTargets.trackerGazeGood();
        gaze.gaze = focusTargets.trackerGaze();
        gaze.pointing = hitPanel && headValid;
        gaze.held = hand.select;
        gaze.dragged = menu.dragging();
        if (gaze.pointing) {
          const Mat4 toHead = rotation(conjugate(headRot));
          gaze.dot = normalize(transformDir(toHead, hitPoint - headPos));
          gaze.pressed = clicked && menu.pointedButton();
        }
        if (input.focused && xr.userPresence() != 0)
          gazeSitting = true;
        else
          endGazeSitting();
        gazeCorrection.update(gaze);  // learn also while the correction is not applied
      }
      if (clicked) execute(menu.click());
      // Sliders and lists follow the pointer while the trigger stays down; a
      // list's row is clicked on letting go, if the list was not dragged.
      if (!input.focused) menu.cancelDrag();
      execute(menu.drag(panel.open && hand.select));
      // Felt as on Steam's dashboard (SteamVR 1791331621 on the Frame), each a
      // single impulse: with the menu open, every pull of a trigger, on a
      // button or not; then what the menu felt on the pointing hand.
      if (panel.open)
        for (int h = 0; h < 2; ++h)
          if (input.hands[h].selectPressed) buzz(h, 0.2f, 0, 20);
      switch (menu.takeFeel()) {
        case Menu::Feel::Tick: buzz(activeHand, 0.08f, 0, 1); break;
        case Menu::Feel::Enter: buzz(activeHand, 0.16f, 0, 1); break;
        case Menu::Feel::Detent: buzz(activeHand, 0.2f, 0, 1); break;
        case Menu::Feel::Edge: buzz(activeHand, 1.0f, 0, 1); break;
        case Menu::Feel::None: break;
      }
      const bool resolutionWasHeld = resolutionHeld;
      resolutionHeld = menu.draggingSlider() == MenuCommand::SetVideoResolution;
      if (pendingResolution > 0 && ((resolutionWasHeld && !resolutionHeld) || glfwGetTime() >= pendingResolutionAt))
        applyPendingResolution();
      // A command may have closed or replaced the picture chosen above (Stop
      // frees the video's images, a new file is loading): choose it again.
      choosePicture();
      sendBuzzes();

      // The hints after setup: their situations, and the controls they name
      // used; a hint's line drawn once as it starts.
      if (hints.any()) {
        if (!menu.setupRunning() && controlsVersion != hintBindingsSeen) {
          hintBindingsSeen = controlsVersion;
          menu.setHintBindings(shownBindings);
        }
        Hints::Frame f;
        f.now = now;
        f.allowed = !menu.setupRunning() && input.focused;
        f.menuOpen = panel.open;
        f.menuBroughtBack = menuBroughtBack;
        f.longListPointed = panel.open && menu.pointingAtLongList(f.listPage);
        // Only where a gesture sets the volume (the hint names its buttons).
        f.volumePointed = panel.open &&
                          std::any_of(shownBindings.twists.begin(), shownBindings.twists.end(),
                                      [](const ControllerBindings::Twist& twist) {
                                        return twist.value == static_cast<int>(GestureValue::Volume);
                                      }) &&
                          menu.pointingAtVolume();
        if (videoImage && !photo && !player.playback().paused && !panel.open && headValid) {
          const Vec3 ahead = transformDir(rotation(headRot), {0.0f, 0.0f, -1.0f});
          const Vec3 centre = transformDir(running.screen.rotation(), {0.0f, 0.0f, -1.0f});
          // Only where something recenters (a hint names it).
          const int recenter = static_cast<int>(ControlAction::Recenter);
          f.videoAway = dot(ahead, centre) < std::cos(70.0f * kPi / 180) &&
                        (!shownBindings.sources[recenter].empty() ||
                         !shownBindings.gripSources[recenter].empty());
        }
        f.scrolled = stickScrolled;
        f.recentered = recentered;
        f.volumeTwisted = volumeTwisted;
        const Hint shown = hints.update(f);
        if (shown != Hint::Count) hintTexture = menu.drawHint(shown, hintW, hintH);
        if (hints.takeChanged()) {
          settings.hints = hints.line();
          saveSettings(settings);
        }
      }

      // --- Drawing ---
      // About when this frame is shown: what eases is shown as it is then
      // (menu_feel.h, Ease).
      const double easedAt = now + std::max(0.0, xr.displayPeriod());
      // The strip and the browser shown now; a held seek's strip with the
      // menu closed, in place of the menu's (which goes at once).
      const bool panelOn[2] = {panel.open && menu.panelShown(Menu::kMain),
                               panel.open && menu.panelShown(Menu::kBrowser)};
      const bool heldStrip = !panel.open && stickSeek.stripShown(now);
      if (heldStrip) {
        menuFade.hide();
        for (PanelFade& f : panelFades) {
          f.fade.hide();
          f.shown = false;
        }
      }
      // The seek bar preview: a frame of the video where the pointer is.
      double hoverFraction = 0;
      if (panel.open && !loading && !running.currentFile.empty() && menu.seekHover(hoverFraction)) {
        SeekPreview::Playing playing;
        playing.location = running.currentFile;
        playing.duration = player.playback().duration;
        playing.format = running.format;
        playing.views = player.playback().views;
        playing.flip = running.flipVideo;
        seekPreview.show(hoverFraction, playing, panel.panel(menu, Menu::kMain));
      } else if (panelOn[Menu::kMain] || !panelFades[Menu::kMain].fade.drawn()) {  // it fades with the strip
        seekPreview.hide();
      }
      scene.clear = blended || peek ? ClearMode::Transparent
                    : keyPassthrough ? ClearMode::KeyGreen
                                     : ClearMode::Dark;
      // The night sky around the flat screen (with passthrough: the room).
      scene.sky = running.format.projection == Projection::Flat && settings.background &&
                  scene.clear == ClearMode::Dark;
      VideoDraw& video = scene.video.emplace();
      video.texture = videoImage;
      video.format = running.format;
      video.frameAspect = frameAspect;
      video.flip = running.flipVideo;
      video.screen = running.screen;
      video.photo = photo;
      video.seeThrough = focusIn.seeThrough;
      video.opacity = peek ? 0 : running.video.opacity;
      const std::array<float, 3> gains = passthrough.balance(roomMatching).gains();
      for (int i = 0; i < 3; ++i) video.balance[i] = gains[i] - 1.0f;
      // Brightness etc. are applied while drawing to photos, and to videos
      // while a mask or key reads the frame; else mpv (or YuvColor) does it.
      const bool readsFrame = video.readsFrame(scene.clear);
      player.setPlainPicture(readsFrame);
      if (photo || readsFrame) {
        video.adjust[0] = running.video.brightness / 100.0f;
        video.adjust[1] = running.video.contrast / 100.0f;
        video.adjust[2] = running.video.saturation / 100.0f;
      }
      video.tvGamma = !photo && player.tvGammaImage();
      video.mipmaps = photo || settings.videoMipmaps;  // photos always have theirs
      video.antiShimmer = antiShimmer;
      video.look = settings.toneLook;
      video.sharpen = settings.sharpen / 100.0f;
      video.key = running.video.keySliders();
      video.correction = focusOut.correction;
      shownCorrection = focusOut.correction.eyes[0];
      if (videoImage && xr.eyeCount() > 0) {
        const ScreenSurface screen(running.screen, eyeDisplayAspect(running.format, frameAspect));
        samplingLog.update(describeEyeSampling(running.format, photo ? photoWidth : player.width(),
                                               photo ? photoHeight : player.height(),
                                               running.screen.zoom,
                                               screen.width() / screen.distance(),
                                               xr.eyePixelsPerDegree(0, false),
                                               photo || settings.videoMipmaps),
                           now);
      }
      if (menu.setupRunning() && !videoImage) scene.video.reset();  // setup floats in the sky

      // Over the video, in drawing order: the screen's corner handles, the
      // subtitles, loading text, the performance readout, the menu's panels
      // (browser, then strip), the seek preview, the pointer, the focus markers
      // and the messages in front of the eyes (Notices).
      std::vector<OverlayDraw>& overlays = scene.overlays;
      const std::vector<Quad>& handles = screenControl.handle();
      if (!handles.empty()) overlays.push_back(QuadsItem{handles});
      auto addText = [&](SubtitleOverlay& text, const Mat4& model) {
        PanelItem item;
        item.texture = text.texture();
        item.model = model;
        overlays.push_back(item);
      };
      if (!subtitles.empty() && headValid && !loading)
        addText(subtitles, subtitlePlacement.update(running, player.aspect(), headPos,
                                                        yawFromQuat(headRot), settings));
      if ((stillLoading || (!photo && player.failed())) && headValid) {
        // Where the picture will be, the size of the messages in front of the eyes.
        using decoder_holders::Busy;
        const Busy busy = player.decoderRefused() && decoderRescue.path == running.currentFile
                              ? decoderRescue.busy
                              : Busy::No;
        // While the decoder's holders are found or freed, it isn't a failure yet.
        const bool failed = player.failed() && busy != Busy::Checking && busy != Busy::Freeing;
        loadingText.set(menu, failed ? NoticeIcon::Warning : NoticeIcon::None,
                        busy == Busy::Freeing                ? strings().decoderBusy.freeing
                        : !failed                            ? strings().loading
                        : bdmv::isEncrypted(running.currentFile) ? strings().discEncrypted
                        : busy != Busy::No                   ? strings().decoderBusy.busy
                                                             : strings().playbackFailed);
        constexpr float kAt = 2.0f, kScale = kAt / notice_place::kNear * notice_place::kScale;
        const Vec3 center = headPos + transformDir(frame, {0.0f, 0.0f, -kAt});
        overlays.push_back(loadingText.item(translation(center) * frame, kScale));
      }
      if (showPerformance) {
        if (readout.due(now)) {
          PerformanceReadout::State state;
          const double period = xr.displayPeriod();
          state.displayRate = period > 0 ? 1.0 / period : 0;
          state.video = !photo && !running.currentFile.empty();
          state.paused = player.playback().paused;
          state.waiting = loading || player.seeking();
          state.sourceFps = player.playback().fps;
          state.counts = &player.counts();
          state.codec = &player.playback().videoCodec;
          state.pixelFormat = &player.playback().pixelFormat;
          state.direct = player.directFrame();
          state.eyeWidth = xr.eyeWidth(0);
          state.foveation = foveation.active() ? static_cast<int>(foveation.level()) : -1;
          foveation.sharpDegrees(state.sharpWidth, state.sharpHeight);
          readout.update(now, state);
        }
        if (headValid) overlays.push_back(readout.item(headPos, headRot));
      }
      // The dot at the laser's end: tighter while the trigger is held.
      auto dotLook = [&](LaserItem& laser) {
        const MenuFeel& feel = menuFeel();
        dotTight.to(hand.select ? 1.0f : 0.0f, feel.dotEase, now);
        const float tight = dotTight.update(easedAt);
        laser.dotRadius = kLaserDotRadius * (1 + (static_cast<float>(feel.dotPressed) - 1) * tight);
      };
      // The menu's panels: the strip and the browser each fade in as they
      // start being shown and out as they stop (menu_feel.h), whatever the
      // cause (the menu opening or closing, the browser coming or going in
      // it); the laser with the menu. Input is never held back: a panel
      // takes none from the frame it stops being shown, and is drawn as it
      // was last until it has faded, with nothing pointed. Setup's panel
      // fades as it is placed.
      const float menuOpacity = menuFade.update(panel.open, now, easedAt);
      if (panel.open || menuOpacity > 0 || panelFades[Menu::kMain].fade.drawn() ||
          panelFades[Menu::kBrowser].fade.drawn()) {
        // The textures first: a click may have changed the page and with it
        // what the panels show; then the pointed control eases on them.
        const Texture* textures[3] = {};
        if (panel.open) {
          for (const Menu::PanelId id : {Menu::kBrowser, Menu::kMain, Menu::kSetup})
            if (menu.panelShown(id)) textures[id] = menu.texture(id);
          menu.easeHover(now, easedAt);
        }
        float opacity[2] = {};
        for (const Menu::PanelId id : {Menu::kBrowser, Menu::kMain}) {
          PanelFade& f = panelFades[id];
          // The browser coming starts fading in once the page it opens on
          // can be drawn (it is drawn a band of rows a frame); a page
          // changed while it is shown doesn't fade.
          if (id == Menu::kBrowser && panelOn[id] && !f.shown) f.waiting = true;
          if (f.waiting) f.waiting = panelOn[id] && !menu.browserReady();
          f.shown = panelOn[id];
          opacity[id] = f.fade.update(panelOn[id] && !f.waiting, now, easedAt);
          if (panelOn[id]) {  // as it is now, kept for fading out
            f.count = 0;
            PanelItem& item = f.drawn[0];
            item.texture = textures[id];
            item.curved = panel.panel(menu, id);
            item.hover.reset();
            // A value held: the strip fades away (the browser in its shader).
            item.opacity = id == Menu::kMain ? static_cast<float>(1 - menu.heldFade()) : 1.0f;
            if (item.opacity <= 0) continue;
            PanelHover hover;
            if (menu.hoverLook(id, hover)) item.hover = hover;
            f.count = 1;
            // A list's faded edges, over it.
            for (int edge = 0; id == Menu::kBrowser && edge < 2; ++edge) {
              PanelItem& fade = f.drawn[f.count];
              CurvedPanel part;
              if (!item.texture || !menu.listFade(edge, *item.curved, part, fade.texture)) continue;
              fade.curved = part;
              ++f.count;
            }
          }
          for (int i = 0; i < f.count && opacity[id] > 0; ++i) {
            overlays.push_back(f.drawn[i]);
            PanelItem& item = std::get<PanelItem>(overlays.back());
            item.opacity *= opacity[id];
            // Fading out, nothing is pointed on it (but a held value's fade).
            if (!panelOn[id] && item.hover && !(item.hover->fade > 0)) item.hover.reset();
          }
        }
        if (panel.open && menu.panelShown(Menu::kSetup)) {
          PanelItem item;
          item.texture = textures[Menu::kSetup];
          item.curved = panel.panel(menu, Menu::kSetup);
          item.opacity = setupPlacement.opacity();
          if (setupIntro.playing(now)) {  // its layers under the page, which fades in last
            setupIntro.addUnder(overlays, menu, *item.curved, item.opacity, now);
            item.opacity *= setupIntro.pageOpacity(now);
          }
          PanelHover hover;
          if (menu.hoverLook(Menu::kSetup, hover)) item.hover = hover;
          overlays.push_back(item);
        }
        if (opacity[Menu::kMain] > 0) seekPreview.addTo(overlays, opacity[Menu::kMain]);  // with the strip
        if (pointing) {
          LaserItem laser;
          laser.from = rayOrigin;
          laser.to = hitPanel ? hitPoint : rayOrigin + rayDir * laserLength;
          dotLook(laser);
          laser.opacity = menuOpacity;
          overlays.push_back(laser);
        } else if (!panel.open && menuOpacity > 0 && hand.poseValid && !setupPlacement.fading()) {
          LaserItem laser;  // where the hand points now
          laser.from = hand.aimPosition;
          laser.to = laser.from + aimDirection(hand) * laserLength;
          dotLook(laser);
          laser.opacity = menuOpacity;
          overlays.push_back(laser);
        }
      } else if (heldStrip) {
        // A held seek with the menu closed: the strip alone, no pointer.
        PanelItem item;
        item.texture = menu.texture(Menu::kMain);
        item.curved = panel.panel(menu, Menu::kMain);
        overlays.push_back(item);
      }
      // Keep the focus target visible over the menus.
      focusMarker.addTo(overlays);
      // Messages in front of the eyes, last, over everything: the recenter
      // countdown, and the viewpoint as the stick moves it (or Recenter puts
      // it back), the opacity a button's job set, or a gesture's value as a
      // twist changes it, redrawn only as what they show changes, or what a
      // button's job said.
      if (app.recenterAt > 0 && headValid) {
        char text[64];
        std::snprintf(text, sizeof(text), strings().recenterIn,
                      static_cast<int>(std::ceil(app.recenterAt - now)));
        notices.show(menu, Notices::Countdown, NoticeIcon::Recenter, text);
      }
      // One at a time, the newest: a gesture's value shows on the flat
      // screen too, as does the screen's distance or height the stick sets.
      // The flat screen has no viewpoint, but See-through's opacity.
      const bool flat = running.format.projection == Projection::Flat;
      const double valueUntil =
          flat ? opacityShownUntil : std::max(viewpointShownUntil, opacityShownUntil);
      const bool screenNewer = screenControl.shown() != GestureValue::Count &&
                               screenControl.shownAt() > gestureInput.shownAt();
      const GestureValue gestureShown = screenNewer ? screenControl.shown() : gestureInput.shown();
      const double gestureUntil =
          gestureShown != GestureValue::Count
              ? (screenNewer ? screenControl.shownAt() : gestureInput.shownAt()) + kValueShown
              : 0;
      if (now < gestureUntil && gestureUntil >= std::max(valueUntil, jobSaid.until) && headValid) {
        // Its ring, filled to the value along its range.
        const GestureValueInfo& info = gestureValueInfo(gestureShown);
        const double value = gestureHost.read(gestureShown);
        double least = info.range.least, most = info.range.most;
        gestureHost.range(gestureShown, least, most);
        const PlaybackState& p = player.playback();
        if (info.format == GestureFormat::Clock) most = std::max(0.0, p.duration);
        const auto along = [&](double x) { return most > least ? std::clamp((x - least) / (most - least), 0.0, 1.0) : 0.0; };
        RingNotice ring;
        if (info.format == GestureFormat::Stars) {
          ring.kind = RingNotice::Kind::Stars;
          ring.halves = static_cast<int>(std::lround(value));
          ring.name = gestureValueLabel(gestureShown, strings());
        } else if (info.format == GestureFormat::Clock) {
          ring.kind = RingNotice::Kind::Position;
          ring.at = along(value);
          for (const Chapter& c : p.chapters) ring.ticks.push_back(along(c.time));
          ring.number = clockTime(value);
          ring.after = "/ " + clockTime(most);
        } else {
          ring.at = along(value);
          ring.centred = info.centred;
          ring.from = info.centred ? along(0) : 0;
          if (info.mark != kNoMark) ring.mark = along(info.mark);
          // Within float rounding of an end (the screen's 0.1 m is 10.0000001 cm).
          const double near = (most - least) * 1e-6;
          ring.limit = value <= least + near || value >= most - near;
          ring.name = gestureValueLabel(gestureShown, strings());
          ring.number = gestureValueText(gestureShown, value);
        }
        notices.show(menu, Notices::Value, ring);
      } else if (now < jobSaid.until && jobSaid.until > valueUntil && headValid) {
        notices.show(menu, Notices::Value, NoticeIcon::None, jobSaid.text);
      } else if (now < valueUntil && headValid) {
        char text[160];
        const bool opacity = flat || opacityShownUntil > viewpointShownUntil;
        if (opacity) {
          std::snprintf(text, sizeof(text), strings().opacityOverlay, running.video.opacity);
        } else {
          const int percent = static_cast<int>(std::lround(running.video.originForward * 100));
          std::snprintf(text, sizeof(text), strings().viewpoint.overlay,
                        viewpointText(percent).c_str());
        }
        notices.show(menu, Notices::Value, opacity ? NoticeIcon::None : NoticeIcon::Viewpoint, text);
      }
      if (const char* why = passthrough.notice(now); why && headValid)
        notices.show(menu, Notices::Passthrough, NoticeIcon::Passthrough,
                     std::string(why) + "\n" + strings().autoPassthrough.undo);
      if (now < driveShownUntil && headValid)
        notices.show(menu, Notices::Drive, driveIcon, driveText);
      if (headValid) {
        notices.addTo(overlays, headPos, headRot, hintTexture && hints.showing());
        hints.addTo(overlays, hintTexture, hintW, hintH, headPos, headRot, now);
      }
      if (menu.setupRunning() && headValid) setupPlacement.addCues(overlays, menu, headPos, headRot, now);
      if (menu.setupRunning() && headValid && (menu.setupIntroDue() || setupIntro.playing(now)))
        setupIntro.addCover(overlays, menu, headPos, headRot, now);
    }
    // Match the room reads the view: nothing drawn meanwhile, so it shows the
    // room alone.
    const bool roomBlank = passthrough.roomBlank();
    if (roomBlank) {
      scene.video.reset();
      scene.overlays.clear();
      scene.sky = false;
      scene.clear = ClearMode::Transparent;
    }
    // The GL work of this iteration is all issued: what it drew goes to Vulkan.
    interop.publish();
    timer.lap(FrameTimer::Draw);

    // One submission per drawn frame: a frame slot once the GPU is done with
    // its last frame, then the eye images, recorded, submitted and given back.
    bool drawn = false;
    bool shownAgain = false;  // the last drawn frame instead (frame_reuse.h)
    // --resolution-cycle: the next of its sizes, as the command line's size.
    if (!opts.resolutionCycle.empty() && glfwGetTime() >= resolutionCycleAt) {
      const size_t step = resolutionCycleStep++ % opts.resolutionCycle.size();
      overrideScale = pixelScale(opts.resolutionCycle[step]);
      xr.requestEyeScale(overrideScale);
      resolutionCycleAt += opts.resolutionCycleSeconds;
    }
    if (shouldRender) {
      // A new eye size or foveation level, between frames: nothing acquired
      // or being recorded (docs/vulkan-renderer.md, 5.5).
      const auto switchStart = std::chrono::steady_clock::now();
      const bool resized = xr.updateEyeSize();
      if (resized) foveation.resized();
      foveation.prepare();
      if (!eyeTargets.update()) break;
      if (resized)
        std::printf("Eye resolution: %dx%d in %dx%d images, switched in %.2f ms\n", xr.eyeWidth(0),
                    xr.eyeHeight(0), xr.eyeImageWidth(0), xr.eyeImageHeight(0),
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - switchStart)
                        .count());
      foveation.update(headValid && settings.foveationLearned ? gazeCorrection.fit() : GazeFit{},
                       headRot);
      // Not over the room while Match the room reads it.
      scene.showFoveation = !roomBlank && showFoveation && foveation.active();
      if (scene.showFoveation)
        for (int i = 0; i < xr.eyeCount(); ++i) scene.foveation[i] = foveation.debug(i);
      // What this frame's eye images depend on: if the last drawn frame's
      // would do as well, the runtime shows those again and nothing is drawn.
      FrameReuse::Frame shown;
      shown.scene = &scene;
      shown.picture = player.pictureVersion();
      shown.alphaBlend = blended || peek;
      shown.displayTime = xr.displayTime();
      shown.eyeCount = xr.eyeCount();
      shown.eyes = shown.eyeCount > 0 ? &xr.eye(0) : nullptr;
      shown.targets = xr.eyeTargetsVersion();
      shown.mapsKnown = !foveation.active() || xr.appDensityMaps();
      shown.maps = foveation.mapsVersion();
      for (int i = 0; i < 2; ++i) {
        shown.mapOffset[i] = foveation.offset(i);
        shown.sharp[i] = foveation.sharpSize(i);
      }
      const FrameReuse::Reason reason = roomBlank || opts.xrTest || !xr.canShowAgain()
                                            ? FrameReuse::Off : frameReuse.compare(shown);
      shownAgain = reason == FrameReuse::Same;
      ++framesBy[reason];
      GpuContext::FrameSlot* slot = shownAgain ? nullptr : gpu.beginFrame();
      timer.lap(FrameTimer::Fence);
      if (slot) {
        SceneRenderer::GpuTimes gpuTimes;
        if (renderer.readTimes(*slot, gpuTimes)) {
          timer.gpu(gpuTimes.upload, gpuTimes.keyMask, gpuTimes.eye[0], gpuTimes.eye[1], gpuTimes.mirror,
                    gpuTimes.frame);
          if (showPerformance) readout.gpuTime(gpuTimes.frame);
        }
        const EyeFrame* eyes = xr.acquireEyes();
        if (eyes) mirror.acquire(*slot);
        timer.lap(FrameTimer::Acquire);
        if (eyes) {
          std::vector<SemaphoreWait> waits;
          const bool whole = renderer.record(*slot, *eyes, &xr.eye(0), scene, glfwGetTime(), waits);
          timer.lap(FrameTimer::Record);
          std::vector<VkSemaphore> signals;
          mirror.addSignals(signals);
          const uint64_t value = gpu.submit(*slot, waits, signals);
          drawn = value != 0;
          mirror.submitted(drawn);
          if (drawn) {
            interop.frameSubmitted(value);
            direct.frameSubmitted(value);
            // A frame recorded without its picture is not one to show again.
            if (whole) frameReuse.drawn(shown);
            else frameReuse.forget();
            timer.drew();
          } else {
            interop.frameAbandoned();
            direct.frameAbandoned();
            renderer.frameAbandoned();
          }
          timer.lap(FrameTimer::Submit);
        } else {
          gpu.abandonFrame(*slot);
        }
      }
    }
    gpu.collect();
    if (drawn && roomBlank) passthrough.blankSubmitted();
    xr.setAlphaBlend(blended || peek || roomBlank);
    xr.releaseEyes();
    if (shownAgain) xr.endFrameShownAgain();
    else xr.endFrame(drawn);
    directDepth.submit();  // after the frame's work, which it never delays
    if (maskProbeDirectReady) maskProbeDirect.submit();
    timer.lap(FrameTimer::XrEnd);
    mirror.present();  // after the XR frame, which never waits for the desktop
    timer.lap(FrameTimer::Swap);
    if (gpu.lost() || xr.failed()) {
      std::fprintf(stderr, "Stopping: the %s failed\n", gpu.lost() ? "GPU" : "OpenXR session");
      break;
    }
    timer.memory(gpu.liveBytes(), gpu.retiredBytes());
    if (timer.finishFrame() && opts.timing) {
      std::printf("[render] eye=%dx%d images=%dx%d %s\n", xr.eyeWidth(0), xr.eyeHeight(0),
                  xr.eyeImageWidth(0), xr.eyeImageHeight(0), foveation.report().c_str());
      // Frames shown again (same), and those drawn by why they were.
      std::printf("[reuse] %s\n", reuseReport(framesReported).c_str());
      std::copy(std::begin(framesBy), std::end(framesBy), std::begin(framesReported));
    }
    player.logTiming(panel.open, seekPreview.busy(), shouldRender, xr.isFocused());
  }
  if (quitSignal) std::printf("Quitting (signal %d)\n", static_cast<int>(quitSignal));
  if (opts.timing) std::printf("Frame reuse: frames %s\n", reuseReport(nullptr).c_str());
  running.saveUnsaved();
  running.keepVolume();
  running.keepSharpen();
  keepResume();
  gazeCorrection.endSitting();
  if (gazeCorrection.takeChanged()) {
    settings.gazeLearned = gazeCorrection.saved();
    saveSettings(settings);
  }
  gpu.savePipelineCache();
  return gpu.lost() || xr.failed() ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  // Line by line, so a log of the output is complete even if the player is killed.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  // glibc raises its mmap threshold to as much as 32 MiB once a large block is
  // freed; 8K pictures (about 30 MB a plane, e.g. the green screen check's)
  // then come from heaps that keep the memory after each file.
  mallopt(M_MMAP_THRESHOLD, 1 << 20);
  Options opts;
  if (const std::optional<int> code = parseOptions(argc, argv, opts)) return *code;
  std::atexit(restartIfRequested);  // before the run log starts, so it runs after the log ends
  startRunLog();
  printRunHeader();
  if (opts.stashSync) {
    // The setup in use; with none on, the one that has a server.
    const Settings saved = loadSettings();
    StashSettings stash = saved.stash;
    for (const StashSettings& other : saved.otherServers)
      if (stash.url.empty() && !other.url.empty()) stash = other;
    setServerName(mediaServer(stash.kind).name());
    return runStashSyncDiagnostic(stash);
  }
  // The file browser starts next to the video, which needs an absolute path.
  if (!opts.file.empty() && opts.file.find("://") == std::string::npos) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(opts.file, ec);
    if (!ec) opts.file = absolute.string();
  }

  glfwSetErrorCallback([](int code, const char* desc) {
    std::fprintf(stderr, "GLFW error %d: %s\n", code, desc);
  });
  if (!glfwInit()) return 1;

  catchQuitSignals();
  const int rc = run(opts);
  disarmQuitWatchdog();
  Stash::finishStopping();  // a sync under way gives up within about a second

  glfwTerminate();
  return rc;
}
