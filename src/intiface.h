#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A connection to Intiface (Intiface Central or Intiface Engine,
// intiface.com): the app that drives the devices themselves, over Bluetooth,
// USB or serial, on this computer or another one on the network. It listens
// on a WebSocket (not an encrypted one: it offers none) and speaks the
// Buttplug protocol (buttplug.io), JSON messages. This client speaks version
// 3 of it, which Intiface Central 2 and later all take, and only what playing
// a script needs: the devices that move along a stroke (LinearCmd) are
// listed and moved, the others left alone.
//
// Not threaded: ScriptPlayer's worker owns it and makes every call. Only
// connect() waits, for at most its timeout; the rest returns at once.
class Intiface {
 public:
  // A device that moves along a stroke.
  struct Device {
    int index = 0;      // the server's number for it
    std::string name;   // the user's name for it in Intiface, else its model
    int strokes = 1;    // how many of its parts move so: all of them get each move
  };

  Intiface() = default;
  ~Intiface();
  Intiface(const Intiface&) = delete;
  Intiface& operator=(const Intiface&) = delete;

  // Connects to "host", "host:port" or "ws://host:port" (port 12345,
  // Intiface's own, when none is given) and asks for its devices. False if
  // nothing there answers as Intiface within `timeoutMs`.
  bool connect(const std::string& address, int timeoutMs);
  void close();
  bool connected() const { return fd_ >= 0; }
  // The socket, to wait on for what the server sends (-1: not connected).
  int fd() const { return fd_; }
  // Takes in what the server has sent. False once the connection is lost
  // (it is closed then).
  bool receive();

  const std::vector<Device>& devices() const { return devices_; }
  // Whether the server has said which devices it has: until then the list is empty.
  bool listed() const { return listed_; }
  // Counts the devices that have appeared, over all connections.
  int added() const { return added_; }
  // Moves every device to `position` (0 the stroke's bottom .. 1 its top)
  // over `ms` milliseconds.
  void move(double position, int ms);

  // Whether the server looks for devices (Bluetooth ones are only found
  // while it does). It may stop by itself.
  bool scanning() const { return scanning_; }
  void scan(bool on);

  // The server closes a connection it hears no Ping on for this long,
  // milliseconds; 0: it asks for none.
  int maxPingMs() const { return maxPingMs_; }
  void ping();

 private:
  bool sendFrame(int opcode, std::string_view payload);
  // One Buttplug message of `type` with these fields and the next Id.
  bool sendMessage(const char* type);
  bool sendMessage(const char* type, nlohmann::json fields);
  bool waitReadable(int64_t untilMs);
  bool readSome();
  // The next whole message of those read; false if none is complete.
  bool nextMessage(std::string& text);
  void handle(const std::string& text);

  int fd_ = -1;
  bool gotServerInfo_ = false;
  bool listed_ = false;
  int added_ = 0;
  bool scanning_ = false;
  bool errorShown_ = false;
  int maxPingMs_ = 0;
  uint32_t nextId_ = 1;
  std::string in_;       // bytes read, not yet a whole frame
  std::string message_;  // the frames of a message still arriving
  bool fragmentText_ = false;
  std::vector<Device> devices_;
};
