#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

typedef struct _cairo_surface cairo_surface_t;

// Stash's images for the menu: scene covers, performer portraits and studio
// logos. Each is downloaded once and kept on disk as it came, and copies
// 256, 512, 1024 and 2048 pixels wide (never wider than the original) are
// made from it on first need. A view asks for the width it draws and gets
// the smallest copy at least that wide; a cover's copy in memory is shrunk
// further, to just cover a 16:9 picture that wide. Downloads, decoding and
// resizing run on worker threads, so the GL thread never waits, and only the
// small copies are ever decoded for display. Made only when Stash is on.
class StashImages {
 public:
  enum class Kind { Cover, Performer, Studio };

  struct Request {
    Kind kind = Kind::Cover;
    std::string id;   // the Stash id of the scene, performer or studio
    std::string url;  // its image_path, with the ?t= that versions it
    int width = 0;    // in texture pixels, as the view draws it
    // Covers: the video's size and whether it is side by side 3D. A cover
    // the video's size is then a raw frame, and only its left eye is shown.
    int videoWidth = 0, videoHeight = 0;
    bool stereo = false;
  };

  // What the cache holds on disk.
  struct DiskUsage {
    int originals = 0, copies = 0;
    uint64_t originalBytes = 0, copyBytes = 0;
  };
  static DiskUsage diskUsage(const std::filesystem::path& root);

  // `root` is the cache folder (e.g. cacheDir() / "stash"), `url` Stash's
  // address as typed: every image is fetched from it, whatever host its
  // URL names (StashAddress::pathOn()). `apiKey` is sent when set;
  // `unverified`: the server's certificate is accepted unverified.
  StashImages(std::filesystem::path root, const std::string& url, std::string apiKey,
              bool unverified);
  StashImages(const StashImages&) = delete;
  StashImages& operator=(const StashImages&) = delete;
  // Waits for the workers, so from any thread once nothing else uses this;
  // frees the surfaces find() gave.
  ~StashImages();

  // Nothing is created on disk or downloaded before this.
  void start();
  // Tells the workers to stop, without waiting: a download under way ends
  // soon after. Another StashImages on the same folder touches it only once
  // this one's workers are gone.
  void stop();
  // The image as a premultiplied ARGB32 surface to paint as it is: the
  // smallest copy in memory at least as wide as asked, else the widest one,
  // else null (not there yet, failed, or no image).
  cairo_surface_t* find(const Request& request) const;
  // True if the image could not be had this session (no image, a failed
  // download, a file that cannot be read): show the app's own thumbnail.
  bool failed(const Request& request) const;
  // The images that should be shown, most urgent first; replaces earlier
  // wishes. These are also kept in memory when it runs full.
  void want(const std::vector<Request>& requests);
  // Images to fetch and size on disk when nothing is wanted, without
  // decoding them for display; replaces the earlier list.
  void prefetch(const std::vector<Request>& requests);
  // Takes over images the workers have finished. True if there were any.
  bool update();

 private:
  struct State;
  std::unique_ptr<State> state_;
};

// Opaque BGRx pixels (top row first) as a JPEG, as the copies are kept, and
// such a JPEG read back. Also for the file thumbnails' disk cache.
bool encodeJpeg(const std::vector<uint32_t>& pixels, int width, int height, std::string& out);
bool decodeJpeg(const std::string& data, std::vector<uint32_t>& pixels, int& width, int& height);
