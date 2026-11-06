#pragma once

#include "bluray_disc.h"

#include <atomic>
#include <memory>
#include <string>

struct mpv_handle;
struct mpv_stream_cb_info;

// Blu-ray disc images (.iso) played as any video: the image is read once
// (bluray_disc.h) for its main title, and mpv plays that through this app's
// own protocols, without mounting the image:
//   bdimage://<path>  the title: its clip, or an EDL of its clips with
//                     their in and out times
//   bdimagebase://<path>  the same with a 3D title's base view only
//   bdclip://<clip>/<path>  one clip file read from inside the image
//                     ("00000.m2ts", "00000.ssif")
// <path> is a local path or a remote locator (remote.h), whose reads go
// through playback's read-ahead (previews: background reads).
namespace bdmv {

bool isDiscImage(const std::string& path);

// What 3D titles play from: where mpv decodes both views of MVC video (this
// app's own mpv build, packaging/mpv), SSIF/<clip>.ssif (both views in one
// transport stream); otherwise their base view, STREAM/<clip>.m2ts (one
// H.264 stream), as 2D titles do. Set once, before any title plays. Any
// thread.
void setPlayBothViews(bool on);

// The disc in an image, read once and kept while the file keeps its size
// and date. Blocking (it reads the image): workers and mpv's threads only.
std::shared_ptr<const Disc> read(const std::string& path);
// What read() kept for `path`, without reading: null if nothing yet. Any
// thread.
std::shared_ptr<const Disc> known(const std::string& path);

// Whether `path` is a disc image read() found encrypted (AACS). Any thread,
// without reading.
bool isEncrypted(const std::string& path);

// Whether the title plays both views: a 3D title, where mpv decodes them.
bool playsStereo(const Disc& disc);

// Options for mpv's loadfile command (",name=value..."; empty if none):
// which eye the base view of a 3D title is, once the disc has been read.
// Without reading.
std::string mpvOptions(const std::string& path);

// mpv: registers the protocols on this instance (call once after
// mpv_initialize). background: seek previews' instance. failed: as
// remote::addTo()'s.
void addTo(mpv_handle* mpv, bool background = false, std::atomic<bool>* failed = nullptr);
// The URL mpv loads the image's main title with; baseView: a 3D title's
// base view alone, as 2D (seek previews).
std::string mpvUrl(const std::string& path, bool baseView = false);

// The main title's first clip, its base view only (FFmpeg decodes no MVC),
// as mpv's protocols open it (background reads), for FFmpeg's thumbnails
// and checks (remote_avio.h).
bool openFirstClip(const std::string& path, mpv_stream_cb_info& info);

}  // namespace bdmv
