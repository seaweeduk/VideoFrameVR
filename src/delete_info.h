#pragma once

#include <cstdint>
#include <string>

// The delete dialog's (docs/alpha/delete-file.md): the file playing, and
// what the app found out about it in the background. Until `ready`, only
// Cancel can be pressed.
struct DeleteInfo {
  std::string file;  // the file playing: a path or a remote locator (remote.h)
  std::string name;  // its file name
  bool photo = false;
  bool ready = false;
  uint64_t size = 0;
  // A downloaded copy (`file`) of `source`, on the drive at `copyRoot`:
  // Remove download deletes the copy; Delete everywhere, once the source
  // was found, the source too.
  std::string source, copyRoot;
  bool sourceFound = false;
  bool sourceChanged = false;  // not the video downloaded any more
  // A Stash scene's file (with Send to Stash on): Stash deletes it, the
  // scene too if it has no other files (`otherFiles`), into its trash
  // folder if it has one (`stashTrash`); or it is deleted here and Stash
  // lists it until its Clean runs, also when Stash's files didn't show
  // which one it is (Unsure).
  enum class Stash : uint8_t { None, Deletes, Lists, Unsure } stash = Stash::None;
  int otherFiles = 0;
  bool stashTrash = false;
};
