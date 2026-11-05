#pragma once

#include "read_ahead.h"
#include "remote.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The connection to one server that all of remote.h's users share. Its
// worker thread makes every call of the server's protocol (a Backend):
// jobs, playback's first, and between them the reads that keep playback's
// read-ahead filled (read_ahead.h).
namespace remote {

using Clock = std::chrono::steady_clock;

// A file open on the server.
struct Handle {
  void* file = nullptr;  // the backend's
  uint32_t tree = 0;     // SMB: its share's tree
  uint64_t epoch = 0;    // the connection's when it was opened (Connection::epoch)
  uint64_t length = 0;
};

// A file read ahead: playback's (mpv) or a download's (Reader).
struct Stream : ReadAhead {
  Handle handle;
  std::string locator;
  std::mutex mutex;
  std::condition_variable cv;
  std::atomic<bool> cancelled{false};
  bool failed = false, disconnected = false, closing = false, closed = false;
  bool reconnecting = false;
  bool background = false;  // a download's: reads only while no playback stream is open
  Clock::time_point retryUntil{};

  // With `mutex` held: a read failed. lost: the connection broke, so the
  // stream reconnects and reads on.
  void broke(bool lost) {
    disconnected = reconnecting = lost;
    failed = !lost;
    cv.notify_all();
  }
};

class Connection;

// A server's protocol: everything is called on its connection's worker
// thread. A call that waits for the server may pump() its connection.
class Backend {
 public:
  explicit Backend(Connection& connection) : c(connection) {}
  virtual ~Backend() = default;

  // Signs in anew (closing any connection) with `login`, which it may
  // change to the login to keep (sftp.cpp).
  virtual Status signIn(Login& login) = 0;
  virtual Status list(const std::string& folder, std::vector<Entry>& entries) = 0;
  virtual Status stat(const std::string& locator, Entry& entry) = 0;
  virtual Status remove(const std::string& locator, uint64_t size, int64_t modified) = 0;

  // Opens a file to read, connecting and signing in first if need be.
  virtual Status open(const std::string& locator, Handle& handle) = 0;
  // Closes a file of this connection (its epoch's), waiting.
  virtual void close(const Handle& handle) = 0;
  // Up to `count` bytes at `offset` (within its length) of a file open();
  // -1 on error.
  virtual int64_t readAt(const Handle& handle, uint64_t offset, void* out, uint64_t count) = 0;

  // Read-ahead blocks' size.
  virtual uint64_t blockSize() const { return ReadAhead::kBlock; }
  // With the stream's mutex held: starts reading the rest of `block`
  // (pending until every byte of it arrived, or it failed: Stream::broke()).
  virtual void read(Stream& stream, ReadAhead::Block& block) = 0;
  // With the stream's mutex held, its reads all ended: starts closing its
  // file, `closed` once done.
  virtual void closeStream(Stream& stream) = 0;

  virtual bool connected() const = 0;
  // Ends the connection; reads under way end as they are (blocks are
  // dropped after).
  virtual void disconnect() = 0;
  // What pump() polls for it: fd -1 for nothing.
  virtual int fd() const = 0;
  virtual short events() const = 0;
  // After the poll: handles what arrived (revents: fd's); false if the
  // connection broke.
  virtual bool service(short revents) = 0;
  // A job just ran.
  virtual void jobDone() {}

 protected:
  Connection& c;
};

class Connection {
 public:
  using MakeBackend = std::function<std::unique_ptr<Backend>(Connection&)>;
  Connection(std::string root, const MakeBackend& make);
  ~Connection();
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  const std::string root;

  // Any thread.
  void notify();
  void post(std::function<void()> job, bool priority = false);
  // Runs `fn` as a job and returns what it returns.
  template<class F> auto call(F fn, bool priority = false) {
    auto task = std::make_shared<std::packaged_task<decltype(fn())()>>(std::move(fn));
    auto result = task->get_future();
    post([task] { (*task)(); }, priority);
    return result.get();
  }

  // The worker thread's.
  Backend& backend() { return *backend_; }
  // Polls for up to `timeout` ms, keeping streams filled; false if the
  // connection broke.
  bool pump(int timeout = 20);
  // Ends the connection. retry: its streams reconnect (for up to 10 s), else
  // they fail.
  void destroy(bool retry = false);
  uint64_t epoch = 0;
  std::vector<std::shared_ptr<Stream>> streams;
  Clock::time_point deadline{};  // reconnecting a stream: requests give up by then

 private:
  void reconnect();
  void fill();
  void run();

  int wake_;
  std::mutex mutex_;
  std::deque<std::function<void()>> urgent_, background_;
  bool stop_ = false;
  Clock::time_point retryAt_{};
  std::unique_ptr<Backend> backend_;
  std::thread worker_;
};

// `root`'s connection (remote.cpp), made on first use.
std::shared_ptr<Connection> connection(const std::string& root);

// The backends (smb.cpp, sftp.cpp).
std::unique_ptr<Backend> smbBackend(Connection& connection);
std::unique_ptr<Backend> sftpBackend(Connection& connection);

}  // namespace remote
