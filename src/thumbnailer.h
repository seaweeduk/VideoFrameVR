#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

typedef struct _cairo_surface cairo_surface_t;

// A file the app deleted (with the size and date it had): its picture in
// the disk cache goes too. Workers only.
void dropThumbnail(const std::string& path, uint64_t size, int64_t seconds, long nanos);

// Makes thumbnails of local videos and photos for the file browser: a frame
// from 20% into a video (one eye for 3D), or the photo itself, filling 16:9.
// They are decoded with FFmpeg in a few background threads, each on its own
// file, so the GL thread, which also draws the headset's frames, never waits
// for them. Each is made at the gallery's size, and for the list from that,
// so both paste it pixel for pixel. The gallery's are also kept on disk as JPEGs (cacheDir() /
// "thumbnails", about 500 MB at most) and read back from there.
class Thumbnailer {
 public:
  struct Size {
    int width = 0, height = 0;
  };

  Thumbnailer() = default;
  Thumbnailer(const Thumbnailer&) = delete;
  Thumbnailer& operator=(const Thumbnailer&) = delete;
  ~Thumbnailer();

  // The widths in pixels of the gallery's pictures and the list's (16:9).
  // Before start().
  void setSizes(int galleryWidth, int listWidth);
  void start();
  // The thumbnail if it is ready, else null. Failed files also give null.
  cairo_surface_t* find(const std::string& path) const;
  // Whether the file's thumbnail was made or failed (until the cache drops it).
  bool tried(const std::string& path) const { return done_.count(path) > 0; }
  // The files that should get thumbnails, most urgent first (e.g. the rows on
  // screen, then those around them), drawn `width` pixels wide: at the
  // list's size up to its width, else the gallery's. Replaces earlier
  // wishes. These are also kept when the memory runs full.
  void want(const std::vector<std::string>& paths, int width);
  // As want(), for first-run setup's cards: the first `shown` are made
  // `width` wide themselves (up to the gallery's), to paste pixel for
  // pixel; the rest only ahead of the Library, kept at the list's size.
  void wantAhead(const std::vector<std::string>& paths, size_t shown, int width);
  // While a video plays (open and not paused), none are made and one being
  // made stops; those on disk are still read.
  void setPlaying(bool playing);
  // Takes over thumbnails the workers have finished. True if there were any.
  bool update();

 private:
  struct Frame {
    std::string path;
    cairo_surface_t* surface = nullptr;  // null: could not be made
  };
  struct Worker;
  static void work(const std::shared_ptr<Worker>& worker);
  void request(const std::vector<std::string>& paths, size_t shown, int width, bool exact);

  Size gallery_, list_;
  std::vector<std::thread> workers_;
  std::shared_ptr<Worker> state_;  // the workers' shared state
  std::map<std::string, cairo_surface_t*> done_;  // GL thread only; null = could not be made
  size_t doneBytes_ = 0;                          // of done_'s surfaces
  std::set<std::string> wanted_;                  // GL thread only: the last want()
  bool playing_ = false;                          // GL thread only: the last setPlaying()
};
