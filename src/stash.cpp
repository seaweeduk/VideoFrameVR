#include "stash.h"

#include "library.h"
#include "offline.h"
#include "run_log.h"
#include "app_dirs.h"
#include "stash_stream.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <system_error>
#include <thread>

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

// Both give the same place here for each of the files: /a → /b and
// /a/c → /b/c are one.
bool samePlace(const StashFolderFinder::Mapping& a, const StashFolderFinder::Mapping& b,
               const std::vector<StashFolderFinder::File>& files) {
  StashSettings one, other;
  one.folders = {{a.pathFrom, a.pathTo}};
  other.folders = {{b.pathFrom, b.pathTo}};
  for (const StashFolderFinder::File& f : files)
    if (one.localPath(f.path) != other.localPath(f.path)) return false;
  return true;
}

}  // namespace

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

Stash::Stash(const StashSettings& settings, Library& library)
    : settings_(settings),
      library_(library),
      sync_(std::make_unique<StashSync>(settings)),
      // A cache per kind, as the copy: each kind's ids are its own.
      images_(std::make_unique<StashImages>(cacheDir() / mediaServer(settings.kind).id(),
                                            settings.kind, settings.url, settings.apiKey,
                                            settings.unverified())),
      preview_(std::make_unique<StashPreview>(settings.kind, settings.url, settings.apiKey,
                                              settings.unverified())),
      finder_(std::make_unique<StashFolderFinder>()) {
  const char* name = mediaServer(settings.kind).name();
  hideInRunLog(urlHost(settings.url), kHiddenServer);
  if (settings.unverified()) std::printf("%s: its certificate is not verified, as chosen\n", name);
  stashServe(settings.kind, settings.url, settings.apiKey, settings.unverified());
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
  stashServe(settings_.kind, {}, {}, false);
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
    foldersLookedThrough_ = foldersLookedThrough_ || folderSearching_;
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
    const int half = static_cast<int>(f.samples.size() + 1) / 2;
    if (f.listed > 0 || found.empty() || found.front().found < half) continue;
    // In several places: which one is for the user to say.
    bool only = true;
    for (size_t i = 1; i < found.size(); ++i)
      only = only && (found[i].found < half || samePlace(found.front(), found[i], f.samples));
    if (!only) continue;
    list = with(std::move(list), *catalog, f.folder, {found.front().pathFrom, found.front().pathTo});
  }
  return list;
}

std::vector<StashFolderMap> Stash::foldersWith(const std::string& folder, StashFolderMap map) const {
  const auto& catalog = sync_->catalog();
  if (!catalog) return {map};
  return with(settings_.folders, *catalog, folder, std::move(map));
}

const StashFolderMap* Stash::folderMapping(const std::string& folder) const {
  return mappingIn(settings_, folder);
}

const StashFolderMap* Stash::mappingIn(const StashSettings& settings,
                                       const std::string& folder) const {
  if (!folder.empty()) return settings.mappingFor(folder);
  const auto& catalog = sync_->catalog();
  if (!catalog) return nullptr;
  for (const StashCatalogFolder& f : catalog->folders())
    if (f.folder.empty() && !f.samples.empty()) return settings.mappingFor(f.samples.front().path);
  return nullptr;
}

std::vector<StashFolderMap> Stash::foldersWithout(const std::string& folder) const {
  StashSettings left = settings_;
  const auto& catalog = sync_->catalog();
  // Each mapping it goes by in turn: its own, then one for a folder above.
  for (size_t tries = left.folders.size(); tries > 0; --tries) {
    const StashFolderMap* map = mappingIn(left, folder);
    if (!map) break;
    const StashFolderMap gone = *map;
    std::vector<StashFolderMap> kept;
    if (catalog)
      for (const StashCatalogFolder& f : catalog->folders()) {
        if (f.folder.empty() || f.folder == folder) continue;
        const StashFolderMap* by = left.mappingFor(f.folder);
        if (by && *by == gone) kept.push_back({f.folder, stashMapped(gone, f.folder)});
      }
    left.folders.erase(std::remove(left.folders.begin(), left.folders.end(), gone),
                       left.folders.end());
    for (StashFolderMap& k : kept)
      if (std::find(left.folders.begin(), left.folders.end(), k) == left.folders.end())
        left.folders.push_back(std::move(k));
  }
  return left.folders;
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
                           scene.stashPath.substr(scene.stashPath.find_last_of("/\\") + 1),
                           scene.streamFile);
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
