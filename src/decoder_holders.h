#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

// Who else has the Frame's hardware video decoder open. The decoder fits a
// few sessions, and an 8K stream needs nearly all of it. Steam's interface
// (its web helper's GPU process) opens a session as the headset goes to
// sleep and keeps it until that process ends, one more after each sleep:
// after two, some 8K videos no longer start. Ending that process frees its
// sessions; Steam starts a new one at once and its interface carries on.
// Elsewhere (no such decoder) nothing holds it. These calls read /proc and
// wait: never on the render thread.
namespace decoder_holders {

// Why the decoder refused a video, as far as the player knows.
enum class Busy {
  No,         // not busy (or not known to be): the video itself
  Checking,   // the holders are being looked for
  Steam,      // Steam holds it: freeing it may let the video play
  Freeing,    // ending Steam's holders
  StillBusy,  // freed, or held by something else, and still refused
};

struct Holder {
  pid_t pid = 0;
  std::string name;  // the process's name, as /proc/PID/comm gives it
  bool steam = false;  // Steam's web helper, run by this user
};

// The processes besides this one holding the decoder open, this user's.
// Where their open files can't be read (the Steam Runtime's container),
// only Steam's web helper is found, by its decoding thread.
std::vector<Holder> find();

// Ends the Steam holders (find()) and waits until the decoder is free of
// them: whether it is. Asks them to end first, and forces those still there
// after two seconds.
bool freeFromSteam();

}  // namespace decoder_holders
