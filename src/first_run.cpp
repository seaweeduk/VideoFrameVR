#include "first_run.h"

#include "file_util.h"
#include "settings.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace {

fs::path markerPath() { return configDir() / "setup.conf"; }

void writeMarker(const SetupChoices& choices) {
  std::error_code ec;
  fs::create_directories(configDir(), ec);
  std::ofstream out(markerPath(), std::ios::trunc);
  out << "language=" << choices.language << "\n";
  for (const std::string& place : choices.places) out << "place=" << place << "\n";
}

}  // namespace

bool newProfile() {
  std::error_code ec;
  return !fs::exists(configDir() / "settings.conf", ec) && !fs::exists(dataDir() / "library.db", ec);
}

bool setupDue() {
  std::error_code ec;
  return fs::exists(markerPath(), ec) || newProfile();
}

SetupChoices loadSetupChoices() {
  SetupChoices choices;
  std::ifstream in(markerPath());
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("language=", 0) == 0) choices.language = line.substr(9);
    if (line.rfind("place=", 0) == 0 && line.size() > 6) choices.places.push_back(line.substr(6));
  }
  return choices;
}

void markSetupStarted(const SetupChoices& choices) { writeMarker(choices); }

void saveSetupChoices(const SetupChoices& choices) {
  queueConfigWork([choices] { writeMarker(choices); });
}

void endSetup() {
  queueConfigWork([] {
    std::error_code ec;
    fs::remove(markerPath(), ec);
  });
}
