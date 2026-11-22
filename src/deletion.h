#pragma once

#include "delete_info.h"
#include "file_ops.h"
#include "menu_command.h"
#include "stash_delete.h"

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>

class Library;
class LibraryIndex;
class Menu;
class Offline;
class Stash;
class VideoPlayer;
struct Settings;

// Deleting the playing file (docs/alpha/delete-file.md): what the dialog
// shows, looked up in the background (a slow share must not freeze the
// headset), then the deletion, one at a time.
class Deletion {
 public:
  Deletion(Menu& menu, VideoPlayer& player, const Settings& settings, Library& library,
           LibraryIndex& libraryIndex, Offline& offline, const std::unique_ptr<Stash>& stash)
      : menu_(menu), player_(player), settings_(settings), library_(library),
        libraryIndex_(libraryIndex), offline_(offline), stash_(stash) {}

  // Closes the running video; set once, before the frames.
  std::function<void()> closeVideo;

  // The menu's RequestDelete, CancelDelete and DeleteFile, for the file
  // playing (`currentFile`). request() is true if the dialog's lookup started.
  bool request(const MenuCommand& c, const std::string& currentFile, const std::string& streamOshash);
  void cancel(const std::string& currentFile);
  void confirm(const MenuCommand& c, const std::string& currentFile);
  // Each frame: the lookup's and the deletion's results, once they are there.
  void poll();
  // Playback moves on (another video, or none): a delete dialog still
  // asking goes, with its lookup, so its confirm can't act on what came
  // next. One confirmed (Deleting) stays.
  void dropDialog();

 private:
  // The lookup's: the file's stamp (none for Stash's stream), and with
  // Send to Stash on, its oshash and its scene's files as Stash has them.
  struct Look {
    DeleteStatus status = DeleteStatus::Ok;
    FileStamp stamp;
    std::string oshash;
    bool asked = false;
    StashFileLook stash;
  };

  Menu& menu_;
  VideoPlayer& player_;
  const Settings& settings_;
  Library& library_;
  LibraryIndex& libraryIndex_;
  Offline& offline_;
  const std::unique_ptr<Stash>& stash_;

  DeleteInfo info_;
  // What is deleted: the file playing, or a copy's source (with the
  // copy), a scene's stream locator for one from Stash's; empty: a source
  // on another Stash server.
  std::string target_;
  FileStamp stamp_;  // the file's as the dialog shows it
  int64_t scene_ = 0;  // the Stash scene it is a file of; 0: none
  size_t sceneFiles_ = 0;
  std::string stashPath_;      // Stash's path of it; empty: not known for sure
  std::string oshash_;         // the file's, as the lookup read it
  // The oshash the file deleted must have: the download's for a copy's
  // source, the one opened for a stream; empty: any (a file here).
  std::string expected_;
  bool throughStash_ = false;  // Stash deletes it: Send to Stash on, its file found
  std::future<Look> lookup_;   // dropped without waiting
  // From std::async: dropping it waits for the deletion, so it is only
  // replaced once taken.
  std::future<DeleteOutcome> result_;
  bool resume_ = false;  // the video played when the button was pressed
  int64_t copy_ = 0;     // the download whose copy plays
};
