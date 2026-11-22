#pragma once

#include "stash_settings.h"
#include "stash_worker.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// A video of the app's library that was rated or played, by its oshash, as
// the one-off send reads it (a copy; the library is never changed).
struct StashLibraryEntry {
  std::string oshash;
  float rating = 0;  // stars, 0 = unrated
  int plays = 0;
  int64_t lastPlayedAt = 0;  // Unix seconds
};

// How write-back stands, for Settings.
struct StashWriteBackStatus {
  int waitingRatings = 0, waitingPlays = 0;  // kept until Stash can be reached
  int sentRatings = 0, sentPlays = 0;        // this session
  std::string error;                         // why the last send failed; empty if it didn't
};

// "Send my ratings and plays": what it would send, then what it sent.
struct StashSendPlan {
  enum class State { None, Checking, Ready, Sending, Done, Failed } state = State::None;
  struct Item {
    std::string oshash;
    int64_t scene = 0;
    std::string name;
    float stars = 0;          // the rating to send, 0 = none
    int stashRating100 = -1;  // ... and Stash's now (-1 = none)
    int plays = 0;            // plays to send
    int found = 0;            // ... and those Stash has already (not sent)
    int64_t playedAt = 0;     // their time: the app keeps only the last play's
  };
  std::vector<Item> items;  // those with something to send, by name
  int ratings = 0, plays = 0, scenes = 0;
  int found = 0;    // plays Stash has already, left out
  int waiting = 0;  // changes waiting from before, sent too
  int sentRatings = 0, sentPlays = 0;  // Done
  std::string error;                   // Failed
};

// Tags edited on the Scene tab, saved to one scene: how the last save went.
// Not kept anywhere: the page keeps the edits until a save works.
struct StashTagSave {
  enum class State { None, Saving, Saved, Failed } state = State::None;
  int64_t scene = 0;
  int64_t at = 0;  // Saved: when (Unix seconds)
};

// Sends the app's ratings and plays to Stash (docs/stash.md, "Write-back
// safety"), push only, on a worker thread of its own: the changes the app
// makes as they happen, kept in the server's outbox until Stash takes them
// (sent again whenever asked, e.g. after a sync), and the one-off send.
// Only scenes whose file Stash knows by its oshash; a play Stash already
// has (in its play_history) is never sent again. Main thread only.
class StashWriteBack {
 public:
  explicit StashWriteBack(const StashSettings& settings);
  // Stops a request under way and waits for the worker.
  ~StashWriteBack();
  StashWriteBack(const StashWriteBack&) = delete;
  StashWriteBack& operator=(const StashWriteBack&) = delete;
  // Tells the worker to stop, without waiting: what was handed to it is
  // still kept in the file first, then nothing more is sent.
  void stop();

  // A rating made in the app (0: cleared there, sent as null); plays.
  void rated(const std::string& oshash, float stars);
  void played(const std::string& oshash, int64_t at, int count);
  // Sends what waits (e.g. Stash may be reachable again).
  void deliver();
  // Write-back switched off: the request under way is stopped, nothing more
  // is sent and the one-off is closed; what waits is kept, and the changes
  // handed over before still go to the file. On again: what waits is sent.
  // Never waits for the worker.
  void setEnabled(bool on);

  // The one-off send: plan() compares the library with Stash and says what
  // send() would do (sendPlan()); send() does it, with the library as it is
  // then. Neither ever clears a rating.
  void plan(std::vector<StashLibraryEntry> library);
  void send(std::vector<StashLibraryEntry> library);
  void closePlan();  // back to None

  // Adds and removes these Stash tag ids on a scene, the rest of its tags
  // kept; tagSave() says how it went. One at a time.
  void saveTags(int64_t scene, std::vector<int64_t> add, std::vector<int64_t> remove);

  // Takes what the worker finished; true if status(), sendPlan() or
  // tagSave() changed.
  bool poll();
  const StashWriteBackStatus& status() const { return status_; }
  const StashSendPlan& sendPlan() const { return plan_; }
  const StashTagSave& tagSave() const { return tagSave_; }

 private:
  struct Shared;
  static void work(std::shared_ptr<Shared> shared);

  StashWriteBackStatus status_;
  StashSendPlan plan_;
  StashTagSave tagSave_;
  StashWorker<Shared> shared_;  // last: stopped and waited for first
};
