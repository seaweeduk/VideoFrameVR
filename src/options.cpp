#include "options.h"

#include "diagnostics.h"
#include "platform.h"
#include "strings.h"
#include "value_ranges.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

void printUsage(const char* argv0) {
  std::printf("Usage: %s [--hwdec=API] [--preview-hwdec=API] [--no-mirror] [--timing] [--xr-test]\n"
              "          [--fixed-view] [--open-menu=PAGE] [--foveation=LEVEL] [--foveation-gaze=WHERE]\n"
              "          [--resolution=SIZE] [--resolution-cycle=SIZES[:SECONDS]] [--sharp-area=WHAT]\n"
              "          [--video-pipeline=on|off] [--decoder-queue=on|off] [--anti-shimmer=off|on|strong]\n"
              "          [--frame-reuse=on|off]\n"
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
              "--decoder-queue=off disables the Frame's two-frame V4L2 decoder queue for this run.\n"
              "--frame-reuse=on shows the last drawn frame again, reprojected by the runtime, while\n"
              "a new one would look the same (no new video frame, nothing else drawn, the head\n"
              "nearly still); off draws every frame. Without it, the Steam Frame follows the setting\n"
              "Skip repeated frames (on by default) and other devices draw every frame. The option\n"
              "is never saved; changing the setting in the menu replaces it for the rest of the run.\n",
              argv0);
  std::printf("%s\n", strings().refreshRateHelp);
}

}  // namespace

std::optional<int> parseOptions(int argc, char** argv, Options& opts) {
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
    } else if (arg == "--frame-reuse=on" || arg == "--frame-reuse=off") {
      opts.frameReuse = arg == "--frame-reuse=on";
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
      opts.stashSync = true;
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
  return std::nullopt;
}
