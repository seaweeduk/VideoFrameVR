#pragma once

#include <filesystem>

// Where the player keeps its files: ~/.config/videoframevr (or under
// $XDG_CONFIG_HOME). A folder from the former name "vrplayer" is moved there.
std::filesystem::path configDir();
// Where it keeps what it collects (the media library): ~/.local/share/videoframevr
// (or under $XDG_DATA_HOME).
std::filesystem::path dataDir();
// Where it keeps what can be fetched again (Stash's images): ~/.cache/videoframevr
// (or under $XDG_CACHE_HOME).
std::filesystem::path cacheDir();
