#include "location.h"

#include "remote.h"
#include "stash_stream.h"

#include <filesystem>

bool isStream(const std::string& location) {
  return location.find("://") != std::string::npos && !remote::isRemote(location);
}

bool isLink(const std::string& location) { return isStream(location) && !isStashScene(location); }

std::string parentFolder(const std::string& path) {
  return remote::isRemote(path) ? remote::parent(path) : std::filesystem::path(path).parent_path().string();
}
