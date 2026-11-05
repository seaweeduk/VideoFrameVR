#pragma once

#include "stash_catalog.h"
#include "stash_folders.h"
#include "stash_images.h"
#include "stash_preview.h"
#include "stash_settings.h"
#include "stash_sync.h"
#include "stash_write_back.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

class Library;
class Offline;

// The Stash integration (docs/stash.md), all the rest of the app sees of
// it. Made only while Stash is on: without one, nothing of Stash runs. It
// syncs in the background from the start, gives the library Stash's
// fingerprints of files it would otherwise read, and holds the catalog the
// Stash pages browse and the cache of Stash's images. Main thread only.
class Stash {
 public:
  // Starts: loads the local copy, then syncs. Hooks into `library`, which
  // must outlive this.
  Stash(const StashSettings& settings, Library& library);
  // Unhooks from the library and tells the workers to stop, without waiting
  // for them: they finish on a thread of their own (a sync under way takes
  // up to about a second to give up), touching nothing of the app's. A
  // Stash made right after works as one made at start.
  ~Stash();
  Stash(const Stash&) = delete;
  Stash& operator=(const Stash&) = delete;

  // Each frame: takes a finished load or sync, and the images the cache's
  // workers finished. True if catalog() or status() changed.
  bool poll();
  void syncNow();  // "Sync now"

  // Covers, portraits and logos, kept in cacheDir() / "stash". Views ask
  // it for what they show (want(), find()); poll() takes what arrives.
  StashImages& images() { return *images_; }
  // Whether the last poll() took new images (or failures): views showing
  // Stash's images draw them again.
  bool imagesArrived() const { return imagesArrived_; }
  // One hover preview, kept only while pointed at. No disk cache.
  StashPreview& preview() { return *preview_; }
  // Settings → Stash's Covers, Titles and male performers switches,
  // changed while running.
  void setCovers(bool on);
  void setTitles(bool on);
  void setMalePerformers(bool on);
  // Changes with each new catalog and each switch: views showing Stash's
  // covers or titles lay out again.
  uint64_t generation() const { return generation_; }
  // What to ask images() for: a scene's cover, a performer's portrait or a
  // studio's logo drawn `width` texture pixels wide.
  static StashImages::Request coverRequest(const StashScene& scene, int width);
  static StashImages::Request portraitRequest(const StashPerformer& performer, int width);
  static StashImages::Request logoRequest(const StashStudio& studio, int width);
  // Fills the cache in the background with every scene's cover at these
  // widths (a grid's and a list's), newest in Stash first, now and with
  // each new catalog, while covers are on; only when nothing is wanted.
  void prefetchCovers(const std::vector<int>& widths);

  // Write-back (docs/stash.md, "Write-back safety"): Settings' switch,
  // changed while running. While on, the app's ratings and plays go to
  // Stash as they are made (kept until Stash can be reached, then sent at
  // the next sync or change); nothing comes back.
  void setWriteBack(bool on);
  // Null while write-back has not been on.
  const StashWriteBack* writeBack() const { return writeBack_.get(); }
  // "Send my ratings and plays": planSend() compares the library with Stash
  // (writeBack()->sendPlan() says what it would send), sendPlanned() sends
  // it, closeSend() forgets it. Only while write-back is on.
  void planSend();
  void sendPlanned();
  void closeSend();
  // The Scene tab's Edit tags: adds and removes these Stash tag ids on a
  // scene (writeBack()->tagSave() says how it went), only while write-back
  // is on. Once saved, a sync brings the catalog up to date.
  void saveTags(int64_t scene, std::vector<int64_t> add, std::vector<int64_t> remove);

  // A scene whose only file the app deleted (docs/alpha/delete-file.md):
  // left out of the Stash pages and the Library for the rest of the
  // session, whatever Stash still lists. Views lay out again (generation()).
  void hideScene(int64_t id);
  bool hidden(int64_t id) const { return hidden_.count(id) > 0; }
  bool hidesScenes() const { return !hidden_.empty(); }

  // Where Stash's library folders are here (docs/stash.md, "Folders"), so
  // nobody types the mappings. lookForFolders(): each of the catalog's
  // folders is looked for, once per catalog and only while no video is open,
  // in `alsoIn` first then the likely places; call it each frame while it is
  // wanted (Settings open, after first-run setup). lookForFolder(): one
  // folder, in `root` only (the folder chosen in the browser), at once.
  // Either replaces a search under way. The finder says
  // how far it got and what it found; folderSearches() changes as each
  // search finishes, folderCheck() tells lookForFolder()'s.
  void lookForFolders(const std::vector<std::string>& alsoIn = {});
  void lookForFolder(const std::string& folder, const std::string& root);
  const StashFolderFinder& folderFinder() const { return *finder_; }
  uint64_t folderSearches() const { return folderSearches_; }
  bool folderCheck() const { return folderCheck_; }
  // The settings' mappings with what the last search found put to use: each
  // of Stash's folders none of whose scenes are here gets the mapping that
  // found the most of its files (at least half). Unchanged if none did.
  std::vector<StashFolderMap> foundFolders() const;
  // The settings' mappings with `map` as the one for Stash's `folder`
  // (stashFoldersWith(); for scenes in none of Stash's folders, beside some
  // in them, one more).
  std::vector<StashFolderMap> foldersWith(const std::string& folder, StashFolderMap map) const;
  // A video is open: no background search (one under way stops, and runs
  // again once it closes).
  void setVideoOpen(bool open);

  // Null until the local copy or the first sync is there. A new one may
  // come with any poll(): keep no copy of the pointer beyond that (the old
  // catalog is freed on the worker, as that takes milliseconds), and no
  // indexes into it either.
  const std::shared_ptr<const StashCatalog>& catalog() const { return sync_->catalog(); }
  const StashStatus& status() const { return sync_->status(); }
  const StashSettings& settings() const { return settings_; }

  // The app's own history of each catalog scene's file, indexed like its
  // scenes(), for StashCatalog::query(). Reads the library's memory only
  // (about a millisecond for every scene): when its generation() changes.
  std::vector<StashHistory> history() const;

  // At quit: waits for every Stash destroyed to finish stopping, so no
  // thread of theirs outlives main().
  static void finishStopping();

 private:
  StashSettings settings_;
  Library& library_;
  // Held apart, to be handed to the thread that waits for them.
  std::unique_ptr<StashSync> sync_;
  std::unique_ptr<StashImages> images_;
  std::unique_ptr<StashPreview> preview_;
  std::unique_ptr<StashWriteBack> writeBack_;  // made when write-back is first on
  std::unique_ptr<StashFolderFinder> finder_;
  int64_t foldersLookedFor_ = -1;  // the catalog (syncedAt) last looked for
  uint64_t folderSearches_ = 0;
  bool folderCheck_ = false, folderSearching_ = false, videoOpen_ = false;
  int64_t deliveredAfter_ = 0;                 // the sync write-back last sent after
  int64_t tagsSavedAt_ = 0;                    // the tag save last synced after
  void ratedOrPlayed(const std::string& oshash, bool rated, float rating, int plays,
                     int64_t playedAt);
  std::vector<StashLibraryEntry> libraryEntries() const;
  bool imagesArrived_ = false;
  std::vector<int> prefetchWidths_;
  void prefetch();
  const StashCatalog* polledCatalog_ = nullptr;  // the catalog generation_ counts
  uint64_t generation_ = 1;
  std::unordered_set<int64_t> hidden_;
};

// Where a scene plays from, the one place that picks it for the Stash pages
// and Previous/Next: its file here (on this device before a server, as the
// catalog chose it), else Stash's stream; the stream too for a file on an
// server that doesn't answer (`offline`), if it serves that file. A
// copy on this device goes before either however a video is opened
// (openVideo in main.cpp), and a server's file that doesn't open plays from the
// stream instead (stashStream()). Empty while Stash is off.
std::string stashSceneLocation(const StashScene& scene, const Offline* offline = nullptr);
// Its stream's locator (stash_stream.h), which serves Stash's first file.
std::string stashStream(const StashScene& scene);
// ... if that is the file of `oshash`, else empty.
std::string stashStreamOf(const StashScene& scene, const std::string& oshash);
// The catalog's scene at a location: a file of it here, or its stream's
// locator; -1 if none.
int stashSceneAt(const StashCatalog& catalog, const std::string& location);
// Stash's path of the file at that location, for deleting it
// (docs/alpha/delete-file.md): a file here's (StashCatalog::stashPathOf()),
// or for a stream the scene's (its primary's when the stream plays, as
// stashStreamOf() requires); empty if not known for sure.
std::string stashFilePathAt(const StashCatalog& catalog, const std::string& location);

// --stash-sync: one sync without the headset, with the settings (Stash on
// for this run whatever they say), printing what it found and how long each
// step and a few queries took. The exit code.
int runStashSyncDiagnostic(const StashSettings& settings);
