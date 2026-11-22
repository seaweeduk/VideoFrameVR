#pragma once

struct MenuCommand;
struct MenuView;
struct Settings;

// The menu commands that only change a setting and save it. Returns false,
// changing nothing, for any other command.
bool applySettingCommand(const MenuCommand& c, Settings& settings);
// Settings' Controls: a button's job, a controller type's buttons and
// gestures (SetButtonJob, RestoreButton, ResetButtons, AddGesture,
// ChangeGesture, RemoveGesture, RestoreGestures). True if they changed
// (saved).
bool changeControls(const MenuCommand& c, Settings& settings);
// Funscripts' settings (ToggleScripts, SetScriptAddress, SetScriptOffset,
// SetScriptRangeLow, SetScriptRangeHigh), saved.
void changeScripts(const MenuCommand& c, Settings& settings);
// The settings the menu shows as they are.
void fillSettings(MenuView& view, const Settings& settings);
