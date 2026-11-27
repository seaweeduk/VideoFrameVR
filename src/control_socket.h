#pragma once

#include <poll.h>
#include <sys/types.h>

#include <cstddef>
#include <string>
#include <vector>

class CommandInbox;
class PlaybackSession;
class VideoPlayer;

// --control=PATH: a Unix socket at PATH, for scripts on this machine. Each
// side sends one JSON object a line: a command by its MenuCommand type's
// name, with its value, path and option, which is posted to the inbox and
// runs in this frame's poll; each line is answered. Answers are given as
// lines are read, before the frame runs the commands: a status asked in the
// same frame as a command shows the state before it. {"get": "status"} is
// answered with what plays (PlaybackStatus), whether the menu is open, the
// player's load count and, while it counts them, its frame and drop counts.
// Without the flag there
// is no socket and update() does nothing. Render thread only: one
// non-blocking poll() a frame, and nothing that waits.
class ControlSocket {
 public:
  ControlSocket() = default;
  ControlSocket(const ControlSocket&) = delete;
  ControlSocket& operator=(const ControlSocket&) = delete;
  ~ControlSocket();

  // Listens at `path`, for this user only. A socket left there by a run
  // that was killed is replaced; anything else there is left alone, and
  // then there is no socket (false, with the reason printed).
  bool listen(const std::string& path);
  // Each frame: takes new clients, reads their whole lines, posts the
  // commands and answers.
  struct Status {
    const PlaybackSession& session;
    const VideoPlayer& player;
    bool menuOpen;
  };
  void update(CommandInbox& inbox, const Status& status);

 private:
  struct Client {
    int fd = -1;
    std::string in, out;  // read, not yet a whole line; answers not yet sent
  };
  int listen_ = -1;
  std::string path_;
  dev_t device_ = 0;  // of the socket file made, removed only if still that one
  ino_t inode_ = 0;
  std::vector<Client> clients_;
  std::vector<pollfd> polls_;  // kept from frame to frame

  void accept();
  bool read(Client& client);
  // Answers and posts up to a few whole lines; false drops the client.
  bool handleLines(Client& client, CommandInbox& inbox, const Status& status);
  void answer(Client& client, const char* begin, const char* end, CommandInbox& inbox,
              const Status& status);
  bool flush(Client& client);
  void drop(size_t i);
};
