#include "remote_connection.h"

#include "file_util.h"
#include "run_log.h"

#include <smb2/smb2.h>
#include <smb2/smb2-errors.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/libsmb2-dcerpc-srvsvc.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>

// SMB2/3 shares through libsmb2 (remote.h): smb://host/share/path.
namespace remote {
namespace {

std::mutex contextList;  // libsmb2's process-wide context list

struct Location {
  std::string server, share, path;
  explicit Location(const std::string& locator) : server(host(locator)) {
    const size_t slash = locator.find('/', 6);
    if (slash == std::string::npos) return;
    const size_t end = locator.find('/', slash + 1);
    share = locator.substr(slash + 1, end - slash - 1);
    if (end != std::string::npos) path = locator.substr(end + 1);
    hideInRunLog(server, "<smb-host>");
    if (!share.empty()) hideInRunLog(share, "<smb-share>");
  }
};

Status statusOf(int rc) {
  if (rc >= 0) return Status::Ok;
  if (rc == -ENOENT || rc == -ENOTDIR) return Status::Missing;
  if (rc == -EACCES || rc == -EPERM) return Status::Denied;
  return Status::Offline;
}

bool connectionLost(int rc, uint32_t nt = 0) {
  if (rc == -ENETRESET)
    return nt != SMB2_STATUS_FILE_CLOSED && nt != SMB2_STATUS_CANCELLED;
  return rc == -ECONNRESET || rc == -ECONNABORTED || rc == -ENOTCONN ||
         rc == -EPIPE || rc == -ETIMEDOUT ||
         (rc < 0 && nt == SMB2_STATUS_USER_SESSION_DELETED);
}

struct Result {
  bool done = false;
  int status = 0;
  void* data = nullptr;
  uint32_t tree = 0;
  uint32_t nt = 0;
  static void cb(smb2_context* ctx, int status, void* data, void* opaque) {
    auto& r = *static_cast<Result*>(opaque);
    r.done = true;
    r.status = status;
    r.data = data;
    r.nt = status < 0 ? smb2_get_nterror(ctx) : 0;
    smb2_get_tree_id_for_pdu(ctx, nullptr, &r.tree);
  }
  static void treeCb(smb2_context* ctx, int status, void*, void* opaque) {
    cb(ctx, status ? -nterror_to_errno(status) : 0, nullptr, opaque);
    static_cast<Result*>(opaque)->nt = status;
  }
};

Entry entry(const std::string& name, const smb2_stat_64& st) {
  Entry e{name, st.smb2_type == SMB2_TYPE_DIRECTORY, st.smb2_size,
          static_cast<int64_t>(st.smb2_mtime)};
  if (e.folder)
    e.stamp = {static_cast<int64_t>(st.smb2_mtime), static_cast<int64_t>(st.smb2_mtime_nsec),
               static_cast<int64_t>(st.smb2_ctime), static_cast<int64_t>(st.smb2_ctime_nsec),
               st.smb2_ino};
  return e;
}

// All libsmb2 calls for a server. A call may wait for its one request;
// Connection::pump() keeps playback's two-block windows filled during that
// wait.
class Smb : public Backend {
 public:
  using Backend::Backend;

  Status signIn(Login&) override {
    c.destroy();
    return connect("IPC$");
  }

  Status list(const std::string& folder, std::vector<Entry>& entries) override {
    const Location loc(folder);
    Status status = connect(loc.share.empty() ? "IPC$" : loc.share);
    if (status != Status::Ok) return status;
    Result r;
    if (loc.share.empty()) {
      const int rc = wait(r, smb2_share_enum_async(ctx_, SHARE_INFO_1, Result::cb, &r));
      if (rc < 0) return statusOf(rc);
      auto* reply = static_cast<srvsvc_NetrShareEnum_rep*>(r.data);
      status = reply->status ? Status::Denied : Status::Ok;
      auto& info = reply->ses.ShareInfo.Level1;
      for (uint32_t i = 0; status == Status::Ok && i < info.EntriesRead; ++i) {
        auto& share = info.Buffer->share_info_1[i];
        const std::string name = share.netname.utf8;
        if ((share.type & 3) == SHARE_TYPE_DISKTREE && !name.empty() && name.back() != '$') {
          hideInRunLog(name, "<smb-share>");
          entries.push_back({name, true});
        }
      }
      smb2_free_data(ctx_, reply);
      return status;
    }
    const int rc = wait(r, smb2_opendir_async(ctx_, loc.path.c_str(), Result::cb, &r));
    if (rc < 0) return statusOf(rc);
    auto* dir = static_cast<smb2dir*>(r.data);
    while (auto* item = smb2_readdir(ctx_, dir)) {
      const std::string name = item->name;
      if (name != "." && name != "..") entries.push_back(entry(name, item->st));
    }
    smb2_closedir(ctx_, dir);
    return Status::Ok;
  }

  Status stat(const std::string& locator, Entry& out) override {
    const Location loc(locator);
    const Status status = connect(loc.share.empty() ? "IPC$" : loc.share);
    if (status != Status::Ok) return status;
    smb2_stat_64 st{};
    Result r;
    const int rc = wait(r, smb2_stat_async(ctx_, loc.path.c_str(), &st, Result::cb, &r));
    if (rc >= 0) out = entry(locator.substr(locator.rfind('/') + 1), st);
    return statusOf(rc);
  }

  void listAsync(const std::string& folder, Listed done) override {
    const Location loc(folder);
    if (loc.share.empty()) {
      Backend::listAsync(folder, std::move(done));
      return;
    }
    const Status status = connect(loc.share);
    if (status != Status::Ok) {
      done({status, {}});
      return;
    }
    auto* d = new Directory{*this, std::move(done), serviceTree_};
    smb2_create_request req{};
    req.impersonation_level = SMB2_IMPERSONATION_IMPERSONATION;
    req.desired_access = SMB2_FILE_LIST_DIRECTORY | SMB2_FILE_READ_ATTRIBUTES;
    req.file_attributes = SMB2_FILE_ATTRIBUTE_DIRECTORY;
    req.share_access = SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_WRITE | SMB2_FILE_SHARE_DELETE;
    req.create_disposition = SMB2_FILE_OPEN;
    req.create_options = SMB2_FILE_DIRECTORY_FILE;
    req.name = loc.path.c_str();
    auto* pdu = smb2_cmd_create_async(ctx_, &req, Directory::opened, d);
    if (!pdu) {
      auto callback = std::move(d->done);
      delete d;
      callback({Status::Offline, {}});
      return;
    }
    ++awaited_;
    lowWater(1);
    smb2_queue_pdu(ctx_, pdu);
  }

  Status remove(const std::string& locator, uint64_t size, int64_t modified) override {
    const Location loc(locator);
    Status status = connect(loc.share);
    if (status != Status::Ok) return status;
    // The file itself, never a folder, a link not followed; opened to be
    // deleted, letting others go on reading it meanwhile but not writing
    // (one open to write: a sharing violation, InUse).
    smb2_create_request req{};
    req.requested_oplock_level = SMB2_OPLOCK_LEVEL_NONE;
    req.impersonation_level = SMB2_IMPERSONATION_IMPERSONATION;
    req.desired_access = SMB2_DELETE | SMB2_FILE_READ_ATTRIBUTES;
    req.share_access = SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_DELETE;
    req.create_disposition = SMB2_FILE_OPEN;
    req.create_options = SMB2_FILE_NON_DIRECTORY_FILE | SMB2_FILE_OPEN_REPARSE_POINT;
    req.name = loc.path.c_str();
    Opened opened;
    const int rc = send(opened.result, smb2_cmd_create_async(ctx_, &req, Opened::cb, &opened));
    if (rc < 0) {
      switch (opened.result.nt) {
        case SMB2_STATUS_SHARING_VIOLATION: return Status::InUse;
        case SMB2_STATUS_DELETE_PENDING: return Status::Missing;
        case SMB2_STATUS_ACCESS_DENIED:
        case SMB2_STATUS_CANNOT_DELETE:
        case SMB2_STATUS_MEDIA_WRITE_PROTECTED: return Status::Denied;
        case SMB2_STATUS_FILE_IS_A_DIRECTORY: return Status::Changed;
        default: return statusOf(rc);
      }
    }
    // Windows' times count 100 ns steps from 1601.
    const smb2_create_reply& file = opened.reply;
    const bool same = file.end_of_file == size &&
                      static_cast<int64_t>(file.last_write_time / 10000000) - 11644473600 == modified &&
                      !(file.file_attributes & (SMB2_FILE_ATTRIBUTE_DIRECTORY |
                                                SMB2_FILE_ATTRIBUTE_REPARSE_POINT));
    status = Status::Changed;
    if (same && ctx_) {
      smb2_file_disposition_info gone{1};
      smb2_set_info_request mark{};
      mark.info_type = SMB2_0_INFO_FILE;
      mark.file_info_class = SMB2_FILE_DISPOSITION_INFORMATION;
      std::memcpy(mark.file_id, file.file_id, SMB2_FD_SIZE);
      mark.input_data = &gone;
      Result r;
      const int rc = send(r, smb2_cmd_set_info_async(ctx_, &mark, Result::treeCb, &r));
      status = rc < 0 && r.nt == SMB2_STATUS_CANNOT_DELETE ? Status::Denied : statusOf(rc);
    }
    // Deleted as it closes (or as the server drops a broken connection).
    if (ctx_) {
      smb2_close_request close{};
      std::memcpy(close.file_id, file.file_id, SMB2_FD_SIZE);
      Result r;
      send(r, smb2_cmd_close_async(ctx_, &close, Result::treeCb, &r));
    }
    return status;
  }

  Status open(const std::string& locator, Handle& h) override {
    const Location loc(locator);
    Status status = connect(loc.share);
    if (status != Status::Ok) return status;
    void* file = nullptr;
    int rc = 0;
    if (isPartialDownload(locator)) {
      rc = openGrowing(loc.path, file);
    } else {
      Result opened;
      rc = wait(opened, smb2_open_async(ctx_, loc.path.c_str(), O_RDONLY, Result::cb, &opened));
      file = opened.data;
    }
    if (rc < 0) return statusOf(rc);
    h = {file, serviceTree_, c.epoch, 0};
    smb2_stat_64 st{};
    Result r;
    rc = wait(r, smb2_fstat_async(ctx_, fh(h), &st, Result::cb, &r));
    if (rc < 0) {
      close(h);
      h.file = nullptr;
      return statusOf(rc);
    }
    h.length = st.smb2_size;
    return Status::Ok;
  }

  void close(const Handle& h) override {
    if (!ctx_ || h.epoch != c.epoch || !h.file) return;
    serviceTree_ = h.tree;
    smb2_select_tree_id(ctx_, h.tree);
    Result r;
    wait(r, smb2_close_async(ctx_, fh(h), Result::cb, &r));
  }

  int64_t readAt(const Handle& h, uint64_t offset, void* out, uint64_t count) override {
    serviceTree_ = h.tree;
    smb2_select_tree_id(ctx_, h.tree);
    const auto n = std::min<uint64_t>(count, smb2_get_max_read_size(ctx_));
    Result r;
    const int rc = wait(r, smb2_pread_async(ctx_, fh(h), static_cast<uint8_t*>(out), n, offset,
                                            Result::cb, &r));
    return rc > 0 ? rc : -1;
  }

  uint64_t blockSize() const override {
    return std::min<uint64_t>(ReadAhead::kBlock, smb2_get_max_read_size(ctx_));
  }

  void read(Stream& s, ReadAhead::Block& b) override {
    smb2_select_tree_id(ctx_, s.handle.tree);
    b.pending = true;
    const size_t count = b.bytes.size() - b.filled;
    const int rc = smb2_pread_async(ctx_, fh(s.handle), b.bytes.data() + b.filled, count,
                                    b.offset + b.filled, readDone, &b);
    if (rc < 0) {
      b.pending = false;
      s.broke(connectionLost(rc));
      return;
    }
    // The only reply awaited, as each block's is while a video plays: the
    // socket reports it once it is nearly all there, not as each part of it
    // arrives (dozens of wake-ups a block). Its last 64 KiB is not waited
    // for: a server may hold that tail back until the rest is acknowledged
    // (Nagle's algorithm), and it is small.
    const bool alone = ++awaited_ == 1 && wholeReplies_ && count > 4 * kReplyEnd;
    lowWater(alone ? static_cast<int>(count - kReplyEnd) : 1);  // count: a block at most
    if (alone) patientUntil_ = Clock::now() + kPatience;
  }

  void closeStream(Stream& s) override {
    smb2_select_tree_id(ctx_, s.handle.tree);
    const int rc = smb2_close_async(ctx_, fh(s.handle),
        [](smb2_context*, int, void*, void* p) { static_cast<Stream*>(p)->closed = true; }, &s);
    if (rc < 0) s.closed = true;
  }

  void refresh(const std::shared_ptr<Stream>& s) override {
    smb2_query_info_request req{};
    req.info_type = SMB2_0_INFO_FILE;
    req.file_info_class = SMB2_FILE_ALL_INFORMATION;  // its length, and its name now
    req.output_buffer_length = 0xffff;
    std::memcpy(req.file_id, *smb2_get_file_id(fh(s->handle)), SMB2_FD_SIZE);
    smb2_select_tree_id(ctx_, s->handle.tree);
    auto* asked = new Asked{s, *this};
    auto* pdu = smb2_cmd_query_info_async(ctx_, &req, Asked::answered, asked);
    if (!pdu) {
      delete asked;
      s->broke(false);
      return;
    }
    s->refreshing = true;
    ++awaited_;
    lowWater(1);
    smb2_queue_pdu(ctx_, pdu);
  }

  bool connected() const override { return ctx_; }

  void disconnect() override {
    disconnecting_ = true;
    if (ctx_) {
      std::lock_guard lock(contextList);
      smb2_destroy_context(ctx_);  // completes outstanding callbacks before freeing their data
    }
    ctx_ = nullptr;
    trees_.clear();
    serviceTree_ = 0;
    awaited_ = 0;
    lowWater_ = 1;
    disconnecting_ = false;
    listsBroken_ = false;
  }

  int fd() const override { return ctx_ ? smb2_get_fd(ctx_) : -1; }
  short events() const override { return ctx_ ? static_cast<short>(smb2_which_events(ctx_)) : short(0); }

  bool service(short revents) override {
    // Directory/RPC callbacks can enqueue another request on their tree.
    if (ctx_ && serviceTree_) smb2_select_tree_id(ctx_, serviceTree_);
    // What follows a block's first part is read as it arrives; so is a reply
    // that is late, or shorter than what was asked for (an error).
    if (lowWater_ > 1 && ((revents & POLLIN) || Clock::now() >= patientUntil_)) lowWater(1);
    return !ctx_ || (smb2_service(ctx_, revents) >= 0 && !listsBroken_);
  }

  void jobDone() override { serviceTree_ = 0; }

 private:
  // Raw directory requests select their own tree before each continuation.
  // Other jobs and playback may select another share between replies.
  struct Directory {
    Smb& smb;
    Listed done;
    uint32_t tree;
    smb2_file_id file{};
    Listing result{Status::Ok, {}};

    void finish(Status status) {
      result.status = status;
      if (status != Status::Ok) result.entries.clear();
      --smb.awaited_;
      auto callback = std::move(done);
      auto value = std::move(result);
      delete this;
      callback(std::move(value));
    }
    bool aborted(uint32_t status) {
      if (smb.disconnecting_) {
        finish(Status::Offline);
        return true;
      }
      if (connectionLost(-nterror_to_errno(status), status)) {
        smb.listsBroken_ = true;
        finish(Status::Offline);
        return true;
      }
      return false;
    }
    void query() {
      uint32_t previous = 0;
      smb2_get_tree_id_for_pdu(smb.ctx_, nullptr, &previous);
      smb2_select_tree_id(smb.ctx_, tree);
      smb2_query_directory_request req{};
      req.file_information_class = SMB2_FILE_ID_FULL_DIRECTORY_INFORMATION;
      std::memcpy(req.file_id, file, SMB2_FD_SIZE);
      req.output_buffer_length = 64 * 1024;
      req.name = "*";
      auto* pdu = smb2_cmd_query_directory_async(smb.ctx_, &req, queried, this);
      smb2_select_tree_id(smb.ctx_, previous);
      if (pdu) smb2_queue_pdu(smb.ctx_, pdu);
      else close(Status::Offline);
    }
    void close(Status status) {
      result.status = status;
      uint32_t previous = 0;
      smb2_get_tree_id_for_pdu(smb.ctx_, nullptr, &previous);
      smb2_select_tree_id(smb.ctx_, tree);
      smb2_close_request req{};
      std::memcpy(req.file_id, file, SMB2_FD_SIZE);
      auto* pdu = smb2_cmd_close_async(smb.ctx_, &req, closed, this);
      smb2_select_tree_id(smb.ctx_, previous);
      if (pdu) smb2_queue_pdu(smb.ctx_, pdu);
      else {
        smb.listsBroken_ = true;  // release the server handle on disconnect
        finish(Status::Offline);
      }
    }
    static void opened(smb2_context*, int status, void* data, void* opaque) {
      auto* d = static_cast<Directory*>(opaque);
      if (d->aborted(status)) return;
      if (status != SMB2_STATUS_SUCCESS) {
        d->finish(statusOf(-nterror_to_errno(status)));
        return;
      }
      std::memcpy(d->file, static_cast<smb2_create_reply*>(data)->file_id, SMB2_FD_SIZE);
      d->query();
    }
    static void queried(smb2_context* ctx, int status, void* data, void* opaque) {
      auto* d = static_cast<Directory*>(opaque);
      if (d->aborted(status)) return;
      if (static_cast<uint32_t>(status) == SMB2_STATUS_NO_MORE_FILES) {
        d->close(Status::Ok);
        return;
      }
      if (status != SMB2_STATUS_SUCCESS) {
        // Only opening a nonexistent directory authorizes deleting cached paths.
        d->close(Status::Offline);
        return;
      }
      const auto& reply = *static_cast<smb2_query_directory_reply*>(data);
      uint32_t offset = 0;
      while (offset < reply.output_buffer_length) {
        if (reply.output_buffer_length - offset < 80) {
          d->close(Status::Offline);
          return;
        }
        smb2_iovec v{reply.output_buffer + offset, reply.output_buffer_length - offset, nullptr};
        smb2_fileidfulldirectoryinformation f{};
        if (smb2_decode_fileidfulldirectoryinformation(ctx, &f, &v) < 0 || !f.name) {
          d->close(Status::Offline);
          return;
        }
        const std::string name = f.name ? f.name : "";
        free(const_cast<char*>(f.name));
        if (name != "." && name != "..") {
          Entry e{name, bool(f.file_attributes & SMB2_FILE_ATTRIBUTE_DIRECTORY),
                  f.end_of_file, f.last_write_time.tv_sec};
          e.link = f.file_attributes & SMB2_FILE_ATTRIBUTE_REPARSE_POINT;
          if (e.folder)
            e.stamp = {f.last_write_time.tv_sec, f.last_write_time.tv_usec * 1000,
                       f.change_time.tv_sec, f.change_time.tv_usec * 1000, f.file_id};
          d->result.entries.push_back(std::move(e));
        }
        if (!f.next_entry_offset) break;
        if (f.next_entry_offset < 80 || f.next_entry_offset > reply.output_buffer_length - offset) {
          d->close(Status::Offline);
          return;
        }
        offset += f.next_entry_offset;
      }
      if (!reply.output_buffer_length) d->close(Status::Offline);
      else d->query();
    }
    static void closed(smb2_context*, int status, void*, void* opaque) {
      auto* d = static_cast<Directory*>(opaque);
      if (d->aborted(status)) return;
      d->finish(status == SMB2_STATUS_SUCCESS ? d->result.status : Status::Offline);
    }
  };

  struct Opened {
    Result result;
    smb2_create_reply reply{};
    static void cb(smb2_context* ctx, int status, void* data, void* opaque) {
      auto& o = *static_cast<Opened*>(opaque);
      if (status == 0) o.reply = *static_cast<smb2_create_reply*>(data);
      Result::treeCb(ctx, status, nullptr, &o.result);
    }
  };

  // A partial download, opened to read as O_RDONLY does, but letting its
  // browser go on writing it and rename it once it finishes (O_RDONLY's
  // open refuses that rename). Every open of one comes here: playback's,
  // and any other reader's.
  int openGrowing(const std::string& path, void*& file) {
    smb2_create_request req{};
    req.impersonation_level = SMB2_IMPERSONATION_IMPERSONATION;
    req.desired_access = SMB2_FILE_READ_DATA | SMB2_FILE_READ_EA | SMB2_FILE_READ_ATTRIBUTES;
    req.share_access = SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_WRITE | SMB2_FILE_SHARE_DELETE;
    req.create_disposition = SMB2_FILE_OPEN;
    req.create_options = SMB2_FILE_NON_DIRECTORY_FILE;
    req.name = path.c_str();
    Opened opened;
    const int rc = send(opened.result, smb2_cmd_create_async(ctx_, &req, Opened::cb, &opened));
    if (rc < 0) return rc;
    file = smb2_fh_from_file_id(ctx_, &opened.reply.file_id);
    return file ? 0 : -ENOMEM;
  }

  // A growing stream's length asked (refresh()); its stream kept until the
  // answer, which comes also when the connection ends (SHUTDOWN).
  struct Asked {
    std::shared_ptr<Stream> stream;
    Smb& smb;
    static void answered(smb2_context* ctx, int status, void* data, void* opaque) {
      const std::unique_ptr<Asked> asked(static_cast<Asked*>(opaque));
      Stream& s = *asked->stream;
      --asked->smb.awaited_;
      const auto* reply = static_cast<smb2_query_info_reply*>(data);
      auto* info = status == SMB2_STATUS_SUCCESS && reply
                       ? static_cast<smb2_file_all_info*>(reply->output_buffer) : nullptr;
      std::lock_guard lock(s.mutex);
      s.refreshing = false;
      // Closed already when the connection ended (a growing stream isn't
      // reopened): nothing more to read.
      if (!info && !s.closed) s.broke(connectionLost(-nterror_to_errno(status), status));
      if (info && !s.closed) {
        s.handle.length = std::max<uint64_t>(s.handle.length, info->standard.end_of_file);
        // Its browser renamed it: finished, this is all of it. A share
        // that doesn't say ends it by the wait for more instead.
        if (info->name) {
          std::string name = reinterpret_cast<const char*>(info->name);
          std::replace(name.begin(), name.end(), '\\', '/');
          s.complete = !isPartialDownload(name);
        }
      }
      if (info) smb2_free_data(ctx, info);
      s.cv.notify_all();
    }
  };

  static smb2fh* fh(const Handle& h) { return static_cast<smb2fh*>(h.file); }

  static void readDone(smb2_context* ctx, int status, void*, void* opaque) {
    auto& b = *static_cast<ReadAhead::Block*>(opaque);
    auto& s = static_cast<Stream&>(*b.owner);
    auto& smb = *static_cast<Smb*>(smb2_get_opaque(ctx));
    --smb.awaited_;
    std::lock_guard lock(s.mutex);
    b.pending = false;
    if (s.reconnecting || s.closed) return;
    if (b.generation != s.generation) {
      b.done = true;
    } else if (status <= 0) {
      s.broke(connectionLost(status, smb2_get_nterror(ctx)));
      return;
    } else {
      s.retryUntil = {};
      b.filled += status;
      b.done = b.filled == b.bytes.size();
      // A server that answers with less than a block is never waited on for
      // a whole one again.
      if (!b.done) smb.wholeReplies_ = false;
    }
    s.cv.notify_all();
  }

  // Has the socket report what arrived only from `bytes` on (1: any).
  void lowWater(int bytes) {
    if (bytes == lowWater_) return;
    lowWater_ = bytes;
    setsockopt(smb2_get_fd(ctx_), SOL_SOCKET, SO_RCVLOWAT, &bytes, sizeof(bytes));
  }

  int wait(Result& r, int rc) {
    if (rc < 0) {
      if (connectionLost(rc)) c.destroy(true);
      return rc;
    }
    auto until = Clock::now() + std::chrono::seconds(5);
    if (c.deadline != Clock::time_point{}) until = std::min(until, c.deadline);
    ++awaited_;
    if (ctx_) lowWater(1);
    while (rc >= 0 && !r.done && Clock::now() < until) {
      if (!c.pump()) { rc = -EIO; break; }
    }
    --awaited_;
    if (rc < 0 || !r.done) {
      c.destroy(true);
      return rc < 0 ? rc : -ETIMEDOUT;
    }
    if (connectionLost(r.status, r.nt)) c.destroy(true);
    return r.status;
  }

  // One request of the raw API on the share connect() chose; its NT status
  // in r.nt.
  int send(Result& r, smb2_pdu* pdu) {
    if (!pdu) return -ENOMEM;
    smb2_queue_pdu(ctx_, pdu);
    return wait(r, 0);
  }

  Status connect(const std::string& share) {
    Login login;
    bool remembered;
    if (!knownLogin(c.root, login, remembered)) return Status::NeedsLogin;
    const std::string server = host(c.root);
    if (!ctx_) {
      { std::lock_guard lock(contextList); ctx_ = smb2_init_context(); }
      if (!ctx_) return Status::Offline;
      std::string user = login.user;
      const auto slash = user.find('\\');
      if (slash != std::string::npos) {
        smb2_set_domain(ctx_, user.substr(0, slash).c_str());
        user.erase(0, slash + 1);
      }
      smb2_set_user(ctx_, user.c_str());
      smb2_set_password(ctx_, login.password.c_str());
      smb2_set_security_mode(ctx_, SMB2_NEGOTIATE_SIGNING_ENABLED);
      smb2_set_timeout(ctx_, 5);
      smb2_set_opaque(ctx_, this);
      wholeReplies_ = true;
      // Authenticate once; a refused share below is Denied, not NeedsLogin.
      Result r;
      const int rc = wait(r, smb2_connect_share_async(ctx_, server.c_str(), "IPC$",
                                                      user.c_str(), Result::cb, &r));
      if (rc < 0) {
        c.destroy(true);
        switch (r.nt) {
          case SMB2_STATUS_LOGON_FAILURE:
          case SMB2_STATUS_ACCESS_DENIED:
          case SMB2_STATUS_WRONG_PASSWORD:
          case SMB2_STATUS_ACCOUNT_DISABLED:
          case SMB2_STATUS_ACCOUNT_RESTRICTION:
          case SMB2_STATUS_ACCOUNT_LOCKED_OUT:
          case SMB2_STATUS_PASSWORD_EXPIRED:
          case SMB2_STATUS_PASSWORD_MUST_CHANGE: return Status::NeedsLogin;
          default: return statusOf(rc);
        }
      }
      trees_["IPC$"] = r.tree;
    }
    if (!trees_.count(share)) {
      const std::string unc = "\\\\" + server + "\\" + share;
      auto* utf16 = smb2_utf8_to_utf16(unc.c_str());
      if (!utf16) return Status::Missing;
      smb2_tree_connect_request req{};
      req.path_length = utf16->len * 2;
      req.path = utf16->val;
      Result r;
      auto* pdu = smb2_cmd_tree_connect_async(ctx_, &req, Result::treeCb, &r);
      free(utf16);
      if (!pdu) return Status::Offline;
      smb2_queue_pdu(ctx_, pdu);
      const int rc = wait(r, 0);
      if (rc < 0) return statusOf(rc);
      trees_[share] = r.tree;
    }
    serviceTree_ = trees_[share];
    smb2_select_tree_id(ctx_, serviceTree_);
    return Status::Ok;
  }

  // A block's reply waited for whole (read()): all but its end, and for so
  // long.
  static constexpr int kReplyEnd = 64 * 1024;
  static constexpr std::chrono::milliseconds kPatience{200};

  smb2_context* ctx_ = nullptr;
  uint32_t serviceTree_ = 0;
  std::map<std::string, uint32_t> trees_;
  int awaited_ = 0;  // replies awaited: blocks' and a job's
  int lowWater_ = 1;  // the socket's (lowWater())
  bool wholeReplies_ = true;
  bool disconnecting_ = false, listsBroken_ = false;
  Clock::time_point patientUntil_{};
};

}  // namespace

std::unique_ptr<Backend> smbBackend(Connection& connection) { return std::make_unique<Smb>(connection); }

}  // namespace remote
