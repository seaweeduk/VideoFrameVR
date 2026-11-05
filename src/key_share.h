#pragma once

#include <memory>
#include <string>

// This device's public key, served over HTTP on the local network for a
// while, so an SFTP server taking keys only can fetch it into
// authorized_keys with one command ("curl address:port >> ...").
//
// It listens on this device's IPv4 address on the route to the server only,
// on the first free port from 47321 to 47330, for ten minutes, and answers
// GET / with the key and nothing else. All network work is on a thread of its
// own; destruction stops it without waiting.
class KeyShare {
 public:
  // Starts serving `key` (an authorized_keys line) for the server `host`.
  KeyShare(const std::string& host, int port, const std::string& key);
  ~KeyShare();
  KeyShare(const KeyShare&) = delete;
  KeyShare& operator=(const KeyShare&) = delete;

  // Stopped: it couldn't start (no IPv4 route, no free port), or its time
  // is up.
  enum class State { Starting, Serving, Stopped };
  State state() const;
  // Serving: where, "10.0.0.5:47321".
  std::string address() const;
  // How many times the key was fetched.
  int fetches() const;

 private:
  struct Shared;
  std::shared_ptr<Shared> shared_;
};
