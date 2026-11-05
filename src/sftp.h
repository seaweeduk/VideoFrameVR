#pragma once

#include "remote.h"

#include <string>

// What only SFTP servers (sftp://, remote.h) need before signing in.
//
// A server is trusted by its host key, as ssh's known_hosts does: the first
// time it is seen the user trusts it (trust()), and a server that answers
// with another key afterwards is refused (Status::HostKeyChanged) until it
// is trusted again. The keys trusted are kept in sftp-hosts.json.
//
// This device has one key of its own (sftp-key, mode 0600), made on first
// use, that signs in to any account that lists it (Login::key).
//
// All may block, bounded by timeouts (publicKey() only on its first use, to
// make and save the key): call them from workers, never from the render loop.
namespace sftp {

struct Probe {
  enum class HostKey { New, Trusted, Changed } hostKey = HostKey::New;
  std::string fingerprint;  // the server's key, as ssh shows it: "SHA256:..."
  bool password = false;    // `user` may sign in with a password
  bool key = false;         // ... or with a key
};
// Connects to the server `root` names without signing in: its key, and
// how `user` may sign in.
remote::Status probe(const std::string& root, const std::string& user, Probe& probe);
// Trusts `fingerprint` (Probe's) as the server's key from now on.
void trust(const std::string& root, const std::string& fingerprint);
// The trusted key, "" if none.
std::string trusted(const std::string& root);

// This device's public key: the line authorized_keys holds for it.
std::string publicKey();
// Takes this device's key off the account signed in to on `root` (its
// ~/.ssh/authorized_keys); Ok if it wasn't there.
remote::Status removeKey(const std::string& root);

// The account's home folder on `root` (signed in), as a locator.
remote::Status home(const std::string& root, std::string& locator);

}  // namespace sftp
