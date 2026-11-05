#include "remote_connection.h"

#include <algorithm>
#include <cerrno>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace remote {

Connection::Connection(std::string name, const MakeBackend& make)
    : root(std::move(name)), wake_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)), backend_(make(*this)),
      worker_([this] { run(); }) {}

Connection::~Connection() {
  { std::lock_guard lock(mutex_); stop_ = true; }
  notify();
  worker_.join();
  ::close(wake_);
}

void Connection::notify() { uint64_t n = 1; (void)::write(wake_, &n, sizeof(n)); }

void Connection::post(std::function<void()> job, bool priority) {
  { std::lock_guard lock(mutex_); (priority ? urgent_ : background_).push_back(std::move(job)); }
  notify();
}

void Connection::destroy(bool retry) {
  for (auto& s : streams) {
    std::lock_guard lock(s->mutex);
    s->reconnecting = retry && !s->failed && !s->cancelled && !s->closing;
    if (!s->reconnecting) s->failed = s->closed = true;
    if (s->reconnecting && s->retryUntil == Clock::time_point{})
      s->retryUntil = Clock::now() + std::chrono::seconds(10);
  }
  backend_->disconnect();
  ++epoch;
  for (auto& s : streams) {
    std::lock_guard lock(s->mutex);
    s->blocks.clear();
    s->disconnected = false;
    s->failed = s->closed = !s->reconnecting;
    s->cv.notify_all();
  }
  if (!retry) streams.clear();
}

void Connection::reconnect() {
  const auto it = std::find_if(streams.begin(), streams.end(),
                               [](auto& s) { return s->reconnecting && !s->closed; });
  if (it == streams.end()) return;
  const auto s = *it;
  if (Clock::now() < retryAt_) return;
  Status status = Status::Offline;
  Handle handle;
  if (!s->cancelled && Clock::now() < s->retryUntil) {
    deadline = s->retryUntil;
    status = backend_->open(s->locator, handle);
    deadline = {};
  }
  std::lock_guard lock(s->mutex);
  if (status == Status::Ok) {
    s->handle = handle;
    ++s->generation;
    s->windows.clear();
    s->reconnecting = false;
  } else if (s->cancelled || status != Status::Offline || Clock::now() >= s->retryUntil) {
    s->failed = s->closed = true;
    s->reconnecting = false;
    s->cv.notify_all();
  } else {
    retryAt_ = Clock::now() + std::chrono::milliseconds(250);
  }
}

void Connection::fill() {
  streams.erase(std::remove_if(streams.begin(), streams.end(),
                               [](auto& s) { return s->closed; }), streams.end());
  if (!backend_->connected()) return;
  const bool playing = std::any_of(streams.begin(), streams.end(),
                                   [](auto& s) { return !s->background && !s->closed; });
  for (auto& s : streams) {
    std::lock_guard lock(s->mutex);
    if (s->closed || s->closing || s->reconnecting) continue;
    if (s->cancelled || s->failed) {
      if (std::any_of(s->blocks.begin(), s->blocks.end(), [](auto& b) { return b.pending; }))
        continue;
      s->closing = true;
      backend_->closeStream(*s);
      continue;
    }
    if (s->windows.empty()) s->seek(s->position);
    s->retire();
    if (s->background && playing) continue;
    s->queue(backend_->blockSize(), s->handle.length);
    for (auto& b : s->blocks)
      if (!b.pending && !b.done && b.generation == s->generation) backend_->read(*s, b);
  }
}

bool Connection::pump(int timeout) {
  fill();
  pollfd fds[2] = {{backend_->fd(), backend_->events(), 0}, {wake_, POLLIN, 0}};
  const int rc = poll(fds, 2, timeout);
  if (fds[1].revents) { uint64_t n; (void)::read(wake_, &n, sizeof(n)); }
  if ((rc < 0 && errno != EINTR) || !backend_->service(fds[0].revents)) return false;
  return std::none_of(streams.begin(), streams.end(),
                      [](auto& s) { return s->disconnected && !s->closed; });
}

void Connection::run() {
  for (;;) {
    std::function<void()> job;
    {
      std::lock_guard lock(mutex_);
      if (stop_ && urgent_.empty() && background_.empty()) break;
      auto& jobs = urgent_.empty() ? background_ : urgent_;
      if (!jobs.empty()) { job = std::move(jobs.front()); jobs.pop_front(); }
    }
    reconnect();
    fill();
    if (job) {
      job();
      backend_->jobDone();
    }
    // A job's waits may have consumed another queued job's wake-up.
    if (!pump(job ? 0 : 20)) destroy(true);
  }
  destroy();
}

}  // namespace remote
