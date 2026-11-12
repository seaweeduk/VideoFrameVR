#include "foveation.h"

#include "density_map.h"
#include "xr_context.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

const char* const kLevelNames[] = {"off", "low", "medium", "high", "custom"};

float angle(float from, float to, float a, float b) {
  return std::abs(std::atan(from + (to - from) * std::clamp(b, 0.f, 1.f)) -
                  std::atan(from + (to - from) * std::clamp(a, 0.f, 1.f))) * 57.29578f;
}

// One eye's offset in pixels, on the GPU's offset grid: what moves the map's
// anchor to a centre in NDC.
VkOffset2D offsetFor(const float centre[2], int width, int height, VkOffset2D anchor,
                     VkExtent2D granularity) {
  auto snap = [](float px, uint32_t step) {
    const float s = static_cast<float>(std::max(1u, step));
    return static_cast<int32_t>(std::lround(px / s) * s);
  };
  return {snap((centre[0] + 1) * width * 0.5f - anchor.x, granularity.width),
          snap((centre[1] + 1) * height * 0.5f - anchor.y, granularity.height)};
}

}  // namespace

const char* foveationLevelName(FoveationLevel level) { return kLevelNames[static_cast<int>(level)]; }

bool parseFoveationLevel(const std::string& name, FoveationLevel& level) {
  for (int i = 0; i < 5; ++i) {
    if (name == kLevelNames[i]) {
      level = static_cast<FoveationLevel>(i);
      return true;
    }
  }
  return false;
}

void foveationPreset(FoveationLevel level, int& sharpBins, bool& ring, int& outside) {
  sharpBins = level == FoveationLevel::Low || level == FoveationLevel::Medium ? 3 : 1;
  ring = level != FoveationLevel::Low;
  outside = level == FoveationLevel::Low ? 2 : 4;
}

FoveationLevel foveationLevel(bool enabled, int sharpBins, bool ring, int outside) {
  if (!enabled) return FoveationLevel::Off;
  if (outside == 2) return sharpBins == 3 ? FoveationLevel::Low : FoveationLevel::Custom;
  if (!ring) return FoveationLevel::Custom;
  return sharpBins == 3 ? FoveationLevel::Medium : FoveationLevel::High;
}

int foveationMapIndex(bool enabled, int sharpBins, bool ring, int outside) {
  return enabled ? 1 + (sharpBins == 3 ? 4 : 0) + (ring ? 2 : 0) + (outside == 4 ? 1 : 0) : 0;
}

bool parseFoveationGaze(const std::string& text, FoveationGaze& gaze) {
  if (text == "tracked") {
    gaze.mode = FoveationGaze::Tracked;
    return true;
  }
  if (text == "centre" || text == "center") {
    gaze.mode = FoveationGaze::Centre;
    return true;
  }
  float x = 0, y = 0;
  char end = 0;
  if (std::sscanf(text.c_str(), "%f,%f%c", &x, &y, &end) != 2 || !std::isfinite(x) ||
      !std::isfinite(y) || std::abs(x) > 1 || std::abs(y) > 1)
    return false;
  gaze = {FoveationGaze::Fixed, x, y};
  return true;
}

void Foveation::init(XrContext& xr, FoveationLevel level, const FoveationGaze& gaze, int sharpBins, bool ring, int outside) {
  requested_ = level;
  setShape(sharpBins, ring, outside);
  gaze_ = gaze;
  if (!xr.foveated()) return;
  xr_ = &xr;
  granularity_ = xr.gpuDevice().caps.densityOffsetGranularity;
  for (int i = 0; i < xr.eyeCount() && i < 2; ++i) {
    anchor_[i] = {xr.eyeWidth(i) / 2, xr.eyeHeight(i) / 2};
    sharp_[i] = {};
  }
  // The rough menu sizes need the FOV from the first located frame.
  if (xr.appDensityMaps()) {
    resized_ = true;
  } else {
    if (!apply(level)) std::fprintf(stderr, "Foveation: could not start at %s\n", foveationLevelName(level));
    requested_ = applied_;
  }
  std::printf("Foveation: %s, %s density maps, sharp area %s\n", foveationLevelName(requested_),
              xr.appDensityMaps() ? "the app's" : "SteamVR's",
              gaze_.mode == FoveationGaze::Tracked  ? "where SteamVR's eye tracking puts it"
              : gaze_.mode == FoveationGaze::Centre ? "straight ahead"
                                                    : "at a fixed point");
}

bool Foveation::apply(FoveationLevel level) {
  if (!xr_->appDensityMaps()) {
    // SteamVR's maps are made for the whole images; the size in use is a
    // rectangle at their origin.
    for (int i = 0; i < xr_->eyeCount() && i < 2; ++i)
      anchor_[i] = {xr_->eyeImageWidth(i) / 2, xr_->eyeImageHeight(i) / 2};
    const bool ok = xr_->setFoveationLevel(level);
    applied_ = xr_->foveationLevel();
    return ok;
  }
  const bool ok = xr_->drawDensityMaps([&](int eye, const DensityMapPixels& map) {
    const VkExtent2D size{static_cast<uint32_t>(xr_->eyeWidth(eye)), static_cast<uint32_t>(xr_->eyeHeight(eye))};
    const DensityMapLayout layout =
        drawDensityMap(level, eyePassTileGrid(xr_->gpuDevice().caps, size.width, size.height), size,
                       sharpBins_, ring_, outside_, map);
    if (eye < 2) {
      anchor_[eye] = layout.anchor;
      sharp_[eye] = layout.sharp;
      std::copy_n(layout.bins, 3, bins_[eye]);
    }
    if (eye == 0) {
      const auto grid = xr_->tileGrid();
      const auto& view = xr_->eye(0);
      const float x = float(grid.binWidth) / size.width, y = float(grid.binHeight) / size.height;
      binDegrees_ = {int(std::lround(angle(view.tanLeft, view.tanRight, .5f - x / 2, .5f + x / 2))),
                     int(std::lround(angle(view.tanLeft, view.tanRight, .5f - 3 * x / 2, .5f + 3 * x / 2))),
                     int(std::lround(angle(view.tanUp, view.tanDown, .5f - y / 2, .5f + y / 2)))};
    }
  });
  if (ok) {
    applied_ = level;
    drawnSharpBins_ = sharpBins_;
    drawnRing_ = ring_;
    drawnOutside_ = outside_;
  }
  return ok;
}

void Foveation::compareMaps() {
  if (!xr_->appDensityMaps()) return;
  const VkExtent2D size{static_cast<uint32_t>(xr_->eyeWidth(0)), static_cast<uint32_t>(xr_->eyeHeight(0))};
  const auto grid = eyePassTileGrid(xr_->gpuDevice().caps, size.width, size.height);
  std::array<std::vector<uint8_t>, 9> rates;
  for (int i = 0; i < 9; ++i) {
    const int shape = std::max(0, i - 1);
    drawDensityMap(i == 0 ? FoveationLevel::Off : FoveationLevel::Custom, grid, size,
                   shape & 4 ? 3 : 1, shape & 2, shape & 1 ? 4 : 2, {}, &rates[i]);
    equivalentMaps_[i] = i;
    for (int j = 0; j < i; ++j) {
      if (rates[i] == rates[j]) {
        equivalentMaps_[i] = equivalentMaps_[j];
        break;
      }
    }
  }
}

void Foveation::prepare() {
  if (!xr_) return;
  const bool resized = resized_;
  resized_ = false;
  if (resized) compareMaps();
  if (requested_ != applied_ || resized ||
      (xr_->appDensityMaps() && (sharpBins_ != drawnSharpBins_ || ring_ != drawnRing_ || outside_ != drawnOutside_)) ||
      (!xr_->appDensityMaps() && applied_ != xr_->foveationLevel())) {
    ++mapsVersion_;
    if (!apply(requested_)) {
      std::fprintf(stderr, "Foveation: could not change to %s, still %s\n",
                   foveationLevelName(requested_), foveationLevelName(applied_));
      // The maps drawn for the old size no longer fit: drawn without any.
      if (resized && xr_->appDensityMaps()) applied_ = FoveationLevel::Off;
      requested_ = applied_;
      sharpBins_ = drawnSharpBins_;
      ring_ = drawnRing_;
      outside_ = drawnOutside_;
    }
  }
  if (!xr_->foveated()) xr_ = nullptr;  // given up on (XrContext::setFoveationLevel)
}

void Foveation::update() {
  if (!xr_) return;
  const auto start = std::chrono::steady_clock::now();

  // The centres SteamVR reports, also while it flags them invalid: it
  // handles blinks and lost tracking itself (docs/vulkan-renderer.md, 5.2).
  // On an error or a malformed centre the sharp area stays ahead.
  float centres[2][2] = {};
  bool valid = false;
  if (applied_ != FoveationLevel::Off) {
    if (gaze_.mode == FoveationGaze::Tracked) {
      // Asking for the latest pattern right before reading the centre
      // (XR_META_foveation_eye_tracked).
      bool ok = xr_->refreshFoveation();
      ok = xr_->foveationCentres(centres, valid) && ok;
      if (!ok) {
        ++failures_;
        centres[0][0] = centres[0][1] = centres[1][0] = centres[1][1] = 0;
      }
      for (auto& c : centres) {
        if (!std::isfinite(c[0]) || !std::isfinite(c[1]) || std::abs(c[0]) > 1.5f ||
            std::abs(c[1]) > 1.5f) {
          ++malformed_;
          c[0] = c[1] = 0;
        }
      }
    } else if (gaze_.mode == FoveationGaze::Fixed) {
      for (auto& c : centres) {
        c[0] = gaze_.x;
        c[1] = gaze_.y;
      }
    }
  }
  // Off: drawn without a density map, at full rate everywhere.
  xr_->useDensityMaps(applied_ != FoveationLevel::Off);
  for (int i = 0; i < xr_->eyeCount() && i < 2; ++i) {
    offset_[i] = offsetFor(centres[i], xr_->eyeWidth(i), xr_->eyeHeight(i), anchor_[i], granularity_);
    xr_->setDensityOffset(i, offset_[i]);
    centre_[i][0] = centres[i][0];
    centre_[i][1] = centres[i][1];
  }
  ++frames_;
  if (valid) ++valid_;
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  cpuMs_ += ms;
  cpuMaxMs_ = std::max(cpuMaxMs_, ms);
}

FoveationDebug Foveation::debug(int eye) const {
  return {{(centre_[eye][0] + 1) * xr_->eyeWidth(eye) * 0.5f,
           (centre_[eye][1] + 1) * xr_->eyeHeight(eye) * 0.5f, 0, 0}};
}

void Foveation::sharpDegrees(float& width, float& height) const {
  width = height = 0;
  if (!xr_ || !xr_->appDensityMaps()) return;
  const EyeMatrices& e = xr_->eye(0);
  const float w = float(xr_->eyeWidth(0)), h = float(xr_->eyeHeight(0));
  const VkRect2D& r = sharp_[0];
  const float x = float(r.offset.x + offset_[0].x), y = float(r.offset.y + offset_[0].y);
  width = angle(e.tanLeft, e.tanRight, x / w, (x + r.extent.width) / w);
  height = angle(e.tanUp, e.tanDown, y / h, (y + r.extent.height) / h);
}

std::string Foveation::report() {
  char buf[384];
  if (!xr_) {
    std::snprintf(buf, sizeof(buf), "level=none (not foveated)");
  } else {
    const int n = std::max(1, frames_);
    const TileGrid& grid = xr_->tileGrid();
    char gridText[48] = "unknown", sharpText[48] = "unknown";
    if (grid.known())
      std::snprintf(gridText, sizeof(gridText), "%ux%u_of_%ux%u", grid.columns, grid.rows, grid.binWidth,
                    grid.binHeight);
    if (xr_->appDensityMaps()) {
      float width, height;
      sharpDegrees(width, height);
      const VkRect2D& r = sharp_[0];
      std::snprintf(sharpText, sizeof(sharpText), "%ux%upx/%.0fx%.0fdeg", r.extent.width, r.extent.height,
                    width, height);
    }
    std::snprintf(buf, sizeof(buf),
                  "level=%s map=%s grid=%s sharp=%s sharp_bins=%d ring=%s outside=%d centre_bins_1_2_4=%u,%u,%u/%u,%u,%u gaze=%s gaze_valid=%.0f%% centre=%.2f,%.2f "
                  "fov_cpu_ms=%.3f max %.3f fov_errors=%d malformed=%d",
                  foveationLevelName(applied_), xr_->appDensityMaps() ? "app" : "steamvr", gridText,
                  sharpText, drawnSharpBins_, drawnRing_ ? "on" : "off", drawnOutside_,
                  bins_[0][0], bins_[0][1], bins_[0][2], bins_[1][0], bins_[1][1], bins_[1][2],
                  gaze_.mode == FoveationGaze::Tracked  ? "tracked"
                  : gaze_.mode == FoveationGaze::Centre ? "centre"
                                                        : "fixed",
                  100.0 * valid_ / n, centre_[0][0], centre_[0][1], cpuMs_ / n, cpuMaxMs_, failures_,
                  malformed_);
  }
  frames_ = valid_ = failures_ = malformed_ = 0;
  cpuMs_ = cpuMaxMs_ = 0;
  return buf;
}
