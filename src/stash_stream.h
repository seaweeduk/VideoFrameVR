#pragma once

#include "oshash.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct mpv_handle;
struct mpv_stream_cb_info;

// Stash's scenes played straight from its server, without SMB or a mount
// (docs/alpha/stash-streaming.md): the original file at
// <server>/scene/<id>/stream, read with byte ranges, never a transcoded one.
// The ApiKey goes in a header, never in an address.
//
// A scene is named by a locator, stashscene://<server>/<id>/<file name>,
// <server> a number for Stash's server in this run, the name its file's, for
// what is told from a name (the format, passthrough). It reads only while
// Stash is on with that server, and is never saved.

// The server scenes are read from: the Stash facade's while it runs, none
// with an empty url; `unverified`: its certificate is accepted unverified.
// Any thread.
void stashServe(const std::string& url, const std::string& apiKey, bool unverified);
// Scene `id`'s locator on the server served now; empty if none.
std::string stashSceneLocator(int64_t id, std::string_view fileName);
bool isStashScene(std::string_view location);
// The scene a locator names on the server served now; 0 if none.
int64_t stashSceneId(std::string_view location);
// The files scenes' streams serve, by scene id, as the catalog knows them:
// what is told of a locator without asking Stash (a thumbnail's disk cache).
// Replaced with each catalog. Any thread.
void stashKnowFiles(std::unordered_map<int64_t, FileFingerprint> files);
// A locator's file; no oshash if the catalog doesn't know it.
FileFingerprint stashSceneFile(std::string_view location);

// mpv: registers stashscene:// on this instance (once, after mpv_initialize).
// Playback reads ahead as SMB's does (read_ahead.h) and reconnects at where
// it was; background (seek previews) reads one request at a time, none
// started while a playback stream opens. failed: as remote::addTo's.
void addStashStreams(mpv_handle* mpv, bool background = false,
                     std::atomic<bool>* failed = nullptr);
// A scene read for FFmpeg (remote_avio.h: thumbnails) through mpv's stream
// callbacks, as the seek previews read it but in larger requests; false
// if it doesn't open.
bool openStashScene(const std::string& locator, mpv_stream_cb_info& info);

// Downloads (offline.h) keep a scene as a source, never as its locator:
// stash:<server>/<id>/<file name>, <server> a hash of Stash's address as
// typed (as its outbox's file name), the same after a restart. It holds
// neither the host nor the key.
// A locator's source; empty if it isn't one on the server served now.
std::string stashSource(std::string_view locator);
bool isStashSource(std::string_view source);
// A source's locator on the server served now; empty if it is another's.
std::string stashSourceLocator(std::string_view source);
// A source's file as Stash serves it now: its size, date (Last-Modified)
// and the oshash of its ends. No oshash if it wasn't read, and `gone` if
// Stash has no such scene. Blocks: worker threads only.
FileFingerprint stashFingerprint(const std::string& source, bool& gone);
// Whether the server served now answers a small GraphQL request (true if
// none is served). Blocks for up to 5 s: worker threads only.
bool stashAnswers();

// A scene's captions (its .srt and .vtt files Stash found), for a locator or
// a source, from the server served now. Stash serves each as WebVTT,
// whatever its file. Blocks: worker threads only.
struct StashCaption {
  std::string language;  // empty for none
  std::string type;      // its file's: srt or vtt
  std::string text;
};
std::vector<StashCaption> stashCaptions(std::string_view location);

// A source's file read in order from `offset`, by playback's reader (two
// blocks ahead, again from where it stopped after a dropped connection).
// Every request is If-Range the date it had: a file changed meanwhile
// isn't read on (changed()).
class StashReader {
 public:
  static std::unique_ptr<StashReader> open(const std::string& source, uint64_t offset,
                                           const FileFingerprint& file);
  ~StashReader();
  // The next bytes, up to `count`; 0 at the end, -1 on error or once cancelled.
  int64_t read(void* out, uint64_t count);
  void cancel();  // any thread: a read waiting returns -1
  bool changed() const;

 private:
  StashReader() = default;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
