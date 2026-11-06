#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct mpv_handle;
struct mpv_stream_cb_info;

// Files on other computers, browsed and read in the app without an OS
// mount, on the desktop and the Steam Frame: SMB2/3 shares (smb.cpp,
// libsmb2) and SFTP servers (sftp.cpp, libssh). Read-only, but for deleting
// the playing file (remove()).
//
// A remote file or folder is named by a locator: raw UTF-8 bytes, '/'
// between parts, never percent-encoded and never with credentials. The rest
// of the app keeps a locator as it keeps a local path (library path cache,
// pins, history, folders.conf).
//   smb://host/share/path   smb://host lists the host's shares.
//   sftp://host[:port]/path the server's absolute path; sftp://host is its
//                           "/", and the port is left out when it is 22.
// host is lowercase: an address, or a name as typed. A locator's root
// (smb://host, sftp://host:2222) names its server: its login, connection
// and name.
//
// Everything here may block on the network, bounded by timeouts: call it from
// workers (or mpv's and FFmpeg's own threads), never from the render loop.
// One connection per server, shared by all its users; playback's reads go
// first.
namespace remote {

bool isRemote(std::string_view location);
// The server's own locator: smb://h of smb://h/s/a; "" if not a locator.
std::string root(const std::string& locator);
// "h" of smb://h/... or sftp://h:2222/..., "" if not a locator.
std::string host(const std::string& locator);
// The TCP port the locator's server answers on (SMB's 445, SSH's 22 or
// the one given); 0 if not a locator.
int port(const std::string& locator);
// smb://h/s/a -> smb://h/s -> smb://h; "" above the root.
std::string parent(const std::string& locator);

enum class Status {
  Ok,
  Missing,     // no such file, folder or share: the only one that may drop library paths
  Denied,      // signed in, but not allowed (a share list refused, a folder off limits)
  NeedsLogin,  // no login known for the server this run, or the server refused it
  Offline,     // no answer, timed out, or the connection broke
  InUse,       // remove(): open elsewhere without letting it be deleted (another device)
  Changed,     // remove(): not a file of that size and date (any more)
  // SFTP: the server isn't the one trusted (sftp.h): never trusted yet, or
  // it answers with another key than it did (another server, or one
  // reinstalled).
  HostKeyUnknown,
  HostKeyChanged,
  // signIn() with a key and a password: signed in with the password for
  // this run, but the server didn't take this device's key.
  KeyRefused,
};

struct Entry {
  std::string name;
  bool folder = false;
  uint64_t size = 0;
  int64_t modified = 0;  // seconds since 1970
  bool link = false;     // SFTP: a link to it, as what it points to
};
// smb://host: its shares (as folders; hidden "$" shares left out).
// Any other locator: that folder's entries.
Status list(const std::string& folder, std::vector<Entry>& entries);
Status stat(const std::string& locator, Entry& entry);
// Deletes one file for good (docs/alpha/delete-file.md), never a folder and
// never what a link points to, if it is still `size` bytes dated
// `modified`. SMB: our own handles on it close by themselves after use;
// while one is open the server refuses (InUse), which is tried again for a
// few seconds.
Status remove(const std::string& locator, uint64_t size, int64_t modified);
std::vector<std::string> subtitlesBeside(const std::string& locator);

// A file read at any position, apart from playback (oshash, thumbnails,
// photos). Background priority: it never delays playback's reads.
class File {
 public:
  static std::unique_ptr<File> open(const std::string& locator, Status& status);
  ~File();
  uint64_t size() const;
  // Up to `count` bytes at `offset`; 0 only at the end of the file, -1 on error.
  int64_t read(uint64_t offset, void* out, uint64_t count);

  struct Impl;

 private:
  File() = default;
  std::unique_ptr<Impl> impl_;
};

// A file read from `offset` to its end (an offline download): two 1 MiB
// blocks in flight, as playback's, but only while no video plays from the
// server. Reconnects as playback does.
class Reader {
 public:
  static std::unique_ptr<Reader> open(const std::string& locator, uint64_t offset, Status& status);
  ~Reader();
  uint64_t size() const;
  // The next bytes, up to `count`; 0 at the end, -1 on error or once cancelled.
  int64_t read(void* out, uint64_t count);
  void cancel();  // any thread: a read waiting returns -1

  struct Impl;

 private:
  Reader() = default;
  std::unique_ptr<Impl> impl_;
};

// Signing in. SMB: an empty user is a guest; "DOMAIN\user" names a domain.
// SFTP: key signs in with this device's key (sftp.h). With a password too,
// the password signs in and the key is added to the account on the server
// (~/.ssh/authorized_keys), then the key alone is kept: the password is
// never saved. Without a password, the key must have been added already.
struct Login {
  std::string user, password;
  bool key = false;
};
// Signs in to the server `root` names with `login`. Ok: kept for this run,
// and saved if `remember` (a file of its own, mode 0600, never in settings
// or logs). KeyRefused: kept as a password login for this run, not saved
// (remember() saves it).
Status signIn(const std::string& root, const Login& login, bool remember);
// The login known for `root` (saved or this run's), if any. Main thread too:
// no network.
bool knownLogin(const std::string& root, Login& login, bool& remembered);
// Saves `root`'s login of this run, as signIn() with `remember` would have
// (first-run setup under --first-run: only once it ends).
void remember(const std::string& root);
// Forgets `root`'s login, saved and this run's, and closes its connection.
void forget(const std::string& root);

// Servers the user signed in to, saved or this run's, with the name to show
// (the discovered name, else the host). Main thread too: no network.
struct Server {
  std::string root, name;
};
std::vector<Server> servers();
// The name to show for `root` (e.g. from discovery), kept with its login.
void setName(const std::string& root, const std::string& name);

// mpv: registers the stream protocols for locators on this instance (call
// once after mpv_initialize), and the URL to load a locator with.
// background: seek previews share the single background-request budget.
// failed: optional read-error flag, kept alive until mpv is destroyed.
void addTo(mpv_handle* mpv, bool background = false, std::atomic<bool>* failed = nullptr);
std::string mpvUrl(const std::string& locator, bool background = false);
// Opens `locator` as those protocols do, for a protocol of mpv's that
// serves parts of it (disc images, bluray.h). False if it can't be opened.
bool openMpvStream(const std::string& locator, bool background, std::atomic<bool>* failed,
                   mpv_stream_cb_info* info);

}  // namespace remote
