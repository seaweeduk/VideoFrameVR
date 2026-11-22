#pragma once

#include "foveation_level.h"
#include "tile_grid.h"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// One eye's density map as the app draws it: R8G8 texels (horizontal and
// vertical density), `pitch` bytes per row, each texel covering texel ×
// texel pixels of the eye image.
struct DensityMapPixels {
  uint8_t* texels = nullptr;
  size_t pitch = 0;
  VkExtent2D size{};
  uint32_t texel = 0;
};

// Where a drawn map puts things, in pixels of the map's own frame (the eye
// image before the offset moves it).
struct DensityMapLayout {
  VkOffset2D anchor{};  // where the gaze goes: the centre of a bin
  VkRect2D sharp{};     // the bins drawn at full rate around it
  uint32_t bins[3] = {};  // visible 1×1, 2×2, 4×4 bins at centred gaze
  double shadedFragments = 0;  // clipped to the centred eye, all rings
};

// The size in texels of `texel` pixels of the maps for an eye image of `eye`
// pixels drawn in `grid`'s bins, for every level: larger than the image, to
// keep coarse bins between the sharp area and the map's edges.
VkExtent2D densityMapExtent(const TileGrid& grid, VkExtent2D eye, uint32_t texel);

// Draws `level`'s map, of densityMapExtent(), for an eye image of `eye`
// pixels drawn in `grid`'s bins. Turnip
// shades each bin at the rate the map has at the bin's centre, and with
// offsets the bins keep their place in the map's frame, so the map is drawn
// in whole bins around one bin's centre, and the offsets move that centre
// to the gaze: the bins around the gaze are full rate at every eye size and
// the sharp area moves smoothly. The map stays the same from frame to
// frame. Off is full rate everywhere.
DensityMapLayout drawDensityMap(FoveationLevel level, const TileGrid& grid, VkExtent2D eye,
                                int sharpBins, bool ring, int outside, const DensityMapPixels& map,
                                std::vector<uint8_t>* binRates = nullptr);
