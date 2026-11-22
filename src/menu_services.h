#pragma once

#include "folder_listing.h"
#include "remote.h"
#include "server_kind.h"
#include "sftp.h"

#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

class Library;
class Offline;
struct VideoRecord;

// A sign-in on a thread of its own, or for SFTP the connect job (probe,
// trust a new key, try this device's key), or Forget's key removal.
struct RemoteJob {
  enum class Task { SignIn, Connect, RemoveKey } task = Task::SignIn;
  std::atomic<bool> done{false};
  remote::Status status = remote::Status::Ok;
  sftp::Probe probe;
  std::string publicKey;  // Connect for a server taking keys only: this device's
  std::string trusted;    // HostKeyChanged: the key trusted before
};

// Stash's Save signing in (StashMenu::signingIn).
struct StashSignIn {
  bool ok = false, refused = false;
  bool untrusted = false;  // failed on a certificate that isn't trusted
  std::string host;        // ... the server's, as Save sent it
  std::string url;  // where Stash answered, if it did
  ServerKind kind = ServerKind::Stash;  // ... and as which kind (findServer())
  std::string apiKey, error;
};

// What the menu changes outside itself. It reads the library and offline
// downloads through const pointers of its own; every change it makes there
// goes through here, as do the logins it keeps for servers, the SFTP keys
// it trusts and the sign-ins it runs. Set once at startup
// (Menu::setServices()); each call runs at once, on the calling (main)
// thread, and a sign-in then goes on on a thread of its own.
struct MenuServices {
  Library* library = nullptr;
  Offline* offline = nullptr;

  // The records of a listed folder (Library::lookup()): also what makes the
  // library fingerprint changed, moved and copied files in the background.
  std::vector<VideoRecord> lookUp(const std::vector<FolderListing::Entry>& entries) const;
  // A video's rating, by its id if it has one, else by its path.
  void rate(const std::string& path, int64_t video, float rating) const;

  // The download queue (Offline's add(), cancel(), remove(), retry() and pauseAll()).
  void addDownload(const std::string& location, uint64_t size, const std::string& oshash,
                   const std::string& root) const;
  void cancelDownload(int64_t id) const;
  void removeDownload(int64_t id) const;
  void retryDownload(int64_t id) const;
  void pauseDownloads(bool paused) const;

  // Logins kept for servers (remote.h): this run's one saved, one forgotten
  // (saved and this run's), the name shown for a server.
  void rememberLogin(const std::string& root) const;
  void forgetLogin(const std::string& root) const;
  void setServerName(const std::string& root, const std::string& name) const;

  // Each starts a detached thread that fills `job` in and sets its `done`.
  // A sign-in (remote::signIn()); SFTP's connect job, trusting `trust`
  // first if it isn't empty; SFTP's sign-in; this device's key taken off
  // the account on `root`, and its login forgotten.
  void signIn(const std::shared_ptr<RemoteJob>& job, const std::string& root,
              const remote::Login& login, bool remember) const;
  void connectSftp(const std::shared_ptr<RemoteJob>& job, const std::string& root,
                   const std::string& user, const std::string& trust, bool remember) const;
  void signInSftp(const std::shared_ptr<RemoteJob>& job, const std::string& root,
                  const remote::Login& login, bool remember) const;
  void removeKey(const std::shared_ptr<RemoteJob>& job, const std::string& root) const;
  // ... and Stash's Save (StashPages::saveServer()) into `done`: the
  // address looked for again if it `moved`, and with a password, signed in
  // to there.
  void signInStash(const std::shared_ptr<std::promise<StashSignIn>>& done, bool moved,
                   bool unverified, ServerKind kind, const std::string& url,
                   const std::string& user, const std::string& password) const;
};
