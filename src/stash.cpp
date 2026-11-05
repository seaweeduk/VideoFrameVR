#include "stash.h"

#include "library.h"
#include "offline.h"
#include "run_log.h"
#include "settings.h"  // cacheDir
#include "stash_store.h"
#include "stash_stream.h"
#include "strings.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;

namespace {

// Stashes destroyed whose workers are still stopping.
std::mutex stoppingMutex;
std::condition_variable stopped;
int stopping = 0;

// `list` with `map` for Stash's `folder` (stashFoldersWith()); scenes in
// none of Stash's folders, beside some in them, get one more.
std::vector<StashFolderMap> with(std::vector<StashFolderMap> list, const StashCatalog& catalog,
                                 const std::string& folder, StashFolderMap map) {
  if (!folder.empty() || catalog.folders().size() <= 1)
    return stashFoldersWith(std::move(list), folder, std::move(map));
  if (std::find(list.begin(), list.end(), map) == list.end()) list.push_back(std::move(map));
  return list;
}

std::vector<StashHistory> historyOf(const StashCatalog& catalog, const Library& library) {
  std::vector<StashHistory> history(catalog.scenes().size());
  for (size_t i = 0; i < history.size(); ++i) {
    const StashScene& s = catalog.scenes()[i];
    // By content, so a moved file not listed in Files yet keeps its
    // history; by path for a new file's changes waiting for its fingerprint.
    VideoRecord r;
    std::optional<VideoFormat> format;
    if (s.oshash.empty() || !library.videoByHash(s.oshash, r, format)) {
      r = library.record(s.path);
      VideoSettings settings;
      if (r.known() && library.settings(s.path, settings) && settings.hasFormat)
        format = settings.format;
    }
    StashHistory& h = history[i];
    h.rating = r.rating;
    h.plays = r.playCount;
    h.lastPlayedAt = r.lastPlayedAt;
    if (format) h.vr = format->projection != Projection::Flat ? 1 : 0;
  }
  return history;
}

}  // namespace

Stash::Stash(const StashSettings& settings, Library& library)
    : settings_(settings),
      library_(library),
      sync_(std::make_unique<StashSync>(settings)),
      images_(std::make_unique<StashImages>(cacheDir() / "stash", settings.url, settings.apiKey)),
      preview_(std::make_unique<StashPreview>(settings.url, settings.apiKey)),
      finder_(std::make_unique<StashFolderFinder>()) {
  hideInRunLog(urlHost(settings.url), "<Stash server>");
  stashServe(settings.url, settings.apiKey);
  library_.setFingerprintSource([this](const std::string& path, uint64_t size, int64_t mtime) {
    const auto& catalog = sync_->catalog();
    return catalog ? catalog->oshashFor(path, size, mtime) : std::string();
  });
  sync_->start(library_.formatsByHash());
  images_->start();
  if (settings_.writeBack) writeBack_ = std::make_unique<StashWriteBack>(settings_);
  library_.setRatingPlayObserver([this](const std::string& oshash, bool rated, float rating,
                                        int plays, int64_t playedAt) {
    ratedOrPlayed(oshash, rated, rating, plays, playedAt);
  });
}

Stash::~Stash() {
  stashServe({}, {});
  library_.setFingerprintSource(nullptr);
  library_.setRatingPlayObserver(nullptr);
  sync_->stop();
  images_->stop();
  preview_->stop();
  finder_->find({});  // stops a search under way
  if (writeBack_) writeBack_->stop();  // keeps what it was handed, sends nothing more
  {
    std::lock_guard<std::mutex> lock(stoppingMutex);
    ++stopping;
  }
  // The catalog and the image cache's surfaces go with them: no one else
  // holds them any more.
  try {
    std::thread([sync = std::move(sync_), images = std::move(images_),
                 preview = std::move(preview_), writeBack = std::move(writeBack_),
                 finder = std::move(finder_)]() mutable {
      sync.reset();
      images.reset();
      preview.reset();
      writeBack.reset();
      finder.reset();
      std::lock_guard<std::mutex> lock(stoppingMutex);
      if (--stopping == 0) stopped.notify_all();
    }).detach();
  } catch (const std::system_error&) {  // no thread: they were waited for here
    std::lock_guard<std::mutex> lock(stoppingMutex);
    --stopping;
  }
}

void Stash::finishStopping() {
  std::unique_lock<std::mutex> lock(stoppingMutex);
  stopped.wait(lock, [] { return stopping == 0; });
}

bool Stash::poll() {
  imagesArrived_ = images_->update();
  bool changed = sync_->poll();
  if (finder_->poll()) {
    ++folderSearches_;
    folderSearching_ = false;
    changed = true;
  }
  if (writeBack_) {
    changed = writeBack_->poll() || changed;
    // A sync that worked: Stash can be reached, and the copy knows its
    // scenes; what waits is sent.
    const StashStatus& status = sync_->status();
    if (settings_.writeBack && status.synced && status.lastSync != deliveredAfter_) {
      deliveredAfter_ = status.lastSync;
      writeBack_->deliver();
    }
    const StashTagSave& tags = writeBack_->tagSave();
    if (tags.state == StashTagSave::State::Saved && tags.at != tagsSavedAt_) {
      tagsSavedAt_ = tags.at;
      syncNow();
    }
  }
  if (sync_->catalog().get() != polledCatalog_) {
    polledCatalog_ = sync_->catalog().get();
    // The pages rate scenes by their streams: those without a file here,
    // and those whose server (SMB, SFTP) doesn't answer. Their thumbnails are kept
    // by the files they serve.
    if (polledCatalog_) {
      std::unordered_map<int64_t, FileFingerprint> files;
      files.reserve(polledCatalog_->scenes().size());
      for (const StashScene& s : polledCatalog_->scenes()) {
        const std::string stream = stashStreamOf(s, s.oshash);
        library_.locate(stream, s.oshash);
        if (!stream.empty()) files[s.id] = {s.oshash, s.size, s.modified};
      }
      stashKnowFiles(std::move(files));
    }
    ++generation_;
    prefetch();
  }
  return changed;
}

void Stash::setCovers(bool on) {
  if (on == settings_.covers) return;
  settings_.covers = on;
  ++generation_;
  prefetch();
}

StashImages::Request Stash::coverRequest(const StashScene& scene, int width) {
  StashImages::Request r;
  r.kind = StashImages::Kind::Cover;
  r.id = std::to_string(scene.id);
  r.url = scene.cover;
  r.width = width;
  r.videoWidth = scene.width;
  r.videoHeight = scene.height;
  // Side by side 3D, by the frame and the name as the player would guess.
  r.stereo = scene.vr && guessVideoFormat(scene.fileName(), scene.width, scene.height).stereo ==
                             StereoLayout::SideBySide;
  return r;
}

StashImages::Request Stash::portraitRequest(const StashPerformer& performer, int width) {
  StashImages::Request r;
  r.kind = StashImages::Kind::Performer;
  r.id = std::to_string(performer.id);
  r.url = performer.image;
  r.width = width;
  return r;
}

StashImages::Request Stash::logoRequest(const StashStudio& studio, int width) {
  StashImages::Request r;
  r.kind = StashImages::Kind::Studio;
  r.id = std::to_string(studio.id);
  r.url = studio.image;
  r.width = width;
  return r;
}

void Stash::prefetchCovers(const std::vector<int>& widths) {
  if (widths == prefetchWidths_) return;
  prefetchWidths_ = widths;
  prefetch();
}

void Stash::prefetch() {
  const auto& catalog = sync_->catalog();
  std::vector<StashImages::Request> requests;
  if (catalog && settings_.covers) {
    std::vector<const StashScene*> scenes;
    for (const StashScene& s : catalog->scenes())
      if (!s.cover.empty()) scenes.push_back(&s);
    std::sort(scenes.begin(), scenes.end(),
              [](const StashScene* a, const StashScene* b) { return a->createdAt > b->createdAt; });
    requests.reserve(scenes.size() * prefetchWidths_.size());
    for (const StashScene* s : scenes) {
      StashImages::Request r = coverRequest(*s, 0);
      for (const int width : prefetchWidths_) {
        r.width = width;
        requests.push_back(r);
      }
    }
  }
  images_->prefetch(requests);
}

void Stash::setTitles(bool on) {
  if (on == settings_.titles) return;
  settings_.titles = on;
  ++generation_;
}

void Stash::setMalePerformers(bool on) {
  if (on == settings_.malePerformers) return;
  settings_.malePerformers = on;
  ++generation_;
}

void Stash::setWriteBack(bool on) {
  settings_.writeBack = on;
  if (!on) {
    if (writeBack_) writeBack_->setEnabled(false);  // stops sending at once
    return;
  }
  if (!writeBack_) writeBack_ = std::make_unique<StashWriteBack>(settings_);
  writeBack_->setEnabled(true);
}

// From the library's hook: only while write-back is on, and only handed
// over (the worker keeps it in its file and sends it).
void Stash::ratedOrPlayed(const std::string& oshash, bool rated, float rating, int plays,
                          int64_t playedAt) {
  if (!settings_.writeBack || !writeBack_) return;
  if (rated) writeBack_->rated(oshash, rating);
  if (plays > 0) writeBack_->played(oshash, playedAt, plays);
}

std::vector<StashLibraryEntry> Stash::libraryEntries() const {
  std::vector<StashLibraryEntry> list;
  for (const auto& [oshash, r] : library_.ratedOrPlayed())
    list.push_back({oshash, r.rating, r.playCount, r.lastPlayedAt});
  return list;
}

void Stash::planSend() {
  if (settings_.writeBack && writeBack_) writeBack_->plan(libraryEntries());
}

void Stash::sendPlanned() {
  if (settings_.writeBack && writeBack_ &&
      writeBack_->sendPlan().state == StashSendPlan::State::Ready)
    writeBack_->send(libraryEntries());
}

void Stash::closeSend() {
  if (writeBack_) writeBack_->closePlan();
}

void Stash::saveTags(int64_t scene, std::vector<int64_t> add, std::vector<int64_t> remove) {
  if (!settings_.writeBack || !writeBack_) return;
  tagsSavedAt_ = -1;  // synced after once saved
  writeBack_->saveTags(scene, std::move(add), std::move(remove));
}

void Stash::hideScene(int64_t id) {
  if (hidden_.insert(id).second) ++generation_;
}

void Stash::syncNow() { sync_->syncNow(library_.formatsByHash()); }

void Stash::lookForFolders(const std::vector<std::string>& alsoIn) {
  const auto& catalog = sync_->catalog();
  if (!catalog || videoOpen_ || catalog->syncedAt() == foldersLookedFor_) return;
  foldersLookedFor_ = catalog->syncedAt();
  std::vector<StashFolderFinder::Job> jobs;
  // The folders whose scenes aren't here first (the others are looked for
  // too, for other places they are in).
  for (const bool here : {false, true})
    for (const StashCatalogFolder& f : catalog->folders())
      if (!f.samples.empty() && (f.listed * 2 >= f.scenes) == here)
        jobs.push_back({f.folder, f.samples});
  finder_->find(std::move(jobs), {}, alsoIn);
  folderCheck_ = false;
  folderSearching_ = true;
}

void Stash::lookForFolder(const std::string& folder, const std::string& root) {
  const auto& catalog = sync_->catalog();
  if (!catalog) return;
  for (const StashCatalogFolder& f : catalog->folders())
    if (f.folder == folder) finder_->find({{f.folder, f.samples}}, {root});
  folderCheck_ = true;
  folderSearching_ = false;
  foldersLookedFor_ = -1;  // the others are looked for again after
}

std::vector<StashFolderMap> Stash::foundFolders() const {
  std::vector<StashFolderMap> list = settings_.folders;
  const auto& catalog = sync_->catalog();
  if (!catalog) return list;
  for (const StashCatalogFolder& f : catalog->folders()) {
    const auto& found = finder_->foundFor(f.folder);
    if (f.listed > 0 || found.empty() ||
        found.front().found < static_cast<int>(f.samples.size() + 1) / 2)
      continue;
    list = with(std::move(list), *catalog, f.folder, {found.front().pathFrom, found.front().pathTo});
  }
  return list;
}

std::vector<StashFolderMap> Stash::foldersWith(const std::string& folder, StashFolderMap map) const {
  const auto& catalog = sync_->catalog();
  if (!catalog) return {map};
  return with(settings_.folders, *catalog, folder, std::move(map));
}

void Stash::setVideoOpen(bool open) {
  videoOpen_ = open;
  if (!open || !folderSearching_) return;
  finder_->find({});
  folderSearching_ = false;
  foldersLookedFor_ = -1;
}

std::string stashSceneLocation(const StashScene& scene, const Offline* offline) {
  if (scene.path.empty()) return stashStream(scene);
  if (offline && offline->serverDown(scene.path))
    if (std::string stream = stashStreamOf(scene, scene.oshash); !stream.empty()) return stream;
  return scene.path;
}

std::string stashStream(const StashScene& scene) {
  return stashSceneLocator(scene.id,
                           scene.stashPath.substr(scene.stashPath.find_last_of("/\\") + 1));
}

std::string stashStreamOf(const StashScene& scene, const std::string& oshash) {
  return !scene.oshashes.empty() && scene.oshashes.front() == oshash ? stashStream(scene) : std::string();
}

int stashSceneAt(const StashCatalog& catalog, const std::string& location) {
  return isStashScene(location) ? catalog.sceneById(stashSceneId(location))
                                : catalog.sceneByPath(location);
}

std::string stashFilePathAt(const StashCatalog& catalog, const std::string& location) {
  if (!isStashScene(location)) return catalog.stashPathOf(location);
  const int i = catalog.sceneById(stashSceneId(location));
  return i >= 0 ? catalog.scenes()[i].stashPath : std::string();
}

std::vector<StashHistory> Stash::history() const {
  const auto& catalog = sync_->catalog();
  return catalog ? historyOf(*catalog, library_) : std::vector<StashHistory>();
}

namespace {

double msSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

uintmax_t folderSize(const fs::path& dir) {
  std::error_code ec;
  uintmax_t total = 0;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    if (it->is_regular_file(ec)) total += it->file_size(ec);
  return total;
}

// The time one query takes, in microseconds: the median of many.
double timeQuery(const StashCatalog& catalog, const StashFilter& filter, const StashOrder& order,
                 const std::vector<StashHistory>& history, size_t& matches) {
  std::vector<double> times;
  for (int i = 0; i < 101; ++i) {
    const auto start = std::chrono::steady_clock::now();
    const StashResult result = catalog.query(filter, order, &history);
    times.push_back(msSince(start) * 1000);
    matches = result.scenes.size();
  }
  std::nth_element(times.begin(), times.begin() + 50, times.end());
  return times[50];
}

}  // namespace

int runStashSyncDiagnostic(const StashSettings& settings) {
  if (settings.url.empty()) {
    std::printf("Stash: %s\n", strings().stash.setupHint);
    return 0;
  }
  hideInRunLog(urlHost(settings.url), "<Stash server>");
  std::printf("Stash: syncing from %s\n", settings.url.c_str());
  // The library's formats and history, if it is free (not open in a
  // running VideoFrameVR).
  Library library;
  library.load();
  const StashLibraryFormats formats = library.formatsByHash();

  StashStatus status;
  const auto start = std::chrono::steady_clock::now();
  const auto catalog = stashSyncOnce(settings, formats, status);
  const double totalMs = msSince(start);
  if (!catalog) {
    std::printf("Stash: sync failed: %s\n", status.error.c_str());
    return 1;
  }
  if (!status.error.empty()) std::printf("Stash: %s\n", status.error.c_str());
  const StashCatalogStats& stats = catalog->stats();

  std::printf("Server: Stash %s\n", catalog->version().c_str());
  std::printf("Scenes: %d in Stash, %d with a file here, %d from its stream, %d not listed\n",
              stats.stashScenes, stats.listed, stats.streamed,
              stats.stashScenes - stats.listed - stats.streamed);
  for (const StashCatalogFolder& f : catalog->folders()) {
    const std::string here = f.folder.empty() ? "" : settings.localPath(f.folder);
    std::printf("  %s: %d scenes, %d here, %d from its stream%s%s\n",
                f.folder.empty() ? "in none of Stash's folders" : f.folder.c_str(), f.scenes,
                f.listed, f.streamed, here.empty() ? "" : ", at ", here.c_str());
  }
  std::printf("  VR %d (own format %d, frame %d, name %d, Stash tag %d), 2D %d\n", stats.vr,
              stats.vrBy[1], stats.vrBy[2], stats.vrBy[3], stats.vrBy[4], stats.listed + stats.streamed - stats.vr);
  int covers = 0;
  for (const StashScene& s : catalog->scenes()) covers += !s.cover.empty();
  std::printf("  with a cover %d\n", covers);
  int shown = 0, images = 0;
  for (const StashPerformer& p : catalog->performers()) {
    shown += stashShown(p.gender, false);
    images += !p.image.empty();
  }
  std::printf("Performers: %zu (%d shown without the male performers setting, %d with an image)\n",
              catalog->performers().size(), shown, images);
  int logos = 0, withScenes = 0;
  for (const StashStudio& s : catalog->studios()) {
    logos += !s.image.empty();
    withScenes += s.scenes > 0;
  }
  std::printf("Studios: %zu (%d with scenes here, %d with a logo)\n", catalog->studios().size(),
              withScenes, logos);
  int mergedGroups = 0;
  for (const StashTag& t : catalog->tags()) mergedGroups += t.ids.size() > 1;
  std::printf("Tags: %d in Stash, %zu after merging duplicates (%d merged from 2 or more)\n",
              stats.stashTags, catalog->tags().size(), mergedGroups);
  std::printf("Groups: %zu\n", catalog->groups().size());
  if (library.available())
    std::printf("Library: %d fingerprinted videos, %d of them in Stash\n", stats.libraryVideos,
                stats.libraryMatched);
  else
    std::printf("Library: not available (in use?): no own formats, no history\n");

  std::printf("Timings: fetch %.0f ms (%.1f MB), parse %.0f ms, store %.0f ms, files %.0f ms, "
              "catalog %.0f ms; %.0f ms in all\n",
              status.fetchMs, status.bytes / 1e6, status.parseMs, status.storeMs, stats.checkMs,
              stats.buildMs, totalMs);
  {
    const auto loadStart = std::chrono::steady_clock::now();
    StashStore store;
    StashData data;
    if (store.load(data)) {
      const double loadMs = msSince(loadStart);
      const StashCatalogStats copy = StashCatalog::build(data, settings, formats)->stats();
      std::printf("  from the local copy (at start): load %.0f ms, files %.0f ms, catalog %.0f ms\n",
                  loadMs, copy.checkMs, copy.buildMs);
    }
  }

  // Queries over every scene, with the facets.
  const std::vector<StashHistory> history = historyOf(*catalog, library);
  int topTag = -1, topPerformer = -1, topHair = -1;
  {
    StashFilter vr;
    vr.format = StashFormat::VR;
    const StashResult r = catalog->query(vr, {}, &history);
    if (!r.tags.empty()) topTag = r.tags.front().index;
    if (!r.performers.empty()) topPerformer = r.performers.front().index;
    const auto& hair = r.looks[static_cast<size_t>(StashLookKind::Hair)];
    if (!hair.empty()) topHair = hair.front().index;
  }
  struct Case {
    const char* name;
    StashFilter filter;
    StashOrder order;
  };
  std::vector<Case> cases;
  cases.push_back({"all scenes, by release date", {}, {}});
  {
    Case c{"VR, by title", {}, {StashSort::Title, false, 0}};
    c.filter.format = StashFormat::VR;
    cases.push_back(c);
  }
  {
    Case c{"VR, top performer and tag, by rating", {}, {StashSort::Rating, true, 0}};
    c.filter.format = StashFormat::VR;
    if (topPerformer >= 0) c.filter.performers.include = {topPerformer};
    if (topTag >= 0) c.filter.tags.include = {topTag};
    cases.push_back(c);
  }
  {
    Case c{"looks, cast, resolution, history, random", {}, {StashSort::Random, true, 42}};
    if (topHair >= 0) c.filter.looks[static_cast<size_t>(StashLookKind::Hair)].include = {topHair};
    c.filter.looks[static_cast<size_t>(StashLookKind::Cup)].include = {
        static_cast<int>(StashCup::C), static_cast<int>(StashCup::D)};
    c.filter.women = stashBit(StashWomen::One);
    c.filter.men = stashBit(StashMen::None) | stashBit(StashMen::One);
    c.filter.resolutions = stashBit(StashResolution::VR8K) | stashBit(StashResolution::VR7K) |
                           stashBit(StashResolution::VR6K) | stashBit(StashResolution::VR5K);
    c.filter.history = stashBit(StashHistoryOption::Unplayed);
    c.filter.addedSince = {static_cast<int64_t>(std::time(nullptr)) - 30 * 86400};
    cases.push_back(c);
  }
  {
    Case c{"text search \"an\", tags any of, by plays", {}, {StashSort::Plays, true, 0}};
    c.filter.text = "an";
    c.filter.tags.all = false;
    for (int i = 0; i < 3 && i < static_cast<int>(catalog->tags().size()); ++i)
      c.filter.tags.include.push_back(i);
    cases.push_back(c);
  }
  std::printf("Queries over %zu scenes (filter, facets and order; median of 101):\n",
              catalog->scenes().size());
  for (const Case& c : cases) {
    size_t matches = 0;
    const double us = timeQuery(*catalog, c.filter, c.order, history, matches);
    std::printf("  %-44s %5zu scenes  %6.0f us\n", c.name, matches, us);
  }
  {
    const auto historyStart = std::chrono::steady_clock::now();
    historyOf(*catalog, library);
    std::printf("  the library's history of every scene: %.2f ms\n", msSince(historyStart));
  }

  std::error_code ec;
  std::printf("Files: stash.db %.1f MB", fs::file_size(StashStore::path(), ec) / 1e6);
  const fs::path imageCache = cacheDir() / "stash";
  if (fs::is_directory(imageCache, ec))
    std::printf(", image cache %.0f MB\n", folderSize(imageCache) / 1e6);
  else
    std::printf(", no image cache\n");
  return 0;
}
