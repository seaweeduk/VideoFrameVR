#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>

// Reads the room's light on the Frame: its colour and how bright it looks.
// No supported interface gives apps the passthrough cameras' picture
// (OpenXR has no extension for it there; OpenVR's IVRTrackedCamera hands out
// no frames), so this reads the view SteamVR shows in the headset, which
// SteamOS also offers as a video device (its v4l2cam service, /dev/video99:
// the cameras' picture with what apps draw over it). It is undocumented: if
// it goes, available() is false and Match the room is not offered. While it
// reads, the player draws nothing for a moment (blank()), so the view shows
// the room alone. Frames stay in memory, one at a time, and only their sums
// are kept.
class RoomLight {
 public:
  struct Reading {
    bool colour = false;  // false: the cameras see no colour (grey, perhaps tinted)
    float light[3] = {1, 1, 1};  // the light's colour, linear red, green and blue (any scale)
    float white = 1;  // where the room's white sits, linear (1 = the display's)
  };

  ~RoomLight();
  // Whether the view can be read (the device is there).
  static bool available();
  // Starts a reading, unless one runs: whether it did (a reading that
  // couldn't start ends at once, failed).
  bool start();
  bool reading() const { return running_.load(); }
  // While true, the player draws nothing.
  bool blank() const { return blank_.load(); }
  // A frame drawn while blank() went to the headset.
  void blankSubmitted();
  // A reading that ended since the last call: true and `reading` set if it
  // succeeded, true and nothing set if it failed; false while none ended.
  bool take(std::optional<Reading>& reading);

 private:
  using Clock = std::chrono::steady_clock;
  void run();
  std::optional<Reading> read();

  std::thread thread_;
  std::atomic<bool> running_{false}, blank_{false}, stop_{false};
  std::atomic<Clock::rep> blankSince_{0};  // the first blank frame's submission; 0 none yet
  std::mutex mutex_;
  bool ended_ = false;
  std::optional<Reading> result_;
};
