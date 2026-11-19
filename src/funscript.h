#pragma once

#include <string>
#include <vector>

// A funscript: a device's stroke position over a video's time, as the
// .funscript file beside the video has it (JSON: "actions" of {"at":
// milliseconds, "pos": 0..100}). Only the stroke's script is read, not those
// of other axes (video.roll.funscript and the like).
struct Funscript {
  struct Action {
    int at = 0;   // milliseconds from the video's start
    int pos = 0;  // 0 (the stroke's bottom) .. 100 (its top)
  };
  std::vector<Action> actions;  // in time order, one per time, at least two

  // The action playing at `ms`: the last one at or before it; -1 before the first.
  int actionAt(double ms) const;
  // Where the stroke is at `ms`, 0..100: on the line between its actions,
  // at the first one's place before it and the last one's after it.
  double positionAt(double ms) const;
};

// A script's text as a Funscript, "inverted" applied. Its "range" is left
// out: the range set in Settings limits the stroke instead. False if the
// text isn't a script with at least two actions.
bool parseFunscript(const std::string& text, Funscript& script);

// The text of the script of the video at `location`: the file beside a local
// or remote (remote.h) video with its name and .funscript, or what Stash
// serves for a scene played from its stream. Empty if there is none. Blocks:
// worker threads only.
std::string readFunscript(const std::string& location);
// Where that file is, for a local or remote video; empty for anything else.
std::string funscriptBeside(const std::string& location);
