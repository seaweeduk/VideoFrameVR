#pragma once

#include "gesture_input.h"
#include "settings.h"
#include "video_format.h"

#include <cstdint>
#include <future>
#include <memory>
#include <random>
#include <string>

class Deletion;
class Library;
class Menu;
class Notices;
class Offline;
class Passthrough;
class PhotoPreloader;
class RunningVideo;
class SeekPreview;
class Stash;
class StickSeek;
class VideoMemory;
class VideoPlayer;
struct MenuCommand;
struct MenuView;

// What is open and how it is going (PlaybackSession::status()).
struct PlaybackStatus {
  const std::string& file;  // what plays, empty if nothing
  // While something is open: what it was opened as, before a downloaded
  // copy or a media server's stream took its place.
  const std::string& asked;
  bool open;      // something is
  bool photo;     // ... a photo
  bool video;     // ... a video
  double position, duration;  // seconds
  bool paused, ended, loading, failed, seeking;
  // Something open that goes on now: not paused, at its end, seeking,
  // loading or failed.
  bool moving;
};

// What plays: opening and closing videos and photos, Previous and Next
// through a folder or a list, what happens at a video's end, the slideshow,
// resuming, play counts, the record a file is kept under arriving, and its
// format found from its size. Main thread only.
class PlaybackSession {
 public:
  PlaybackSession(VideoPlayer& player, Settings& settings, Library& library, VideoMemory& memory,
                  Offline& offline, const std::unique_ptr<Stash>& stash, ScriptPlayer& scripts,
                  RunningVideo& running, Menu& menu, Notices& notices, PhotoPreloader& photoPreloader,
                  SeekPreview& seekPreview, Deletion& deletion, Passthrough& passthrough,
                  StickSeek& stickSeek, GestureInput& gestureInput, GestureInput::Host& gestureHost,
                  ControlInput& controlInput);

  int slideshowSeconds = 0;  // photos: show the next one after this long; 0 = off
  // At the end of a video, the menu opens on the list it was opened from.
  bool menuAtEnd = false;

  // What the command line gave: opened now, or a Stash scene once Stash's
  // catalog is there.
  void start(const std::string& file);
  // Opens a video with what is remembered about it. True if its format was
  // remembered. A video given `startAt` (seconds, 0 or more) starts there,
  // and is not resumed where it was left.
  bool open(const std::string& requested, double startAt = -1);
  // ... chosen in the menu (MenuCommand::OpenFile, OpenFileAt), after open().
  void openedByHand(const MenuCommand& c);
  // Closes the running video or photo and leaves the scene empty, keeping
  // where it was left as switching to another would.
  void close();
  // `atEnd`: a video's end going on with the next one, not Previous or Next.
  void step(int step, bool atEnd = false);
  // Stores the current format for this video (after the user chose it).
  void rememberFormat();
  AfterVideo afterVideo() const { return listShuffle ? AfterVideo::Shuffle : afterChosen; }
  // The menu's SetAfterVideo and SetSlideshow.
  void chooseAfterVideo(const MenuCommand& c);
  void setSlideshow(const MenuCommand& c);
  // A seek of the user's: this open's resume no longer applies.
  void userSeeked() { awaitingResume = false; }
  void keepResume();
  // The command, if it is one of these; false for any other.
  bool handle(const MenuCommand& c);

  // Each frame, in this order, with the app's own work between them.
  void pollFailure();
  void countPlay(bool opened);
  void pollRecord(bool libraryChanged);
  void poll();
  void detectFormat();
  // The video's size (so its format) and its settings are known.
  bool settled() const { return knownWidth > 0 && !awaitingRecord; }
  // A photo's pixels are there (photoWidth x photoHeight).
  void photoShown(int photoWidth, int photoHeight);
  void fill(MenuView& view) const;
  // Worked out when asked: a command earlier in the frame may have opened
  // or closed a file.
  PlaybackStatus status() const;

 private:
  VideoPlayer& player;
  Settings& settings;
  Library& library;
  VideoMemory& memory;
  Offline& offline;
  const std::unique_ptr<Stash>& stash;
  ScriptPlayer& scripts;
  RunningVideo& running;
  Menu& menu;
  Notices& notices;
  PhotoPreloader& photoPreloader;
  SeekPreview& seekPreview;
  Deletion& deletion;
  Passthrough& passthrough;
  StickSeek& stickSeek;
  GestureInput& gestureInput;
  GestureInput::Host& gestureHost;
  ControlInput& controlInput;

  void say(std::string text);
  // The open video's rating up or down a half star (MenuCommand::StepRating),
  // and what it is now said.
  void stepRating(int halves);
  // Another video, or none: what the last one had going on ends.
  void videoChanged();
  void startShuffle();
  void applyAfterVideo();
  void saveResume();

  double nextSlideAt = 0;    // glfwGetTime() when it does
  bool photoSized = false;  // the current photo's size is known (its format guessed)
  bool wasAtEnd = false;
  int knownWidth = 0, knownHeight = 0;
  TwoViews knownViews = TwoViews::No;
  bool formatDetected = false;  // for this file: logged once, then as it changes
  // A Stash scene given on the command line opens once Stash's catalog is there.
  std::string startScene;
  // The playing file's content record is still being found: its settings
  // apply when it is (they are newer than any videos.conf has).
  bool awaitingRecord = false;
  int64_t openingRecord = 0;
  bool awaitingResume = false;
  double resolvedResume = -1, videoOpenedAt = 0;
  std::future<std::string> neighbor, nextPhoto;
  // What the playing video was opened as, before a copy or Stash's stream
  // took its place: Previous and Next go on from there through a list.
  std::string openedAs;
  double openedAt = -1;  // ... and the start it was given (open()'s startAt)
  int neighborStep = 0;
  bool neighborWrap = false;
  // What happens when a video ends: the setting, or Repeat or Shuffle once
  // chosen in this run. Stash's Shuffle play makes it Shuffle until a video
  // is opened by hand or it is chosen again. Shuffle goes round a list once
  // from the video opened by hand (as it was opened, and its file), in a
  // new order each time it starts.
  AfterVideo afterChosen;
  bool listShuffle = false;
  std::string playStart, playStartFile;
  std::random_device seedSource;
  uint64_t shuffleSeed = 0;
  bool endStep = false;  // the step under way is a video's end going on: with no next one, the menu opens
  uint64_t endStepRestarts = 0;  // ... and the player's seeks sent when it began
  // The playing video file was opened (its play counted): where it stops is
  // kept when it is left (and every minute meanwhile), and its duration
  // once known.
  bool videoOpened = false, durationKept = false;
  double resumeSaveAt = 0;  // glfwGetTime() to keep the position next
  double resumeSaved = -2;  // ... and the one kept last
};
