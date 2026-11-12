#include "video_memory.h"

#include "file_util.h"
#include "settings.h"  // configDir

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace {

fs::path configFile(const char* name) { return configDir() / name; }

fs::path memoryPath() { return configFile("videos.conf"); }

// FNV-1a, 64 bit, as hex.
std::string hashOf(const std::string& location) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : location) {
    h ^= c;
    h *= 1099511628211ull;
  }
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

}  // namespace

void VideoMemory::load() {
  entries_.clear();
  std::ifstream in(memoryPath());
  std::string line;
  while (std::getline(in, line)) {
    VideoSettings s;
    // Named fields come last, in any order ("packing=2", "focus2=..."); the
    // numbers before them are read below.
    auto takeNamed = [&line](const char* name) {
      const std::string key = std::string(" ") + name + "=";
      const size_t at = line.find(key);
      if (at == std::string::npos) return std::string();
      const size_t end = line.find(' ', at + 1);
      const std::string token = line.substr(at + 1, end == std::string::npos ? end : end - at - 1);
      line.erase(at, end == std::string::npos ? end : end - at);
      return token;
    };
    const std::string packingToken = takeNamed("packing");
    const int packing = packingToken.empty() ? 0 : std::atoi(packingToken.c_str() + 8);
    const std::string focus = takeNamed("focus2"), oldFocus = takeNamed("focus");
    s.hasFocus = parseFocusToken(focus.empty() ? oldFocus : focus, s.focus);
    std::istringstream fields(line);
    std::string hash;
    int hasFormat = 0, projection = 0, stereo = 0;
    if (!(fields >> hash >> hasFormat >> projection >> stereo >> s.brightness >> s.contrast >>
          s.saturation >> s.zoom))
      continue;
    if (!(fields >> s.volume)) s.volume = -1;  // written before volumes were remembered
    int keyOn = 0;
    GreenScreen key;
    KeySliders& k = key.green;
    if (fields >> keyOn >> k.strength >> k.softness >> k.spill) {  // newer lines only
      key.on = keyOn != 0;
      if (!(fields >> k.shrink)) k.shrink = 0;
      // Versions wrote their own defaults for every video: those mean
      // "never set", so the video gets today's defaults.
      const bool oldDefaults = !key.on && k.shrink == 0 && k.strength == 50 &&
                               ((k.softness == 30 && k.spill == 50) ||
                                (k.softness == 0 && k.spill == 35));
      if (!oldDefaults) s.greenScreen = key;
    }
    if (projection < 0 || projection >= static_cast<int>(Projection::kCount) || stereo < 0 ||
        stereo >= static_cast<int>(StereoLayout::kCount))
      continue;
    s.hasFormat = hasFormat != 0;
    s.format.projection = static_cast<Projection>(projection);
    s.format.stereo = static_cast<StereoLayout>(stereo);
    if (packing > 0 && packing < static_cast<int>(Packing::kCount))
      s.format.packing = static_cast<Packing>(packing);
    entries_[hash] = s;
  }
}

bool VideoMemory::get(const std::string& location, VideoSettings& settings) const {
  const auto it = entries_.find(hashOf(location));
  if (it == entries_.end() || it->second.isDefault()) return false;
  settings = it->second;
  return true;
}

void FolderMemory::load() {
  entries_.clear();
  std::ifstream in(configFile("folders.conf"));
  std::string hash;
  FolderView view;
  int gallery = 0;
  while (in >> hash >> view.sort >> gallery) {
    view.gallery = gallery != 0;
    entries_[hash] = view;
  }
}

bool FolderMemory::get(const std::string& folder, FolderView& view) const {
  const auto it = entries_.find(hashOf(folder));
  if (it == entries_.end()) return false;
  view = it->second;
  return true;
}

void FolderMemory::set(const std::string& folder, const FolderView& view) {
  entries_[hashOf(folder)] = view;
  save();
}

void FolderMemory::save() const {
  queueConfigWork([entries = entries_] {
    const fs::path path = configFile("folders.conf");
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    for (const auto& [hash, view] : entries)
      out << hash << ' ' << view.sort << ' ' << (view.gallery ? 1 : 0) << '\n';
  });
}
