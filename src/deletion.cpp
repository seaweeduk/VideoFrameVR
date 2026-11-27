#include "deletion.h"

#include "file_util.h"
#include "library.h"
#include "library_index.h"
#include "media_server.h"
#include "menu.h"
#include "offline.h"
#include "oshash.h"
#include "remote.h"
#include "settings.h"
#include "stash.h"
#include "stash_stream.h"
#include "strings.h"
#include "thumbnailer.h"
#include "video_player.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <utility>

namespace {

// Runs `job` on a thread of its own; dropping the future doesn't wait for it.
template <typename Job>
auto detached(Job job) {
  std::packaged_task<decltype(job())()> task(std::move(job));
  auto result = task.get_future();
  std::thread(std::move(task)).detach();
  return result;
}

// Why the playing file couldn't be deleted, as the delete dialog says it,
// and as the run log does.
std::string deleteError(DeleteStatus status, const std::string& error) {
  const DeleteStrings& t = strings().deletion;
  switch (status) {
    case DeleteStatus::Changed: return t.changed;
    case DeleteStatus::Gone: return t.gone;
    case DeleteStatus::NotAFile: return t.notAFile;
    case DeleteStatus::Denied: return t.denied;
    case DeleteStatus::InUse: return t.inUse;
    case DeleteStatus::Offline: return t.offline;
    default: return error;
  }
}
const char* deleteLogged(DeleteStatus status) {
  static const char* const names[] = {"deleted", "changed", "gone", "not a file", "not allowed",
                                      "in use", "server not answering", "failed"};
  return names[static_cast<int>(status)];
}

}  // namespace

bool Deletion::request(const MenuCommand& c, const std::string& currentFile,
                       const std::string& streamOshash) {
  // The dialog is open; the video pauses meanwhile, a slideshow stops.
  if (c.path != currentFile) {  // it changed meanwhile
    menu_.cancelDelete();
    return false;
  }
  if (result_.valid()) return false;
  info_ = {};
  info_.file = c.path;
  info_.name = std::filesystem::path(c.path).filename().string();
  info_.photo = isPhotoFile(c.path);
  expected_.clear();
  // A downloaded copy plays instead of its source (openVideo).
  if (const Offline::Download* d = offline_.find(c.path);
      d && d->state == Offline::State::Done && d->present && d->path == c.path) {
    info_.source = d->source;
    info_.copyRoot = d->root;
    info_.size = d->size;
    copy_ = d->id;
    expected_ = d->oshash;
  }
  const std::string& source = info_.source;
  target_ = source.empty()          ? c.path
            : isStashSource(source) ? stashSourceLocator(source)
                                    : source;
  const bool stream = isStashScene(target_);
  if (stream && source.empty()) expected_ = streamOshash;
  // A Stash scene's file, at the scene's location (a file here or its
  // stream), never by oshash alone.
  scene_ = 0;
  sceneFiles_ = 0;
  stashPath_.clear();
  oshash_.clear();
  throughStash_ = false;
  if (const auto catalog = stash_ ? stash_->catalog() : nullptr; catalog && !target_.empty())
    if (const int i = stashSceneAt(*catalog, target_); i >= 0) {
      const StashScene& s = catalog->scenes()[i];
      scene_ = s.id;
      sceneFiles_ = s.oshashes.size();
      stashPath_ = stashFilePathAt(*catalog, target_);
      if (isStashScene(target_) && source.empty()) info_.size = s.size;
    }
  resume_ = !info_.photo && !player_.playback().paused;
  if (resume_) player_.setPaused(true);
  lookup_ = detached([file = target_, stream, check = stream || !source.empty(),
                      expected = expected_,
                      ask = settings_.stash.writeBack && scene_ != 0 &&
                            mediaServer(settings_.stash.kind).abilities().deleteFiles,
                      scene = scene_, stashPath = stashPath_,
                      url = settings_.stash.url, key = settings_.stash.apiKey,
                      unverified = settings_.stash.unverified()] {
    Deletion::Look r;
    if (file.empty())
      r.status = DeleteStatus::Gone;
    else if (!stream)
      r.status = stampOf(file, r.stamp);
    if (r.status != DeleteStatus::Ok || (!ask && !check)) return r;
    r.oshash = stream ? expected : fingerprintFile(file).oshash;
    // A copy's source still the video downloaded; a stream the one
    // opened (Stash's fresh query below matches it).
    if (check && (r.oshash.empty() || r.oshash != expected)) r.status = DeleteStatus::Changed;
    if (r.status != DeleteStatus::Ok || !ask || r.oshash.empty()) return r;
    r.asked = true;
    r.stash = lookUpStashFile(url, key, unverified, scene, r.oshash, stashPath);
    return r;
  });
  return true;
}

void Deletion::cancel(const std::string& currentFile) {
  lookup_ = {};
  if (resume_ && currentFile == info_.file) player_.setPaused(false);
  resume_ = false;
}

void Deletion::confirm(const MenuCommand& c, const std::string& currentFile) {
  // Only the file the dialog showed, as it showed it.
  if (!info_.ready || c.path != info_.file || c.path != currentFile ||
      result_.valid())
    return;
  resume_ = false;
  const std::string target = target_;
  // Stash deletes it only with Send to Stash on still; its stream
  // only through Stash; a copy's source only if it is still the
  // video downloaded.
  const bool throughStash = throughStash_ && settings_.stash.writeBack;
  if (c.value == 0 && (target.empty() || (isStashScene(target) && !throughStash) ||
                       (!info_.source.empty() && !info_.sourceFound))) {
    menu_.showDeleteError(deleteError(DeleteStatus::Denied, std::string()));
    return;
  }
  // The player, the seek preview and a download from it let go of it.
  const std::string from = isStashScene(target) ? stashSource(target) : target;
  if (const Offline::Download* d = offline_.find(from);
      d && d->source == from && d->state != Offline::State::Done)
    offline_.cancel(d->id);
  closeVideo();
  if (c.value > 0) {  // only the downloaded copy
    offline_.remove(copy_);
    library_.forgetFile(c.path);
    libraryIndex_.deleted(c.path);
    std::printf("Delete: a downloaded copy: deleted\n");
    menu_.deleteEnded(std::string());
    return;
  }
  throughStash_ = throughStash;
  result_ = std::async(std::launch::async, [target, stamp = stamp_, throughStash,
                                            writeBack = settings_.stash.writeBack,
                                            url = settings_.stash.url,
                                            key = settings_.stash.apiKey,
                                            unverified = settings_.stash.unverified(),
                                            scene = scene_,
                                            stashPath = stashPath_,
                                            oshash = oshash_,
                                            trash = info_.stashTrash] {
    const bool here = !isStashScene(target);
    DeleteOutcome r;
    if (throughStash)
      r = deleteThroughStash(url, key, unverified, writeBack, scene, oshash, stashPath,
                             here ? target : std::string(), stamp, trash);
    else
      r.status = removeFile(target, stamp, r.error);
    if (r.status == DeleteStatus::Ok && here)
      dropThumbnail(target, stamp.size, stamp.modified, stamp.nanos);
    return r;
  });
}

void Deletion::poll() {
  // The delete dialog's file looked up; a deletion finished.
  if (lookup_.valid() &&
      lookup_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    const Deletion::Look look = lookup_.get();
    DeleteInfo& info = info_;
    const bool stream = isStashScene(target_);
    DeleteStatus status = look.status;
    if (status == DeleteStatus::Ok && !stream) {
      stamp_ = look.stamp;
      info.size = look.stamp.size;
    }
    oshash_ = look.oshash;
    // Stash deletes it if a fresh query found the scene's one file with
    // its path and oshash; its stream's file nobody else can.
    throughStash_ = look.asked && look.stash.file != 0;
    if (throughStash_) {
      sceneFiles_ = look.stash.files.size();
      info.stash = DeleteInfo::Stash::Deletes;
      info.otherFiles = static_cast<int>(look.stash.files.size()) - 1;
      info.stashTrash = !look.stash.trash.empty();
    } else if (stream) {
      if (status == DeleteStatus::Ok)
        status = !look.asked             ? DeleteStatus::Denied
                 : !look.stash.answered ? DeleteStatus::Offline
                 : !look.stash.scene    ? DeleteStatus::Gone
                                        : DeleteStatus::Changed;
    } else if (status == DeleteStatus::Ok && scene_ &&
               !(look.stash.answered && !look.stash.scene)) {
      info.stash = look.stash.answered ? DeleteInfo::Stash::Unsure : DeleteInfo::Stash::Lists;
    }
    // A copy can go without its source.
    info.sourceFound = !info.source.empty() && status == DeleteStatus::Ok;
    info.sourceChanged = !info.source.empty() && status == DeleteStatus::Changed;
    info.ready = status == DeleteStatus::Ok || !info.source.empty();
    if (info.ready)
      menu_.askDelete(info);
    else
      menu_.showDeleteError(deleteError(status, std::string()));
  }
  if (result_.valid() &&
      result_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    const DeleteOutcome outcome = result_.get();
    const std::string& target = target_;
    const bool stream = isStashScene(target);
    std::printf("Delete: %s%s%s: %s%s%s\n",
                stream                ? "a scene's file in Stash"
                : remote::isRemote(target) ? "a file on a server"
                                     : "a file on this device",
                info_.source.empty() ? "" : " and its downloaded copy",
                throughStash_ ? " through Stash" : "", deleteLogged(outcome.status),
                outcome.listed ? ", Stash still lists it" : "",
                outcome.stashDeleted && outcome.status != DeleteStatus::Ok ? ", Stash deleted its file"
                                                                           : "");
    if (outcome.status == DeleteStatus::Ok) {
      if (!stream) {
        library_.forgetFile(target);
        libraryIndex_.deleted(target);
      }
      if (!info_.source.empty()) {
        offline_.remove(copy_);
        library_.forgetFile(info_.file);
        libraryIndex_.deleted(info_.file);
      }
      forgetFolderListing();
    }
    if (stash_ && scene_ && (outcome.status == DeleteStatus::Ok || outcome.stashDeleted)) {
      if (sceneFiles_ == 1 && !stashPath_.empty()) stash_->hideScene(scene_);
      if (throughStash_) stash_->syncNow();
    }
    // Once Stash has deleted its file, never "Nothing was deleted".
    menu_.deleteEnded(outcome.status == DeleteStatus::Ok ? std::string()
                      : outcome.stashDeleted             ? std::string(strings().deletion.stashKeptHere)
                                                         : deleteError(outcome.status, outcome.error),
                      outcome.listed ? strings().deletion.stashLists : "");
  }
}

void Deletion::dropDialog() {
  lookup_ = {};
  resume_ = false;
  menu_.cancelDelete();
}
