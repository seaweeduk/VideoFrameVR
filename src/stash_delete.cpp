#include "stash_delete.h"

#include "stash_client.h"

namespace {

StashFileLook lookUp(StashClient& client, int64_t scene, const std::string& oshash,
                     const std::string& stashPath) {
  StashFileLook look;
  std::vector<StashClient::SceneFile> files;
  look.answered = client.sceneFiles(scene, files, look.trash, look.scene);
  if (!look.answered) {
    look.error = client.error();
    return look;
  }
  int matches = 0;
  for (const StashClient::SceneFile& f : files) {
    look.files.push_back(f.id);
    if (!oshash.empty() && !stashPath.empty() && f.oshash == oshash && f.path == stashPath) {
      look.file = f.id;
      ++matches;
    }
  }
  if (matches != 1) look.file = 0;  // which one is meant isn't known
  return look;
}

// Stash deletes the file (`fromDisk`) or only its entry: the scene with it
// if it is the scene's only file, else the file alone.
bool remove(StashClient& client, int64_t scene, const StashFileLook& look, bool fromDisk) {
  if (look.files.size() == 1) return client.destroyScene(scene, fromDisk);
  if (look.files.front() == look.file && !client.setPrimaryFile(scene, look.files[1])) return false;
  return client.destroyFile(look.file, fromDisk);
}

}  // namespace

StashFileLook lookUpStashFile(const std::string& url, const std::string& apiKey, bool unverified,
                              int64_t scene, const std::string& oshash,
                              const std::string& stashPath) {
  StashClient client(url, apiKey, unverified);
  return lookUp(client, scene, oshash, stashPath);
}

DeleteOutcome deleteThroughStash(const std::string& url, const std::string& apiKey,
                                 bool unverified, bool sendToStash, int64_t scene, const std::string& oshash,
                                 const std::string& stashPath, const std::string& here,
                                 const FileStamp& stamp, bool trashPromised) {
  DeleteOutcome r;
  // The file here first, as the dialog showed it.
  FileStamp now;
  if (!here.empty()) {
    r.status = stampOf(here, now);
    if (r.status == DeleteStatus::Ok && !(now == stamp)) r.status = DeleteStatus::Changed;
    if (r.status != DeleteStatus::Ok) return r;
  }
  StashClient client(url, apiKey, unverified);
  client.setWriteBack(sendToStash);
  const StashFileLook look = lookUp(client, scene, oshash, stashPath);
  if (look.file && remove(client, scene, look, true)) {
    r.stashDeleted = true;
    // Stash deleted its own path of it; this one too, if that was another.
    r.status = here.empty() || stampOf(here, now) == DeleteStatus::Gone
                   ? DeleteStatus::Ok
                   : removeFile(here, stamp, r.error);
    return r;
  }
  const std::string why = look.file ? client.error() : look.error;
  const bool reached = look.answered && client.reached();
  // Nothing else deletes Stash's stream's file, nor deletes for good what
  // the dialog said Stash would put in its trash.
  if (here.empty() || trashPromised) {
    r.status = !reached                     ? DeleteStatus::Offline
               : look.file                  ? DeleteStatus::Failed
               : look.scene || !here.empty() ? DeleteStatus::Changed
                                            : DeleteStatus::Gone;
    r.error = why;
    return r;
  }
  r.status = removeFile(here, stamp, r.error);
  if (r.status != DeleteStatus::Ok) return r;
  // Stash refused (its media read-only, say): its entry goes as Clean would.
  r.listed = look.scene || !look.answered;
  if (look.file && reached && remove(client, scene, look, false)) r.listed = false;
  return r;
}
