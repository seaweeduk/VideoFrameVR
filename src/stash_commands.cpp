#include "stash_commands.h"

#include "media_server.h"
#include "menu.h"
#include "menu_command.h"
#include "menu_view.h"
#include "settings.h"
#include "stash.h"
#include "stash_http.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

void StashCommands::restart() {
  menu.setStash(nullptr);
  stash.reset();
  if (settings.stash.enabled && !settings.stash.url.empty())
    stash = std::make_unique<Stash>(settings.stash, library);
  menu.setStash(stash.get());
}

bool StashCommands::handle(const MenuCommand& c) {
  switch (c.type) {
    case MenuCommand::UseServer:
      // None (-1), or a kind's own setup in use and on: the one it
      // replaces is kept as it was left.
      if (c.value < 0) {
        settings.stash.enabled = false;
      } else {
        useServer(settings.stash, settings.otherServers,
                  static_cast<ServerKind>(static_cast<int>(c.value)));
        settings.stash.enabled = true;
      }
      menu.findStashFolders(false);  // its folders are as they were left
      saveSettings(settings);
      restart();
      break;
    case MenuCommand::SetStashServer: {
      // The URL, then the API key; its value, the kind of server it is,
      // whose setup becomes the one in use.
      StashSettings changed = settings.stash;
      std::vector<StashSettings> others = settings.otherServers;
      useServer(changed, others, static_cast<ServerKind>(static_cast<int>(c.value)));
      const size_t line = c.path.find('\n');
      std::string url = c.path.substr(0, line);
      std::string key = line == std::string::npos ? std::string() : c.path.substr(line + 1);
      const size_t first = url.find_first_not_of(" \t");
      url = first == std::string::npos
                ? std::string()
                : url.substr(first, url.find_last_not_of(" \t") - first + 1);
      while (url.size() > 1 && url.back() == '/') url.pop_back();
      // A kind not set up yet, saved without an address: the setup in use
      // stays.
      if (url.empty() && changed.kind != settings.stash.kind) break;
      // The server this kind had, however its address was typed.
      const bool sameServer =
          !url.empty() && StashAddress(url).server == StashAddress(changed.url).server;
      // Its certificate accepted unverified (option 1) for its host alone.
      std::string unverifiedHost = (c.option & 1) && !url.empty() ? stashHost(url) : std::string();
      // Found to be another kind than its fields were for (option 2):
      // nothing was typed for it. At the server it had it keeps its own
      // key and whether its certificate is verified; at another, nothing
      // is sent to it unasked.
      if (c.option & 2) {
        if (sameServer) {
          key = changed.apiKey;
          unverifiedHost = changed.unverifiedHost;
        } else {
          changed.writeBack = false;
        }
      }
      changed.url = url;
      changed.apiKey = key;
      if (!url.empty()) changed.enabled = true;
      changed.card = changed.card && url.empty();  // set up: no "Use Stash?" card
      changed.unverifiedHost = std::move(unverifiedHost);
      if (changed.url != settings.stash.url || changed.apiKey != settings.stash.apiKey ||
          changed.kind != settings.stash.kind || changed.enabled != settings.stash.enabled ||
          changed.unverifiedHost != settings.stash.unverifiedHost) {
        // Another server than this kind had: where its folders are here
        // is looked for after its first sync, and one found in a single
        // place is used without asking. A setup gone back to keeps its
        // folders as they were left.
        if (!changed.url.empty() && !sameServer) menu.findStashFolders(true);
        else if (c.option & 2) menu.findStashFolders(false);
        settings.stash = std::move(changed);
        settings.otherServers = std::move(others);
        saveSettings(settings);
        restart();
      } else if (stash) {
        stash->syncNow();
      }
      break;
    }
    case MenuCommand::SetStashFolders: {
      // Each mapping's folder on the server and the folder here, one per
      // line.
      std::vector<std::string> lines;
      for (size_t start = 0; start < c.path.size();) {
        const size_t end = std::min(c.path.find('\n', start), c.path.size());
        lines.push_back(c.path.substr(start, end - start));
        start = end + 1;
      }
      std::vector<StashFolderMap> folders;
      for (size_t i = 0; i + 1 < lines.size(); i += 2)
        if (!lines[i].empty()) folders.push_back({lines[i], lines[i + 1]});
      if (folders != settings.stash.folders) {
        settings.stash.folders = std::move(folders);
        saveSettings(settings);
        restart();
      }
      break;
    }
    case MenuCommand::ToggleStashCovers:
      settings.stash.covers = !settings.stash.covers;
      saveSettings(settings);
      if (stash) stash->setCovers(settings.stash.covers);
      break;
    case MenuCommand::ToggleStashTitles:
      settings.stash.titles = !settings.stash.titles;
      saveSettings(settings);
      if (stash) stash->setTitles(settings.stash.titles);
      break;
    case MenuCommand::ToggleStashMalePerformers:
      settings.stash.malePerformers = !settings.stash.malePerformers;
      saveSettings(settings);
      if (stash) stash->setMalePerformers(settings.stash.malePerformers);
      break;
    case MenuCommand::SyncStash:
      if (stash) stash->syncNow();
      break;
    case MenuCommand::ToggleStashWriteBack:
      settings.stash.writeBack = !settings.stash.writeBack;
      saveSettings(settings);
      if (stash) stash->setWriteBack(settings.stash.writeBack);
      break;
    default: return false;
  }
  return true;
}

void StashCommands::connectInSetup(const MenuCommand& c) {
  // Connected in setup, as Settings sets it (the kind that answered
  // in its value), and setup ends there: its first sync starts at
  // once, and its folders are looked for once it is done, in the
  // places ticked too.
  const ServerKind kind = static_cast<ServerKind>(static_cast<int>(c.value));
  const size_t line = c.path.find('\n');
  const std::string url = c.path.substr(0, line);
  const std::string key = line == std::string::npos ? "" : c.path.substr(line + 1);
  const std::string unverifiedHost = c.option ? stashHost(url) : std::string();
  const bool same = stash && settings.stash.kind == kind && settings.stash.url == url &&
                    settings.stash.apiKey == key &&
                    settings.stash.unverifiedHost == unverifiedHost;
  useServer(settings.stash, settings.otherServers, kind);
  // Another server than this kind had: nothing is sent to it unasked.
  if (StashAddress(url).server != StashAddress(settings.stash.url).server)
    settings.stash.writeBack = false;
  settings.stash.enabled = true;
  settings.stash.url = url;
  settings.stash.apiKey = key;
  settings.stash.unverifiedHost = unverifiedHost;
  settings.stash.card = false;
  saveSettings(settings);
  if (!same) restart();
  std::printf("Setup: %s connected\n", mediaServer(kind).name());
}

void StashCommands::fill(MenuView& view) const {
  view.stash = &settings.stash;
  for (const MediaServer* server : mediaServers())
    if (serverSetUp(settings.stash, settings.otherServers, server->kind()))
      view.serversSetUp |= 1u << static_cast<int>(server->kind());
}
