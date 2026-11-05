#pragma once

#include <filesystem>
#include <string>

// Copies stdout/stderr for this run; drains the copy at normal process exit.
// The copy leaves out the home folder (as ~) and the user's name.
void startRunLog();

// Drains the copy now, for an exit that skips the normal one (_exit).
void endRunLog();

// The version, build and system at the top of the log: what a report needs
// first. No host or user names.
void printRunHeader();

// From now on the log shows `word` (a server's host, say) as `shownAs`, where
// it stands alone (not inside a longer name).
void hideInRunLog(const std::string& word, const std::string& shownAs);

// The run before this one crashed (its log ends with a crash).
bool previousRunCrashed();

// Where the run logs are kept ($XDG_STATE_HOME/<app>/logs).
std::filesystem::path runLogDir();

// Text as the log writes it: without the home folder, the user's name and
// the hidden words (for logs written before these were left out).
std::string anonymised(const std::string& text);

// The host of a URL ("http://user@host:port/path" gives "host"), to hide.
std::string urlHost(const std::string& url);
