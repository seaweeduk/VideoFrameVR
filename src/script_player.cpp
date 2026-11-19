#include "script_player.h"

#include "funscript.h"
#include "intiface.h"
#include "platform.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>

namespace {

// A position that hasn't changed for this long is a video standing still.
// mpv reports one per video frame, so this is three frames of a 10 fps video.
constexpr double kStallSeconds = 0.3;
// The video's clock is where the last position says, adjusted a little by
// each new one; one further off than this is a seek.
constexpr double kSeekSeconds = 0.1;
constexpr int kConnectMs = 3000;      // a try to reach Intiface gives up after this
constexpr int kRetryMs = 5000;        // ... and the next one starts this much later
constexpr int kScanAgainMs = 15000;   // a scan the server ended is asked for again after this
// Commands are sent at least this far apart: no device takes them faster,
// and a Bluetooth one's queue would fall behind the video. Of actions closer
// together than this, the ones in between are left out. (The gap Intiface
// reports for a device isn't used: to this version of the protocol it says
// 100 ms for all of them.)
constexpr int kMinGapMs = 20;
// The first move after the video starts, resumes or jumps brings the device
// from wherever it is to the script: over at least this long for a whole
// stroke, less in proportion.
constexpr int kCatchUpMs = 200;
constexpr int kHoldMs = 100;  // the move that stops the device where the video stopped

double steadySeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

std::string scriptAddress(const ScriptSettings& settings) {
  if (!settings.address.empty()) return settings.address;
  return kStandaloneHeadset ? "" : "localhost";
}

struct ScriptPlayer::Shared {
  std::mutex mutex;
  int wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);  // written to wake the worker
  bool quit = false;
  ScriptSettings settings;
  std::shared_ptr<const Funscript> script;  // the playing video's; null: none (or not read yet)
  uint64_t load = 0;                        // counts open(): a script read for an earlier one is dropped
  // The video's clock: while `playing`, it is at steadySeconds() + base.
  // `jump` counts every break in it (stops, starts and seeks), after which
  // the device is sent afresh.
  bool playing = false;
  double base = 0;
  uint64_t jump = 0;
  ScriptStatus status;
  std::promise<void> done;  // the worker has stopped the device and gone

  ~Shared() {
    if (wake >= 0) close(wake);
  }
  void wakeWorker() {
    const uint64_t one = 1;
    if (wake >= 0 && write(wake, &one, sizeof(one)) < 0) {
      // Full: it is waking already.
    }
  }
};

namespace {

void run(std::shared_ptr<ScriptPlayer::Shared> shared) {
  ScriptPlayer::Shared& s = *shared;
  Intiface link;
  std::string connectedTo;
  bool tried = false;  // the first try's outcome is known
  int64_t retryAt = 0, scanAt = 0, pingAt = 0;
  int devicesSeen = 0;  // Intiface::added() as of the last move
  // What the device was last sent: which break in the clock, script and
  // settings it was for, the action it is on its way from, and where to.
  uint64_t sentJump = 0;
  const Funscript* sentScript = nullptr;
  ScriptSettings sentSettings;
  int sentAction = -2;
  bool sent = false;  // any of the script since the video last started moving
  // The last move sent, which says where the device is: from and to where
  // along the stroke, when, and how long it takes.
  struct Move {
    double from = 0, to = 0, ms = 1;
    int64_t at = 0;
    bool known = false;  // anything was sent since the device appeared
    bool under = false;  // it may still be on its way
    bool guessed = false;  // it set off from an unknown place: only where it ends is known
  } move;

  const auto nowMs = [] { return static_cast<int64_t>(steadySeconds() * 1000); };
  const auto where = [&] {
    return move.from + (move.to - move.from) * std::clamp((nowMs() - move.at) / move.ms, 0.0, 1.0);
  };
  // Whether where() is where the device is.
  const auto placed = [&] { return move.known && (!move.guessed || nowMs() - move.at >= move.ms); };
  for (;;) {
    ScriptSettings settings;
    std::shared_ptr<const Funscript> script;
    bool quit, playing;
    double base;
    uint64_t jump;
    {
      std::lock_guard lock(s.mutex);
      quit = s.quit;
      settings = s.settings;
      script = s.script;
      playing = s.playing;
      base = s.base;
      jump = s.jump;
    }
    const auto place = [&](double pos) {
      return (settings.rangeLow + pos / 100.0 * (settings.rangeHigh - settings.rangeLow)) / 100.0;
    };
    // Stops the device: a short move to where its last one has taken it.
    // Whatever stopped the video (or the script, the connection's address,
    // the app), the device doesn't finish a stroke the video didn't. Except
    // one from an unknown place: sent where it is guessed to be, it would
    // hurry there. (Intiface's StopDeviceCmd doesn't stop a stroke.)
    const auto hold = [&] {
      if (move.under && !move.guessed && link.connected() && !link.devices().empty() &&
          nowMs() - move.at < move.ms) {
        const double here = where();
        link.move(here, kHoldMs);
        move = {here, here, kHoldMs, nowMs(), true, false, false};
      }
      move.under = false;
    };
    if (quit) {
      hold();
      break;
    }

    // The connection: another address closes it, and none is tried again
    // every few seconds.
    const std::string address = scriptAddress(settings);
    ScriptStatus status;
    if (link.connected() && address != connectedTo) {
      hold();
      link.close();
      retryAt = 0;
      tried = false;
    }
    if (address != connectedTo && !link.connected()) {
      tried = false;
      retryAt = 0;
    }
    if (!link.connected() && !address.empty() && nowMs() >= retryAt) {
      if (!tried) {
        std::lock_guard lock(s.mutex);
        s.status.link = ScriptLink::Connecting;
        s.status.device.clear();
        s.status.devices = 0;
      }
      connectedTo = address;
      if (link.connect(address, kConnectMs)) {
        std::printf("Intiface: connected\n");
        pingAt = scanAt = 0;
        sent = false;
      } else {
        retryAt = nowMs() + kRetryMs;
      }
      tried = true;
      continue;  // the settings may have changed meanwhile
    }
    int64_t wakeAt = INT64_MAX;
    const int64_t now = nowMs();
    const bool wasConnected = link.connected();
    if (!link.connected()) {
      status.link = address.empty() ? ScriptLink::NoAddress
                    : tried         ? ScriptLink::NoServer
                                    : ScriptLink::Connecting;
      if (!address.empty()) wakeAt = retryAt;
      move = {};
      sent = false;
    } else {
      // Bluetooth devices are only found while the server scans: it does
      // until one is there. Not before it has said which it has already.
      if (link.devices().empty()) {
        if (link.listed() && !link.scanning() && now >= scanAt) {
          link.scan(true);
          scanAt = now + kScanAgainMs;
        }
        if (link.listed() && !link.scanning()) wakeAt = std::min(wakeAt, scanAt);
      } else if (link.scanning()) {
        link.scan(false);
      }
      if (link.maxPingMs() > 0) {
        if (now >= pingAt) {
          link.ping();
          pingAt = now + std::max(1, link.maxPingMs() / 2);
        }
        wakeAt = std::min(wakeAt, pingAt);
      }
      status.link = !link.listed()            ? ScriptLink::Connecting
                    : link.devices().empty() ? ScriptLink::NoDevice
                                             : ScriptLink::Ready;
      status.devices = static_cast<int>(link.devices().size());
      if (!link.devices().empty()) status.device = link.devices().front().name;
      // A device that has just appeared may be anywhere, and is sent to the
      // script at once, with the others.
      if (link.devices().empty() || link.added() != devicesSeen) {
        devicesSeen = link.added();
        move = {};
        sent = false;
      }
    }

    // The script: at each action, the move to the next one.
    if (link.connected() && !link.devices().empty() && script && playing) {
      const double t = (steadySeconds() + base) * 1000 + settings.offsetMs;
      const std::vector<Funscript::Action>& actions = script->actions;
      const int count = static_cast<int>(actions.size());
      const int at = script->actionAt(t);
      const bool fresh = !sent || jump != sentJump || script.get() != sentScript ||
                         settings != sentSettings;
      const bool due = fresh || at > sentAction;
      const int64_t soonest = move.known ? move.at + kMinGapMs : 0;
      if (due && nowMs() < soonest) {
        wakeAt = std::min(wakeAt, soonest);
      } else {
        if (due) {
          // After the last action, its place: where the script ends, also
          // when the actions before it came too close together to send.
          const Funscript::Action& next = actions[std::min(at + 1, count - 1)];
          const double target = place(next.pos);
          const bool known = placed();
          const double from = known ? where() : target;
          // A device not where the script has it takes its time to get there.
          const double catchUp = !fresh ? 0 : (known ? std::abs(target - from) : 1.0) * kCatchUpMs;
          // While the script rests it is already there, not on its way until the rest ends.
          const bool resting = fresh && (at < 0 || actions[at].pos == next.pos);
          const double ms = std::max({resting ? 0.0 : next.at - t, catchUp, 1.0});
          // One already there (the script rests) isn't sent again.
          if (fresh || target != move.to) {
            link.move(target, static_cast<int>(std::lround(ms)));
            move = {from, target, ms, nowMs(), true, true, !known};
          }
          sentAction = at;
          sentJump = jump;
          sentScript = script.get();
          sentSettings = settings;
          sent = true;
        }
        if (sentAction + 1 < count)
          wakeAt = std::min(wakeAt, nowMs() + static_cast<int64_t>(std::ceil(std::max(
                                                  0.0, actions[sentAction + 1].at - t))));
      }
    } else if (link.connected()) {
      hold();
      sent = false;
    }
    // A command that couldn't be sent closed the connection.
    if (wasConnected && !link.connected()) {
      std::printf("Intiface: connection lost\n");
      retryAt = nowMs() + 1000;
      continue;
    }

    {
      std::lock_guard lock(s.mutex);
      if (s.quit) continue;
      status.script = s.script != nullptr;
      s.status = status;
    }
    pollfd waits[2] = {{s.wake, POLLIN, 0}, {link.fd(), POLLIN, 0}};
    const int64_t wait = wakeAt == INT64_MAX ? -1 : std::max<int64_t>(0, wakeAt - nowMs());
    poll(waits, link.connected() ? 2 : 1, static_cast<int>(std::min<int64_t>(wait, 60000)));
    if (waits[0].revents) {
      uint64_t count;
      while (read(s.wake, &count, sizeof(count)) > 0) {
      }
    }
    if (link.connected() && waits[1].revents && !link.receive()) {
      std::printf("Intiface: connection lost\n");
      retryAt = nowMs() + 1000;
    }
  }
  link.close();
  s.done.set_value();
}

}  // namespace

ScriptPlayer::~ScriptPlayer() { stop(true); }

void ScriptPlayer::configure(const ScriptSettings& settings) {
  const bool was = settings_.enabled;
  settings_ = settings;
  if (was && !settings.enabled) stop(false);
  if (!settings.enabled) return;
  if (!was) {
    start();
    return;
  }
  std::lock_guard lock(shared_->mutex);
  if (shared_->settings == settings) return;
  shared_->settings = settings;
  shared_->wakeWorker();
}

void ScriptPlayer::start() {
  shared_ = std::make_shared<Shared>();
  shared_->settings = settings_;
  shared_->status.link = ScriptLink::Connecting;
  playing_ = false;
  lastSeconds_ = -1;
  std::thread(run, shared_).detach();
  load();
}

// The worker is told to go and left to: it may be waiting on a connection
// for seconds. `wait`: but for the moment it takes to stop the device.
void ScriptPlayer::stop(bool wait) {
  if (!shared_) return;
  std::future<void> done = shared_->done.get_future();
  {
    std::lock_guard lock(shared_->mutex);
    shared_->quit = true;
    shared_->wakeWorker();
  }
  if (wait) done.wait_for(std::chrono::milliseconds(250));
  shared_.reset();
}

void ScriptPlayer::open(const std::string& location) {
  location_ = location;
  if (shared_) load();
}

// Reads the playing video's script on a thread of its own: its server may
// take seconds to answer, and the worker keeps time meanwhile.
void ScriptPlayer::load() {
  uint64_t mine;
  // The last video's clock isn't this one's: it starts when this one moves.
  playing_ = false;
  {
    std::lock_guard lock(shared_->mutex);
    mine = ++shared_->load;
    shared_->script.reset();
    shared_->status.script = false;
    shared_->playing = false;
    shared_->wakeWorker();
  }
  if (location_.empty()) return;
  std::thread([shared = shared_, location = location_, mine] {
    auto script = std::make_shared<Funscript>();
    if (!parseFunscript(readFunscript(location), *script)) return;
    std::lock_guard lock(shared->mutex);
    if (shared->quit || shared->load != mine) return;
    std::printf("Script: %zu actions\n", script->actions.size());
    shared->script = std::move(script);
    shared->status.script = true;
    shared->wakeWorker();
  }).detach();
}

void ScriptPlayer::sync(double seconds, bool playing) {
  if (!shared_) return;
  const double now = steadySeconds();
  if (seconds != lastSeconds_) {
    lastSeconds_ = seconds;
    lastChange_ = now;
  } else if (!playing_ && !playing) {
    return;
  }
  const bool moving = playing && now - lastChange_ < kStallSeconds;
  const double base = seconds - now;
  if (moving == playing_) {
    if (!moving || lastChange_ != now) return;
    // Each new position corrects the clock a little: readily forward (a
    // late one only looks behind), slowly back.
    const double off = base - base_;
    const bool seek = std::abs(off) > kSeekSeconds;
    base_ = seek ? base : base_ + off * (off > 0 ? 0.5 : 0.02);
    std::lock_guard lock(shared_->mutex);
    shared_->base = base_;
    if (seek) {
      ++shared_->jump;
      shared_->wakeWorker();
    }
    return;
  }
  playing_ = moving;
  base_ = base;
  std::lock_guard lock(shared_->mutex);
  shared_->playing = moving;
  shared_->base = base;
  ++shared_->jump;
  shared_->wakeWorker();
}

ScriptStatus ScriptPlayer::status() const {
  if (!shared_) return {};
  std::lock_guard lock(shared_->mutex);
  return shared_->status;
}
