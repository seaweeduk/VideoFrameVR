#include "menu_services.h"

#include "library.h"
#include "media_client.h"
#include "offline.h"

#include <thread>

std::vector<VideoRecord> MenuServices::lookUp(const std::vector<FolderListing::Entry>& entries) const {
  return library->lookup(entries);
}

void MenuServices::rate(const std::string& path, int64_t video, float rating) const {
  if (video)
    library->setRating(video, rating);
  else
    library->setRating(path, rating);
}

void MenuServices::addDownload(const std::string& location, uint64_t size, const std::string& oshash,
                               const std::string& root) const {
  offline->add(location, size, oshash, root);
}

void MenuServices::cancelDownload(int64_t id) const { offline->cancel(id); }

void MenuServices::removeDownload(int64_t id) const { offline->remove(id); }

void MenuServices::retryDownload(int64_t id) const { offline->retry(id); }

void MenuServices::pauseDownloads(bool paused) const { offline->pauseAll(paused); }

void MenuServices::rememberLogin(const std::string& root) const { remote::remember(root); }

void MenuServices::forgetLogin(const std::string& root) const { remote::forget(root); }

void MenuServices::setServerName(const std::string& root, const std::string& name) const {
  remote::setName(root, name);
}

void MenuServices::signIn(const std::shared_ptr<RemoteJob>& job, const std::string& root,
                          const remote::Login& login, bool remember) const {
  std::thread([job, root, login, remember] {
    job->status = remote::signIn(root, login, remember);
    job->done = true;
  }).detach();
}

void MenuServices::connectSftp(const std::shared_ptr<RemoteJob>& job, const std::string& root,
                               const std::string& user, const std::string& trust,
                               bool remember) const {
  std::thread([job, root, user, trust, remember] {
    using Status = remote::Status;
    using HostKey = sftp::Probe::HostKey;
    RemoteJob& j = *job;
    // Trusting writes sftp-hosts.json at once, under --first-run too: the
    // sign-ins after it check the server's key against that file.
    if (!trust.empty()) sftp::trust(root, trust);
    j.status = sftp::probe(root, user, j.probe);
    if (j.status == Status::Ok && j.probe.hostKey == HostKey::Changed) {
      j.status = Status::HostKeyChanged;
    } else if (j.status == Status::Ok) {
      if (j.probe.hostKey == HostKey::New) sftp::trust(root, j.probe.fingerprint);
      j.status = j.probe.key ? remote::signIn(root, {user, "", true}, remember) : Status::NeedsLogin;
      if (j.status == Status::NeedsLogin && !j.probe.password) j.publicKey = sftp::publicKey();
      // Changed since the probe: the warning shows the key it has now.
      if (j.status == Status::HostKeyChanged) {
        const Status probed = sftp::probe(root, user, j.probe);
        if (probed != Status::Ok) j.status = probed;
      }
    }
    if (j.status == Status::HostKeyChanged) j.trusted = sftp::trusted(root);
    j.done = true;
  }).detach();
}

void MenuServices::signInSftp(const std::shared_ptr<RemoteJob>& job, const std::string& root,
                              const remote::Login& login, bool remember) const {
  // With a password and the key, the key is added to the account on the
  // server at once, under --first-run too: it changes the server's
  // account, not this profile.
  std::thread([job, root, login, remember] {
    using Status = remote::Status;
    RemoteJob& j = *job;
    j.status = remote::signIn(root, login, remember);
    if (j.status == Status::HostKeyUnknown || j.status == Status::HostKeyChanged) {
      const Status probed = sftp::probe(root, login.user, j.probe);
      if (probed != Status::Ok) {
        j.status = probed;
      } else if (j.probe.hostKey == sftp::Probe::HostKey::Changed) {
        j.status = Status::HostKeyChanged;
        j.trusted = sftp::trusted(root);
      } else {
        if (j.probe.hostKey == sftp::Probe::HostKey::New) sftp::trust(root, j.probe.fingerprint);
        j.status = remote::signIn(root, login, remember);
      }
    }
    j.done = true;
  }).detach();
}

void MenuServices::removeKey(const std::shared_ptr<RemoteJob>& job, const std::string& root) const {
  // Forgotten here, not when the menu takes the result: closing the
  // menu meanwhile drops the form, and with it the job.
  std::thread([job, root] {
    job->status = sftp::removeKey(root);
    if (job->status == remote::Status::Ok) remote::forget(root);
    job->done = true;
  }).detach();
}

void MenuServices::signInStash(const std::shared_ptr<std::promise<StashSignIn>>& done, bool moved,
                               bool unverified, ServerKind kind, const std::string& url,
                               const std::string& user, const std::string& password) const {
  // Its own thread, as the SMB sign-in: a server that doesn't answer
  // mustn't hold up anything else, and Cancel just drops the result. What
  // it says names the kind asked, which may not be the one in use.
  std::thread([done, moved, unverified, kind, url, user, password] {
    StashSignIn r;
    r.host = stashHost(url);
    r.kind = kind;
    if (moved) {
      const FoundServer found = findServer(url, unverified, kind, true);
      r.kind = found.kind;
      r.url = found.url;
      r.untrusted = found.untrusted;
    }
    r.ok = !r.untrusted;
    if (r.ok && !password.empty() && mediaServer(r.kind).abilities().signIn) {
      const std::unique_ptr<MediaClient> client =
          mediaServer(r.kind).client(r.url.empty() ? url : r.url, std::string(), unverified);
      client->nameOwnKind();
      r.ok = client->signIn(user, password, r.apiKey, r.refused);
      r.error = client->error();
      r.untrusted = !r.ok && client->problem() == StashProblem::Certificate;
    }
    done->set_value(std::move(r));
  }).detach();
}
