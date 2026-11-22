#pragma once

#include "stash_catalog.h"
#include "stash_http.h"
#include "stash_settings.h"
#include "stash_worker.h"

#include <cstdint>
#include <memory>
#include <string>

// How the last load or sync went, for the Settings page.
struct StashStatus {
  bool syncing = false;      // a sync is under way
  // ... and what it is doing: fetching (received: bytes so far, updated a
  // few times a second), keeping the copy, or building the catalog (which
  // looks for Stash's files here).
  enum class Step { Fetching, Saving, Building } step = Step::Fetching;
  size_t received = 0;
  bool synced = false;       // the last sync (this session) worked
  int scenes = 0;            // in Stash
  int listed = 0;            // ... with a file here
  int streamed = 0;          // ... listed without one, from its stream
  int libraryVideos = 0;     // fingerprinted videos of the library
  int libraryMatched = 0;    // ... found in Stash
  int64_t lastSync = 0;      // when the copy in use was fetched (Unix seconds), 0 = none
  std::string version;       // Stash's
  std::string error;         // why the last sync failed; empty if it didn't
  StashProblem problem = StashProblem::None;  // ... the kind the menu acts on
  // ... because something answered, but not as this kind of server would:
  // another kind may be there (Settings looks when the server is saved).
  bool otherAnswered = false;
  // The last sync's steps, in milliseconds.
  double fetchMs = 0, parseMs = 0, storeMs = 0, checkMs = 0, buildMs = 0;
  size_t bytes = 0;          // received
};

// Keeps the Stash catalog up to date in the background: loads the local
// copy (stash.db), so a catalog is there at once, then syncs from the
// server (StashClient → StashStore → StashCatalog) at start and whenever
// asked. A server that can't be reached keeps the catalog there was, and
// says why in the status. Nothing here blocks: poll() each frame takes
// what the worker finished. Main thread only.
class StashSync {
 public:
  explicit StashSync(const StashSettings& settings);
  StashSync(const StashSync&) = delete;
  StashSync& operator=(const StashSync&) = delete;
  // Stops a sync under way (within about a second) and waits for the worker.
  ~StashSync();
  // Tells the worker to stop, without waiting. Once this returns it keeps
  // nothing more in stash.db, so a StashSync made next may keep its own.
  void stop();

  // Loads the local copy, then syncs. `library`: the library's videos and
  // their formats, as the catalog is built with them.
  void start(StashLibraryFormats library);
  // Syncs again ("Sync now"); after the one under way if there is one.
  void syncNow(StashLibraryFormats library);
  // Takes what the worker finished; true if the catalog or the status changed.
  bool poll();

  // Null until the local copy or a sync is there. The catalog a new one
  // replaces is freed on the worker, unless a copy of its pointer outlives
  // it: keep none past the next poll().
  const std::shared_ptr<const StashCatalog>& catalog() const { return catalog_; }
  const StashStatus& status() const { return status_; }

 private:
  struct Shared;
  static void work(std::shared_ptr<Shared> shared);

  std::shared_ptr<const StashCatalog> catalog_;
  StashStatus status_;
  double receivedAt_ = 0;  // when status_.received was last taken (steady seconds)
  StashWorker<Shared> shared_;  // last: stopped and waited for first
};

// One sync without the worker, as --stash-sync runs it: blocks. Fills in
// `status`; the catalog, or null if the sync failed.
std::shared_ptr<const StashCatalog> stashSyncOnce(const StashSettings& settings,
                                                  const StashLibraryFormats& library,
                                                  StashStatus& status);
