#include "anti_shimmer.h"
#include "autofocus.h"
#include "direct_depth.h"
#include "direct_video.h"
#include "eye_resolution.h"
#include "eye_targets.h"
#include "file_ops.h"
#include "file_util.h"
#include "first_run.h"
#include "focus_marker.h"
#include "focus_target.h"
#include "foveation.h"
#include "frame_timer.h"
#include "gaze_notice.h"
#include "hints.h"
#include "green_screen_check.h"
#include "removable_drives.h"
#include "room_light.h"
#include "gl_interop.h"
#include "gl_util.h"
#include "gpu_context.h"
#include "library.h"
#include "library_index.h"
#include "menu.h"
#include "mirror_window.h"
#include "motion.h"
#include "offline.h"
#include "performance_readout.h"
#include "photo_preloader.h"
#include "pixel_path.h"
#include "platform.h"
#include "presets.h"
#include "refresh_rate.h"
#include "diagnostics.h"
#include "release_update.h"
#include "run_log.h"
#include "scene_renderer.h"
#include "screen_control.h"
#include "see_through.h"
#include "seek_preview.h"
#include "settings.h"
#include "setup_intro.h"
#include "setup_placement.h"
#include "remote.h"
#include "stash.h"
#include "stash_delete.h"
#include "stash_stream.h"
#include "strings.h"
#include "subtitle_overlay.h"
#include "thumbnailer.h"
#include "uploader.h"
#include "video_format.h"
#include "video_memory.h"
#include "video_player.h"
#include "vr_input.h"
#include "vr_space.h"
#include "xmath.h"
#include "xr_context.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <malloc.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
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
#include <vector>

namespace {

// The pointer beam starts 1 m long; pointing at no panel, it
// keeps the length of its last hit. Its dot is 20 menu layout units wide.
constexpr float kLaserStartLength = 1.0f;  // meters
constexpr float kLaserDotRadius = 10 * menu_geometry::kUnit;
constexpr float kSurroundSubtitleDistance = 2.5f;
constexpr float kSurroundSubtitleWidth = 2.0f;
constexpr float kSurroundSubtitleDrop = 0.55f;
constexpr float kTurnStep = kPi / 6;  // 30 degrees per stick flick
// The viewpoint's and opacity's overlay stays this long after its gesture lets go.
constexpr double kValueShown = 1.5;
// ... and the seek bar a held seek shows with the menu closed.
constexpr double kSeekBarLinger = 0.6;
// The stick's seeks stop this far before the end: a seek into it would end
// the video (and open the menu).
constexpr double kSeekEndMargin = 5.0;
// While a seek is held, the paused video shows the picture at the target at
// most this often, once the last one is shown and the target has moved on
// this far (each is a decoder restart on the Frame, about 250 ms).
constexpr double kHeldPictureEvery = 0.5;  // seconds
constexpr double kHeldPictureMove = 1.0;   // seconds of video
// Passthrough switched on by itself says so this long.
constexpr double kPassthroughNoticeShown = 5.0;
// A microSD card or USB drive put in says so this long.
constexpr double kDriveNoticeShown = 5.0;
// Dragging the picture with the trigger: a smoothed copy of the hand's turn
// catches up with the real one at this rate (per second, about a 250 ms lag),
// which calms hand jitter. Frame gaps longer than kDragMaxStep count as that long.
constexpr float kDragResponse = 4.0f;
constexpr double kDragMaxStep = 0.1;  // seconds
// The volume gesture changes one of mpv's 0..100 per degree of wrist twist.
constexpr double kVolumePerDegree = 1.0;
// The opacity gesture: past its dead zone, a quarter turn takes it from 100 % to 50 %.
constexpr double kOpacityPerDegree = 50.0 / 90.0;
// Reaching 100 % (unblended) or 0 %, it stays there until the wrist turns
// back as far as the dead zone again, so the hand relaxing as it lets go
// doesn't take it off.
constexpr double kOpacityHold = GripTwist::kDeadZone * kOpacityPerDegree;

struct App {
  VideoPlayer* player = nullptr;
  std::string currentFile;
  std::string streamOshash;  // a Stash stream playing: its file's oshash as opened
  ScreenPlacement screen;
  VideoFormat format;
  bool formatChosenByUser = false;  // stops auto-detection from overriding the user
  bool rememberFormat = false;      // the user changed the format by key: store it
  VideoSettings video;  // what is remembered about the running video
  int slideshowSeconds = 0;  // photos: show the next one after this long; 0 = off
  double nextSlideAt = 0;    // glfwGetTime() when it does
  bool recenter = false;
  bool togglePassthrough = false;  // T key; handled where the settings are
  double recenterAt = 0;  // glfwGetTime() of a recenter counting down; 0 = none
  // Settings (VideoSettings::Part) of the running video changed in a run of
  // small steps (a slider, a held stick, a drag), stored at glfwGetTime()
  // saveAt; 0 = none waiting.
  unsigned unsavedParts = 0;
  double saveAt = 0;
  bool screenChanged = false;  // by key: keep the flat screen's new size
  bool flipVideo = false;
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

void printUsage(const char* argv0) {
  std::printf("Usage: %s [--hwdec=API] [--preview-hwdec=API] [--no-mirror] [--timing] [--xr-test]\n"
              "          [--fixed-view] [--open-menu=PAGE] [--foveation=LEVEL] [--foveation-gaze=WHERE]\n"
              "          [--resolution=SIZE] [--resolution-cycle=SIZES[:SECONDS]] [--sharp-area=WHAT]\n"
              "          [--video-pipeline=on|off] [--decoder-queue=on|off] [--anti-shimmer=off|on|strong]\n"
              "          [--show-performance] [--menu-sharpness=BIAS]\n"
              "          [--show-foveation]\n"
              "          [--sharp-bins=1|3] [--ring=on|off]\n"
              "          [--first-run] [--stash-sync] [--save-diagnostics] [--version]\n"
              "          [video file or URL]\n"
              "Without a video the file browser opens. --stash-sync syncs from Stash once, without\n"
              "the headset, and prints what it found and how long it took. --save-diagnostics saves\n"
              "the logs and settings, anonymised, in one zip in Downloads and prints its path.\n"
              "--first-run shows the first-run setup, changing nothing in the profile until it ends.\n"
              "For repeatable measurements without anyone wearing the headset: --fixed-view draws\n"
              "the scene in front of the view wherever the headset points; --open-menu opens the\n"
              "menu at start on PAGE: strip, browser, settings, or seek (the strip with the pointer\n"
              "sweeping along the seek bar, for the seek preview).\n"
              "For this run only: --resolution=PIXELS or PERCENT%% (eye image width; 100%% is 1x);\n"
              "--resolution-cycle=PIXELS,PIXELS,...[:SECONDS] changes the eye width to each in turn\n"
              "every SECONDS (10 by default), to compare sizes or measure the switches.\n"
              "--anti-shimmer=off|on|strong (the setting: fine lines smoothed with mipmaps off).\n"
              "--show-performance shows frame rates and dropped frames at the bottom left (the\n"
              "setting Show performance).\n"
              "--menu-sharpness=BIAS adds BIAS (-2 to 1) to the mip level the menu is drawn from;\n"
              "negative is sharper, 0 the default.\n"
              "Steam Frame, for this run only: --foveation=off|low|medium|high; --foveation-gaze=\n"
              "tracked (SteamVR's eye tracking), centre, or X,Y (a fixed point, -1..1, +y down);\n"
              "--sharp-area=bins (whole bins around the gaze, our density maps; the default where\n"
              "the GPU's bins are known) or runtime (SteamVR's density maps).\n"
              "--video-pipeline=on shows each new video frame from the next headset frame on, so\n"
              "drawing never waits for its decoding and conversion (on by default with\n"
              "--hwdec=v4l2m2m); off shows it in the frame it arrives in.\n"
              "--decoder-queue=off disables the Frame's two-frame V4L2 decoder queue for this run.\n",
              argv0);
  std::printf("%s\n", strings().refreshRateHelp);
}

void printControls() {
  std::printf(
      "\nControllers:\n"
      "  Trigger        click in menu, hold on a list to drag it; elsewhere hold and\n"
      "                 move to drag the picture\n"
      "                 (sideways, up/down; twist the wrist to level the horizon)\n"
      "  Left menu / B  open or close menu (Frame: right B, X or Y)\n"
      "  A              play / pause\n"
      "  Stick click    recenter (right stick; also left Y)\n"
      "  Right stick    left/right: seek (hold to go faster)\n"
      "  Left stick     left/right: turn the view by 30 degrees\n"
      "  Stick up/down  scroll lists; flat screen: distance (with a grip held: height);\n"
      "                 180°/360°: move toward or away from the picture (also while dragging)\n"
      "  Grip           hold to move the screen or the menu; on a screen corner: resize it;\n"
      "                 180°/360° on the Frame: hold one and twist that wrist: opacity\n"
      "  Grip + trigger hold both and twist the wrist: volume\n"
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
      "  T            passthrough on / off (the room in green, for the Frame's camera)\n"
      "  W/S          screen closer / farther\n"
      "  Q/E          screen lower / higher (its height; the width follows)\n"
      "  R            recenter in front of you\n"
      "  F            flip video vertically\n"
      "  Esc          leave the search keyboard\n"
      "  Esc Esc      quit (press twice)\n\n");
}

void printFormat(const VideoFormat& f, const char* reason) {
  std::printf("Format (%s): %s, %s%s%s\n", reason, projectionName(f.projection), stereoName(f.stereo),
              f.packing != Packing::Auto ? ", packing " : "",
              f.packing != Packing::Auto ? packingName(f.packing) : "");
}

// Text from the desktop clipboard, on one line.
std::string clipboardText(GLFWwindow* window) {
  const char* clip = glfwGetClipboardString(window);
  std::string text = clip ? clip : "";
  std::replace(text.begin(), text.end(), '\n', ' ');
  std::replace(text.begin(), text.end(), '\r', ' ');
  return text;
}

void onKey(GLFWwindow* window, int key, int /*scancode*/, int action, int mods) {
  App& app = *static_cast<App*>(glfwGetWindowUserPointer(window));
  if (app.textInput) {
    // Letters arrive through onChar; only editing keys matter here.
    if (action == GLFW_RELEASE) return;
    if (key == GLFW_KEY_V && (mods & GLFW_MOD_CONTROL)) app.typed += clipboardText(window);
    else if (key == GLFW_KEY_BACKSPACE) ++app.backspaces;
    else if (key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT) app.cursorSteps += key == GLFW_KEY_LEFT ? -1 : 1;
    else if (key == GLFW_KEY_HOME || key == GLFW_KEY_END) app.cursorSteps = key == GLFW_KEY_HOME ? -(1 << 20) : 1 << 20;
    else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) app.submit = true;
    else if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) app.stopTyping = true;
    else if (key == GLFW_KEY_TAB && action == GLFW_PRESS) app.toggleMenu = true;
    return;
  }
  // Seeking repeats on its own while the key is held, so track press/release.
  if (key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT) {
    const bool down = action != GLFW_RELEASE;
    (key == GLFW_KEY_LEFT ? app.seekBackKey : app.seekForwardKey) = down;
    return;
  }
  if (key == GLFW_KEY_A || key == GLFW_KEY_D) {
    const bool down = action != GLFW_RELEASE;
    (key == GLFW_KEY_A ? app.turnLeftKey : app.turnRightKey) = down;
    return;
  }
  if (action != GLFW_PRESS) return;

  VideoPlayer& player = *app.player;
  ScreenPlacement& screen = app.screen;
  switch (key) {
    case GLFW_KEY_ESCAPE: {
      // Quitting takes two presses, so a stray Esc doesn't end the video.
      const double now = glfwGetTime();
      if (now - app.lastEscape < 1.5) {
        app.quit = true;
      } else {
        app.lastEscape = now;
        std::printf("Press Esc again to quit\n");
      }
      break;
    }
    case GLFW_KEY_TAB: app.toggleMenu = true; break;
    case GLFW_KEY_SPACE: player.command("cycle pause"); break;
    case GLFW_KEY_UP: app.videoStep = 1; break;
    case GLFW_KEY_DOWN: app.videoStep = -1; break;
    case GLFW_KEY_M: player.command("cycle mute"); break;
    case GLFW_KEY_P:
      app.format.projection = nextProjection(app.format.projection);
      app.rememberFormat = true;
      printFormat(app.format, "manual");
      break;
    case GLFW_KEY_L:
      app.format.stereo = nextStereo(app.format.stereo);
      app.rememberFormat = true;
      printFormat(app.format, "manual");
      break;
    case GLFW_KEY_C:
      screen.size.curve = screen.size.curve > 0 ? 0.0f : ScreenSize::kMaxCurve;
      app.screenChanged = true;
      break;
    case GLFW_KEY_T: app.togglePassthrough = true; break;
    case GLFW_KEY_R: app.recenter = true; break;
    case GLFW_KEY_F:
      app.flipVideo = !app.flipVideo;
      std::printf("Video flip: %s\n", app.flipVideo ? "on" : "off");
      break;
    case GLFW_KEY_W:
    case GLFW_KEY_S:
      screen.size.distance += key == GLFW_KEY_W ? -0.25f : 0.25f;
      app.screenChanged = true;
      break;
    case GLFW_KEY_Q:
    case GLFW_KEY_E:
      screen.size.height *= key == GLFW_KEY_Q ? 1 / 1.1f : 1.1f;
      app.screenChanged = true;
      break;
    default: break;
  }
}

// + and - by the typed character, so they work on any keyboard layout and
// on the number pad. Held keys repeat through the system's key repeat.
void onChar(GLFWwindow* window, unsigned int codepoint) {
  App& app = *static_cast<App*>(glfwGetWindowUserPointer(window));
  if (app.textInput) {
    app.typed += utf8(codepoint);
    return;
  }
  if (codepoint == '+') app.player->command("add volume 5");
  else if (codepoint == '-') app.player->command("add volume -5");
}

// Runs `job` on a thread of its own; dropping the future doesn't wait for it.
template <typename Job>
auto detached(Job job) {
  std::packaged_task<decltype(job())()> task(std::move(job));
  auto result = task.get_future();
  std::thread(std::move(task)).detach();
  return result;
}

// Why the playing file couldn't be deleted, as the delete dialog says it,
// and as the run log does.
std::string deleteError(DeleteStatus status, const std::string& error) {
  const DeleteStrings& t = strings().deletion;
  switch (status) {
    case DeleteStatus::Changed: return t.changed;
    case DeleteStatus::Gone: return t.gone;
    case DeleteStatus::NotAFile: return t.notAFile;
    case DeleteStatus::Denied: return t.denied;
    case DeleteStatus::InUse: return t.inUse;
    case DeleteStatus::Offline: return t.offline;
    default: return error;
  }
}
const char* deleteLogged(DeleteStatus status) {
  static const char* const names[] = {"deleted", "changed", "gone", "not a file", "not allowed",
                                      "in use", "server not answering", "failed"};
  return names[static_cast<int>(status)];
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
  Mat4 update(const App& app, float frameAspect, Vec3 headPos, float headYaw,
              const Settings& settings) {
    const ScreenPlacement& s = app.screen;
    const float scale = settings.subtitleSize / 100.0f;
    // 0 is where the user found them best: 30% of the picture lower than the
    // first placement (on the flat screen: below the picture).
    const float raise = (settings.subtitleRaise - 30) / 100.0f;
    if (app.format.projection == Projection::Flat) {
      const ScreenSurface screen(s, eyeDisplayAspect(app.format, frameAspect));
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

// --open-menu: the menu page opened at start.
enum class StartMenu { None, Strip, Browser, Settings, Seek };

struct Options {
  std::string file;
  std::string hwdec = "nvdec,nvdec-copy";
  std::string previewHwdec = "nvdec-copy";
  bool mirror = true;
  bool timing = false;
  float refreshRate = -1;  // --refresh-rate, never saved; -1 makes no request
  bool xrTest = false;
  bool fixedView = false;
  StartMenu startMenu = StartMenu::None;
  // Foveation for this run (--foveation, --foveation-gaze); never saved.
  bool foveationSet = false;
  FoveationLevel foveation = FoveationLevel::High;
  FoveationGaze foveationGaze;
  bool runtimeDensityMaps = false;  // --sharp-area=runtime
  // --resolution for this run, never saved: eye pixels across, or a percent
  // of 1× (eye_resolution.h); 0 = the setting's.
  int resolutionPixels = 0;
  double resolutionPercent = 0;
  // --resolution-cycle: eye pixels across, taken in turn every so many
  // seconds for this run (scripted switches, A/B comparison by eye).
  std::vector<int> resolutionCycle;
  double resolutionCycleSeconds = 10;
  // --video-pipeline: -1 = on with V4L2 decoding (the Frame), else 0/1.
  int videoPipeline = -1;
  bool decoderQueue = true;  // Frame V4L2 only; --decoder-queue=off for comparison
  // --anti-shimmer for this run, never saved.
  bool antiShimmerSet = false;
  AntiShimmer antiShimmer = AntiShimmer::On;
  bool showPerformance = false;  // --show-performance for this run, never saved
  bool showFoveation = false;  // --show-foveation, never saved
  float menuLodBias = 0.0f;    // --menu-sharpness
  int sharpBins = 0, ring = -1;  // run-only overrides
  bool firstRun = false;  // --first-run: setup whatever the profile has
};

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

int run(GLFWwindow* window, const Options& opts) {
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
  // Not on the Frame: no glass there (docs/frame-plan.md, W11).
  if (!kStandaloneHeadset && !renderer.initGlass(xr)) return 1;
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
  GLFWwindow* input = mirrorWindow ? mirrorWindow.get() : window;
  DirectVideo direct;  // before the player, which hands it frames
  if (requirements.directVideo && direct.init(gpu)) renderer.setDirectVideo(&direct);
  VideoPlayer player;
  QuitWatchdogArm quitWatchdogArm;
  if (!player.init(opts.hwdec, interop, opts.timing, opts.decoderQueue)) return 1;
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
  DiagnosticsExport diagnostics;
  SubtitleOverlay subtitles;
  if (!subtitles.init(uploader)) return 1;
  SubtitleOverlay loadingText;  // "Loading …" shown instead of the video while a file opens
  if (!loadingText.init(uploader)) return 1;
  // The first font lookup can be slow: draw this before playback starts.
  loadingText.setText(strings().loading);
  loadingText.texture();
  SeekPreview seekPreview;
  // Not on the Frame: a second full-size decoder beside playback fails there
  // (docs/frame-plan.md, W4). Without init() it never shows.
  if (!kStandaloneHeadset &&
      !seekPreview.init(interop, opts.hwdec == "no" ? "no" : opts.previewHwdec, uploader))
    return 1;
  GazeNotice countdownNotice(0);  // "Recentering in 2 …"
  // "Viewpoint: 15 % closer" while the stick moves it, or "Opacity: 70 %"
  // while a twist changes it, whichever changed last.
  GazeNotice valueNotice(1);
  GazeNotice passthroughNotice(2);  // "Passthrough on: green screen video"
  GazeNotice driveNotice(3);        // "microSD card: HOLIDAY", "Now in the file explorer"
  if (!countdownNotice.init(uploader) || !valueNotice.init(uploader) ||
      !passthroughNotice.init(uploader) || !driveNotice.init(uploader))
    return 1;
  // microSD cards and USB drives; on the Frame the app mounts those SteamOS
  // leaves alone.
  RemovableDrives drives;
  drives.start(kStandaloneHeadset);
  std::string driveText;
  double driveShownUntil = 0;
  double viewpointShownUntil = 0;  // glfwGetTime() it goes away
  double opacityShownUntil = 0;    // ... and the opacity's
  double softwareSince = -1;       // the video has played in software decoding since
  // Passthrough switching itself on for videos made for it (see_through.h):
  // once per video, when its size and settings are known.
  GreenScreenCheck greenCheck;
  bool passthroughSettled = false;
  double greenCheckAt = 0;  // glfwGetTime() the check starts, a moment after the video opens
  const char* passthroughWhy = nullptr;  // the notice's first line
  double passthroughShownUntil = 0;
  // The colour balance holds for every video until the player closes: the
  // room's light (Match the room) belongs to the room, not to a video.
  ColourBalance colourBalance;
  RoomLight roomLight;
  const bool roomReadable = kStandaloneHeadset && RoomLight::available();
  double roomFailedUntil = 0;  // glfwGetTime() Match the room stops saying it failed
  // Match the room, a switch: on, the room's reading (the run's first, or
  // Read again's) times the strengths in the sliders; off, the sliders as
  // they were before it went on. Moved by hand while on, they stay as moved
  // until the match is applied again (a strength, Read again). A reading is
  // kept when it ends, but applied only if the sliders were not moved by
  // hand or reset after it was asked for.
  std::optional<RoomLight::Reading> roomReading;  // this run's latest
  bool roomMatched = false;
  ColourBalance beforeMatch;
  unsigned balanceEdits = 0, roomAsked = 0;  // hand moves and resets; their count when last asked
  // A new reading, unless one runs (which keeps the count it was asked with).
  auto readRoom = [&] {
    if (roomLight.start()) roomAsked = balanceEdits;
  };
  auto applyRoomMatch = [&] {
    if (roomMatched && roomReading)
      colourBalance.match(roomReading->light, roomReading->colour, roomReading->white,
                          settings.matchColour / 100.0f, settings.matchBrightness / 100.0f);
  };

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
  autofocus.init();
  FocusTargetSelector focusTargets;  // head direction or eye gaze
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
  seekPreview.setEnabled(settings.seekPreview);
  FolderMemory folders;
  folders.load();
  menu.setFolderMemory(&folders);
  menu.setStartDirectory(settings.lastFolder);
  menu.setGallery(settings.gallery);
  menu.setKeyboardLayout(settings.keyboardLayout, settings.keyboardOther);

  App app;
  app.player = &player;
  glfwSetWindowUserPointer(input, &app);
  glfwSetKeyCallback(input, onKey);
  glfwSetCharCallback(input, onChar);
  printControls();

  // Deleting the playing file (docs/alpha/delete-file.md): what the dialog
  // shows, looked up in the background (a slow share must not freeze the
  // headset), then the deletion, one at a time.
  struct Deletion {
    // The lookup's: the file's stamp (none for Stash's stream), and with
    // Send to Stash on, its oshash and its scene's files as Stash has them.
    struct Look {
      DeleteStatus status = DeleteStatus::Ok;
      FileStamp stamp;
      std::string oshash;
      bool asked = false;
      StashFileLook stash;
    };
    DeleteInfo info;
    // What is deleted: the file playing, or a copy's source (with the
    // copy), a scene's stream locator for one from Stash's; empty: a source
    // on another Stash server.
    std::string target;
    FileStamp stamp;  // the file's as the dialog shows it
    int64_t scene = 0;  // the Stash scene it is a file of; 0: none
    size_t sceneFiles = 0;
    std::string stashPath;      // Stash's path of it; empty: not known for sure
    std::string oshash;         // the file's, as the lookup read it
    // The oshash the file deleted must have: the download's for a copy's
    // source, the one opened for a stream; empty: any (a file here).
    std::string expected;
    bool throughStash = false;  // Stash deletes it: Send to Stash on, its file found
    std::future<Look> lookup;   // dropped without waiting
    std::future<DeleteOutcome> result;
    bool resume = false;  // the video played when the button was pressed
    int64_t copy = 0;     // the download whose copy plays
  } deletion;
  // Holding the grip on the menu moves it with the hand; elsewhere the grip
  // holds the flat screen (screenControl).
  struct MenuGrab {
    int hand = -1;  // -1 = not held
    Mat4 startInverse;  // inverse of the hand pose at the grab
    Vec3 startPoint;    // the menu's front at the grab
  } menuGrab;
  ScreenControl screenControl;
  WristRoll volumeRoll;
  double rolledVolume = -1;  // the volume the wrist has set so far, -1 = not rolling
  GripTwist opacityTwist;
  // The opacity the wrist has set so far, beyond 0..100 by up to
  // kOpacityHold at a limit; none while not twisting.
  std::optional<double> twistedOpacity;
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
  uint64_t hintBindingsSeen = 0;  // xr.bindingsVersion() the menu's hints have
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
  uint64_t bindingsSeen = 0;    // xr.bindingsVersion() setup has
  StartMenu startMenu = opts.startMenu;  // ... or on this page, then
  int activeHand = 1;  // the hand whose pointer drives the menu
  float laserLength = kLaserStartLength;
  int knownWidth = 0, knownHeight = 0;
  // The stick's seek (HeldSeek): from where, whether it paused the video,
  // and whether the bar shows where it goes until the video is there.
  HeldSeek heldSeek;
  double heldSeekFrom = 0, heldSeekTarget = 0;
  bool heldSeekPaused = false, heldSeekLanding = false;
  bool heldSeekBar = false;  // the strip placed for it, the menu closed
  double heldSeekPicture = 0, heldSeekPictureAt = 0;  // the last picture's target, when sent
  double heldSeekBarUntil = 0;  // ... shown after the stick let go
  // Driven by the stick (not the arrow keys), it buzzes: a tick each 10 s
  // it goes, at most 8 a second, and a bump at the start or the end.
  bool heldSeekStick = false, heldSeekAtLimit = false;
  double heldSeekTens = 0, heldSeekTickAt = 0;
  HoldRepeat turnLeft(0.5, 0.35), turnRight(0.5, 0.35);
  HoldRepeat cursorLeft(0.5, 0.07), cursorRight(0.5, 0.07);  // as the keyboard's keys repeat
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
    library.setResume(app.currentFile, point);
  };
  auto keepResume = [&] {
    saveResume();
    videoOpened = false;
  };
  // Stores these parts (VideoSettings::Part) of the running video's
  // settings: only what the user changed, so the rest of a record still
  // being found stays as it is.
  auto storeVideo = [&](unsigned parts) { library.setSettings(app.currentFile, app.video, parts); };
  // Keeps the volume for all videos, as mpv reports it: a second after it
  // changes, so at most once a second while it keeps changing. Not before
  // mpv has reported the one set at start, so its default can't overwrite it.
  bool volumeApplied = false;
  double volumeSaveAt = 0;
  auto keepVolume = [&] {
    volumeSaveAt = 0;
    const int volume = static_cast<int>(std::lround(player.playback().volume));
    if (!volumeApplied || volume == settings.volume) return;
    settings.volume = volume;
    saveSettings(settings);
  };
  // Stores these parts soon, with whatever else changes meanwhile: at most
  // twice a second while they keep changing.
  auto storeVideoSoon = [&](unsigned parts) {
    app.unsavedParts |= parts;
    if (app.saveAt == 0) app.saveAt = glfwGetTime() + 0.5;
  };
  // Stores what storeVideoSoon() still holds back.
  auto saveUnsaved = [&] {
    // The screen's size is a setting for all videos (kScreen only marks it).
    if (const unsigned parts = app.unsavedParts & ~VideoSettings::kScreen) storeVideo(parts);
    if (app.unsavedParts & VideoSettings::kScreen) saveSettings(settings);  // settings.screen
    app.unsavedParts = 0;
    app.saveAt = 0;
  };
  // The flat screen's size was changed: for all videos.
  auto keepScreenSize = [&] {
    app.screen.size = app.screen.size.clamped();
    settings.screen = app.screen.size;
    storeVideoSoon(VideoSettings::kScreen);
  };
  // Passthrough switched on for a video made for it, saying so the first time ever.
  auto passthroughByItself = [&](AutoPassthrough why) {
    if (why == AutoPassthrough::None || app.video.passthrough) return;
    app.video.passthrough = true;
    if (why == AutoPassthrough::GreenScreen && app.video.greenScreen.isDefault())
      app.video.greenScreen.on = true;
    passthroughWhy = why == AutoPassthrough::Mask ? strings().autoPassthrough.mask
                                                  : strings().autoPassthrough.greenScreen;
    if (!settings.passthroughNoticeSeen) {
      passthroughShownUntil = glfwGetTime() + kPassthroughNoticeShown;
      settings.passthroughNoticeSeen = true;
      saveSettings(settings);
    }
    std::printf("Passthrough: on by itself (%s)\n",
                why == AutoPassthrough::Mask ? "mask" : "green screen");
  };  // Puts the running video's remembered settings into effect.
  auto applyVideoSettings = [&] {
    if (app.video.hasFormat) {
      app.format = app.video.format;
      app.formatChosenByUser = true;
      printFormat(app.format, "remembered");
    }
    app.screen.zoom = app.video.zoom;
    app.screen.size = settings.screen;
    player.setPicture(app.video.brightness, app.video.contrast, app.video.saturation);
    player.setTvGamma(settings.tvGamma);
  };
  // Playback moves on (another video, or none): a delete dialog still
  // asking goes, with its lookup, so its confirm can't act on what came
  // next. One confirmed (Deleting) stays.
  auto dropDeleteDialog = [&] {
    deletion.lookup = {};
    deletion.resume = false;
    menu.cancelDelete();
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
        std::printf("Stash scene not available\n");
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
    saveUnsaved();
    keepResume();
    dropDeleteDialog();
    app.currentFile = path;
    app.streamOshash = isStashScene(path) ? oshash : std::string();
    app.formatChosenByUser = false;
    app.format = guessVideoFormat(path, 0, 0);
    // Its settings from the library, else as kept before it (videos.conf),
    // which the library takes over. A file the library doesn't know yet may
    // be a moved one with settings of its own: they apply once it is found.
    app.video = VideoSettings{};
    openingRecord = library.record(path).id;
    if (!isPhotoFile(path)) library.open(path);
    awaitingRecord = library.available() && !library.known(path);
    if (!library.settings(path, app.video)) {
      VideoSettings legacy;
      if (memory.get(path, legacy)) {
        // Queue the fallback before user edits can create settings. A known
        // path must still resolve to the same identity before it can use it.
        library.carryOverSettings(path, legacy, awaitingRecord ? openingRecord : 0);
        if (!openingRecord || !awaitingRecord) app.video = legacy;
      }
    }
    awaitingResume = awaitingRecord && settings.resume && !isStream(path) && !isPhotoFile(path);
    resolvedResume = -1;
    passthroughSettled = false;
    greenCheckAt = 0;
    passthroughShownUntil = 0;
    opacityTwist.cancel();  // a twist under way was the last video's
    opacityShownUntil = 0;
    applyVideoSettings();
    knownWidth = knownHeight = 0;  // re-detect the format once the size is known
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
      heldSeek.cancel();
      heldSeekPaused = heldSeekLanding = heldSeekBar = false;
      player.load(path, std::max(0.0, start));
      if (start > 0) std::printf("Resuming at %s\n", clockTime(start).c_str());
    }
    durationKept = false;
    // As for links, Stash's stream opens its preview at once: the first one
    // pointed at then comes about 0.4 s sooner over a LAN.
    seekPreview.videoChanged(path, isStream(path) || isStashScene(path));
    app.nextSlideAt = glfwGetTime() + app.slideshowSeconds;  // each photo gets its full time
    return app.video.hasFormat;
  };
  // Closes the running video or photo and leaves the scene empty, keeping
  // where it was left as switching to another would.
  auto closeVideo = [&] {
    if (app.currentFile.empty()) return;
    dropDeleteDialog();
    saveUnsaved();
    keepResume();
    player.stop();
    heldSeek.cancel();
    heldSeekPaused = heldSeekLanding = heldSeekBar = false;
    seekPreview.close();
    app.currentFile.clear();
    app.streamOshash.clear();
    app.video.passthrough = false;
    app.video.opacity = 100;  // the empty scene isn't blended; the video keeps its saved value
    opacityTwist.cancel();
    opacityShownUntil = 0;
    neighbor = {};
    nextPhoto = {};
    neighborStep = 0;
    awaitingRecord = false;
    awaitingResume = false;
  };
  // Stores the current format for this video (after the user chose it).
  auto rememberFormat = [&] {
    app.formatChosenByUser = true;
    app.video.hasFormat = true;
    app.video.format = app.format;
    storeVideo(VideoSettings::kFormat);
  };
  unsigned pendingPicture = 0;
  auto applyPicture = [&](unsigned parts) {
    if (parts != VideoSettings::kZoom) {
      // mpv's picture properties can stall; the held slider shows the choice until release.
      if (menu.draggingSlider() != MenuCommand::None && !isPhotoFile(app.currentFile)) {
        pendingPicture |= parts;
      } else {
        player.setPicture(app.video.brightness, app.video.contrast, app.video.saturation);
        pendingPicture = 0;
      }
    }
    app.screen.zoom = app.video.zoom;
    storeVideo(parts);
  };
  // A Stash scene given on the command line opens once Stash's catalog is there.
  std::string startScene = isStashScene(opts.file) ? opts.file : "";
  if (!opts.file.empty() && startScene.empty()) openVideo(opts.file);
  auto stepVideo = [&](int step) {
    if (app.currentFile.empty() || isStream(app.currentFile)) return;
    // Opened from a list in the menu (Stash's scenes): through that list.
    std::string next;
    if (!menu.listNeighbor(openedAs, step, next)) {
      neighborStep = neighbor.valid() && !neighborWrap ? neighborStep + step : step;
      neighborWrap = false;
      neighbor = mediaNeighbor(app.currentFile, neighborStep);
      return;
    }
    if (next.empty())
      std::printf("No %s video in this folder\n", step > 0 ? "next" : "previous");
    else
      openVideo(next);
  };

  // The Resolution row's choice: shown at once, switched to (and saved)
  // once it settles: on letting go of its slider, or a pause after its
  // last click, so several clicks make one switch. 0 = none.
  constexpr double kResolutionSettle = 0.35;
  int pendingResolution = 0;
  double pendingResolutionAt = 0;
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

  auto execute = [&](const MenuCommand& c) {
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
        break;
      case MenuCommand::PreviousVideo: stepVideo(-1); break;
      case MenuCommand::NextVideo: stepVideo(1); break;
      case MenuCommand::SetProjection:
        app.format.projection = static_cast<Projection>(static_cast<int>(c.value));
        rememberFormat();
        printFormat(app.format, "manual");
        break;
      case MenuCommand::SetStereo:
        app.format.stereo = static_cast<StereoLayout>(static_cast<int>(c.value));
        rememberFormat();
        printFormat(app.format, "manual");
        break;
      case MenuCommand::SetPacking:
        app.format.packing = static_cast<Packing>(static_cast<int>(c.value));
        rememberFormat();
        printFormat(app.format, "manual");
        break;
      case MenuCommand::SetBrightness:
        app.video.brightness = static_cast<int>(c.value);
        applyPicture(VideoSettings::kBrightness);
        break;
      case MenuCommand::SetContrast:
        app.video.contrast = static_cast<int>(c.value);
        applyPicture(VideoSettings::kContrast);
        break;
      case MenuCommand::SetSaturation:
        app.video.saturation = static_cast<int>(c.value);
        applyPicture(VideoSettings::kSaturation);
        break;
      case MenuCommand::SetWarmth:  // the colour balance: applied while drawing
        colourBalance.warmth = static_cast<int>(c.value);
        ++balanceEdits;
        break;
      case MenuCommand::SetTint:
        colourBalance.tint = static_cast<int>(c.value);
        ++balanceEdits;
        break;
      case MenuCommand::SetExposure:
        colourBalance.exposure = static_cast<int>(c.value);
        ++balanceEdits;
        break;
      case MenuCommand::ToggleMatchRoom:
        roomFailedUntil = 0;
        roomMatched = !roomMatched;
        if (!roomMatched) {
          colourBalance = beforeMatch;
        } else {
          beforeMatch = colourBalance;
          if (roomReading) applyRoomMatch();
          else readRoom();
        }
        break;
      case MenuCommand::ReadRoomAgain:
        roomFailedUntil = 0;
        if (roomMatched) readRoom();
        break;
      case MenuCommand::SetMatchColour:
        settings.matchColour = std::clamp(static_cast<int>(c.value), 0, 100);
        saveSettings(settings);
        applyRoomMatch();
        break;
      case MenuCommand::SetMatchBrightness:
        settings.matchBrightness = std::clamp(static_cast<int>(c.value), 0, 100);
        saveSettings(settings);
        applyRoomMatch();
        break;
      case MenuCommand::SetOpacity:  // applied while drawing
        app.video.opacity = static_cast<int>(c.value);
        storeVideoSoon(VideoSettings::kOpacity);
        break;
      case MenuCommand::SetZoom:
        app.video.zoom = static_cast<float>(c.value / 100.0);
        applyPicture(VideoSettings::kZoom);
        break;
      case MenuCommand::ToggleTvGamma:
        settings.tvGamma = !settings.tvGamma;
        player.setTvGamma(settings.tvGamma);
        matchPreset();
        saveSettings(settings);
        break;
      case MenuCommand::ResetPicture:
        app.video.brightness = app.video.contrast = app.video.saturation = 0;
        colourBalance = {};
        roomMatched = false;
        ++balanceEdits;
        app.video.zoom = 1.0f;
        app.video.originForward = 0;
        applyPicture(VideoSettings::kPicture | VideoSettings::kOrigin);
        break;
      case MenuCommand::SetOriginForward:
        app.video.originForward = std::clamp(static_cast<float>(c.value / 100.0),
                                             -VideoSettings::kMaxOriginForward,
                                             VideoSettings::kMaxOriginForward);
        storeVideoSoon(VideoSettings::kOrigin);
        break;
      case MenuCommand::Paste: menu.typeText(clipboardText(input)); break;
      case MenuCommand::SetSlideshow:
        app.slideshowSeconds = static_cast<int>(c.value);
        if (!app.slideshowSeconds && neighborWrap) neighbor = {};
        app.nextSlideAt = glfwGetTime() + app.slideshowSeconds;
        break;
      case MenuCommand::SetTheme:
        settings.theme = static_cast<Theme>(static_cast<int>(c.value));
        saveSettings(settings);
        break;
      case MenuCommand::SetAccent:
      case MenuCommand::SetAccentHue:
      case MenuCommand::SetAccentBrightness:
      case MenuCommand::SetAccentSaturation: {
        Accent& accent = settings.accent;
        const int value = static_cast<int>(c.value);
        if (c.type == MenuCommand::SetAccent) accent.preset = std::clamp(value, 0, kAccentCustom);
        else if (c.type == MenuCommand::SetAccentHue) accent.hue = (value % 360 + 360) % 360;
        else if (c.type == MenuCommand::SetAccentBrightness) accent.brightness = std::clamp(value, 0, 100);
        else accent.saturation = std::clamp(value, 0, 100);
        saveSettings(settings);
        break;
      }
      case MenuCommand::SetScreenDistance:
      case MenuCommand::SetScreenHeight:
      case MenuCommand::SetScreenCurve: {
        ScreenSize& size = app.screen.size;
        (c.type == MenuCommand::SetScreenDistance ? size.distance
         : c.type == MenuCommand::SetScreenHeight ? size.height
                                                  : size.curve) = static_cast<float>(c.value / 100);
        keepScreenSize();
        break;
      }
      case MenuCommand::ResetScreen:
        app.screen.size = settings.screen = ScreenSize{};
        storeVideoSoon(VideoSettings::kScreen);
        break;
      case MenuCommand::Recenter:
        // From the menu: two seconds to look where "front" should be.
        app.recenterAt = glfwGetTime() + 2.0;
        break;
      case MenuCommand::ToggleAutoPassthrough:
        settings.autoPassthrough = !settings.autoPassthrough;
        saveSettings(settings);
        passthroughSettled = false;  // for the video playing too
        break;
      case MenuCommand::ToggleBackground:
        settings.background = !settings.background;
        saveSettings(settings);
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
      case MenuCommand::ToggleAdvancedSettings:
        settings.advancedSettings = !settings.advancedSettings;
        saveSettings(settings);
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
        // Connected in setup, as Settings → Stash sets it, and setup ends
        // there: its first sync starts at once, and its folders are looked
        // for once it is done, in the places ticked too.
        const size_t line = c.path.find('\n');
        const std::string url = c.path.substr(0, line);
        const std::string key = line == std::string::npos ? "" : c.path.substr(line + 1);
        const bool same = stash && settings.stash.url == url && settings.stash.apiKey == key;
        settings.stash.enabled = true;
        settings.stash.url = url;
        settings.stash.apiKey = key;
        settings.stash.card = false;
        stashInSetup = true;
        saveSettings(settings);
        if (!same) restartStash();
        std::printf("Setup: Stash connected\n");
        finishSetup(true);
        break;
      }
      case MenuCommand::DismissSetupBanner:
        hideSetupBanner();
        break;
      case MenuCommand::StashCard:
        // The menu's "Use Stash?" card, gone for good: dismissed, or used
        // (Stash on, Settings → Stash open at its address).
        settings.stash.card = false;
        if (c.value == 1 && !settings.stash.enabled) {
          settings.stash.enabled = true;
          restartStash();
        }
        saveSettings(settings);
        break;
      case MenuCommand::SetKeyboardLayout: {
        const size_t line = c.path.find('\n');
        settings.keyboardLayout = c.path.substr(0, line);
        settings.keyboardOther = line == std::string::npos ? "" : c.path.substr(line + 1);
        saveSettings(settings);
        break;
      }
      case MenuCommand::ToggleShowFoveation:
        if (!foveation.active()) break;
        settings.showFoveation = showFoveation = !showFoveation;
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
      case MenuCommand::ToggleMotion:
        settings.motionEmulation = !settings.motionEmulation;
        saveSettings(settings);
        break;
      case MenuCommand::SetMotionDistance:
        settings.motionDistance = clampMotionDistance(static_cast<int>(c.value));
        saveSettings(settings);
        break;
      case MenuCommand::SetSharpen:
        settings.sharpen = std::clamp(static_cast<int>(c.value), 0, 300);
        player.setSharpened(settings.sharpen > 0);
        matchPreset();
        saveSettings(settings);
        break;
      case MenuCommand::ToggleToneLook:
        settings.toneLook = !settings.toneLook;
        matchPreset();
        saveSettings(settings);
        break;
      case MenuCommand::ToggleResume:
        settings.resume = !settings.resume;
        saveSettings(settings);
        break;
      case MenuCommand::SetDownloadsTo:
        settings.downloadsTo = c.path;
        saveSettings(settings);
        break;
      case MenuCommand::ToggleSeekPreview:
        settings.seekPreview = !settings.seekPreview;
        saveSettings(settings);
        seekPreview.setEnabled(settings.seekPreview);
        break;
      case MenuCommand::ToggleThumbnails:
        settings.thumbnails = !settings.thumbnails;
        saveSettings(settings);
        break;
      case MenuCommand::ToggleShowPerformance:
        settings.showPerformance = !showPerformance;
        saveSettings(settings);
        switchReadout(settings.showPerformance);
        break;
      case MenuCommand::ToggleVibration:
        settings.vibration = !settings.vibration;
        saveSettings(settings);
        break;
      case MenuCommand::TogglePassthrough:
        app.togglePassthrough = true;
        break;
      case MenuCommand::TogglePackedMask: {
        // Nothing is stored when the choice is what the name says.
        const bool on = !packedMask(app.currentFile, app.video);
        app.video.packedMask = on == hasPackedAlpha(app.currentFile) ? std::nullopt
                                                                     : std::optional<bool>(on);
        storeVideo(VideoSettings::kPackedMask);
        std::printf("Passthrough: mask in the video %s\n", on ? "on" : "off");
        break;
      }
      case MenuCommand::ToggleGreenScreen:
      case MenuCommand::SetKeyStrength:
      case MenuCommand::SetKeySoftness:
      case MenuCommand::SetKeySpill:
      case MenuCommand::SetKeyShrink: {
        GreenScreen& key = app.video.greenScreen;
        const int value = static_cast<int>(c.value);
        if (c.type == MenuCommand::ToggleGreenScreen) key.on = !key.on;
        else if (c.type == MenuCommand::SetKeyStrength) key.strength = value;
        else if (c.type == MenuCommand::SetKeySoftness) key.softness = value;
        else if (c.type == MenuCommand::SetKeyShrink) key.shrink = value;
        else key.spill = value;
        storeVideoSoon(VideoSettings::kGreenScreen);
        break;
      }
      case MenuCommand::ResetPassthrough: {
        // What the group shows: the key's sliders only with passthrough on,
        // Match the room's strengths only with advanced settings on.
        if (roomReadable && settings.advancedSettings) {
          const Settings defaults;
          settings.matchColour = defaults.matchColour;
          settings.matchBrightness = defaults.matchBrightness;
          saveSettings(settings);
          applyRoomMatch();
        }
        unsigned parts = VideoSettings::kOpacity;
        if (app.video.passthrough) {
          GreenScreen& key = app.video.greenScreen;
          const bool on = key.on;
          key = GreenScreen{};
          key.on = on;
          parts |= VideoSettings::kGreenScreen;
        }
        app.video.opacity = 100;
        storeVideoSoon(parts);
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
        if (applyFocusControl(static_cast<FocusControl>(c.option), c.value, app.video.hasFocus,
                              app.video.focus, settings.focus, settings.focusPrefs)) {
          matchPreset();  // a user's preset keeps the focus defaults
          saveSettings(settings);
        }
        storeVideo(VideoSettings::kFocus);
        break;
      case MenuCommand::Quit: app.quit = true; break;
      case MenuCommand::OpenFile:
        openVideo(c.path);
        panel.open = false;
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
      case MenuCommand::ToggleFloatingSubtitles:
        settings.floatingSubtitles = !settings.floatingSubtitles;
        saveSettings(settings);
        break;
      case MenuCommand::RequestDelete: {
        // The dialog is open; the video pauses meanwhile, a slideshow stops.
        if (c.path != app.currentFile || deletion.result.valid()) break;
        deletion.info = {};
        deletion.info.file = c.path;
        deletion.info.name = std::filesystem::path(c.path).filename().string();
        deletion.info.photo = isPhotoFile(c.path);
        deletion.expected.clear();
        // A downloaded copy plays instead of its source (openVideo).
        if (const Offline::Download* d = offline.find(c.path);
            d && d->state == Offline::State::Done && d->present && d->path == c.path) {
          deletion.info.source = d->source;
          deletion.info.copyRoot = d->root;
          deletion.info.size = d->size;
          deletion.copy = d->id;
          deletion.expected = d->oshash;
        }
        const std::string& source = deletion.info.source;
        deletion.target = source.empty()          ? c.path
                          : isStashSource(source) ? stashSourceLocator(source)
                                                  : source;
        const bool stream = isStashScene(deletion.target);
        if (stream && source.empty()) deletion.expected = app.streamOshash;
        // A Stash scene's file, at the scene's location (a file here or its
        // stream), never by oshash alone.
        deletion.scene = 0;
        deletion.sceneFiles = 0;
        deletion.stashPath.clear();
        deletion.oshash.clear();
        deletion.throughStash = false;
        if (const auto catalog = stash ? stash->catalog() : nullptr; catalog && !deletion.target.empty())
          if (const int i = stashSceneAt(*catalog, deletion.target); i >= 0) {
            const StashScene& s = catalog->scenes()[i];
            deletion.scene = s.id;
            deletion.sceneFiles = s.oshashes.size();
            deletion.stashPath = stashFilePathAt(*catalog, deletion.target);
            if (isStashScene(deletion.target) && source.empty()) deletion.info.size = s.size;
          }
        deletion.resume = !deletion.info.photo && !player.playback().paused;
        if (deletion.resume) player.command("set pause yes");
        app.slideshowSeconds = 0;
        deletion.lookup = detached([file = deletion.target, stream, check = stream || !source.empty(),
                                    expected = deletion.expected,
                                    ask = settings.stash.writeBack && deletion.scene != 0,
                                    scene = deletion.scene, stashPath = deletion.stashPath,
                                    url = settings.stash.url, key = settings.stash.apiKey] {
          Deletion::Look r;
          if (file.empty())
            r.status = DeleteStatus::Gone;
          else if (!stream)
            r.status = stampOf(file, r.stamp);
          if (r.status != DeleteStatus::Ok || (!ask && !check)) return r;
          r.oshash = stream ? expected : fingerprintFile(file).oshash;
          // A copy's source still the video downloaded; a stream the one
          // opened (Stash's fresh query below matches it).
          if (check && (r.oshash.empty() || r.oshash != expected)) r.status = DeleteStatus::Changed;
          if (r.status != DeleteStatus::Ok || !ask || r.oshash.empty()) return r;
          r.asked = true;
          r.stash = lookUpStashFile(url, key, scene, r.oshash, stashPath);
          return r;
        });
        break;
      }
      case MenuCommand::CancelDelete:
        deletion.lookup = {};
        if (deletion.resume && app.currentFile == deletion.info.file) player.command("set pause no");
        deletion.resume = false;
        break;
      case MenuCommand::DeleteFile: {
        // Only the file the dialog showed, as it showed it.
        if (!deletion.info.ready || c.path != deletion.info.file || c.path != app.currentFile ||
            deletion.result.valid())
          break;
        deletion.resume = false;
        const std::string target = deletion.target;
        // Stash deletes it only with Send to Stash on still; its stream
        // only through Stash; a copy's source only if it is still the
        // video downloaded.
        const bool throughStash = deletion.throughStash && settings.stash.writeBack;
        if (c.value == 0 && (target.empty() || (isStashScene(target) && !throughStash) ||
                             (!deletion.info.source.empty() && !deletion.info.sourceFound))) {
          menu.showDeleteError(deleteError(DeleteStatus::Denied, std::string()));
          break;
        }
        // The player, the seek preview and a download from it let go of it.
        const std::string from = isStashScene(target) ? stashSource(target) : target;
        if (const Offline::Download* d = offline.find(from);
            d && d->source == from && d->state != Offline::State::Done)
          offline.cancel(d->id);
        closeVideo();
        if (c.value > 0) {  // only the downloaded copy
          offline.remove(deletion.copy);
          library.forgetFile(c.path);
          libraryIndex.deleted(c.path);
          std::printf("Delete: a downloaded copy: deleted\n");
          menu.deleteEnded(std::string());
          break;
        }
        deletion.throughStash = throughStash;
        deletion.result = std::async(std::launch::async, [target, stamp = deletion.stamp, throughStash,
                                                          writeBack = settings.stash.writeBack,
                                                          url = settings.stash.url,
                                                          key = settings.stash.apiKey,
                                                          scene = deletion.scene,
                                                          stashPath = deletion.stashPath,
                                                          oshash = deletion.oshash,
                                                          trash = deletion.info.stashTrash] {
          const bool here = !isStashScene(target);
          DeleteOutcome r;
          if (throughStash)
            r = deleteThroughStash(url, key, writeBack, scene, oshash, stashPath,
                                   here ? target : std::string(), stamp, trash);
          else
            r.status = removeFile(target, stamp, r.error);
          if (r.status == DeleteStatus::Ok && here)
            dropThumbnail(target, stamp.size, stamp.modified, stamp.nanos);
          return r;
        });
        break;
      }
      case MenuCommand::ToggleStash:
        settings.stash.enabled = !settings.stash.enabled;
        saveSettings(settings);
        restartStash();
        break;
      case MenuCommand::SetStashServer: {
        // The URL, the API key, then each mapping's Stash folder and folder
        // here, one per line.
        StashSettings changed = settings.stash;
        std::vector<std::string> lines;
        for (size_t start = 0; start <= c.path.size();) {
          const size_t end = std::min(c.path.find('\n', start), c.path.size());
          lines.push_back(c.path.substr(start, end - start));
          start = end + 1;
        }
        lines.resize(std::max<size_t>(lines.size(), 2));
        changed.url = lines[0];
        changed.apiKey = lines[1];
        changed.folders.clear();
        for (size_t i = 2; i + 1 < lines.size(); i += 2)
          if (!lines[i].empty()) changed.folders.push_back({lines[i], lines[i + 1]});
        const size_t first = changed.url.find_first_not_of(" \t");
        changed.url = first == std::string::npos
                          ? std::string()
                          : changed.url.substr(first, changed.url.find_last_not_of(" \t") - first + 1);
        while (changed.url.size() > 1 && changed.url.back() == '/') changed.url.pop_back();
        changed.card = changed.card && changed.url.empty();  // set up: no "Use Stash?" card
        if (changed.url != settings.stash.url || changed.apiKey != settings.stash.apiKey ||
            changed.folders != settings.stash.folders) {
          settings.stash = changed;
          saveSettings(settings);
          restartStash();
        } else if (stash) {
          stash->syncNow();
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
      case MenuCommand::SetStashSections:
        readStashSetting("stash_filters", c.path, settings.stash);
        saveSettings(settings);
        break;
      case MenuCommand::SetStashFormat:
        settings.stash.format = static_cast<StashFormat>(static_cast<int>(c.value));
        saveSettings(settings);
        break;
      case MenuCommand::SetStashScenesSort:
        readStashSetting("stash_scenes_sort", c.path, settings.stash);
        settings.stash.scenesDescending = c.value != 0;
        saveSettings(settings);
        break;
      case MenuCommand::SetStashPerformersSort:
        readStashSetting("stash_performers_sort", c.path, settings.stash);
        settings.stash.performersDescending = c.value != 0;
        saveSettings(settings);
        break;
      case MenuCommand::SetStashStudiosSort:
        readStashSetting("stash_studios_sort", c.path, settings.stash);
        settings.stash.studiosDescending = c.value != 0;
        saveSettings(settings);
        break;
      case MenuCommand::DownloadReleaseUpdate: releaseUpdate.download(); break;
      case MenuCommand::SaveDiagnostics: diagnostics.start(); break;
      case MenuCommand::None: break;
    }
  };

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
  while (!xrQuit && !app.quit && !quitSignal && !glfwWindowShouldClose(input)) {
    timer.start();
    glfwPollEvents();
    xr.pollEvents(xrQuit);
    player.pollEvents();
    if (kStandaloneHeadset && opts.hwdec == "v4l2m2m") {
      if (!xr.isVisible() && heldSeek.active()) {
        if (heldSeekPaused) player.command("set pause no");
        heldSeek.cancel();
        heldSeekPaused = heldSeekLanding = heldSeekBar = false;
      }
      player.setPresentationActive(xr.isVisible());
    }
    timer.lap(FrameTimer::Events);
    if (player.shutdownRequested()) break;

    // A Stash scene's file on a server that doesn't play plays from Stash's
    // stream instead (once: the stream is no server's file), if that serves it.
    if (player.failed() && remote::isRemote(app.currentFile) && stash && stash->catalog())
      if (const int i = stash->catalog()->sceneByPath(app.currentFile); i >= 0) {
        const StashScene& scene = stash->catalog()->scenes()[i];
        if (!scene.oshashes.empty() && scene.oshashes.front() == scene.oshash) {
          std::printf("Server failed: the scene plays from Stash\n");
          const std::string listed = openedAs;
          openVideo(stashStream(scene));
          openedAs = listed;
        }
      }
    // A play counts each time a video file opens (a resumed
    // one too; pausing, looping and seeking don't). Streams and photos don't.
    const bool opened = player.takeOpened();
    if (opened && setupBanner && !isPhotoFile(app.currentFile)) hideSetupBanner();
    if (opened && !app.currentFile.empty() && !isStream(app.currentFile) &&
        !isPhotoFile(app.currentFile)) {
      library.recordPlay(app.currentFile);
      videoOpened = true;
      videoOpenedAt = glfwGetTime();
      resumeSaveAt = glfwGetTime() + 60;
      resumeSaved = -2;
    }
    // Every minute, where it is: kept even if VideoFrameVR is killed.
    if (videoOpened && glfwGetTime() >= resumeSaveAt) saveResume();
    if (videoOpened && !durationKept && player.playback().duration > 0) {
      durationKept = true;
      library.setDuration(app.currentFile, player.playback().duration);
    }

    if (app.videoStep != 0) {
      stepVideo(app.videoStep);
      app.videoStep = 0;
    }

    if (app.saveAt > 0 && glfwGetTime() >= app.saveAt) saveUnsaved();
    if (const int volume = static_cast<int>(std::lround(player.playback().volume));
        !volumeApplied)
      volumeApplied = volume == settings.volume;
    else if (volume != settings.volume && volumeSaveAt == 0)
      volumeSaveAt = glfwGetTime() + 1;
    if (volumeSaveAt > 0 && glfwGetTime() >= volumeSaveAt) keepVolume();
    // The library's background results; the playing file's record may be
    // among them.
    if (library.poll() && awaitingRecord && library.known(app.currentFile)) {
      awaitingRecord = false;
      if (awaitingResume) {
        const VideoRecord r = library.record(app.currentFile);
        resolvedResume = resumePoint(r.resumeSeconds, r.durationSeconds);
      }
      VideoSettings kept;
      bool hasSettings = library.settings(app.currentFile, kept);
      if (!hasSettings && openingRecord == library.record(app.currentFile).id &&
          memory.get(app.currentFile, kept)) {
        library.carryOverSettings(app.currentFile, kept);
        hasSettings = true;
      }
      // The save timer may still hold user edits when identity arrives.
      // Restore the other fields without replacing those pending changes.
      if (hasSettings) kept.take(app.video, app.unsavedParts & ~VideoSettings::kScreen);
      if (hasSettings && kept != app.video) {
        // Without a format of its own, the file's is detected again.
        if (app.video.hasFormat && !kept.hasFormat) {
          app.formatChosenByUser = false;
          knownWidth = -1;
        }
        app.video = kept;
        applyVideoSettings();
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
      startScene.clear();
    }
    if (nextPhoto.valid() &&
        nextPhoto.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
      photoPreloader.request(app.currentFile, nextPhoto.get());
    if (neighbor.valid() &&
        neighbor.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      const std::string next = neighbor.get();
      if (!next.empty())
        openVideo(next);
      else if (!neighborWrap)
        std::printf("No %s video in this folder\n", neighborStep > 0 ? "next" : "previous");
      neighborStep = 0;
      app.nextSlideAt = glfwGetTime() + app.slideshowSeconds;
    }
    // mpv keeps a video that reached its end paused on its last frame.
    const bool atEnd = player.playback().endReached && !app.currentFile.empty() &&
                       !isPhotoFile(app.currentFile);
    if (atEnd != wasAtEnd) menuAtEnd = atEnd;
    wasAtEnd = atEnd;
    // Slideshow: the next photo in the folder, after the last one the first.
    if (app.slideshowSeconds > 0 && isPhotoFile(app.currentFile) &&
        glfwGetTime() >= app.nextSlideAt && !neighbor.valid()) {
      neighborStep = 1;
      neighborWrap = true;
      neighbor = mediaNeighbor(app.currentFile, 1, true);
    }

    // The delete dialog's file looked up; a deletion finished.
    if (deletion.lookup.valid() &&
        deletion.lookup.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      const Deletion::Look look = deletion.lookup.get();
      DeleteInfo& info = deletion.info;
      const bool stream = isStashScene(deletion.target);
      DeleteStatus status = look.status;
      if (status == DeleteStatus::Ok && !stream) {
        deletion.stamp = look.stamp;
        info.size = look.stamp.size;
      }
      deletion.oshash = look.oshash;
      // Stash deletes it if a fresh query found the scene's one file with
      // its path and oshash; its stream's file nobody else can.
      deletion.throughStash = look.asked && look.stash.file != 0;
      if (deletion.throughStash) {
        deletion.sceneFiles = look.stash.files.size();
        info.stash = DeleteInfo::Stash::Deletes;
        info.otherFiles = static_cast<int>(look.stash.files.size()) - 1;
        info.stashTrash = !look.stash.trash.empty();
      } else if (stream) {
        if (status == DeleteStatus::Ok)
          status = !look.asked             ? DeleteStatus::Denied
                   : !look.stash.answered ? DeleteStatus::Offline
                   : !look.stash.scene    ? DeleteStatus::Gone
                                          : DeleteStatus::Changed;
      } else if (status == DeleteStatus::Ok && deletion.scene &&
                 !(look.stash.answered && !look.stash.scene)) {
        info.stash = look.stash.answered ? DeleteInfo::Stash::Unsure : DeleteInfo::Stash::Lists;
      }
      // A copy can go without its source.
      info.sourceFound = !info.source.empty() && status == DeleteStatus::Ok;
      info.sourceChanged = !info.source.empty() && status == DeleteStatus::Changed;
      info.ready = status == DeleteStatus::Ok || !info.source.empty();
      if (info.ready)
        menu.askDelete(info);
      else
        menu.showDeleteError(deleteError(status, std::string()));
    }
    if (deletion.result.valid() &&
        deletion.result.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      const DeleteOutcome outcome = deletion.result.get();
      const std::string& target = deletion.target;
      const bool stream = isStashScene(target);
      std::printf("Delete: %s%s%s: %s%s%s\n",
                  stream                ? "a scene's file in Stash"
                  : remote::isRemote(target) ? "a file on a server"
                                       : "a file on this device",
                  deletion.info.source.empty() ? "" : " and its downloaded copy",
                  deletion.throughStash ? " through Stash" : "", deleteLogged(outcome.status),
                  outcome.listed ? ", Stash still lists it" : "",
                  outcome.stashDeleted && outcome.status != DeleteStatus::Ok ? ", Stash deleted its file"
                                                                             : "");
      if (outcome.status == DeleteStatus::Ok) {
        if (!stream) {
          library.forgetFile(target);
          libraryIndex.deleted(target);
        }
        if (!deletion.info.source.empty()) {
          offline.remove(deletion.copy);
          library.forgetFile(deletion.info.file);
          libraryIndex.deleted(deletion.info.file);
        }
        forgetFolderListing();
      }
      if (stash && deletion.scene && (outcome.status == DeleteStatus::Ok || outcome.stashDeleted)) {
        if (deletion.sceneFiles == 1 && !deletion.stashPath.empty()) stash->hideScene(deletion.scene);
        if (deletion.throughStash) stash->syncNow();
      }
      // Once Stash has deleted its file, never "Nothing was deleted".
      menu.deleteEnded(outcome.status == DeleteStatus::Ok ? std::string()
                       : outcome.stashDeleted             ? std::string(strings().deletion.stashKeptHere)
                                                          : deleteError(outcome.status, outcome.error),
                       outcome.listed ? strings().deletion.stashLists : "");
    }

    // Re-guess the format once the real frame size is known.
    if (app.rememberFormat) {  // changed by key
      rememberFormat();
      app.rememberFormat = false;
    }
    if (player.width() != knownWidth || player.height() != knownHeight) {
      knownWidth = player.width();
      knownHeight = player.height();
      if (knownWidth > 0 && !app.formatChosenByUser) {
        app.format = guessVideoFormat(app.currentFile, knownWidth, knownHeight);
        printFormat(app.format, "detected");
      }
    }

    // Passthrough for videos made for it, once their size (so their format)
    // and their settings are known. Never where it couldn't show the room:
    // on the Frame the runtime must blend; on a PC the setting stands for
    // a headset whose camera keys the green.
    const bool autoPassthroughOn =
        settings.autoPassthrough && (!kStandaloneHeadset || xr.alphaBlendSupported());
    if (!passthroughSettled && knownWidth > 0 && !awaitingRecord && !app.currentFile.empty()) {
      passthroughSettled = true;
      const std::string& path = app.currentFile;
      if (autoPassthroughOn && !isPhotoFile(path) && !isStream(path)) {
        passthroughByItself(autoPassthrough(path, app.format, app.video));
        if (greenCheckWanted(path, app.format, app.video)) greenCheckAt = glfwGetTime() + 2.0;
      }
    }
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
        driveShownUntil = glfwGetTime() + kDriveNoticeShown;
      } else if (!change.added.empty() && !menu.setupRunning()) {  // setup shows it as a card
        const RemovableDrives::Drive& d = change.added.back();
        const char* kind = d.kind == Kind::Card  ? t.card
                           : d.kind == Kind::Usb ? t.usbDrive
                                                 : t.drive;
        driveText = kind + (d.label.empty() ? "" : ": " + d.label) + "\n" + t.inExplorer;
        driveShownUntil = glfwGetTime() + kDriveNoticeShown;
      }
    }
    // Looks for new videos in the pinned folders wait while a video is open.
    libraryIndex.setVideoOpen(!app.currentFile.empty() && !isPhotoFile(app.currentFile));
    // ... and so does looking for Stash's folders.
    if (stash) stash->setVideoOpen(!app.currentFile.empty() && !isPhotoFile(app.currentFile));
    // Downloads and new thumbnails wait while a video plays.
    const bool videoPlaying =
        !app.currentFile.empty() && !isPhotoFile(app.currentFile) && !player.playback().paused;
    offline.setPlaying(videoPlaying);
    thumbnailer.setPlaying(videoPlaying);
    std::vector<Offline::Download> downloaded;
    offline.poll(downloaded);
    // Stash is asked whether it answers while a scene may play from its stream.
    offline.watchStash(stash && stash->catalog() &&
                       (stash->catalog()->stats().streamed > 0 || !offline.downServers().empty()));
    for (const Offline::Download& d : downloaded) library.addCopy(d.path, d.oshash, d.size, d.mtime);
    if (greenCheckAt > 0 && glfwGetTime() >= greenCheckAt) {
      greenCheckAt = 0;
      greenCheck.start(app.currentFile, app.format.stereo);
    }
    {
      std::string checked;
      bool found = false;
      if (greenCheck.result(checked, found)) {
        // Kept for the video, even if another plays by now: it runs once.
        VideoSettings result;
        result.greenCheckVersion = GreenScreenCheck::kVersion;
        result.greenScreenFound = found;
        library.setSettings(checked, result, VideoSettings::kGreenCheck);
        if (checked == app.currentFile) {
          app.video.take(result, VideoSettings::kGreenCheck);
          if (autoPassthroughOn)
            passthroughByItself(autoPassthrough(checked, app.format, app.video));
        }
      }
    }

    // 3D and surround video always get floating subtitles (mpv's would be cut
    // in half or end up behind you); for 2D video the setting decides.
    const bool floatingSubtitles =
        settings.floatingSubtitles ||
        app.format.stereo != StereoLayout::Mono || app.format.projection != Projection::Flat;
    player.setSubtitlesInFrame(!floatingSubtitles);
    subtitles.setText(floatingSubtitles ? player.playback().subtitleText : "");
    loadingText.setText(player.failed() ? strings().playbackFailed : strings().loading);
    // Until the new file's first frame arrives, the texture still holds the old
    // video, which would show cut apart in the new format; show nothing instead.
    bool photo = isPhotoFile(app.currentFile);
    bool loading = !photo && player.loading() && !app.currentFile.empty();
    if (app.togglePassthrough) {
      app.togglePassthrough = false;
      app.video.passthrough = !app.video.passthrough;
      app.video.passthroughChosen = true;  // never switched by itself again
      passthroughShownUntil = 0;
      storeVideo(VideoSettings::kPassthrough);
      std::printf("Passthrough: %s\n", app.video.passthrough ? "on" : "off");
      if (app.video.passthrough && kStandaloneHeadset && !xr.alphaBlendSupported())
        std::printf("Passthrough: the runtime offers no alpha blending, so the camera cannot "
                    "show\n");
    }
    // Passthrough on the Frame itself: the eye images are blended over the
    // camera by their alpha; on a PC (streaming) the Frame keys out green.
    const bool canBlend = kStandaloneHeadset && xr.alphaBlendSupported();
    const bool alphaPassthrough = app.video.passthrough && canBlend;
    const bool keyPassthrough = app.video.passthrough && !kStandaloneHeadset;
    // Opacity below 100 blends the video over the camera too, with or
    // without passthrough (whose mask or key it doesn't bring).
    const bool blended = canBlend && (app.video.passthrough || app.video.opacity < 100);
    // A room reading that ended: kept for the run and matched while the
    // switch is on (unless the sliders were moved or reset since it was asked
    // for), or a moment saying it failed (the switch going off again, nothing
    // matched yet, if there is no earlier reading to use).
    if (std::optional<RoomLight::Reading> reading; roomLight.take(reading)) {
      if (reading) {
        roomReading = reading;
        if (roomAsked == balanceEdits) applyRoomMatch();
      } else {
        roomFailedUntil = glfwGetTime() + 4;
        if (!roomReading) roomMatched = false;
      }
    }
    // Saved on every change of folder, so even a killed player reopens there.
    if (!menu.directory().empty() && menu.directory() != settings.lastFolder) {
      settings.lastFolder = menu.directory();
      saveSettings(settings);
    }
    if (menu.gallery() != settings.gallery) {
      settings.gallery = menu.gallery();
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
    if (pipelineVideo && std::abs(xr.displayPeriod() - pipelinePeriod) > 0.0005) {
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
      photo = isPhotoFile(app.currentFile);
      loading = !photo && player.loading() && !app.currentFile.empty();
      videoImage = nullptr;
      videoTexture = 0;
      photoImage = nullptr;
      frameAspect = player.aspect();
      photoWidth = photoHeight = 0;
      if (photo) {
        photoImage = photoPreloader.image(app.currentFile, photoWidth, photoHeight);
        if (photoImage) {
          videoImage = &photoImage->texture();
          videoTexture = photoImage->glTexture();
          frameAspect = static_cast<float>(photoWidth) / photoHeight;
          if (!photoSized) {  // the size is known now: guess the format
            photoSized = true;
            std::printf("Photo size: %dx%d\n", photoWidth, photoHeight);
            if (!app.formatChosenByUser) {
              app.format = guessVideoFormat(app.currentFile, photoWidth, photoHeight);
              printFormat(app.format, "detected");
            }
          }
        }
      } else if (!loading && !app.currentFile.empty()) {
        videoImage = player.texture();
        videoTexture = player.glTexture();
      }
      stillLoading = !app.currentFile.empty() && !videoImage && (photo || loading);
    };
    choosePicture();

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
      Quat headRot;
      const bool headValid = xr.headPose(headPos, headRot);
      const Mat4 frame = headFrame(headRot);
      const Mat4 menuAt = menuFrame(headRot);
      if (app.recenterAt > 0 && now >= app.recenterAt) {  // the menu's countdown is over
        app.recenter = true;
        app.recenterAt = 0;
        buzz(-1, 0.3f, 0.020);
      }
      if ((initialRecenter || (app.recenter && !menu.setupRunning())) && headValid) {
        if (app.recenter) {
          if (app.video.originForward != 0)
            viewpointShownUntil = now + kValueShown;  // "Centered": it did go back
          app.video.originForward = 0;
          // Also mark a provisional zero: a late identity restore must not
          // bring back a remembered origin after an explicit recenter.
          storeVideoSoon(VideoSettings::kOrigin);
        }
        app.screen.anchorPosition = headPos;
        app.screen.anchorRotation = frame;
        app.screen.turn = 0.0f;
        turnDrag.tracking = false;  // a drag goes on from here, without catching up first
        if (panel.open) panel.placeInFrontOf(headPos, menuAt);  // the menu comes along
        app.recenter = false;
        initialRecenter = false;
      }

      // --- Controller and keyboard input ---
      const ControllerState& input = xr.controllers();
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
        s.bring = input.menuPressed || input.recenterPressed || app.toggleMenu || app.recenter;
        s.pressed = s.bring || input.playPausePressed;
        for (const HandState& h : input.hands) {
          s.pressed |= h.selectPressed || h.gripPressed;
          if (h.poseValid) controllerSeen = now;
        }
        // Welcome's intro plays from when the panel is placed; a press skips
        // it, and does nothing else.
        const bool skipped = setupIntro.playing(now) && s.pressed;
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
        app.recenter = false;
        menu.setSetupController(now - controllerSeen < 1.0);
        // Your controllers: the bindings when the runtime changed them, and
        // what is held.
        if (xr.bindingsVersion() != bindingsSeen) {
          bindingsSeen = xr.bindingsVersion();
          menu.setSetupControls(xr.controllerBindings());
        }
        uint32_t held = 0;
        for (int h = 0; h < 2; ++h) {
          const HandState& g = input.hands[h];
          const bool sideways = std::abs(g.stickX) > 0.5f && std::abs(g.stickX) >= std::abs(g.stickY);
          const bool upDown = std::abs(g.stickY) > 0.5f && std::abs(g.stickY) > std::abs(g.stickX);
          const bool on[] = {g.select, g.grip, xr.held(ControlAction::Menu, h),
                             xr.held(ControlAction::PlayPause, h),
                             xr.held(ControlAction::Recenter, h), sideways, upDown};
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
      } else if (input.menuPressed || app.toggleMenu) {
        panel.open = !panel.open;
        openedNow = panel.open;
        menuBroughtBack = panel.open && input.menuPressed;
        if (panel.open) {
          // Always open on the player page; sub pages (search, files) keep their state.
          menu.showMainPage();
          if (headValid) panel.placeInFrontOf(headPos, menuAt);
        }
      }
      app.toggleMenu = false;
      if (input.playPausePressed) player.command("cycle pause");
      // Grip held and a stick click on a rating clears it;
      // otherwise the click recenters.
      if (input.recenterPressed && !menu.setupRunning() &&
          !((input.hands[0].grip || input.hands[1].grip) && panel.open &&
            menu.clearRatingAtPointer())) {
        app.recenter = true;
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

      // Grip elsewhere, a corner's handle and stick up/down: the flat screen.
      if (app.format.projection == Projection::Flat && videoImage && headValid) {
        ScreenControl::Frame screenFrame;
        screenFrame.up = up;
        screenFrame.aspect = eyeDisplayAspect(app.format, frameAspect);
        screenFrame.dt = dt;
        screenFrame.onMenu[0] = onMenu[0];
        screenFrame.onMenu[1] = onMenu[1];
        screenFrame.menuHeld = menuGrab.hand >= 0;
        screenFrame.sticksFree = !listMode;
        const bool wasHolding = screenControl.holding();
        if (screenControl.update(input, screenFrame, app.screen)) keepScreenSize();
        if (!wasHolding && screenControl.holding()) buzz(screenControl.hand(), 0.25f, 0.012);
      } else {
        screenControl.reset();
      }
      if (app.screenChanged) {
        app.screenChanged = false;
        keepScreenSize();
      }

      // A grip held, the trigger held off the menu and a twist of the wrist:
      // adjust the volume.
      const float rolled = volumeRoll.update(input, onMenu);
      if (volumeRoll.hand() < 0) {
        rolledVolume = -1;
      } else {
        if (rolledVolume < 0) {
          volumeTwisted = true;
          rolledVolume = player.playback().volume;
          buzz(volumeRoll.hand(), 0.25f, 0.012);
        }
        const double next = std::clamp(rolledVolume + rolled * kVolumePerDegree, 0.0, 100.0);
        if (std::lround(next) != std::lround(rolledVolume))
          player.command(("set volume " + std::to_string(std::lround(next))).c_str());
        if (std::lround(next) / 5 != std::lround(rolledVolume) / 5)  // a detent each 5 steps
          buzz(volumeRoll.hand(), 0.1f, 0.006);
        rolledVolume = next;
      }

      // 180°/360° video the headset blends over the room: a grip held
      // without the trigger, off the menu, and a twist of that wrist past
      // its dead zone sets the video's opacity (on the flat screen the grip
      // moves the screen).
      const float twisted =
          opacityTwist.update(input, onMenu,
                              canBlend && app.format.projection != Projection::Flat &&
                                  videoImage && menuGrab.hand < 0);
      if (!opacityTwist.changing()) {
        twistedOpacity.reset();
      } else {
        const auto shown = [](double opacity) {
          return std::clamp(std::lround(opacity), 0L, 100L);
        };
        if (!twistedOpacity) buzz(opacityTwist.hand(), 0.25f, 0.012);
        // It goes on from the video's opacity, with its fraction and hold
        // while that still shows it (a late library restore can change it
        // mid-twist).
        if (!twistedOpacity || shown(*twistedOpacity) != app.video.opacity)
          twistedOpacity = app.video.opacity;
        const double was = *twistedOpacity;
        double next = was + twisted * kOpacityPerDegree;
        if (next >= 100) next = was > 100 ? std::min(next, 100 + kOpacityHold) : 100 + kOpacityHold;
        if (next <= 0) next = was < 0 ? std::max(next, -kOpacityHold) : -kOpacityHold;
        const long from = shown(was), to = shown(next);
        if (to != from) {
          app.video.opacity = static_cast<int>(to);
          storeVideoSoon(VideoSettings::kOpacity);
          // A detent at each multiple of 10 it reaches.
          if (to > from ? to / 10 > from / 10 : (to + 9) / 10 < (from + 9) / 10)
            buzz(opacityTwist.hand(), 0.1f, 0.006);
        }
        twistedOpacity = next;
        opacityShownUntil = now + kValueShown;
      }

      // Trigger away from the menu: drag the picture around you.
      // Releasing stops it where it is shown; what the smoothing had not caught up is dropped.
      const bool screenHeld = screenControl.holding();
      if (turnDrag.hand >= 0 && (!input.hands[turnDrag.hand].select || screenHeld ||
                                 menuGrab.hand == turnDrag.hand))
        turnDrag.hand = -1;
      for (int h = 0; h < 2 && turnDrag.hand < 0 && headValid && !screenHeld; ++h) {
        const HandState& t = input.hands[h];
        if (!t.selectPressed || !t.poseValid || menuGrab.hand == h || onMenu[h] ||
            volumeRoll.hand() == h)
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
          app.screen.anchorRotation =
              rotation(next * conjugate(turnDrag.smoothHand)) * app.screen.rotation();
          app.screen.turn = 0.0f;
          turnDrag.smoothHand = next;
        }
        turnDrag.lastTime = time;
      }

      // Left stick sideways turns the view, right stick sideways seeks; up/down
      // on either scrolls lists, else moves the viewpoint or flat screen. Only
      // the dominant axis counts. While the keyboard is up, sideways on either
      // moves the text cursor instead.
      const bool typing = panel.open && menu.textInputActive();
      const StickDirections left = stickDirections(input.hands[0]);
      const StickDirections right = stickDirections(input.hands[1]);
      if (cursorLeft.update(typing && (left.left || right.left), now)) menu.moveCursor(-1);
      if (cursorRight.update(typing && (left.right || right.right), now)) menu.moveCursor(1);
      for (int i = 0; i < 2; ++i) forwardStick[i].update(input.hands[i], now);

      {
        const bool seekable = !app.currentFile.empty() && !photo;
        const PlaybackState& p = player.playback();
        // The right stick counts sideways once it is mostly sideways; then all
        // of its sideways push, so a thumb drifting up or down keeps the hold.
        const float x = typing ? 0.0f : input.hands[1].stickX;
        const float sideways =
            heldSeek.active() || std::abs(x) > std::abs(input.hands[1].stickY) ? x : 0.0f;
        const float back = seekable ? std::max(app.seekBackKey ? 1.0f : 0.0f, -sideways) : 0.0f;
        const float forward = seekable ? std::max(app.seekForwardKey ? 1.0f : 0.0f, sideways) : 0.0f;
        // Its limits from where it started: the video plays on until a hold.
        const double from = heldSeek.active() ? heldSeekFrom : p.position;
        const HeldSeek::Step step =
            heldSeek.update(back, forward, now, -from,
                            p.duration > 0 ? p.duration - kSeekEndMargin - from : 1e9, p.duration);
        if (step.pressed) {
          awaitingResume = false;
          heldSeekFrom = p.position;
          heldSeekBar = false;  // a tap shows none
          heldSeekStick = std::abs(sideways) >= HeldSeek::kPress;
          heldSeekAtLimit = false;
          if (heldSeekStick) buzz(1, 0.15f, 0.010);
        }
        if (step.holdStarted) {
          // The picture waits, and is silent, until the stick lets go; with
          // the menu closed, the strip alone shows where it goes meanwhile.
          heldSeekPaused = !p.paused;
          if (heldSeekPaused) player.command("set pause yes");
          heldSeekBar = !panel.open && headValid;
          if (heldSeekBar) panel.placeInFrontOf(headPos, menuAt);
          heldSeekPicture = heldSeekFrom;
          heldSeekPictureAt = now;
          heldSeekTens = std::floor(std::abs(heldSeek.distance()) / 10);
        }
        if (heldSeek.holding()) {
          heldSeekTarget = heldSeekFrom + heldSeek.distance();
          if (!player.seeking() && now - heldSeekPictureAt >= kHeldPictureEvery &&
              std::abs(heldSeekTarget - heldSeekPicture) >= kHeldPictureMove) {
            player.seekTo(heldSeekTarget, false);
            heldSeekPicture = heldSeekTarget;
            heldSeekPictureAt = now;
          }
          if (heldSeekStick) {
            const double end = p.duration > 0 ? p.duration - kSeekEndMargin : 1e9;
            const bool atLimit = heldSeekTarget <= 0.01 || heldSeekTarget >= end - 0.01;
            const double tens = std::floor(std::abs(heldSeek.distance()) / 10);
            if (atLimit && !heldSeekAtLimit) {
              buzz(1, 0.3f, 0.020);
            } else if (tens != heldSeekTens && now - heldSeekTickAt >= 0.125) {
              buzz(1, 0.1f, 0.006);
              heldSeekTickAt = now;
            }
            heldSeekAtLimit = atLimit;
            heldSeekTens = tens;
          }
        }
        if (step.released) {
          // A tap jumps on from where the video is now; a hold lands on the
          // keyframe before its target, never past it into the end, unless
          // its last picture is already there.
          if (step.held && heldSeekStick) buzz(1, 0.2f, 0.014);
          bool sent = step.distance != 0;
          if (step.held) {
            const double target = heldSeekFrom + step.distance;
            sent = std::abs(target - heldSeekPicture) > 0.01;
            if (sent) player.seekTo(target, false);
          } else if (sent) {
            player.seekBy(step.distance);
          }
          if (heldSeekPaused) player.command("set pause no");
          heldSeekPaused = false;
          heldSeekLanding = step.held && (sent || player.seeking());
          heldSeekBarUntil = now + kSeekBarLinger;
        }
        if (heldSeekLanding && !player.seeking()) heldSeekLanding = false;
      }
      if (turnLeft.update((left.left && !typing) || app.turnLeftKey, now)) {
        app.screen.turn = wrapAngle(app.screen.turn - kTurnStep);
        if (left.left) buzz(0, 0.2f, 0.014);
      }
      if (turnRight.update((left.right && !typing) || app.turnRightKey, now)) {
        app.screen.turn = wrapAngle(app.screen.turn + kTurnStep);
        if (left.right) buzz(0, 0.2f, 0.014);
      }
      if (listMode) {
        // Lists glide: the further the stick is tilted, the faster (up to
        // ~14 rows a second), with a small dead zone around the middle.
        // Kept pushed well over for more than a second, a long list speeds
        // up smoothly to 4 times that by 3 s. Easing off loses the speed-up
        // within half a second; letting go or reversing loses it at once.
        float tilt = 0.0f;
        for (int i = 0; i < 2; ++i) {
          const HandState& h = input.hands[i];
          if (menuGrab.hand == i || screenControl.hand() == i ||
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
        if (app.format.projection != Projection::Flat && videoImage &&
            !input.hands[0].grip && !input.hands[1].grip) {
          int direction = 0;
          for (int i = 0; i < 2; ++i)
            if (!onMenu[i] && forwardStick[i].held(now))
              direction += input.hands[i].stickY > 0 ? 1 : -1;
          if (direction) {
            constexpr float kMax = VideoSettings::kMaxOriginForward;
            app.video.originForward =
                std::clamp(app.video.originForward + direction * 0.05f * dt, -kMax, kMax);
            storeVideoSoon(VideoSettings::kOrigin);
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
      const FocusSettings& focusSettings = app.video.hasFocus ? app.video.focus : settings.focus;
      FocusInput focusIn;
      focusIn.settings = focusSettings;
      focusIn.format = app.format;
      focusIn.video = app.currentFile;
      focusIn.shown = videoImage != nullptr;
      focusIn.texture = videoTexture;
      if (photoImage)
        focusIn.prepareRead = [&interop, photoImage] { interop.acquireForGl(*photoImage); return true; };
      else if (!photo)
        focusIn.prepareRead = [&player] { return player.prepareRead(); };
      if (!photo && !videoTexture) focusIn.drawer = directDepth.source(player.directCurrent());
      focusIn.flip = app.flipVideo;
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
                                        app.screen.anchorPosition);
      focusIn.head = headRot;
      focusIn.picture = app.screen.rotation();
      // Static placement and head motion share the correction's sphere limit.
      if (app.format.projection != Projection::Flat && app.video.originForward != 0)
        focusIn.motion = focusIn.motion +
                         transformDir(focusIn.picture, {0, 0, -app.video.originForward});
      focusIn.zoom = app.screen.zoom;
      // Only passthrough the eye images show: without alpha blending the
      // Frame draws the video whole.
      focusIn.seeThrough = seeThrough(alphaPassthrough || keyPassthrough,
                                      packedMask(app.currentFile, app.video), app.format,
                                      app.video.greenScreen);
      focusIn.now = now;
      focusIn.target = focusTargets.update(settings.focusPrefs.eyeTracking, xr.gaze(), headRot,
                                           now, settings.focusPrefs.gazeCorrect);
      const FocusOutput focusOut = autofocus.update(focusIn);
      const bool aimed = focusOut.automatic;  // the target only matters for automatic focus
      focusMarker.update(settings.focusPrefs.diagnostics,
                         (focusIn.leftEye + focusIn.rightEye) * 0.5f, headRot, focusOut.marker,
                         focusOut.markerDistance,
                         aimed ? focusOut.brief + "  " + targetStatusTag(focusTargets.status())
                               : focusOut.brief);
      // The dot hangs on the line of sight from between the eyes.
      focusMarker.updateGaze(
          settings.focusPrefs.gazeDot && focusTargets.gazeKnown() && focusIn.headValid,
          (focusIn.leftEye + focusIn.rightEye) * 0.5f,
          transformDir(rotation(headRot), focusTargets.gaze()), focusOut.distance);

      if (panel.open && !loading) releaseUpdate.menuOpened();
      releaseUpdate.poll();
      // Never over playback: a newer release is offered as the menu opens,
      // and an installed one restarts the app while the menu is open.
      if (panel.open && !menuWasOpen && !updateOffered && !menu.setupRunning() &&
          releaseUpdate.state() == ReleaseUpdate::State::Available) {
        updateOffered = true;
        menu.showUpdate();
      }
      menuWasOpen = panel.open;
      if (panel.open && releaseUpdate.state() == ReleaseUpdate::State::Restarting && !app.quit) {
        std::printf("Update: restarting into %s\n", releaseUpdate.version().c_str());
        menu.showUpdate();
        releaseUpdate.restartAtExit();
        app.quit = true;
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
      if (heldSeek.holding() || heldSeekLanding) {
        view.playback.position = heldSeekTarget;
        if (heldSeekPaused) view.playback.paused = false;  // only waiting for the stick
      }
      // Software decoding the strip warns of, once it has lasted a second
      // (not a decoder restarting after a seek).
      if (photo || loading || !videoImage || player.counts().decoder != "no")
        softwareSince = -1;
      else if (softwareSince < 0)
        softwareSince = now;
      view.softwareDecoding = softwareSince >= 0 && now - softwareSince >= 1.0;
      view.format = app.format;
      view.halfPacked = isHalfPacked(app.format);
      view.screen = ScreenView::of(app.screen.size);
      view.currentFile = app.currentFile;
      view.playbackFailed = player.failed();
      // No glass on the Frame (docs/vulkan-renderer.md, 7): Solid there,
      // keeping a saved Glass for the desktop.
      view.theme = kStandaloneHeadset ? Theme::Material : settings.theme;
      view.accent = settings.accent;
      view.floatingSubtitles = settings.floatingSubtitles;
      view.streaming = isStream(app.currentFile);
      view.videoWidth = photo ? photoWidth : player.width();
      view.videoHeight = photo ? photoHeight : player.height();
      if (photo) view.playback.title = std::filesystem::path(app.currentFile).filename().string();
      view.background = settings.background;
      view.autoPassthrough = settings.autoPassthrough;
      view.passthrough = app.video.passthrough;
      view.greenScreen = app.video.greenScreen;
      view.packedAlpha = packedMask(app.currentFile, app.video);
      view.maskFormat = maskFormat(app.format);
      view.opacity = app.video.opacity;
      view.opacityApplies = canBlend;
      view.subtitleSize = settings.subtitleSize;
      view.subtitleRaise = settings.subtitleRaise;
      view.subtitleDistance = settings.subtitleDistance;
      view.photo = !app.currentFile.empty() && isPhotoFile(app.currentFile);
      // A scene from Stash's stream only through Stash (docs/alpha/delete-file.md).
      view.deletable = app.currentFile.empty() || view.streaming ? MenuView::Deletable::No
                       : !isStashScene(app.currentFile)         ? MenuView::Deletable::Yes
                       : !settings.stash.writeBack              ? MenuView::Deletable::NeedsSendToStash
                       : offline.stashDown()                    ? MenuView::Deletable::StashDown
                                                                : MenuView::Deletable::Yes;
      view.slideshowSeconds = app.slideshowSeconds;
      view.brightness = app.video.brightness;
      view.contrast = app.video.contrast;
      view.saturation = app.video.saturation;
      view.balance = colourBalance;
      view.roomMatch = !roomReadable            ? RoomMatch::Hidden
                       : roomLight.reading()    ? RoomMatch::Reading
                       : !blended               ? RoomMatch::Off
                       : now < roomFailedUntil ? RoomMatch::Failed
                                                : RoomMatch::Ready;
      view.roomMatched = roomMatched;
      view.roomRead = roomReading.has_value();
      view.matchColour = settings.matchColour;
      view.matchBrightness = settings.matchBrightness;
      view.tvGamma = settings.tvGamma;
      view.zoom = app.video.zoom;
      view.originForward = static_cast<int>(std::lround(app.video.originForward * 100));
      view.videoMipmaps = settings.videoMipmaps;
      view.antiShimmer = antiShimmer;
      view.sharpen = settings.sharpen;
      view.toneLook = settings.toneLook;
      view.videoResolution = pendingResolution > 0 ? pendingResolution : chosenResolution();
      view.resolutionStops = resolutionStops.empty() ? nullptr : &resolutionStops;
      view.resolutionPending = pendingResolution > 0;
      view.videoResolutionActive = static_cast<int>(std::lround(askedScale() * 100));
      view.videoResolutionEffective = static_cast<int>(std::lround(xr.requestedEyeScale() * 100));
      view.resolutionOverride =
          overrideScale > 0 ? static_cast<int>(std::floor(resolutionBase * overrideScale)) : 0;
      view.resolutionBase = resolutionBase;
      view.panelBase = kPanelWidth > 0;
      view.foveationAvailable = foveation.active();
      view.foveation = foveation.level();
      view.showFoveation = showFoveation;
      view.sharpBins = sharpBins;
      view.foveationRing = foveationRing;
      view.foveationOutside = foveationOutside;
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
      view.focus.own = app.video.hasFocus;
      view.focus.supported = stereoCorrectionSupported(app.format) && xr.eyeCount() == 2;
      view.focus.status = focusOut.status;
      view.focus.targetStatus = targetStatusText(focusTargets.status());
      view.motionEmulation = settings.motionEmulation;
      view.motionDistance = settings.motionDistance;
      view.resume = settings.resume;
      view.downloadsTo = settings.downloadsTo;
      view.seekPreview = settings.seekPreview;
      view.seekPreviewAvailable = !kStandaloneHeadset;
      view.thumbnails = settings.thumbnails;
      view.showPerformance = showPerformance;
      view.vibration = settings.vibration;
      view.stash = settings.stash;
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
      app.textInput = panel.open && menu.textInputActive();
      menu.typeText(app.typed);
      for (; app.backspaces > 0; --app.backspaces) menu.backspace();
      if (app.cursorSteps) menu.moveCursor(std::exchange(app.cursorSteps, 0));
      if (app.submit) execute(menu.submitText());
      if (app.stopTyping) menu.stopEditing();
      app.typed.clear();
      app.submit = false;
      app.stopTyping = false;
      // Trigger on the panel clicks it (anywhere else it drags the picture).
      if (panel.open && hand.selectPressed && !openedNow && hitPanel) execute(menu.click());
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
      if (pendingPicture && menu.draggingSlider() == MenuCommand::None) applyPicture(pendingPicture);
      if (pendingResolution > 0) {
        if (menu.draggingSlider() == MenuCommand::SetVideoResolution)
          pendingResolutionAt = 0;  // switched on letting go
        else if (glfwGetTime() >= pendingResolutionAt)
          applyPendingResolution();
      }
      // A command may have closed or replaced the picture chosen above (Stop
      // frees the video's images, a new file is loading): choose it again.
      choosePicture();
      sendBuzzes();

      // The hints after setup: their situations, and the controls they name
      // used; a hint's line drawn once as it starts.
      if (hints.any()) {
        if (!menu.setupRunning() && xr.bindingsVersion() != hintBindingsSeen) {
          hintBindingsSeen = xr.bindingsVersion();
          menu.setHintBindings(xr.controllerBindings());
        }
        Hints::Frame f;
        f.now = now;
        f.allowed = !menu.setupRunning() && input.focused;
        f.menuOpen = panel.open;
        f.menuBroughtBack = menuBroughtBack;
        f.longListPointed = panel.open && menu.pointingAtLongList(f.listPage);
        f.volumePointed = panel.open && menu.pointingAtVolume();
        if (videoImage && !photo && !player.playback().paused && !panel.open && headValid) {
          const Vec3 ahead = transformDir(rotation(headRot), {0.0f, 0.0f, -1.0f});
          const Vec3 centre = transformDir(app.screen.rotation(), {0.0f, 0.0f, -1.0f});
          f.videoAway = dot(ahead, centre) < std::cos(70.0f * kPi / 180);
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
      // The seek bar preview: a frame of the video where the pointer is.
      double hoverFraction = 0;
      if (panel.open && !loading && !app.currentFile.empty() && menu.seekHover(hoverFraction)) {
        SeekPreview::Playing playing;
        playing.location = app.currentFile;
        playing.duration = player.playback().duration;
        playing.format = app.format;
        playing.flip = app.flipVideo;
        seekPreview.show(hoverFraction, playing, panel.panel(menu, Menu::kMain));
      } else {
        seekPreview.hide();
      }
      scene.clear = blended ? ClearMode::Transparent
                    : keyPassthrough ? ClearMode::KeyGreen
                                     : ClearMode::Dark;
      // The night sky around the flat screen (with passthrough: the room).
      scene.sky = app.format.projection == Projection::Flat && settings.background &&
                  scene.clear == ClearMode::Dark;
      VideoDraw& video = scene.video.emplace();
      video.texture = videoImage;
      video.format = app.format;
      video.frameAspect = frameAspect;
      video.flip = app.flipVideo;
      video.screen = app.screen;
      video.photo = photo;
      video.seeThrough = focusIn.seeThrough;
      video.opacity = app.video.opacity;
      const std::array<float, 3> gains = colourBalance.gains();
      for (int i = 0; i < 3; ++i) video.balance[i] = gains[i] - 1.0f;
      // Brightness etc. are applied while drawing to photos, and to videos
      // while a mask or key reads the frame; else mpv (or YuvColor) does it.
      const bool readsFrame = video.readsFrame(scene.clear);
      player.setPlainPicture(readsFrame);
      if (photo || readsFrame) {
        video.adjust[0] = app.video.brightness / 100.0f;
        video.adjust[1] = app.video.contrast / 100.0f;
        video.adjust[2] = app.video.saturation / 100.0f;
      }
      video.tvGamma = !photo && player.tvGammaImage();
      video.mipmaps = photo || settings.videoMipmaps;  // photos always have theirs
      video.antiShimmer = antiShimmer;
      video.look = settings.toneLook;
      video.sharpen = settings.sharpen / 100.0f;
      video.key = app.video.greenScreen;
      video.correction = focusOut.correction;
      if (videoImage && xr.eyeCount() > 0) {
        const ScreenSurface screen(app.screen, eyeDisplayAspect(app.format, frameAspect));
        samplingLog.update(describeEyeSampling(app.format, photo ? photoWidth : player.width(),
                                               photo ? photoHeight : player.height(),
                                               app.screen.zoom,
                                               screen.width() / screen.distance(),
                                               xr.eyePixelsPerDegree(0, false),
                                               photo || settings.videoMipmaps),
                           now);
      }
      if (menu.setupRunning() && !videoImage) scene.video.reset();  // setup floats in the sky

      // Over the video, in drawing order: the screen's corner handles, the
      // focus marker, subtitles, loading text, the performance readout, the
      // menu's panels (browser, then strip), the seek preview, the pointer and
      // the messages in front of the eyes (GazeNotice).
      std::vector<OverlayDraw>& overlays = scene.overlays;
      const std::vector<Quad>& handles = screenControl.handle();
      if (!handles.empty()) overlays.push_back(QuadsItem{handles});
      focusMarker.addTo(overlays);
      auto addText = [&](SubtitleOverlay& text, const Mat4& model) {
        PanelItem item;
        item.texture = text.texture();
        item.model = model;
        overlays.push_back(item);
      };
      if (!subtitles.empty() && headValid && !loading)
        addText(subtitles, subtitlePlacement.update(app, player.aspect(), headPos,
                                                    yawFromQuat(headRot), settings));
      constexpr float kTextW = 1.4f, kTextH = kTextW / SubtitleOverlay::kAspect;
      if ((stillLoading || (!photo && player.failed())) && headValid) {
        // Raised a little: the text sits at the bottom of its texture.
        const Vec3 center = headPos + transformDir(frame, {0.0f, 0.1f, -2.0f});
        addText(loadingText, translation(center) * frame * scaling(kTextW, kTextH, 1));
      }
      if (showPerformance) {
        if (readout.due(now)) {
          PerformanceReadout::State state;
          const double period = xr.displayPeriod();
          state.displayRate = period > 0 ? 1.0 / period : 0;
          state.video = !photo && !app.currentFile.empty();
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
      if (panel.open) {
        // The textures first: a click may have changed the page and with it
        // what the panels show.
        for (const Menu::PanelId id : {Menu::kBrowser, Menu::kMain, Menu::kSetup}) {
          if (!menu.panelShown(id)) continue;
          PanelItem item;
          item.texture = menu.texture(id);
          item.curved = panel.panel(menu, id);
          if (id == Menu::kSetup) {
            item.opacity = setupPlacement.opacity();
            if (setupIntro.playing(now)) {  // its layers under the page, which fades in last
              setupIntro.addUnder(overlays, menu, *item.curved, item.opacity,
                                  menu.glass() ? Menu::kGlassAlpha : 0.0f, !kStandaloneHeadset, now);
              item.opacity *= setupIntro.pageOpacity(now);
            }
          }
          PanelHover hover;
          if (menu.hoverRect(id, hover.rect, hover.radius, hover.danger, hover.clip)) {
            for (int i = 0; i < 3; ++i)
              hover.rim[i] = static_cast<uint8_t>(std::lround(menu.accent().fill[i] * 255));
            item.hover = hover;
          }
          if (menu.glass()) item.glassAlpha = Menu::kGlassAlpha;
          overlays.push_back(item);
        }
        seekPreview.addTo(overlays);
        if (pointing) {
          LaserItem laser;
          laser.from = rayOrigin;
          laser.to = hitPanel ? hitPoint : rayOrigin + rayDir * laserLength;
          laser.dotRadius = kLaserDotRadius;
          overlays.push_back(laser);
        }
      } else if (heldSeekBar && (heldSeek.holding() || now < heldSeekBarUntil)) {
        // A held seek with the menu closed: the strip alone, no pointer.
        PanelItem item;
        item.texture = menu.texture(Menu::kMain);
        item.curved = panel.panel(menu, Menu::kMain);
        if (menu.glass()) item.glassAlpha = Menu::kGlassAlpha;
        overlays.push_back(item);
      }
      // Messages in front of the eyes, last, over everything: the recenter
      // countdown, and the viewpoint as the stick moves it (or Recenter puts
      // it back) or the opacity as a twist changes it, redrawn only as its
      // percent changes, a few times a second.
      if (app.recenterAt > 0 && headValid) {
        char text[64];
        std::snprintf(text, sizeof(text), strings().recenterIn,
                      static_cast<int>(std::ceil(app.recenterAt - now)));
        countdownNotice.addTo(overlays, text, headPos, headRot);
      }
      if (now < std::max(viewpointShownUntil, opacityShownUntil) &&
          app.format.projection != Projection::Flat && headValid) {
        char text[160];
        if (opacityShownUntil > viewpointShownUntil) {
          std::snprintf(text, sizeof(text), strings().opacityOverlay, app.video.opacity);
        } else {
          const int percent = static_cast<int>(std::lround(app.video.originForward * 100));
          std::snprintf(text, sizeof(text), strings().viewpoint.overlay,
                        viewpointText(percent).c_str());
        }
        valueNotice.addTo(overlays, text, headPos, headRot);
      }
      if (now < passthroughShownUntil && passthroughWhy && app.video.passthrough && headValid)
        passthroughNotice.addTo(
            overlays, std::string(passthroughWhy) + "\n" + strings().autoPassthrough.undo, headPos,
            headRot);
      if (now < driveShownUntil && headValid)
        driveNotice.addTo(overlays, driveText, headPos, headRot);
      if (headValid) hints.addTo(overlays, hintTexture, hintW, hintH, headPos, headRot, now);
      if (menu.setupRunning() && headValid) setupPlacement.addCues(overlays, menu, headPos, headRot, now);
      if (menu.setupRunning() && headValid && (menu.setupIntroDue() || setupIntro.playing(now)))
        setupIntro.addCover(overlays, menu, headPos, headRot, now);
    }
    // Match the room reads the view: nothing drawn meanwhile, so it shows the
    // room alone.
    const bool roomBlank = roomLight.blank();
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
      GpuContext::FrameSlot* slot = gpu.beginFrame();
      timer.lap(FrameTimer::Fence);
      if (slot) {
        SceneRenderer::GpuTimes gpuTimes;
        if (renderer.readTimes(*slot, gpuTimes)) {
          timer.gpu(gpuTimes.upload, gpuTimes.glass, gpuTimes.eye[0], gpuTimes.eye[1], gpuTimes.mirror,
                    gpuTimes.frame);
          if (showPerformance) readout.gpuTime(gpuTimes.frame);
        }
        foveation.update();  // the density maps' offsets for the images acquired now
        // Not over the room while Match the room reads it.
        scene.showFoveation = !roomBlank && showFoveation && foveation.active();
        if (scene.showFoveation)
          for (int i = 0; i < xr.eyeCount(); ++i) scene.foveation[i] = foveation.debug(i);
        const EyeFrame* eyes = xr.acquireEyes();
        if (eyes) mirror.acquire(*slot);
        timer.lap(FrameTimer::Acquire);
        if (eyes) {
          std::vector<SemaphoreWait> waits;
          renderer.record(*slot, *eyes, &xr.eye(0), scene, glfwGetTime(), waits);
          timer.lap(FrameTimer::Record);
          std::vector<VkSemaphore> signals;
          mirror.addSignals(signals);
          const uint64_t value = gpu.submit(*slot, waits, signals);
          drawn = value != 0;
          mirror.submitted(drawn);
          if (drawn) {
            interop.frameSubmitted(value);
            direct.frameSubmitted(value);
          } else {
            interop.frameAbandoned();
            direct.frameAbandoned();
          }
          timer.lap(FrameTimer::Submit);
        } else {
          gpu.abandonFrame(*slot);
        }
      }
    }
    gpu.collect();
    if (drawn && roomBlank) roomLight.blankSubmitted();
    xr.setAlphaBlend(blended || roomBlank);
    xr.releaseEyes();
    xr.endFrame(drawn);
    directDepth.submit();  // after the frame's work, which it never delays
    timer.lap(FrameTimer::XrEnd);
    mirror.present();  // after the XR frame, which never waits for the desktop
    timer.lap(FrameTimer::Swap);
    if (gpu.lost() || xr.failed()) {
      std::fprintf(stderr, "Stopping: the %s failed\n", gpu.lost() ? "GPU" : "OpenXR session");
      break;
    }
    timer.memory(gpu.liveBytes(), gpu.retiredBytes());
    if (timer.finishFrame() && opts.timing)
      std::printf("[render] eye=%dx%d images=%dx%d %s\n", xr.eyeWidth(0), xr.eyeHeight(0),
                  xr.eyeImageWidth(0), xr.eyeImageHeight(0), foveation.report().c_str());
    player.logTiming(panel.open, seekPreview.busy(), shouldRender, xr.isFocused());
  }
  if (quitSignal) std::printf("Quitting (signal %d)\n", static_cast<int>(quitSignal));
  saveUnsaved();
  keepVolume();
  keepResume();
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
  bool stashSync = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--hwdec=", 0) == 0) {
      opts.hwdec = arg.substr(8);
    } else if (arg.rfind("--preview-hwdec=", 0) == 0) {
      opts.previewHwdec = arg.substr(16);
    } else if (arg == "--no-mirror") {
      opts.mirror = false;
    } else if (arg == "--timing") {
      opts.timing = true;
    } else if (arg.rfind("--refresh-rate=", 0) == 0) {
      const std::string value = arg.substr(15);
      char* end = nullptr;
      const float hz = std::strtof(value.c_str(), &end);
      if (end == value.c_str() || *end != '\0' || !std::isfinite(hz) || hz < 0) {
        std::fprintf(stderr, "%s\n", strings().refreshRateUsage);
        return 2;
      }
      opts.refreshRate = hz;
    } else if (arg == "--show-performance") {
      opts.showPerformance = true;
    } else if (arg == "--show-foveation") {
      opts.showFoveation = true;
    } else if (arg.rfind("--menu-sharpness=", 0) == 0) {
      const std::string value = arg.substr(17);
      char* end = nullptr;
      const float bias = std::strtof(value.c_str(), &end);
      if (end == value.c_str() || *end != '\0' || !(bias >= -2.0f && bias <= 1.0f)) {
        std::fprintf(stderr, "--menu-sharpness=%s: give a mip bias from -2 to 1\n", value.c_str());
        return 2;
      }
      opts.menuLodBias = bias;
    } else if (arg.rfind("--sharp-bins=", 0) == 0) {
      const std::string value = arg.substr(13);
      if (value != "1" && value != "3") {
        std::fprintf(stderr, "%s\n", strings().sharpBinsUsage);
        return 2;
      }
      opts.sharpBins = value == "1" ? 1 : 3;
    } else if (arg.rfind("--ring=", 0) == 0) {
      const std::string value = arg.substr(7);
      if (value != "on" && value != "off") {
        std::fprintf(stderr, "%s\n", strings().foveationRingUsage);
        return 2;
      }
      opts.ring = value == "on";
    } else if (arg == "--xr-test") {
      opts.xrTest = true;
    } else if (arg == "--fixed-view") {
      opts.fixedView = true;
    } else if (arg == "--video-pipeline=on" || arg == "--video-pipeline=off") {
      opts.videoPipeline = arg == "--video-pipeline=on";
    } else if (arg == "--decoder-queue=on" || arg == "--decoder-queue=off") {
      opts.decoderQueue = arg == "--decoder-queue=on";
    } else if (arg.rfind("--foveation=", 0) == 0) {
      opts.foveationSet = true;
      if (!parseFoveationLevel(arg.substr(12), opts.foveation) || opts.foveation == FoveationLevel::Custom) {
        std::fprintf(stderr, "Unknown foveation level: %s\n", arg.substr(12).c_str());
        printUsage(argv[0]);
        return 2;
      }
    } else if (arg.rfind("--anti-shimmer=", 0) == 0) {
      opts.antiShimmerSet = true;
      if (!parseAntiShimmer(arg.substr(15), opts.antiShimmer)) {
        std::fprintf(stderr, "Unknown anti-shimmer: %s\n", arg.substr(15).c_str());
        printUsage(argv[0]);
        return 2;
      }
    } else if (arg.rfind("--foveation-gaze=", 0) == 0) {
      if (!parseFoveationGaze(arg.substr(17), opts.foveationGaze)) {
        std::fprintf(stderr, "Unknown foveation gaze: %s\n", arg.substr(17).c_str());
        printUsage(argv[0]);
        return 2;
      }
    } else if (arg.rfind("--resolution=", 0) == 0) {
      const std::string value = arg.substr(13);
      char* end = nullptr;
      const double number = std::strtod(value.c_str(), &end);
      const bool percent = end && *end == '%' && end[1] == '\0';
      if (!end || end == value.c_str() || (*end != '\0' && !percent) || !(number > 0) ||
          (percent ? number < kMinResolution || number > kMaxResolution
                   : number < 256 || number > 16384 || number != std::floor(number))) {
        std::fprintf(stderr, "--resolution=%s: give eye pixels across (256 to 16384) or a percent "
                     "(%d%% to %d%%)\n", value.c_str(), kMinResolution, kMaxResolution);
        return 2;
      }
      if (percent) {
        opts.resolutionPercent = number;
        opts.resolutionPixels = 0;
      } else {
        opts.resolutionPixels = static_cast<int>(number);
        opts.resolutionPercent = 0;
      }
    } else if (arg.rfind("--resolution-cycle=", 0) == 0) {
      // PIXELS,PIXELS,...[:SECONDS]
      std::string value = arg.substr(19);
      bool ok = true;
      const size_t colon = value.find(':');
      if (colon != std::string::npos) {
        char* end = nullptr;
        opts.resolutionCycleSeconds = std::strtod(value.c_str() + colon + 1, &end);
        ok = end && *end == '\0' && opts.resolutionCycleSeconds >= 1;
        value.resize(colon);
      }
      opts.resolutionCycle.clear();
      for (size_t at = 0; ok && at <= value.size();) {
        const size_t comma = std::min(value.find(',', at), value.size());
        char* end = nullptr;
        const long pixels = std::strtol(value.c_str() + at, &end, 10);
        ok = end == value.c_str() + comma && pixels >= 256 && pixels <= 16384;
        opts.resolutionCycle.push_back(static_cast<int>(pixels));
        at = comma + 1;
      }
      if (!ok) {
        std::fprintf(stderr, "--resolution-cycle=%s: give eye pixels across (256 to 16384), separated "
                     "by commas, and optionally :SECONDS (at least 1)\n", arg.substr(19).c_str());
        return 2;
      }
    } else if (arg.rfind("--sharp-area=", 0) == 0) {
      const std::string what = arg.substr(13);
      if (what != "bins" && what != "runtime") {
        std::fprintf(stderr, "Unknown sharp area: %s (bins or runtime)\n", what.c_str());
        return 2;
      }
      opts.runtimeDensityMaps = what == "runtime";
    } else if (arg.rfind("--open-menu=", 0) == 0) {
      const std::string page = arg.substr(12);
      opts.startMenu = page == "strip"      ? StartMenu::Strip
                       : page == "browser"  ? StartMenu::Browser
                       : page == "settings" ? StartMenu::Settings
                       : page == "seek"     ? StartMenu::Seek
                                            : StartMenu::None;
      if (opts.startMenu == StartMenu::None) {
        std::fprintf(stderr, "Unknown menu page: %s\n", page.c_str());
        printUsage(argv[0]);
        return 2;
      }
    } else if (arg == "-h" || arg == "--help") {
      printUsage(argv[0]);
      return 0;
    } else if (arg == "--first-run") {
      opts.firstRun = true;
    } else if (arg == "--stash-sync") {
      stashSync = true;
    } else if (arg == "--save-diagnostics") {
      // No log of its own: it would add one to what it saves.
      std::string error;
      const std::string path = saveDiagnostics(error);
      if (path.empty()) {
        std::fprintf(stderr, "Could not save diagnostics: %s\n", error.c_str());
        return 1;
      }
      std::printf("%s\n", path.c_str());
      return 0;
    } else if (arg == "--version") {
      std::printf("%s %s\n", kAppTitle, VIDEOFRAMEVR_VERSION);
      return 0;
    } else if (arg.rfind("--", 0) == 0) {
      std::fprintf(stderr, strings().unknownOption, arg.c_str());
      return 2;
    } else {
      opts.file = arg;
    }
  }
  std::atexit(restartIfRequested);  // before the run log starts, so it runs after the log ends
  startRunLog();
  printRunHeader();
  if (stashSync) return runStashSyncDiagnostic(loadSettings().stash);
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
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_SRGB_CAPABLE, GLFW_TRUE);
  // mpv imports Frame's V4L2 DMA-BUFs through EGL; GLX cannot expose that path.
  if (opts.hwdec == "v4l2m2m")
    glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
  // The window only carries the GL context; mapped, Frame shows it as an
  // empty black app window beside the headset view and Steam's menus. The
  // mirror has a window of its own.
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  GLFWwindow* window = glfwCreateWindow(1280, 720, kAppTitle, nullptr, nullptr);
  if (!window) {
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(0);  // the headset sets the pace, not the desktop monitor

  catchQuitSignals();
  const int rc = run(window, opts);
  disarmQuitWatchdog();
  Stash::finishStopping();  // a sync under way gives up within about a second

  glfwDestroyWindow(window);
  glfwTerminate();
  return rc;
}
