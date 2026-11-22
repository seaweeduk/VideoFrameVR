#pragma once

struct MenuCommand;
struct Settings;

// The menu commands that only change a setting and save it. Returns false,
// changing nothing, for any other command.
bool applySettingCommand(const MenuCommand& c, Settings& settings);
