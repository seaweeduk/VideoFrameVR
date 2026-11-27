#pragma once

#include "folder_listing.h"
#include "oshash.h"
#include "video_memory.h"  // VideoSettings

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// What the library holds about one video, for the browser and the player.
struct VideoRecord {
  int64_t id = 0;                // 0 = not in the library (yet)
  float rating = 0;              // 0 = unrated, else 0.5 .. 5 in half steps
  int playCount = 0;
  int64_t lastPlayedAt = 0;      // Unix seconds, 0 = never
  double resumeSeconds = -1;     // where playback stopped, -1 = not known
  double durationSeconds = -1;   // -1 = not known
  bool known() const { return id != 0; }
};

// Where a video left at `position` (seconds, of `duration`; 0 or less = not
// known) resumes, -1 = from the start: the first 10 seconds are not worth
// keeping, and the last 30 seconds or 2 % count as watched to the end.
double resumePoint(double position, double duration);

// The media library (docs/library.md): ratings, play counts, last played,
// resume position and per-video settings, in library.db in dataDir(). A
// video is known by its content (Stash's oshash, see oshash.h) or, for a
// link, by its address, so a moved or renamed file keeps its history and
// copies of one file share it. An http(s) link is known by its content
// while it is open, once its fingerprint (range_fingerprint.h) says what
// that is; by its address a link keeps its settings alone. Records earlier
// versions kept for YouTube videos ("youtube:<id>") stay in the file as
// they are; no address matches them, and the lists show no links.
//
// The render loop never waits for it. load() reads the whole library into
// memory once at startup; from then on every call here works on that copy
// and returns at once. Writes go to the copy immediately and to the file
// from a writer thread, which owns the SQLite connection. Whatever reads
// files (a stat of an old path, a fingerprint) runs on a second thread;
// poll() takes its results. Main thread only.
class Library {
 public:
  Library();
  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;
  // Waits a moment for fingerprints still due (so a rating just made on a
  // new file is kept), closes the open link, then writes what is still
  // queued.
  ~Library();

  // Opens or creates library.db and reads it. Without it (it can't be
  // opened, is in use by another instance, or is from a newer VideoFrameVR)
  // the player runs without a library: nothing is found or stored, and the
  // file is left as it is. False then.
  bool load();
  bool available() const { return available_; }

  // Takes the background results; true if any record changed. Each frame.
  bool poll();
  // Changes whenever what record() or lookup() return may have, or a
  // video's format (other settings don't count), so views can tell theirs
  // is old.
  uint64_t generation() const { return generation_; }

  // The records of a listed folder, one per entry (unknown for folders and
  // unknown files). Reads nothing itself. With the entries' sizes and dates
  // known, a recorded file whose size or date changed is fingerprinted again
  // in the background (it takes the record for its current content), and an
  // unknown file the size of a recorded one is fingerprinted to find a moved
  // or copied video; generation() tells when that changed anything.
  std::vector<VideoRecord> lookup(const std::vector<FolderListing::Entry>& entries);
  // A file path or stream location, also with changes still waiting for
  // its fingerprint.
  VideoRecord record(const std::string& location) const;
  // Whether the location has a record (a new file has none until its
  // fingerprint arrives).
  bool known(const std::string& location) const;
  // Whether its record is still to come: a file the library doesn't know
  // (yet: open() asks for its fingerprint), or the open link until its
  // fingerprint's answer.
  bool awaited(const std::string& location) const;
  // When a video of the library was first seen (Unix seconds), 0 if not known.
  int64_t addedAt(int64_t id) const;
  // A video of the library by its id (VideoRecord::id), wherever its files
  // are now; unknown if there is none.
  VideoRecord video(int64_t id) const;
  // The video's own settings; false if the library has none for it.
  bool settings(const std::string& location, VideoSettings& settings) const;
  // Every fingerprinted video (not streams) by its oshash, with the format
  // chosen for it in the player, if one was.
  std::unordered_map<std::string, std::optional<VideoFormat>> formatsByHash() const;
  // A fingerprinted video by its oshash, wherever its files are, with the
  // format chosen for it in the player, if one was; false if there is none.
  bool videoByHash(const std::string& oshash, VideoRecord& record,
                   std::optional<VideoFormat>& format) const;

  // Optional (Stash, docs/stash.md): the oshash of a file at `path` with
  // this size and date, known without reading it; empty if not known. An
  // unknown file listed with them is taken by it, not read: a recorded
  // video's file moved or copied there joins that video, any other is
  // known to be none (and its fingerprint is kept for when it is opened).
  // Called on the main thread; must answer at once. Empty: none.
  using FingerprintSource =
      std::function<std::string(const std::string& path, uint64_t size, int64_t mtime)>;
  void setFingerprintSource(FingerprintSource source) { fingerprintSource_ = std::move(source); }

  // Optional (Stash's write-back): told of each rating set (0 = cleared)
  // and each play (`plays` of them, the last at `playedAt`) once they apply
  // to a video file's record, with its oshash. Called on the main thread;
  // must return at once and not change the library. Empty: none.
  using RatingPlayObserver = std::function<void(const std::string& oshash, bool rated,
                                                float rating, int plays, int64_t playedAt)>;
  void setRatingPlayObserver(RatingPlayObserver observer) { ratingPlayObserver_ = std::move(observer); }
  // The oshash of the video at a file path, if the library knows it; else empty.
  std::string oshashOf(const std::string& location) const;
  // A copy of a video made by the app (an offline download, offline.h),
  // with its oshash, size and date: it shares the video's history at once,
  // without its folder being listed.
  void addCopy(const std::string& path, const std::string& oshash, uint64_t size, int64_t mtime);
  // A file the app deleted: its path goes, the video keeps its history.
  void forgetFile(const std::string& location);
  // A location that is no file (a scene streamed from Stash) as a file of
  // the video with this oshash, for the session: record(), settings(),
  // oshashOf() and the changes below take it as one, and open() makes the
  // video if it is new. Nothing is read, and the location is never stored.
  void locate(const std::string& location, const std::string& oshash);

  // Every video file rated or played, by its oshash, with its record.
  std::vector<std::pair<std::string, VideoRecord>> ratedOrPlayed() const;

  // A video starts playing: a file's identity is checked on every open,
  // even if its path, size and date match. Until its fingerprint arrives the
  // calls below wait for it, in order, and are never lost. An http(s) link
  // is fingerprinted on a thread of its own, and the link open before it is
  // closed: what waits goes to its content's video once the fingerprint
  // comes, else to its address, which keeps its settings alone.
  void open(const std::string& location);
  // ... has stopped playing: an open link is no longer awaited or its
  // content's (opened again, it is fingerprinted again). Nothing for
  // anything else.
  void close(const std::string& location);
  // 0 = unrated, else rounded to half stars within 0.5 .. 5.
  void setRating(const std::string& location, float rating);
  void setRating(int64_t id, float rating);  // a video of the library, by its id
  // One more play, played at `when` (Unix seconds, 0 = now).
  void recordPlay(const std::string& location, int64_t when = 0);
  void setResume(const std::string& location, double seconds);  // < 0 = none
  void setDuration(const std::string& location, double seconds);
  // Only these parts (VideoSettings::Part) of `settings`; the video's other
  // settings stay as they are, also those a new file's record brings once
  // it is found.
  void setSettings(const std::string& location, const VideoSettings& settings,
                   unsigned parts = VideoSettings::kAll);
  // Settings from before the library (videos.conf): kept only if the
  // video has none of its own by the time its record is there. A nonzero
  // record restricts the fallback to that identity after fingerprinting.
  void carryOverSettings(const std::string& location, const VideoSettings& settings,
                         int64_t record = 0);

  // A change to one video's record, made now or once it is found.
  struct Change {
    bool rating = false;
    float ratingValue = 0;
    int plays = 0;
    int64_t playedAt = 0;
    bool resume = false;
    double resumeSeconds = -1;
    bool duration = false;
    double durationSeconds = -1;
    unsigned settingsParts = 0;  // VideoSettings::Part, set from settingsValue
    VideoSettings settingsValue;
    bool carryOver = false;  // ... and those from videos.conf, for a video without any
    VideoSettings carriedValue;
    int64_t carriedRecord = 0;
    bool empty() const {
      return !rating && plays == 0 && !resume && !duration && !settingsParts && !carryOver;
    }
    void merge(const Change& later);
    // The settings a video gets from this, if it has `own` (null: none).
    VideoSettings settingsOver(const VideoSettings* own, int64_t record = 0) const;
  };

 private:
  struct Video {
    std::string oshash, streamKey;  // one of them
    VideoRecord record;
    int64_t addedAt = 0;
    bool hasSettings = false;  // else the settings column is empty (videos.conf may have some)
    VideoSettings settings;
  };
  struct File {
    int64_t videoId = 0;
    uint64_t size = 0;
    int64_t mtime = 0;
    int64_t seenAt = 0;
  };
  struct Writer;   // the thread with the database
  struct Checker;  // the thread that reads files

  void change(const std::string& location, const Change& c);
  void apply(int64_t id, const Change& c);
  int64_t addVideo(const std::string& oshash, const std::string& streamKey);
  // In memory (files_ and bySize_); the write* calls queue it for the file.
  void indexFile(const std::string& path, const File& file);
  void unindexFile(const std::string& path);
  void writeVideo(int64_t id);
  void writeFile(const std::string& path);
  void writeDroppedFile(const std::string& path);
  void link(const std::string& path, const File& file);
  bool checkDue(const std::string& path, const FolderListing::Entry& entry);
  bool takeKnownFingerprint(const std::string& path, const FolderListing::Entry& entry);
  void handleResolved(const std::string& path, const std::string& oshash, uint64_t size,
                      int64_t mtime, bool ok);
  void handleVerified(const std::string& path, const std::string& oshash, uint64_t size,
                      int64_t mtime);
  void handleMoved(const std::string& path, const std::string& oshash, uint64_t size,
                   int64_t mtime, const std::vector<std::string>& gone);
  bool dropGoneFiles(const std::string& path, const std::vector<std::string>& gone);
  const Video* videoAt(const std::string& key, bool stream) const;
  void takeLinkPrint();

  bool available_ = false;
  uint64_t generation_ = 0;
  int64_t lastId_ = 0;  // the highest video id: new ones count on from it
  std::unordered_map<int64_t, Video> videos_;
  std::unordered_map<std::string, int64_t> byHash_, byStream_;  // video ids
  std::unordered_map<std::string, File> files_;                 // by path
  std::unordered_map<std::string, std::string> located_;        // locate()'s oshashes
  std::unordered_multimap<uint64_t, std::string> bySize_;       // paths of files_, for moves
  // Files (and the awaited link) waiting for their fingerprint, with what
  // is to happen to them.
  std::unordered_map<std::string, Change> pending_;
  std::unordered_set<std::string> resolving_;  // ... and those being fingerprinted
  // Files already checked in the background this session (with the size
  // and date they were checked at), so listing a folder again costs nothing;
  // and those being checked.
  std::unordered_map<std::string, std::pair<uint64_t, int64_t>> checked_;
  std::unordered_set<std::string> checking_;
  FingerprintSource fingerprintSource_;
  RatingPlayObserver ratingPlayObserver_;
  // Unknown files whose fingerprint the source gave, with the size and
  // date they were listed with.
  std::unordered_map<std::string, FileFingerprint> knownPrints_;
  // The http(s) link open, from open() to close(). Awaited while its
  // fingerprint is on the way (its changes wait in pending_), then its
  // content's (located_) or its address's.
  std::string openLink_;
  std::future<FileFingerprint> linkPrint_;
  std::shared_ptr<std::atomic<bool>> linkCancel_;
  std::unique_ptr<Writer> writer_;
  std::shared_ptr<Checker> checker_;
};
