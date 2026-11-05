#include "file_ops.h"

#include "remote.h"

#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace {

DeleteStatus statusOf(int error) {
  switch (error) {
    case ENOENT:
    case ENOTDIR: return DeleteStatus::Gone;
    case EACCES:
    case EPERM:
    case EROFS: return DeleteStatus::Denied;
    case EBUSY:
    case ETXTBSY: return DeleteStatus::InUse;
    case EISDIR: return DeleteStatus::NotAFile;
    default: return DeleteStatus::Failed;
  }
}

DeleteStatus statusOf(remote::Status status) {
  switch (status) {
    case remote::Status::Ok: return DeleteStatus::Ok;
    case remote::Status::Missing: return DeleteStatus::Gone;
    case remote::Status::Denied:
    case remote::Status::NeedsLogin: return DeleteStatus::Denied;
    case remote::Status::InUse: return DeleteStatus::InUse;
    case remote::Status::Changed: return DeleteStatus::Changed;
    default: return DeleteStatus::Offline;
  }
}

}  // namespace

DeleteStatus stampOf(const std::string& location, FileStamp& stamp) {
  if (remote::isRemote(location)) {
    remote::Entry entry;
    const remote::Status status = remote::stat(location, entry);
    if (status != remote::Status::Ok) return statusOf(status);
    if (entry.folder) return DeleteStatus::NotAFile;
    stamp = {entry.size, entry.modified, 0};
    return DeleteStatus::Ok;
  }
  struct stat st {};
  if (lstat(location.c_str(), &st) != 0) return statusOf(errno);
  if (!S_ISREG(st.st_mode)) return DeleteStatus::NotAFile;
  stamp = {static_cast<uint64_t>(st.st_size), st.st_mtim.tv_sec, st.st_mtim.tv_nsec};
  return DeleteStatus::Ok;
}

DeleteStatus removeFile(const std::string& location, const FileStamp& expected, std::string& error) {
  if (remote::isRemote(location))
    return statusOf(remote::remove(location, expected.size, expected.modified));
  FileStamp now;
  const DeleteStatus status = stampOf(location, now);
  if (status != DeleteStatus::Ok) return status;
  if (!(now == expected)) return DeleteStatus::Changed;
  // unlink() never removes a folder and removes a link itself, not its target.
  if (unlink(location.c_str()) == 0) return DeleteStatus::Ok;
  error = std::strerror(errno);
  return statusOf(errno);
}
