#pragma once

#include "deo_protocol.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

class CommandInbox;
class StashCatalog;
struct MenuView;
struct PlaybackStatus;

// How the remote control is doing, for Settings.
struct RemoteControlStatus {
  enum class Link : uint8_t {
    Off,        // the setting is off
    Starting,   // on, not listening yet
    Listening,  // on the port
    PortInUse,  // another app has the port (tried again every few seconds)
    Failed,     // the network can't be listened on (tried again too)
  };
  Link link = Link::Off;
  std::string address;  // this device's on the network; empty if none was found
  int clients = 0;      // connected
  bool operator==(const RemoteControlStatus& o) const {
    return link == o.link && address == o.address && clients == o.clients;
  }
  bool operator!=(const RemoteControlStatus& o) const { return !(*this == o); }
};

// The remote control: other apps open, seek, pause and play videos, and
// follow where they are, as they do with DeoVR's remote control API
// (deo_protocol.h). Off by default; while off there is no thread and no
// socket.
//
// A worker thread owns every socket: it listens on DeoVR's port, sends each
// client the status once a second, and at once when what plays changes, and
// reads what they send. Up to eight clients, all equal (DeoVR serves one).
// One that sends no whole packet for 3 seconds, sends a packet over 64 KiB
// or stops reading is dropped. The render thread hands it the
// status and takes what clients asked for, under one lock a frame, and
// never touches a socket.
class DeoRemote {
 public:
  DeoRemote() = default;
  ~DeoRemote();
  DeoRemote(const DeoRemote&) = delete;
  DeoRemote& operator=(const DeoRemote&) = delete;

  struct Frame {
    bool enabled;  // Settings::remoteControl
    const PlaybackStatus& status;
    bool setupRunning;  // first-run setup shows: requests are refused
    // The media server's scenes, while one is in use: a scene streamed from
    // it is told by the server's path of its file.
    const StashCatalog* catalog;
  };
  // Each frame, in poll: starts or stops the worker with `enabled`, hands
  // it the status, and posts what clients asked for to the inbox.
  void update(const Frame& frame, CommandInbox& inbox);
  void fill(MenuView& view) const;

  struct Shared;

 private:
  using Clock = std::chrono::steady_clock;

  void start();
  void stop();
  // What a request comes to: an open, a seek, a pause; refused during setup.
  void act(const Frame& frame, CommandInbox& inbox);
  // What a client's path opens: a path, a locator or a link; empty if refused.
  std::string openable(const std::string& asked) const;
  // The path reported for what is open (worked out when it changes); empty
  // for a scene the server's scenes don't have.
  std::string reportedPath(const Frame& frame) const;

  std::shared_ptr<Shared> shared_;  // while on
  std::thread worker_;
  std::string reported_;  // the path last handed to the worker
  deo::Report said_;      // ... and the rest, as last frame (without the path)
  bool seeking_ = false;  // ... and whether a seek was under way
  // ... worked out for this, and the catalog then.
  std::string reportedFor_;
  const StashCatalog* reportedCatalog_ = nullptr;
  // What clients asked for and the frame hasn't done yet: an open waits a
  // second after the last one, newer requests replacing it.
  deo::Request pending_;
  bool hasPending_ = false;
  Clock::time_point nextOpen_{};
  RemoteControlStatus shown_;
  uint64_t shownGeneration_ = 0;
  Clock::time_point shownAt_{};  // taken at most four times a second
  // Started without a way to wake a worker: started again from this time.
  Clock::time_point startAgainAt_{};
  bool startFailSaid_ = false;
};
