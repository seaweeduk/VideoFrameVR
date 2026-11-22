#pragma once

#include <memory>

class Library;
class Menu;
class Stash;
struct MenuCommand;
struct MenuView;
struct Settings;

// The media server from the menu: which kind is used, its address, its
// folders here and what it shows and sends; `stash` is made anew when its
// server changes. Main thread only.
class StashCommands {
 public:
  StashCommands(Settings& settings, Library& library, Menu& menu, std::unique_ptr<Stash>& stash)
      : settings(settings), library(library), menu(menu), stash(stash) {}

  // Stash switched on or off, or its server changed: made anew (the old one
  // finishes stopping in the background).
  void restart();
  // The command, if it is one of these; false for any other.
  bool handle(const MenuCommand& c);
  // The menu's SetupStash: a server connected in setup.
  void connectInSetup(const MenuCommand& c);
  void fill(MenuView& view) const;

 private:
  Settings& settings;
  Library& library;
  Menu& menu;
  std::unique_ptr<Stash>& stash;
};
