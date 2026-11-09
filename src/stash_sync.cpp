#include "stash_sync.h"

#include "settings.h"
#include "stash_client.h"
#include "stash_store.h"

#include <chrono>
#include <cstdio>
#include <functional>

namespace {

// stash.db is used by one StashStore at a time, of every StashSync: a
// stopped one's worker may still be at it when the next one's starts. A
// save checks under it that its sync was not stopped, so a stopped sync's
// copy never replaces a later one's, and a load never waits on a write
// (SQLite's own busy wait gives up after a second).
std::mutex storeMutex;

double msSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void describe(const StashCatalog& catalog, StashStatus& status) {
  const StashCatalogStats& stats = catalog.stats();
  status.scenes = stats.stashScenes;
  status.listed = stats.listed;
  status.streamed = stats.streamed;
  status.libraryVideos = stats.libraryVideos;
  status.libraryMatched = stats.libraryMatched;
  status.lastSync = catalog.syncedAt();
  status.version = catalog.version();
  status.checkMs = stats.checkMs;
  status.buildMs = stats.buildMs;
}

// The local copy's catalog, or null if there is none.
std::shared_ptr<const StashCatalog> loadCopy(const StashSettings& settings,
                                             const StashLibraryFormats& library, StashStatus& status) {
  StashData data;
  {
    std::lock_guard<std::mutex> lock(storeMutex);
    StashStore store;
    if (!store.load(data)) return nullptr;
  }
  // Another server's: none until this one's sync.
  if (data.server != StashClient(settings.url, settings.apiKey).server()) return nullptr;
  auto catalog = StashCatalog::build(data, settings, library);
  describe(*catalog, status);
  return catalog;
}

// Fetches, keeps and builds, telling `step` as each starts; null (and
// status.error) if the fetch failed.
std::shared_ptr<const StashCatalog> sync(StashClient& client, const StashSettings& settings,
                                         const StashLibraryFormats& library, StashStatus& status,
                                         const std::function<void(const StashStatus&)>& step) {
  StashData data;
  StashClient::Timings timings;
  status.step = StashStatus::Step::Fetching;
  status.received = 0;
  step(status);
  const bool fetched = client.fetch(data, timings);
  status.fetchMs = timings.fetchMs;
  status.parseMs = timings.parseMs;
  status.bytes = timings.bytes;
  status.problem = client.problem();
  if (!fetched) {
    status.error = client.error();
    status.synced = false;
    return nullptr;
  }
  status.step = StashStatus::Step::Saving;
  status.received = client.received();
  step(status);
  const auto storeStart = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(storeMutex);
    // Stopped (Stash off, or another server): the copy may already be the
    // next sync's.
    if (client.cancelled()) {
      status.error = "Stash: stopped";
      status.synced = false;
      return nullptr;
    }
    // Not kept, it is still used: the next start syncs again anyway. Nor
    // while --first-run's setup holds the profile as it was.
    StashStore store;
    status.error = settingsHeld() || store.save(data) ? std::string() : store.error();
  }
  status.storeMs = msSince(storeStart);
  status.step = StashStatus::Step::Building;
  step(status);
  status.synced = true;
  auto catalog = StashCatalog::build(data, settings, library);
  describe(*catalog, status);
  return catalog;
}

}  // namespace

struct StashSync::Shared {
  StashSettings settings;
  std::mutex mutex;
  std::condition_variable wake;
  bool load = false, sync = false, stop = false;
  StashLibraryFormats library;
  StashClient* client = nullptr;  // the request under way, to stop it
  // What the worker finished, for poll().
  bool changed = false;
  std::shared_ptr<const StashCatalog> catalog;
  StashStatus status;
  // The catalog a new one replaced, freed here: that takes a few
  // milliseconds, too long for a frame.
  std::shared_ptr<const StashCatalog> retired;
};

StashSync::StashSync(const StashSettings& settings) : shared_(std::make_shared<Shared>()) {
  shared_->settings = settings;
}

StashSync::~StashSync() {
  stop();
  if (worker_.joinable()) worker_.join();
}

void StashSync::stop() {
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->stop = true;
    if (shared_->client) shared_->client->cancel();
  }
  shared_->wake.notify_one();
}

void StashSync::start(StashLibraryFormats library) {
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->library = std::move(library);
    shared_->load = shared_->sync = true;
  }
  if (!worker_.joinable()) worker_ = std::thread(work, shared_);
  shared_->wake.notify_one();
}

void StashSync::syncNow(StashLibraryFormats library) {
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->library = std::move(library);
    shared_->sync = true;
  }
  if (!worker_.joinable()) worker_ = std::thread(work, shared_);
  shared_->wake.notify_one();
}

bool StashSync::poll() {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  if (!shared_->changed) {
    // While fetching: what has come so far, four times a second (each
    // change redraws what shows it).
    if (!shared_->client || status_.step != StashStatus::Step::Fetching) return false;
    const double now =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const size_t received = shared_->client->received();
    if (now - receivedAt_ < 0.25 || received == status_.received) return false;
    receivedAt_ = now;
    status_.received = received;
    return true;
  }
  shared_->changed = false;
  if (shared_->catalog) {
    shared_->retired = std::move(catalog_);
    catalog_ = std::move(shared_->catalog);
    shared_->catalog = nullptr;
    shared_->wake.notify_one();
  }
  status_ = shared_->status;
  return true;
}

void StashSync::work(std::shared_ptr<Shared> shared) {
  std::unique_lock<std::mutex> lock(shared->mutex);
  for (;;) {
    shared->wake.wait(lock, [&] {
      return shared->stop || shared->load || shared->sync || shared->retired;
    });
    if (shared->stop) return;
    if (shared->retired) {
      auto retired = std::move(shared->retired);
      lock.unlock();
      retired.reset();
      lock.lock();
      continue;
    }
    const bool load = shared->load;
    shared->load = false;
    if (!load) shared->sync = false;
    const StashSettings settings = shared->settings;
    const StashLibraryFormats library = shared->library;
    StashStatus status = shared->status;
    if (load) {
      lock.unlock();
      auto catalog = loadCopy(settings, library, status);
      if (catalog)
        std::printf("Stash: local copy, %d scenes (%d here, %d streamed)\n", status.scenes,
                    status.listed, status.streamed);
      lock.lock();
      if (catalog) shared->catalog = std::move(catalog);
      shared->status = status;
      shared->changed = true;
      continue;  // then the sync
    }
    status.syncing = true;
    shared->status = status;
    shared->changed = true;
    StashClient client(settings.url, settings.apiKey, settings.unverified());
    shared->client = &client;
    lock.unlock();
    const auto start = std::chrono::steady_clock::now();
    auto catalog = sync(client, settings, library, status, [&shared](const StashStatus& now) {
      std::lock_guard<std::mutex> stepLock(shared->mutex);
      shared->status = now;
      shared->changed = true;
    });
    status.syncing = false;
    if (catalog)
      std::printf("Stash: synced, %d scenes (%d here, %d streamed) in %.0f ms\n", status.scenes,
                  status.listed, status.streamed, msSince(start));
    else
      std::fprintf(stderr, "Stash: sync failed: %s\n", status.error.c_str());
    lock.lock();
    shared->client = nullptr;
    if (catalog) shared->catalog = std::move(catalog);
    shared->status = status;
    shared->changed = true;
  }
}

std::shared_ptr<const StashCatalog> stashSyncOnce(const StashSettings& settings,
                                                  const StashLibraryFormats& library,
                                                  StashStatus& status) {
  StashClient client(settings.url, settings.apiKey, settings.unverified());
  return sync(client, settings, library, status, [](const StashStatus&) {});
}
