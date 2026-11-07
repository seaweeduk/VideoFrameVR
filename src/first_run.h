#pragma once

#include <string>
#include <vector>

// First-run setup (docs/alpha/first-run.md): whether it shows, and what it
// has chosen so far. From its start to its end, configDir()/setup.conf says
// it is under way and keeps the choices, so quitting midway brings it back
// with them.

struct SetupChoices {
  std::string language;  // a languageCode(); empty: not chosen
  std::vector<std::string> places;  // Your videos' ticked places, pinned once it ends
};

// No settings.conf and no library.db. Ask before anything writes them.
bool newProfile();
// Setup was started and not finished, or this is a new profile.
bool setupDue();
SetupChoices loadSetupChoices();
// Writes the marker at once: before the files setupDue() looks for exist.
void markSetupStarted(const SetupChoices& choices);
// ... and later, on the config queue.
void saveSetupChoices(const SetupChoices& choices);
// Setup ended: the marker goes (on the config queue).
void endSetup();
