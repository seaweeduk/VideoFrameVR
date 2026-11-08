#pragma once

// Whether this build runs on a standalone headset rather than a PC: the
// ARM64 Linux build. SteamVR for Linux on ARM64 exists only on the Steam
// Frame, so that is the Frame running the app natively (docs/frame-native.md).
// Its defaults suit the Frame's hardware (settings.h).
constexpr bool kStandaloneHeadset =
#if defined(__aarch64__) && defined(__linux__)
    true;
#else
    false;
#endif

// On the Frame, the cores background decoding runs on (thumbnails, the video
// check), of those Steam gives the app: the compositor's render thread uses 4-6.
constexpr int kFrameBackgroundCores[] = {3, 7};

// The app's name: in its config, data and cache paths and as the OpenXR and
// Vulkan application name (SteamVR derives its app key from it; no spaces),
// and as shown to people (window title, version).
constexpr const char kAppName[] = "videoframevr";
constexpr const char kAppTitle[] = "VideoFrameVR";
