#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Offline downloads (docs/alpha/offline-downloads.md): copies of videos kept
// on this device, each remembered as a copy of its video, in one folder,
// "Offline videos", at the top of a drive: internal storage (the home
// folder) or a microSD card or USB drive. One video at a time joins a queue,
// copied in order on a worker thread at the lowest priority: SMB and SFTP
// files with two blocks in flight, Stash's streams with playback's reader
// (stash_stream.h), local ones as they are. The queue is kept in
// dataDir()/offline.json, so a download continues after a restart, a lost
// connection or a card taken out: from its .part file's length less the
// last 8 MiB, or from the start if the source changed. A copy is checked by
// its size and oshash before it is renamed into place; its subtitles come
// with it. Downloads pause while a video plays. A second thread asks the
// servers signed in to whether they answer (away from home they don't), and
// Stash while a scene may play or download from its stream.
// Main thread only; nothing here waits for a file or the network.
class Offline {
 public:
  static constexpr const char* kFolder = "Offline videos";

  struct Drive {
    std::string root;   // where kFolder goes: the home folder, or a mount point
    std::string label;  // a removable drive's (empty if it has none)
    bool internal = false;
    bool card = false;  // a microSD card, else a USB or other drive
    uint64_t total = 0, free = 0;
    uint64_t used = 0;  // by the copies on it
  };
  enum class State { Queued, Copying, Paused, Waiting, Failed, Done };
  enum class Why { None, Server, Drive, Gone, NoRoom, Broken };  // Waiting or Failed
  struct Download {
    int64_t id = 0;
    std::string source;  // a path, a remote locator (remote.h) or a Stash scene's (stashSource())
    std::string root;    // the drive it goes to (Drive::root)
    std::string path;    // the copy, once its name is chosen
    std::string oshash;  // the video's, once known
    uint64_t size = 0, copied = 0;
    int64_t mtime = 0;   // the source's, when copying started
    int64_t doneAt = 0;  // Unix seconds
    std::vector<std::string> subtitles;  // copied beside it
    State state = State::Queued;
    Why why = Why::None;
    bool present = false;   // done, and the copy is there now
    double speed = 0;       // bytes a second, while copying
    double fraction() const { return size ? static_cast<double>(copied) / size : 0; }
  };

  Offline();
  ~Offline();
  Offline(const Offline&) = delete;
  Offline& operator=(const Offline&) = delete;

  // Reads the queue and starts the workers.
  void start();
  // Takes what changed; true if downloads(), drives() or the servers did.
  // `finished`: downloads completed since the last call (for the library).
  bool poll(std::vector<Download>& finished);
  uint64_t generation() const { return generation_; }

  // In the order they were added; those done after those to go.
  const std::vector<Download>& downloads() const { return downloads_; }
  const std::vector<Drive>& drives() const { return drives_; }
  const Drive* drive(const std::string& root) const;
  int toGo() const;
  // Bytes still to copy to the drive.
  uint64_t pending(const std::string& root) const;

  // The download of a file: by its source, its copy's path or the video's
  // oshash (empty: not known); null if none.
  const Download* find(const std::string& path, const std::string& oshash = "") const;
  // A copy of it that is there now (a path), else empty.
  std::string copyOf(const std::string& path, const std::string& oshash = "") const;

  // Queues `location` (`size` bytes as listed; its oshash if known) for the
  // drive at `root`, a Stash scene's locator as its source (stashSource()).
  // Nothing happens if it is queued or downloaded already.
  void add(const std::string& location, uint64_t size, const std::string& oshash,
           const std::string& root);
  void cancel(int64_t id);  // one to go: stops it and deletes its .part
  void remove(int64_t id);  // one done: deletes the copy and the subtitles it brought
  void retry(int64_t id);   // one that failed: queued again
  void pauseAll(bool paused);
  bool pausedAll() const { return pausedAll_; }
  // While a video plays, nothing is copied.
  void setPlaying(bool playing);
  // Drives came or went: looked at again now.
  void drivesChanged();

  // Whether `location` is on a server (remote.h) that doesn't answer now, or is
  // Stash's stream (a locator or a source) while Stash doesn't.
  bool serverDown(const std::string& location) const;
  // Stash is asked whether it answers while `watch` (a scene may play from
  // its stream) or a download from it is to go.
  void watchStash(bool watch);
  bool stashDown() const { return stashDown_; }
  // Whether every server signed in to is down (and there is one).
  bool allServersDown() const;
  const std::set<std::string>& downServers() const { return down_; }  // their roots

 private:
  struct Shared;
  void setServers(const std::vector<std::string>& hosts);
  std::chrono::steady_clock::time_point serversAt_{};
  std::shared_ptr<Shared> shared_;
  std::vector<Download> downloads_;
  std::vector<Drive> drives_;
  std::set<std::string> down_, servers_;
  uint64_t version_ = 0, generation_ = 0;
  bool pausedAll_ = false, playing_ = false, watchStash_ = false, stashDown_ = false;
  std::unordered_map<std::string, size_t> bySource_, byPath_, byHash_;
};
