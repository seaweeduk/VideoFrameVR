#include "app_dirs.h"
#include "autofocus.h"
#include "bluray.h"
#include "button_jobs.h"
#include "command_inbox.h"
#include "control_socket.h"
#include "deo_remote.h"
#include "direct_depth.h"
#include "direct_video.h"
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
#include "key_pick.h"
#include "library.h"
#include "library_index.h"
#include "location.h"
#include "mask_probe.h"
#include "menu.h"
#include "list_scroll.h"
#include "menu_grab.h"
#include "menu_opening.h"
#include "mirror_window.h"
#include "motion.h"
#include "mpv_gl_context.h"
#include "notices.h"
#include "offline.h"
#include "options.h"
#include "panel_fades.h"
#include "passthrough.h"
#include "performance_readout.h"
#include "photo_preloader.h"
#include "picture_drag.h"
#include "pixel_path.h"
#include "playback_session.h"
#include "platform.h"
#include "presets.h"
#include "quality.h"
#include "refresh_rate.h"
#include "remote.h"
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
#include "setup_input.h"
#include "setup_intro.h"
#include "setup_placement.h"
#include "stash.h"
#include "stash_commands.h"
#include "media_server.h"
#include "stick_seek.h"
#include "strings.h"
#include "picture_subtitles.h"
#include "subtitle_overlay.h"
#include "thumbnailer.h"
#include "uploader.h"
#include "video_format.h"
#include "video_memory.h"
#include "value_stick.h"
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
#include <string>
#include <system_error>
#include <utility>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr float kSurroundSubtitleDistance = 2.5f;
constexpr float kSurroundSubtitleWidth = 2.0f;
constexpr float kSurroundSubtitleDrop = 0.55f;
constexpr float kTurnStep = kPi / 6;  // 30 degrees per A or D press

struct App {
  // Asked for until the frame loop takes it: to quit, to open or close the
  // menu, to recenter now.
  bool quit = false;
  bool toggleMenu = false;
  bool recenter = false;
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
// glfwPollEvents): what a command does, they post; the volume keys and the
// flat screen's and the flip's change it at once; the keys held and what is
// typed wait for the frame loop.
struct KeyboardInput {
  CommandInbox& inbox;
  VideoPlayer& player;
  RunningVideo& running;
  bool seekBackKey = false;     // arrow keys held down
  bool seekForwardKey = false;
  bool turnLeftKey = false;     // A / D held down
  bool turnRightKey = false;
  // While the menu's search field is open, the keyboard types into it.
  bool textInput = false;
  std::string typed;
  int backspaces = 0;
  int cursorSteps = 0;  // ← → (Home and End: far enough to the start or the end)
  bool submit = false;
  bool stopTyping = false;  // Esc while typing: leave the search keyboard
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
    else if (key == GLFW_KEY_TAB && action == GLFW_PRESS) inbox.post({MenuCommand::ToggleMenu});
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

  ScreenPlacement& screen = running.screen;
  switch (key) {
    case GLFW_KEY_ESCAPE: {
      // Quitting takes two presses, so a stray Esc doesn't end the video.
      const double now = glfwGetTime();
      if (now - lastEscape < 1.5) {
        inbox.post({MenuCommand::Quit});
      } else {
        lastEscape = now;
        std::printf("Press Esc again to quit\n");
      }
      break;
    }
    case GLFW_KEY_TAB: inbox.post({MenuCommand::ToggleMenu}); break;
    case GLFW_KEY_SPACE: inbox.post({MenuCommand::TogglePause}); break;
    case GLFW_KEY_UP: inbox.post({MenuCommand::NextVideo}); break;
    case GLFW_KEY_DOWN: inbox.post({MenuCommand::PreviousVideo}); break;
    case GLFW_KEY_M: inbox.post({MenuCommand::ToggleMute}); break;
    case GLFW_KEY_P:
      inbox.post({MenuCommand::SetProjection,
                  static_cast<double>(nextProjection(running.format.projection))});
      break;
    case GLFW_KEY_L:
      inbox.post({MenuCommand::SetStereo, static_cast<double>(nextStereo(running.format.stereo))});
      break;
    case GLFW_KEY_C:
      screen.size.curve = screen.size.curve > 0 ? 0.0f : ScreenSize::kMaxCurve;
      running.screenChanged = true;
      break;
    case GLFW_KEY_T: inbox.post({MenuCommand::TogglePassthrough}); break;
    case GLFW_KEY_R: inbox.post({MenuCommand::Recenter, 1}); break;
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
  if (codepoint == '+') player.addVolume(5);
  else if (codepoint == '-') player.addVolume(-5);
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

  // Picture subtitles of surround video: the picture they are laid out on
  // (one eye's, eyeAspect its width / height), ahead at the subtitle
  // distance, as wide as text subtitles can be there and moving with the
  // heading. Its bottom, where they usually are, about where the text goes.
  Mat4 canvasAhead(Vec3 headPos, float headYaw, float eyeAspect, const Settings& settings) const {
    const float distance = settings.subtitleDistance / 100.0f;
    const float k = distance / kSurroundSubtitleDistance;
    const float w = kSurroundSubtitleWidth * k, h = w / std::max(eyeAspect, 0.1f);
    Vec3 center = headPos + forwardFromYaw(headYaw) * distance;
    center.y -= kSurroundSubtitleDrop * k - 0.4f * h;
    return translation(center) * rotationY(headYaw) * scaling(w, h, 1);
  }
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

// While Settings' Controls waits for a new gesture's buttons, a press is
// only for that; the trigger alone still chooses on the menu (Cancel),
// and does nothing off it. Closing the menu stops waiting.
void takeGestureRecordingPresses(const ControllerState& raw, const MenuPanels& panel, Menu& menu,
                                 ControlInput& controlInput) {
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
  // Before the GPU's default decoder is filled in below; that is never the Frame's.
  const PlatformPolicy platform = platformPolicy(opts);
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
  requirements.directVideo = platform.directVideo;

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
  if (!glContext.init(amd || platform.egl, amd && opts.hwdec != "no")) return 1;
  GpuContext gpu;
  if (!gpu.init(xr.gpuDevice())) return 1;
  GlInterop interop;  // what GL renders, the scene samples
  if (!interop.init(gpu)) return 1;
  Uploader uploader;  // what canvases draw goes to the GPU through it
  if (!uploader.init(gpu)) return 1;
  Foveation foveation;
  FrameReuse frameReuse;
  Quality quality(opts, settings, xr, foveation, frameReuse, gpu.caps(), refreshRateAvailable, steamVrRate);
  // What's new (whats_new.h): the notes since the version last seen, shown
  // as the menu first opens; closing them counts this version as seen
  // (markVersionSeen, then saveSettings). With none to show, it counts now.
  WhatsNewLaunch whatsNew = whatsNewAtLaunch(freshProfile, settings.versionSeen, settings.showWhatsNew);
  if (whatsNew.seenNow && markVersionSeen(settings.versionSeen)) saveSettings(settings);
  if (!whatsNew.releases.empty())
    std::printf("What's new: %zu release(s) to show, newest %s\n", whatsNew.releases.size(),
                whatsNew.releases.front().version.c_str());
  if (!quality.createEyeImages(gpu, foveationAsked)) return 1;
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
  player.setVolume(settings.volume);
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
  PictureSubtitles pictureSubtitles(uploader);
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
  auto say = [&](std::string text) { notices.say(std::move(text), glfwGetTime()); };
  bool peeking = false;  // Peek at the room is held: the room shows instead of the video
  double softwareSince = -1;       // the video has played in software decoding since
  Passthrough::Workers passthroughWorkers;  // stopped after the library's last writes

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
  KeyPick keyPick;  // the background key's colour, pointed at
  {
    const auto start = std::chrono::steady_clock::now();
    const bool ready = keyPick.init();
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
  LibraryIndex libraryIndex;  // the videos in the pinned folders (library_index.h)
  if (!setupForced) libraryIndex.start();  // it writes its own file
  menu.setLibraryIndex(&libraryIndex);
  Offline offline;  // offline downloads (offline.h)
  offline.start();
  menu.setServices({&library, &offline});
  // Stash (optional, docs/stash.md): none while it is off or not set up.
  std::unique_ptr<Stash> stash;
  // Funscripts on a device (script_player.h): nothing of it runs while off.
  ScriptPlayer scripts;
  scripts.configure(settings.scripts);
  offline.setScripts(settings.scripts.enabled);
  if (settings.stash.enabled && !settings.stash.url.empty() && !setupForced)
    stash = std::make_unique<Stash>(settings.stash, library);
  menu.setStash(stash.get());
  StashCommands stashCommands{settings, library, menu, stash};
  player.setSubtitleStyle(settings.subtitleSize / 100.0, settings.subtitleRaise);
  player.setMipmaps(settings.videoMipmaps);
  player.setSharpened(settings.sharpen > 0);
  quality.reportAntiShimmer(opts.antiShimmerSet ? "command line" : "settings");
  // Frame: a new video frame is shown from the next headset frame on, so the
  // eyes never wait for its V4L2 decoding, Main 10 conversion and mpv render
  // (--video-pipeline). Set up once the runtime gives its frame period.
  const bool pipelineVideo =
      opts.videoPipeline < 0 ? platform.pipelineByDefault : opts.videoPipeline == 1;
  double pipelinePeriod = 0;
  std::printf("Video pipeline: %s\n", pipelineVideo ? "on, one headset frame ahead" : "off");
  // A frame that would look like the last one drawn is not drawn: the
  // runtime shows that one again (frame_reuse.h). The setting decides on
  // the Steam Frame, --frame-reuse at start anywhere.
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
  RunningVideo running{player, settings, library};
  CommandInbox inbox;  // the keyboard's and the control socket's commands, run in poll
  ControlSocket control;
  if (!opts.control.empty()) control.listen(opts.control);
  DeoRemote deoRemote;  // other apps' remote control (deo_remote.h): nothing runs while off
  KeyboardInput keys{inbox, player, running};
  glfwSetWindowUserPointer(input, &keys);
  glfwSetKeyCallback(input, [](GLFWwindow* window, int key, int /*scancode*/, int action, int mods) {
    static_cast<KeyboardInput*>(glfwGetWindowUserPointer(window))->onKey(window, key, action, mods);
  });
  glfwSetCharCallback(input, [](GLFWwindow* window, unsigned int codepoint) {
    static_cast<KeyboardInput*>(glfwGetWindowUserPointer(window))->onChar(codepoint);
  });
  printControls();

  Deletion deletion{menu, player, settings, library, libraryIndex, offline, stash};
  Passthrough passthrough{settings, library, xr, passthroughWorkers, running.video, running.currentFile,
                          running.format};
  MenuGrab menuGrab;
  ScreenControl screenControl;
  // The gestures (gesture_input.h), and what the app does for them (set up
  // below, before the frames).
  GestureInput gestureInput;
  GestureInput::Host gestureHost;
  PictureDrag turnDrag;
  ForwardStick forwardStick[2];
  bool initialRecenter = true;  // establish the head anchor without clearing saved placement
  double lastFrameTime = glfwGetTime();
  SettledLine samplingLog;  // the pixel path per eye, for the console

  MenuPanels panel;
  SubtitlePlacement subtitlePlacement;
  // Without a video, start in the file browser as soon as the head is tracked.
  bool showBrowserAtStart = opts.file.empty() && !showSetup;
  SetupPlacement setupPlacement;
  SetupIntro setupIntro;
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
  StartMenu startMenu = opts.startMenu;  // ... or on this page, then
  int activeHand = 1;  // the hand whose pointer drives the menu
  PanelFades panelFades{menu, panel, seekPreview, setupPlacement, setupIntro};
  StickSeek stickSeek{player, panel};
  ButtonJobs buttonJobs{player, running, settings, library};
  HoldRepeat turnLeft(0.5, 0.35), turnRight(0.5, 0.35);  // A and D
  // The controllers' buttons as jobs (controls.h), by each hand's type; the
  // bindings Your controllers and the hints show, made again when the
  // profiles change (controlsVersion).
  ControlInput controlInput;
  SetupInput setupInput{menu, setupPlacement, setupIntro, controlInput};
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
    xr.input().pairedProfiles(profiles);
    ControllerBindings bindings = describeControls(profiles, settings.controls, settings.gestures);
    if (bindings == shownBindings) return;
    shownBindings = std::move(bindings);
    ++controlsVersion;
    logControls(shownBindings);
  };
  HoldRepeat cursorLeft(0.5, 0.07), cursorRight(0.5, 0.07);  // as the keyboard's keys repeat
  ValueStick valueStick;  // a pointed value stepped by either stick (value_adjust.h)
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
        xr.input().vibrate(h, buzzes[h].amplitude, buzzes[h].seconds, buzzes[h].frequency);
      buzzes[h] = {};
    }
  };
  ListScroll listScroll;
  PlaybackSession session{player, settings, library, memory, offline, stash, scripts, running, menu,
                          notices, photoPreloader, seekPreview, deletion, passthrough, stickSeek,
                          gestureInput, gestureHost, controlInput};
  deletion.closeVideo = [&session] { session.close(); };
  session.start(opts.file);
  quality.startUserPresetListing();

  running.matchPreset = [&quality] { quality.matchPreset(); };

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
        stashCommands.restart();
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

  // A PC whose runtime doesn't blend, with the setting on: passthrough draws
  // the room in key green, for a streamer that keys it out on the headset.
  auto roomKeyed = [&] {
    return !kStandaloneHeadset && settings.keyGreen && !xr.alphaBlendSupported();
  };

  auto execute = [&](const MenuCommand& c) {
    if (c.type == MenuCommand::None) return;
    if (applySettingCommand(c, settings) || quality.handle(c, player) || session.handle(c) ||
        running.handle(c, passthrough) || passthrough.handle(c) || stashCommands.handle(c) ||
        keyPick.handle(c, running))
      return;
    switch (c.type) {
      case MenuCommand::Paste: menu.typeText(clipboardText(input)); break;
      case MenuCommand::Recenter:
        if (c.value == 1) {
          app.recenter = true;
          break;
        }
        // From the menu: two seconds to look where "front" should be.
        app.recenterAt = glfwGetTime() + 2.0;
        break;
      case MenuCommand::ToggleMenu: app.toggleMenu = true; break;
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
      case MenuCommand::EndSetup:
        if (menu.setupRunning()) finishSetup(c.value == 1);
        break;
      case MenuCommand::SetupPlaces: {
        if (!menu.setupRunning()) break;
        setupChoices.places.clear();
        for (size_t at = 0; at < c.path.size();) {
          const size_t line = std::min(c.path.find('\n', at), c.path.size());
          if (line > at) setupChoices.places.push_back(c.path.substr(at, line - at));
          at = line + 1;
        }
        if (!setupForced) saveSetupChoices(setupChoices);
        break;
      }
      case MenuCommand::SetupStash:
        if (!menu.setupRunning()) break;
        stashCommands.connectInSetup(c);
        stashInSetup = true;
        finishSetup(true);
        break;
      case MenuCommand::DismissSetupBanner:
        hideSetupBanner();
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
      case MenuCommand::SetButtonJob:
      case MenuCommand::RestoreButton:
      case MenuCommand::ResetButtons:
      case MenuCommand::AddGesture:
      case MenuCommand::ChangeGesture:
      case MenuCommand::RemoveGesture:
      case MenuCommand::RestoreGestures:
        if (changeControls(c, settings)) describeBindings();
        break;
      case MenuCommand::ToggleScripts:
      case MenuCommand::SetScriptAddress:
      case MenuCommand::SetScriptOffset:
      case MenuCommand::SetScriptRangeLow:
      case MenuCommand::SetScriptRangeHigh:
        changeScripts(c, settings);
        scripts.configure(settings.scripts);
        offline.setScripts(settings.scripts.enabled);
        break;
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
      case MenuCommand::Quit: app.quit = true; break;
      case MenuCommand::OpenFile:
      case MenuCommand::OpenFileAt:
        session.open(c.path, c.type == MenuCommand::OpenFileAt ? std::max(0.0, c.value) : -1);
        panel.open = false;
        session.openedByHand(c);
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
      // Handled above by their owners, listed so that -Wswitch names a command
      // handled nowhere.
      // applySettingCommand
      case MenuCommand::SetAccent: case MenuCommand::SetAccentHue:
      case MenuCommand::SetAccentBrightness: case MenuCommand::SetAccentSaturation:
      case MenuCommand::ToggleBackground: case MenuCommand::ToggleAdvancedSettings:
      case MenuCommand::StashCard: case MenuCommand::SetKeyboardLayout:
      case MenuCommand::ToggleMotion: case MenuCommand::SetMotionDistance:
      case MenuCommand::ToggleResume: case MenuCommand::SetDownloadsTo:
      case MenuCommand::ToggleThumbnails: case MenuCommand::SetSkipSeconds:
      case MenuCommand::SetBigSkipSeconds: case MenuCommand::ToggleVibration:
      case MenuCommand::ToggleShowWhatsNew: case MenuCommand::ToggleFloatingSubtitles:
      case MenuCommand::SetStashSections: case MenuCommand::SetStashFormat:
      case MenuCommand::SetStashScenesSort: case MenuCommand::SetStashPerformersSort:
      case MenuCommand::SetStashStudiosSort: case MenuCommand::ToggleFreeDecoder:
      case MenuCommand::ToggleRemoteControl:
      // Quality
      case MenuCommand::ToggleTvGamma: case MenuCommand::ToggleVideoMipmaps:
      case MenuCommand::SetAntiShimmer: case MenuCommand::SetVideoResolution:
      case MenuCommand::SetFoveation: case MenuCommand::SetRefreshRate:
      case MenuCommand::ApplyPreset: case MenuCommand::ListUserPresets:
      case MenuCommand::SaveUserPreset: case MenuCommand::ApplyUserPreset:
      case MenuCommand::RenameUserPreset: case MenuCommand::DeleteUserPreset:
      case MenuCommand::MoveUserPreset: case MenuCommand::ToggleShowFoveation:
      case MenuCommand::ToggleFoveationLearned: case MenuCommand::SetSharpBins:
      case MenuCommand::ToggleFoveationRing: case MenuCommand::SetFoveationOutside:
      case MenuCommand::ToggleToneLook: case MenuCommand::ToggleFrameReuse:
      // PlaybackSession
      case MenuCommand::TogglePause: case MenuCommand::SetPaused: case MenuCommand::Stop:
      case MenuCommand::Seek: case MenuCommand::FrameStep: case MenuCommand::StepRating:
      case MenuCommand::SeekTo: case MenuCommand::ToggleMute: case MenuCommand::SetVolume:
      case MenuCommand::PreviousVideo: case MenuCommand::NextVideo: case MenuCommand::SetProjection:
      case MenuCommand::SetStereo: case MenuCommand::SetPacking: case MenuCommand::SetSlideshow:
      case MenuCommand::SetAfterVideo: case MenuCommand::SelectAudio:
      case MenuCommand::SelectSubtitle: case MenuCommand::LoadSubtitle:
      case MenuCommand::RequestDelete: case MenuCommand::CancelDelete: case MenuCommand::DeleteFile:
      // RunningVideo
      case MenuCommand::SetBrightness: case MenuCommand::SetContrast:
      case MenuCommand::SetSaturation: case MenuCommand::SetWarmth: case MenuCommand::SetTint:
      case MenuCommand::SetExposure: case MenuCommand::SetOpacity: case MenuCommand::SetZoom:
      case MenuCommand::ResetPicture: case MenuCommand::SetOriginForward:
      case MenuCommand::SetScreenDistance: case MenuCommand::SetScreenHeight:
      case MenuCommand::SetScreenCurve: case MenuCommand::ResetScreen: case MenuCommand::SetSharpen:
      case MenuCommand::TogglePackedMask: case MenuCommand::ToggleSecondLayer:
      case MenuCommand::ToggleGreenScreen: case MenuCommand::SetKeyStrength:
      case MenuCommand::SetKeySoftness: case MenuCommand::SetKeySpill:
      case MenuCommand::SetKeyShrink: case MenuCommand::SetKeyBackground:
      case MenuCommand::ResetPassthrough: case MenuCommand::SetFocus:
      case MenuCommand::TogglePassthrough:
      // Passthrough
      case MenuCommand::ToggleMatchRoom: case MenuCommand::ReadRoomAgain:
      case MenuCommand::SetMatchColour: case MenuCommand::SetMatchBrightness:
      case MenuCommand::ToggleAutoPassthrough: case MenuCommand::ToggleKeyGreen:
      // StashCommands
      case MenuCommand::UseServer: case MenuCommand::SetStashServer:
      case MenuCommand::SetStashFolders: case MenuCommand::ToggleStashCovers:
      case MenuCommand::ToggleStashTitles: case MenuCommand::ToggleStashMalePerformers:
      case MenuCommand::SyncStash: case MenuCommand::ToggleStashWriteBack:
      // KeyPick
      case MenuCommand::PickKeyColour: case MenuCommand::CancelKeyPick:
        break;
    }
  };

  // The gestures' values: read and set as the menu's sliders set them (their
  // commands), and why one can't change now, as the buttons' jobs say it.
  gestureHost.why = [&](GestureValue v) -> const char* {
    using V = GestureValue;
    const JobStrings& said = strings().jobs;
    const PlaybackStatus status = session.status();
    switch (gestureValueInfo(v).where) {
      case GestureWhere::Anywhere: break;
      case GestureWhere::File:
        if (!status.open) return said.nothingOpen;
        break;
      case GestureWhere::Video:
        if (!status.open) return said.nothingOpen;
        if (status.photo) return said.notForPhotos;
        break;
      case GestureWhere::Surround:
        if (!status.open) return said.nothingOpen;
        if (running.format.projection == Projection::Flat) return said.notForFlat;
        break;
      case GestureWhere::Flat:
        if (!status.open) return said.nothingOpen;
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
        if (!session.recognised()) return said.notForStreams;
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
    session.userSeeked();
  };
  gestureHost.ended = [&](GestureValue v, bool released) {
    if (v != GestureValue::Scrub) return;
    stickSeek.endScrub(released);
  };
  gestureHost.say = [&](const char* text) { say(text); };
  gestureHost.buzz = [&](int hand, float amplitude, double seconds) { buzz(hand, amplitude, seconds); };
  stickSeek.buzz = [&](int hand, float amplitude, double seconds) { buzz(hand, amplitude, seconds); };
  menuGrab.buzz = [&](int hand, float amplitude, double seconds) { buzz(hand, amplitude, seconds); };
  turnDrag.buzz = [&](int hand, float amplitude, double seconds) { buzz(hand, amplitude, seconds); };

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
  // What each frame works out, set by the phase named and read by the ones
  // after it.
  // poll: where passthrough shows this frame.
  bool passthroughAvailable = false, canBlend = false, alphaPassthrough = false,
       keyPassthrough = false, blended = false, peek = false, roomMatching = false;
  bool shouldRender = false;  // startFrame: the runtime has this frame drawn
  // The picture for the scene, and for GL (depth estimation): the video's
  // frame just rendered, if any, or the photo once it is all uploaded.
  bool photo = false, loading = false;
  const Texture* videoImage = nullptr;
  GLuint videoTexture = 0;
  SharedImage* photoImage = nullptr;
  float frameAspect = 1.0f;
  int photoWidth = 0, photoHeight = 0;
  bool stillLoading = false;
  // Until the new file's first frame arrives, the texture still holds the old
  // video, which would show cut apart in the new format; show nothing instead.
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
        session.photoShown(photoWidth, photoHeight);
      }
    } else if (!loading && !running.currentFile.empty()) {
      videoImage = player.texture();
      videoTexture = player.glTexture();
    }
    stillLoading = !running.currentFile.empty() && !videoImage && (photo || loading);
  };
  // trackHead
  double now = 0;
  Vec3 headPos;
  Quat headRot;
  bool headValid = false;
  Mat4 frame, menuAt;
  // readControllers, runJobs, handInput, stickInput: what this frame's input did.
  bool openedNow = false, menuBroughtBack = false;
  bool stickScrolled = false, recentered = false, volumeTwisted = false;
  bool practising = false, listMode = false, keyPicking = false;
  float dt = 0;
  bool onMenu[2] = {false, false};  // each hand's pointer is on a menu panel
  // pointAtMenu: the pointer ray against the menu panels.
  Vec3 rayOrigin, rayDir, hitPoint;
  float hitU = 0, hitV = 0;
  int hitPanelId = -1;
  bool pointing = false, hitPanel = false;

  auto poll = [&] {
    glfwPollEvents();
    xr.pollEvents(xrQuit);
    player.pollEvents();
    if (settings.scripts.enabled) {
      // Held too while the headset is taken off, though the video plays on.
      const PlaybackStatus status = session.status();
      scripts.sync(status.position, status.moving && xr.userPresence() != 0);
    }
    if (platform.followVisibility) {
      if (!xr.isVisible() && stickSeek.active()) {
        if (stickSeek.pausedVideo()) player.setPaused(false);
        stickSeek.cancel();
      }
      player.setPresentationActive(xr.isVisible());
    }
    timer.lap(FrameTimer::Events);
    if (player.shutdownRequested()) return false;
    // Also while nothing is drawn. The input focus here is the last drawn
    // frame's: a frame checks its own before the gaze correction sees its input.
    if (!xr.isRunning() || !xr.input().controllers().focused || xr.userPresence() == 0) endGazeSitting();

    session.pollFailure();
    const bool opened = player.takeOpened();
    // Played after freeing the decoder: a later refusal (after a sleep) is asked about again.
    if (opened && player.loadCount() == decoderRescue.retry) decoderRescue.retry = 0;
    if (opened && setupBanner && !isPhotoFile(running.currentFile)) hideSetupBanner();
    session.countPlay(opened);

    deoRemote.update({settings.remoteControl, session.status(), menu.setupRunning(),
                      stash ? stash->catalog().get() : nullptr},
                     inbox);
    control.update(inbox, {session, player, panel.open});
    for (MenuCommand c; inbox.take(c);) execute(c);

    running.saveDue(buttonJobs.zoomHeld());
    // The library's background results; the playing file's record may be
    // among them.
    session.pollRecord(library.poll());
    if (stash) stash->poll();
    session.poll();

    deletion.poll();

    session.detectFormat();

    // Passthrough for videos made for it, once their size (so their format)
    // and their settings are known. Never where it couldn't show the room:
    // the runtime must blend, or key green stand in for it.
    passthroughAvailable = xr.alphaBlendSupported() || roomKeyed();
    const bool autoPassthroughOn = settings.autoPassthrough && passthroughAvailable;
    passthrough.settle(autoPassthroughOn, session.settled());
    if (RemovableDrives::Change change; drives.poll(change)) {
      menu.drivesChanged(change.removed);
      offline.drivesChanged();
      notices.drivesChanged(change, menu.setupRunning(), glfwGetTime());
    }
    const PlaybackStatus status = session.status();
    // Looks for new videos in the pinned folders wait while a video is open.
    libraryIndex.setVideoOpen(status.video);
    // ... and so does looking for Stash's folders.
    if (stash) stash->setVideoOpen(status.video);
    // Downloads and new thumbnails wait while a video plays.
    const bool videoPlaying = status.video && !status.paused;
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
    player.setSubtitleCanvas(eyeDisplayAspect(running.format, player.aspect()));
    subtitles.setText(floatingSubtitles ? player.playback().subtitleText : "");
    running.applyPassthroughToggle(passthroughAvailable, passthrough);
    // Passthrough, where the runtime blends (the Frame itself, or a PC whose
    // runtime shows the headset's camera): the eye images are blended over
    // the camera by their alpha. Where it doesn't there is none, unless a PC
    // is set to draw the room in key green.
    canBlend = xr.alphaBlendSupported();
    alphaPassthrough = running.video.passthrough && canBlend;
    keyPassthrough = running.video.passthrough && roomKeyed();
    blended = passthrough.roomShown();
    // Peek at the room, held: the video at no opacity over the room, none of
    // its settings changed.
    peek = peeking && canBlend;
    // The room's balance, while the switch is on and the room is shown.
    roomMatching = passthrough.matched() && blended;
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
    return true;
  };

  auto startFrame = [&] {
    // Pipelined (--video-pipeline), the video frame rendered in the last
    // iteration is shown from this headset frame on.
    player.showRendered();
    // Frame's Q10C conversion is asynchronous GPU work, independent of the
    // head pose. Submit it before the runtime wait so it can finish while
    // SteamVR paces the frame, rather than consuming the eye-render deadline.
    const bool earlyVideo = platform.earlyRender;
    if (earlyVideo) {
      player.render();
      glFlush();
      timer.lap(FrameTimer::Mpv);
    }
    // Let xrWaitFrame absorb the user-triggered map replacement.
    if (!quality.beforeWaitFrame(eyeTargets)) return false;
    shouldRender = xr.beginFrame();
    if (pipelineVideo && xr.displayPeriod() > 0 &&
        std::abs(xr.displayPeriod() - pipelinePeriod) > 0.0005) {
      pipelinePeriod = xr.displayPeriod();
      player.setPipelined(true, pipelinePeriod);
    }
    // Emptied, keeping its overlay list's storage: no allocation per frame.
    scene.clear = ClearMode::Dark;
    scene.sky = false;
    scene.video.reset();
    scene.onPicture.clear();
    scene.overlays.clear();
    timer.lap(FrameTimer::XrWait);
    if (!earlyVideo) player.render();
    seekPreview.poll();
    photoPreloader.update();
    timer.lap(FrameTimer::Mpv);
    return true;
  };

  auto trackHead = [&] {
    now = glfwGetTime();
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
    headPos = {};
    headRot = {};
    headValid = xr.headPose(headPos, headRot);
    frame = headFrame(headRot);
    menuAt = menuFrame(headRot);
    if (app.recenterAt > 0 && now >= app.recenterAt) {  // the menu's countdown is over
      app.recenter = true;
      app.recenterAt = 0;
      buzz(-1, 0.3f, 0.020);
    }
    if ((initialRecenter || (app.recenter && !menu.setupRunning())) && headValid) {
      if (app.recenter) {
        if (running.video.originForward != 0)
          notices.viewpointChanged(now);  // "Centered": it did go back
        running.video.originForward = 0;
        // Also mark a provisional zero: a late identity restore must not
        // bring back a remembered origin after an explicit recenter.
        running.storeVideoSoon(VideoSettings::kOrigin);
      }
      running.screen.anchorPosition = headPos;
      running.screen.anchorRotation = frame;
      running.screen.turn = 0.0f;
      turnDrag.restart();  // a drag goes on from here, without catching up first
      if (panel.open) panel.placeInFrontOf(headPos, menuAt);  // the menu comes along
      app.recenter = false;
      initialRecenter = false;
    }
  };

  auto readControllers = [&]() -> const JobInput& {
    // --- Controller and keyboard input ---
    if (xr.input().profilesVersion() != profilesSeen) {
      profilesSeen = xr.input().profilesVersion();
      for (int h = 0; h < 2; ++h) controllerTypes[h] = controllerType(xr.input().interactionProfile(h));
      describeBindings();
      const ControllerType seen =
          controllerTypes[1] != ControllerType::Count ? controllerTypes[1] : controllerTypes[0];
      if (seen != ControllerType::Count && seen != settings.controllersSeen) {
        settings.controllersSeen = seen;
        saveSettings(settings);
      }
    }
    const ControllerState& raw = xr.input().controllers();
    // Presses that do something else, and nothing more: grip held and a
    // stick's click on the stars clears the rating; any press skips setup's
    // intro (below).
    if (panel.open && !menu.setupRunning() && (raw.hands[0].grip || raw.hands[1].grip))
      for (int h = 0; h < 2; ++h)
        if ((raw.hands[h].pressed & controlBit(Control::StickClick)) && menu.clearRatingAtPointer())
          controlInput.consume(h, Control::StickClick);
    setupInput.takeIntroPresses(raw, now);
    takeGestureRecordingPresses(raw, panel, menu, controlInput);
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
        panelFades.pointerMoved();
      }

    openedNow = false;
    menuBroughtBack = false;  // by the menu button (the hints)
    stickScrolled = recentered = volumeTwisted = false;
    if (menu.setupRunning()) {
      SetupInput::Frame s;
      s.now = now;
      s.headValid = headValid;
      s.headPos = headPos;
      s.headRot = headRot;
      s.presence = xr.userPresence();
      s.bring = menuPressed || jobs.wasPressed(Job::Recenter) || app.toggleMenu || app.recenter;
      s.jobs = &jobs;
      s.controlsVersion = controlsVersion;
      s.bindings = &shownBindings;
      openedNow = setupInput.update(s, panel);  // that press doesn't click
      app.recenter = false;
      if (!kStandaloneHeadset) {
        int w = 0, h = 0;
        mirror.setCover(setupPlacement.waiting() ? menu.setupCue(Menu::SetupCue::Mirror, w, h)
                                                 : nullptr);
      }
    } else {
      MenuOpeningInput opening;
      opening.headValid = headValid;
      opening.startPage = startMenu != StartMenu::None;
      opening.startBrowser = showBrowserAtStart;
      opening.videoEnded = session.menuAtEnd;
      opening.pressed = menuPressed || app.toggleMenu;
      opening.keyPicking = keyPick.picking();
      switch (menuOpening(opening)) {
        case MenuOpening::StartPage:
          panel.open = true;
          panel.placeInFrontOf(headPos, menuAt);
          if (startMenu == StartMenu::Browser) menu.showFileBrowser();
          else if (startMenu == StartMenu::Settings) menu.showSettings();
          else menu.showMainPage();
          startMenu = StartMenu::None;
          showBrowserAtStart = false;
          break;
        case MenuOpening::StartBrowser:
          showBrowserAtStart = false;
          panel.open = true;
          panel.placeInFrontOf(headPos, menuAt);
          menu.showFileBrowser();
          break;
        case MenuOpening::AtVideoEnd:
          session.menuAtEnd = false;
          if (!panel.open) panel.placeInFrontOf(headPos, menuAt);
          panel.open = true;
          menu.showPlayedFrom();
          break;
        case MenuOpening::CancelKeyPick:
          keyPick.end();
          break;
        case MenuOpening::Toggle:
          panel.open = !panel.open;
          openedNow = panel.open;
          menuBroughtBack = panel.open && menuPressed;
          if (panel.open) {
            // Always open on the player page; sub pages (search, files) keep their state.
            menu.showMainPage();
            if (headValid) panel.placeInFrontOf(headPos, menuAt);
          }
          break;
        case MenuOpening::Leave: break;
      }
    }
    app.toggleMenu = false;
    return jobs;
  };

  auto runJobs = [&](const JobInput& jobs) {
    buttonJobs.keepSeeThrough();
    // The buttons' other jobs, as the menu's or the strip's controls do
    // them. Menu and Recenter are above and below, Choose is the trigger's.
    // Not while setup's Your controllers shows what each does.
    practising = menu.setupShowsControls();
    for (int j = 0; j < kJobs && !practising; ++j) {
      const Job job = static_cast<Job>(j);
      const int hand = jobs.pressedBy(job);
      if (hand < 0 || job == Job::Recenter || job == Job::Choose) continue;
      buzz(hand, 0.2f, 0.014);
      const JobInfo& info = jobInfo(job);
      const JobStrings& said = strings().jobs;
      if ((info.needs == JobNeeds::File || info.needs == JobNeeds::Video) && !session.status().open) {
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
      context.slideshow = session.slideshowSeconds > 0;
      context.passthroughAvailable = passthroughAvailable;
      context.alphaBlend = xr.alphaBlendSupported();
      context.roomReadable = passthrough.roomReadable;
      context.roomShown = passthrough.roomShown();
      context.roomMatched = passthrough.matched();
      context.afterVideo = session.afterVideo();
      JobAction action = buttonJobs.press(job, hand, context);
      MenuCommand& c = action.command;
      // This needs the menu: done here.
      switch (job) {
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
            if (isLink(running.currentFile)) say(said.notForStreams);
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
          if (keyPick.picking()) keyPick.end();
          c = menu.requestDelete();
          break;
        default: break;
      }
      if (!action.said.empty()) say(std::move(action.said));
      if (action.showOpacity) notices.opacityChanged(now);
      if (action.showRing != GestureValue::Count) gestureInput.show(action.showRing, now);
      if (c.type != MenuCommand::None) execute(c);
      // The switches whose change doesn't show by itself, as they are now.
      if (std::string text = buttonJobs.saidAfter(job, session.afterVideo()); !text.empty()) say(std::move(text));
    }
    peeking = !practising && jobs.held[static_cast<int>(Job::PeekRoom)] != 0;
    if (buttonJobs.holdZoom(jobs, now)) gestureInput.show(GestureValue::Zoom, now);
    if (jobs.wasPressed(Job::Recenter) && !menu.setupRunning()) {
      app.recenter = true;
      recentered = true;
      buzz(-1, 0.3f, 0.020);
    }
  };

  auto handInput = [&](const JobInput& jobs, const ControllerState& input) {
    dt = static_cast<float>(std::min(0.1, now - lastFrameTime));
    lastFrameTime = now;
    const Vec3 up = transformDir(frame, {0.0f, 1.0f, 0.0f});
    listMode =
        panel.open && (menu.setupRunning() ? menu.setupScrolls() : menu.listOpen());
    for (int h = 0; h < 2; ++h) {
      const HandState& g = input.hands[h];
      float u, v, d;
      onMenu[h] = g.poseValid && menuHitTest(panel, menu, g.aimPosition, aimDirection(g), u, v,
                                             d) >= 0;
    }

    {
      MenuGrab::Frame grabFrame;
      grabFrame.onMenu[0] = onMenu[0];
      grabFrame.onMenu[1] = onMenu[1];
      grabFrame.screenHeld = screenControl.holding();
      grabFrame.headValid = headValid;
      grabFrame.headPos = headPos;
      grabFrame.up = up;
      menuGrab.update(input, grabFrame, panel);
    }

    // The gestures: buttons held and a twist of that wrist change a value.
    {
      GestureInput::Frame g;
      g.now = now;
      g.blocked = practising || (panel.open && menu.recordingGesture());
      g.lost = jobs.lost;
      for (int h = 0; h < 2; ++h) {
        g.types[h] = controllerTypes[h];
        g.onMenu[h] = onMenu[h] || menuGrab.hand() == h;
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
      screenFrame.menuHeld = menuGrab.hand() >= 0;
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

    // Picking the key's colour.
    {
      KeyPick::Frame pickFrame;
      pickFrame.menuOpen = panel.open;
      pickFrame.activeHand = activeHand;
      pickFrame.onMenu[0] = onMenu[0];
      pickFrame.onMenu[1] = onMenu[1];
      pickFrame.headValid = headValid;
      pickFrame.peeking = peeking;
      pickFrame.leftEye = &xr.eye(0);
      pickFrame.laserLength = panelFades.laserLength();
      pickFrame.correction = &shownCorrection;
      pickFrame.frameAspect = frameAspect;
      keyPick.update(input, pickFrame, running);
    }
    keyPicking = keyPick.picking();

    // Trigger away from the menu: drag the 180°/360° picture around you.
    {
      PictureDrag::Frame dragFrame;
      dragFrame.noDrag = screenControl.holding() || running.format.projection == Projection::Flat;
      dragFrame.menuHand = menuGrab.hand();
      for (int h = 0; h < 2; ++h) {
        dragFrame.onMenu[h] = onMenu[h];
        dragFrame.triggerTaken[h] = gestureInput.claims(h, Control::Trigger);
      }
      dragFrame.keyPicking = keyPicking;
      dragFrame.headValid = headValid;
      dragFrame.displayTime = xr.displayTime();
      turnDrag.update(input, dragFrame, running.screen);
    }
  };

  auto stickInput = [&](const ControllerState& input) {
    // Either stick sideways seeks; up/down scrolls lists, else moves the
    // viewpoint or flat screen. Only the dominant axis counts. While the
    // keyboard is up, sideways moves the text cursor instead.
    const bool typing = panel.open && menu.textInputActive();
    const StickDirections left = stickDirections(input.hands[0]);
    const StickDirections right = stickDirections(input.hands[1]);
    if (cursorLeft.update(typing && (left.left || right.left), now)) menu.moveCursor(-1);
    if (cursorRight.update(typing && (left.right || right.right), now)) menu.moveCursor(1);
    for (int i = 0; i < 2; ++i) forwardStick[i].update(input.hands[i], now);

    {
      ValueStick::Frame valueFrame;
      valueFrame.now = now;
      valueFrame.menuOpen = panel.open;
      valueFrame.typing = typing;
      valueFrame.seeking = stickSeek.active();
      valueFrame.pointedValue = menu.pointedValue();
      if (const int step = valueStick.update(input, valueFrame)) execute(menu.stepValue(step));
    }

    {
      StickSeek::Frame seekFrame;
      seekFrame.now = now;
      seekFrame.seekable = session.status().open && !photo;
      seekFrame.sticksTaken = typing || valueStick.held();
      seekFrame.backKey = keys.seekBackKey;
      seekFrame.forwardKey = keys.seekForwardKey;
      seekFrame.skipSeconds = settings.skipSeconds;
      seekFrame.headValid = headValid;
      seekFrame.headPos = &headPos;
      seekFrame.menuAt = &menuAt;
      if (stickSeek.update(input, seekFrame)) session.userSeeked();
    }
    if (turnLeft.update(keys.turnLeftKey, now)) running.screen.turn = wrapAngle(running.screen.turn - kTurnStep);
    if (turnRight.update(keys.turnRightKey, now)) running.screen.turn = wrapAngle(running.screen.turn + kTurnStep);
    if (listMode) {
      ListScroll::Frame scrollFrame;
      scrollFrame.dt = dt;
      scrollFrame.valueStick = valueStick.held();
      scrollFrame.menuHand = menuGrab.hand();
      scrollFrame.screenHand = screenControl.hand();
      double rows = 0;
      if (listScroll.update(input, scrollFrame, rows)) {
        menu.scroll(rows);
        stickScrolled = true;
      }
    } else {
      listScroll.reset();
      // Stick up/down off the menu, no grip held: the viewpoint.
      if (running.format.projection != Projection::Flat && videoImage &&
          !input.hands[0].grip && !input.hands[1].grip) {
        int direction = 0;
        for (int i = 0; i < 2; ++i)
          if (!onMenu[i] && forwardStick[i].held(now))
            direction += input.hands[i].stickY > 0 ? 1 : -1;
        if (direction) {
          running.moveViewpoint(direction, dt);
          notices.viewpointChanged(now);
        }
      }
    }
  };

  auto pointAtMenu = [&](const HandState& hand) {
    // Pointer ray against the menu panels; the nearer hit wins.
    rayOrigin = rayDir = hitPoint = {};
    hitU = 0, hitV = 0;
    hitPanelId = -1;
    pointing = panel.open && hand.poseValid && !setupPlacement.fading();
    if (pointing) {
      rayOrigin = hand.aimPosition;
      rayDir = aimDirection(hand);
      float distance = 0;
      hitPanelId = menuHitTest(panel, menu, rayOrigin, rayDir, hitU, hitV, distance);
      if (hitPanelId >= 0) {
        hitPoint = rayOrigin + rayDir * distance;
        panelFades.hit(distance);
      }
    }
    hitPanel = hitPanelId >= 0;
  };

  // Fills this frame's `focusIn`, which later phases read too.
  auto findFocus = [&](FocusInput& focusIn, const FocusSettings& focusSettings) {
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
        !photo && !videoTexture && maskProbeDirectReady && !keyPick.busy()
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
    if (keyPick.wanted() && !maskProbe.drawing()) {
      DirectFrame* direct = !photo && !videoTexture ? player.directCurrent() : nullptr;
      DepthPatchDrawer* drawer =
          direct && maskProbeDirectReady ? maskProbeDirect.source(direct) : nullptr;
      const std::function<bool()> prepare =
          photo ? focusIn.prepareRead : [&player] { return player.prepareFullSizeRead(); };
      keyPick.read(direct && !drawer, videoTexture, !photo && player.tvGammaImage(), prepare, drawer);
    }
    keyPick.takeResult(running);
    focusIn.now = now;
    focusIn.target = focusTargets.update(
        settings.focusPrefs.eyeTracking, xr.input().gaze(), headRot, now,
        settings.focusPrefs.gazeCorrect ? gazeCorrection.fit() : GazeFit{});
    FocusOutput focusOut = autofocus.update(focusIn);
    focusMarker.update(settings.focusPrefs.showTarget,
                       (focusIn.leftEye + focusIn.rightEye) * 0.5f, headRot, focusOut.marker,
                       focusOut.markerDistance, focusOut.distanceText);
    return focusOut;
  };

  auto menuDialogs = [&] {
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
      panelFades.menuOpened();
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
    if (panel.open && releaseUpdate.state() == ReleaseUpdate::State::Restarting && !app.quit) {
      std::printf("Update: restarting into %s\n", releaseUpdate.version().c_str());
      menu.showUpdate();
      releaseUpdate.restartAtExit();
      app.quit = true;
    }
    if (std::string name = serverNameNow(menu.setupRunning()); name != serverNamed) {
      serverNamed = std::move(name);
      setServerName(serverNamed);
      menu.languageChanged();
    }
    diagnostics.poll();
    // Software decoding the strip warns of, once it has lasted a second
    // (not a decoder restarting after a seek).
    if (photo || loading || !videoImage || player.counts().decoder != "no")
      softwareSince = -1;
    else if (softwareSince < 0)
      softwareSince = now;
    quality.pollUserPresets();
  };

  auto updateMenu = [&](const ControllerState& input, const HandState& hand,
                        const FocusSettings& focusSettings, const FocusInput& focusIn) {
    // Filled in place (Menu::nextView()): every field is set below, every
    // frame, as in a new view.
    MenuView& view = menu.nextView();
    view.releaseUpdate = releaseUpdate.state();
    view.releaseVersion = releaseUpdate.version();
    view.diagnostics = diagnostics.state();
    view.diagnosticsFile = diagnostics.fileName();
    view.crashedLastRun =
        previousRunCrashed() && diagnostics.state() != DiagnosticsExport::State::Saved;
    view.playback = player.playback();
    view.hardwareAv1 = player.hardwareAv1();
    stickSeek.fill(view.playback);
    view.softwareDecoding = softwareSince >= 0 && now - softwareSince >= 1.0;
    view.scriptStatus = scripts.status();
    deoRemote.fill(view);
    view.playbackFailed = player.failed();
    view.discEncrypted = player.failed() && bdmv::isEncrypted(running.currentFile);
    view.decoderBusy = player.decoderRefused() && decoderRescue.path == running.currentFile &&
                       (decoderRescue.busy == decoder_holders::Busy::Steam ||
                        decoderRescue.busy == decoder_holders::Busy::StillBusy);
    view.videoWidth = photo ? photoWidth : player.width();
    view.videoHeight = photo ? photoHeight : player.height();
    if (photo) view.playback.title = std::filesystem::path(running.currentFile).filename().string();
    view.passthroughAvailable = passthroughAvailable;
    view.passthrough = alphaPassthrough || keyPassthrough;
    keyPick.fill(view);
    view.opacityApplies = canBlend;
    fillSettings(view, settings);
    running.fill(view);
    session.fill(view);
    passthrough.fill(view, blended, roomMatching, now);
    quality.fill(view);
    stashCommands.fill(view);
    view.focus.settings = focusSettings;
    view.focus.preferences = settings.focusPrefs;
    view.focus.own = running.video.hasFocus;
    view.focus.supported = stereoCorrectionSupported(running.format) && xr.eyeCount() == 2;
    view.focus.unavailable = !autofocus.available();
    view.focus.targetNotice = targetNotice(focusTargets.status());
    view.seekPreviewAvailable = !kStandaloneHeadset;
    view.showPerformance = showPerformance;
    view.controls.type = shownControllerType();
    view.setupBanner = setupBanner;
    view.appFps = appFps;
    view.focus.headsetIpdTenthMm =
        focusIn.headValid
            ? 5 * static_cast<int>(std::lround(2000.0f * length(focusIn.rightEye - focusIn.leftEye)))
            : 0;
    menu.update(view);
    for (MenuCommand c = menu.takeCommand(); c.type != MenuCommand::None; c = menu.takeCommand()) execute(c);
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
    quality.afterMenu(menu.draggingSlider());
  };

  auto showHints = [&](const ControllerState& input) {
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
  };

  auto buildScene = [&](const HandState& hand, const FocusInput& focusIn, const FocusOutput& focusOut) {
    // --- Drawing ---
    // About when this frame is shown: what eases is shown as it is then
    // (menu_feel.h, Ease).
    const double easedAt = now + std::max(0.0, xr.displayPeriod());
    panelFades.update(stickSeek.stripShown(now));
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
    } else if (!panelFades.stripFading()) {  // it fades with the strip
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
    video.antiShimmer = quality.drawnAntiShimmer();
    video.look = settings.toneLook;
    video.sharpen = settings.sharpen / 100.0f;
    video.key = running.video.keySliders();
    video.correction = focusOut.correction;
    shownCorrection = focusOut.correction.eyes[0];
    if (videoImage && xr.eyeCount() > 0) {
      const ScreenSurface screen(running.screen, eyeDisplayAspect(running.format, frameAspect));
      describeEyeSampling(samplingLog.line(), running.format, photo ? photoWidth : player.width(),
                          photo ? photoHeight : player.height(), running.screen.zoom,
                          screen.width() / screen.distance(), xr.eyePixelsPerDegree(0, false),
                          photo || settings.videoMipmaps);
      samplingLog.update(now);
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
    // Picture subtitles where the disc puts them: on a flat screen part of
    // the picture (FrameScene::onPicture).
    pictureSubtitles.update(player.subtitlePicture());
    if (!pictureSubtitles.empty() && !loading && videoImage && !photo) {
      const float eyeAspect = eyeDisplayAspect(running.format, frameAspect);
      // As opaque as the video: its opacity (percent) applies only where the room shows.
      const float opacity = scene.clear == ClearMode::Transparent ? video.opacity / 100.0f : 1.0f;
      if (running.format.projection == Projection::Flat)
        pictureSubtitles.addTo(scene.onPicture, running.screen, eyeAspect, opacity);
      else if (headValid)
        pictureSubtitles.addTo(overlays, subtitlePlacement.canvasAhead(headPos, yawFromQuat(headRot),
                                                                       eyeAspect, settings));
    }
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
    PanelFades::Frame panelFrame;
    panelFrame.now = now;
    panelFrame.easedAt = easedAt;
    panelFrame.hand = &hand;
    panelFrame.pointing = pointing;
    panelFrame.rayOrigin = rayOrigin;
    panelFrame.rayDir = rayDir;
    panelFrame.hitPanel = hitPanel;
    panelFrame.hitPoint = hitPoint;
    panelFades.addTo(overlays, panelFrame);
    // Keep the focus target visible over the menus.
    focusMarker.addTo(overlays);
    // Messages in front of the eyes, last, over everything: the recenter
    // countdown, and the viewpoint as the stick moves it (or Recenter puts
    // it back), the opacity a button's job set, or a gesture's value as a
    // twist changes it, redrawn only as what they show changes, or what a
    // button's job said.
    Notices::Frame noticesFrame;
    noticesFrame.now = now;
    noticesFrame.headValid = headValid;
    noticesFrame.recenterAt = app.recenterAt;
    noticesFrame.projection = running.format.projection;
    noticesFrame.video = &running.video;
    noticesFrame.passthrough = passthrough.notice(now);
    noticesFrame.gestures = &gestureInput;
    noticesFrame.gestureHost = &gestureHost;
    noticesFrame.screenShown = screenControl.shown();
    noticesFrame.screenShownAt = screenControl.shownAt();
    noticesFrame.playback = &player.playback();
    notices.update(menu, noticesFrame);
    if (headValid) {
      notices.addTo(overlays, headPos, headRot, hintTexture && hints.showing());
      hints.addTo(overlays, hintTexture, hintW, hintH, headPos, headRot, now);
    }
    if (menu.setupRunning() && headValid) setupPlacement.addCues(overlays, menu, headPos, headRot, now);
    if (menu.setupRunning() && headValid && (menu.setupIntroDue() || setupIntro.playing(now)))
      setupIntro.addCover(overlays, menu, headPos, headRot, now);
  };

  auto submitFrame = [&] {
    // Match the room reads the view: nothing drawn meanwhile, so it shows the
    // room alone.
    const bool roomBlank = passthrough.roomBlank();
    if (roomBlank) {
      scene.video.reset();
      scene.onPicture.clear();
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
      quality.overrideResolution(opts.resolutionCycle[step]);
      resolutionCycleAt += opts.resolutionCycleSeconds;
    }
    if (shouldRender) {
      // A new eye size or foveation level, between frames: nothing acquired
      // or being recorded (docs/vulkan-renderer.md, 5.5).
      const auto switchStart = std::chrono::steady_clock::now();
      const bool resized = xr.updateEyeSize();
      if (resized) foveation.resized();
      foveation.prepare();
      if (!eyeTargets.update()) return false;
      if (resized)
        std::printf("Eye resolution: %dx%d in %dx%d images, switched in %.2f ms\n", xr.eyeWidth(0),
                    xr.eyeHeight(0), xr.eyeImageWidth(0), xr.eyeImageHeight(0),
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - switchStart)
                        .count());
      foveation.update(headValid && settings.foveationLearned ? gazeCorrection.fit() : GazeFit{},
                       headRot);
      // Not over the room while Match the room reads it.
      scene.showFoveation = !roomBlank && quality.foveationShown() && foveation.active();
      if (scene.showFoveation)
        for (int i = 0; i < xr.eyeCount(); ++i) scene.foveation[i] = foveation.debug(i);
      // What this frame's eye images depend on: if the last drawn frame's
      // would do as well, the runtime shows those again and nothing is drawn.
      FrameReuse::Frame shown;
      shown.scene = &scene;
      shown.picture = player.pictureVersion();
      shown.onPicture = scene.onPicture.empty() ? 0 : pictureSubtitles.version();
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
          std::vector<SemaphoreWait>& waits = slot->waits;
          const bool whole = renderer.record(*slot, *eyes, &xr.eye(0), scene, glfwGetTime(), waits);
          timer.lap(FrameTimer::Record);
          std::vector<VkSemaphore>& signals = slot->signals;
          mirror.addSignals(signals);
          const uint64_t value = gpu.submit(*slot, waits, signals);
          drawn = value != 0;
          mirror.submitted(drawn);
          if (drawn) {
            renderer.frameSubmitted(value);
            // A frame recorded without its picture is not one to show again.
            if (whole) frameReuse.drawn(shown);
            else frameReuse.forget();
            timer.drew();
          } else {
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
      return false;
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
    return true;
  };

  while (!xrQuit && !app.quit && !quitSignal && !glfwWindowShouldClose(input)) {
    timer.start();
    if (!poll()) break;

    if (!xr.isRunning()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    if (!startFrame()) break;
    choosePicture();
    if (shouldRender) {
      trackHead();
      const JobInput& jobs = readControllers();
      const ControllerState& input = jobs.state;
      runJobs(jobs);
      handInput(jobs, input);
      stickInput(input);
      const HandState& hand = input.hands[activeHand];
      pointAtMenu(hand);
      // Stereo focus: the video's own settings, else the defaults; none while
      // it is off for all videos.
      const FocusSettings& focusSettings = running.video.hasFocus ? running.video.focus : settings.focus;
      FocusInput focusIn;
      const FocusOutput focusOut = findFocus(focusIn, focusSettings);
      menuDialogs();
      updateMenu(input, hand, focusSettings, focusIn);
      // A command may have closed or replaced the picture chosen above (Stop
      // frees the video's images, a new file is loading): choose it again.
      choosePicture();
      sendBuzzes();
      showHints(input);
      buildScene(hand, focusIn, focusOut);
    }
    if (!submitFrame()) break;
  }
  if (quitSignal) std::printf("Quitting (signal %d)\n", static_cast<int>(quitSignal));
  if (opts.timing) std::printf("Frame reuse: frames %s\n", reuseReport(nullptr).c_str());
  running.saveUnsaved();
  running.keepVolume();
  running.keepSharpen();
  session.keepResume();
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
  opts.file = remote::lowerScheme(opts.file);

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
