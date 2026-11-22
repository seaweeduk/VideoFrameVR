#pragma once

#include <string>

// How much the eye images are foveated on the Steam Frame
// (docs/vulkan-renderer.md, 5): full detail where you look, coarser shading
// further out. Persisted by name (settings.cpp).
enum class FoveationLevel { Off, Low, Medium, High, Custom };

constexpr int kDefaultSharpBins = 1;
constexpr bool kDefaultFoveationRing = true;

const char* foveationLevelName(FoveationLevel level);
bool parseFoveationLevel(const std::string& name, FoveationLevel& level);
void foveationPreset(FoveationLevel level, int& sharpBins, bool& ring, int& outside);
FoveationLevel foveationLevel(bool enabled, int sharpBins, bool ring, int outside);
// Off, followed by the eight combinations of sharp area, ring and outside.
int foveationMapIndex(bool enabled, int sharpBins, bool ring, int outside);
