#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

class MediaClient;

// What a Stash worker's thread (sync, write-back, folder finding) shares
// with the main thread to be handed work and stopped. Each worker's state
// derives from it, and its thread waits on `wake` for that state to change.
struct StashWorkerShared {
  std::mutex mutex;
  std::condition_variable wake;
  bool stop = false;
  MediaClient* client = nullptr;  // the request under way, to stop it

  // Under the lock: the worker is to stop, and the request under way too.
  void halt();
};

// The thread of a Stash worker, and the state it shares with it (Shared,
// derived from StashWorkerShared). Destroyed, it stops the worker and waits
// for the thread. Main thread only.
template <class Shared>
class StashWorker {
 public:
  StashWorker() : shared_(std::make_shared<Shared>()) {}
  StashWorker(const StashWorker&) = delete;
  StashWorker& operator=(const StashWorker&) = delete;
  ~StashWorker() {
    stop();
    if (thread_.joinable()) thread_.join();
  }

  Shared* operator->() const { return shared_.get(); }

  // Runs work(shared) on the thread, unless it is running.
  template <class Work>
  void start(Work work) {
    if (!thread_.joinable()) thread_ = std::thread(std::move(work), shared_);
  }
  // Makes a change under the lock, then wakes the worker.
  template <class Change>
  void hand(Change change) {
    {
      std::lock_guard<std::mutex> lock(shared_->mutex);
      change();
    }
    shared_->wake.notify_one();
  }
  // Tells the worker to stop, without waiting.
  void stop() {
    hand([this] { shared_->halt(); });
  }

 private:
  std::shared_ptr<Shared> shared_;
  std::thread thread_;
};
