#pragma once

#include "anti_shimmer.h"
#include "foveation.h"

#include <optional>
#include <string>
#include <vector>

// --open-menu: the menu page opened at start.
enum class StartMenu { None, Strip, Browser, Settings, Seek };

struct Options {
  std::string file;
  // Empty selects defaults for the OpenXR GPU, after device creation.
  std::string hwdec;
  std::string previewHwdec;
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
  // --frame-reuse at start, never saved: -1 = the setting's on the Steam
  // Frame, off elsewhere, else 0/1.
  int frameReuse = -1;
  // --anti-shimmer for this run, never saved.
  bool antiShimmerSet = false;
  AntiShimmer antiShimmer = AntiShimmer::On;
  bool showPerformance = false;  // --show-performance for this run, never saved
  bool showFoveation = false;  // --show-foveation, never saved
  float menuLodBias = 0.0f;    // --menu-sharpness
  int sharpBins = 0, ring = -1;  // run-only overrides
  bool firstRun = false;  // --first-run: setup whatever the profile has
  bool stashSync = false;  // --stash-sync
};

// The command line into opts. Returns the exit code when the program ends
// here (usage, --version, --save-diagnostics, a bad option), else nothing.
std::optional<int> parseOptions(int argc, char** argv, Options& opts);
