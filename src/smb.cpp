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

void readDone(smb2_context* ctx, int status, void*, void* opaque) {
  auto& b = *static_cast<ReadAhead::Block*>(opaque);
  auto& s = static_cast<Stream&>(*b.owner);
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
  }
  s.cv.notify_all();
}

Entry entry(const std::string& name, const smb2_stat_64& st) {
  return {name, st.smb2_type == SMB2_TYPE_DIRECTORY, st.smb2_size,
          static_cast<int64_t>(st.smb2_mtime)};
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
    struct Opened {
      Result result;
      smb2_create_reply reply{};
      static void cb(smb2_context* ctx, int status, void* data, void* opaque) {
        auto& o = *static_cast<Opened*>(opaque);
        if (status == 0) o.reply = *static_cast<smb2_create_reply*>(data);
        Result::treeCb(ctx, status, nullptr, &o.result);
      }
    } opened;
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
    Result opened;
    int rc = wait(opened, smb2_open_async(ctx_, loc.path.c_str(), O_RDONLY, Result::cb, &opened));
    if (rc < 0) return statusOf(rc);
    h = {opened.data, serviceTree_, c.epoch, 0};
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
    const int rc = smb2_pread_async(ctx_, fh(s.handle), b.bytes.data() + b.filled,
                                    b.bytes.size() - b.filled, b.offset + b.filled, readDone, &b);
    if (rc < 0) {
      b.pending = false;
      s.broke(connectionLost(rc));
    }
  }

  void closeStream(Stream& s) override {
    smb2_select_tree_id(ctx_, s.handle.tree);
    const int rc = smb2_close_async(ctx_, fh(s.handle),
        [](smb2_context*, int, void*, void* p) { static_cast<Stream*>(p)->closed = true; }, &s);
    if (rc < 0) s.closed = true;
  }

  bool connected() const override { return ctx_; }

  void disconnect() override {
    if (ctx_) {
      std::lock_guard lock(contextList);
      smb2_destroy_context(ctx_);  // completes outstanding callbacks before freeing their data
    }
    ctx_ = nullptr;
    trees_.clear();
    serviceTree_ = 0;
  }

  int fd() const override { return ctx_ ? smb2_get_fd(ctx_) : -1; }
  short events() const override { return ctx_ ? static_cast<short>(smb2_which_events(ctx_)) : short(0); }

  bool service(short revents) override {
    // Directory/RPC callbacks can enqueue another request on their tree.
    if (ctx_ && serviceTree_) smb2_select_tree_id(ctx_, serviceTree_);
    return !ctx_ || smb2_service(ctx_, revents) >= 0;
  }

  void jobDone() override { serviceTree_ = 0; }

 private:
  static smb2fh* fh(const Handle& h) { return static_cast<smb2fh*>(h.file); }

  int wait(Result& r, int rc) {
    if (rc < 0) {
      if (connectionLost(rc)) c.destroy(true);
      return rc;
    }
    auto until = Clock::now() + std::chrono::seconds(5);
    if (c.deadline != Clock::time_point{}) until = std::min(until, c.deadline);
    while (rc >= 0 && !r.done && Clock::now() < until) {
      if (!c.pump()) { rc = -EIO; break; }
    }
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

  smb2_context* ctx_ = nullptr;
  uint32_t serviceTree_ = 0;
  std::map<std::string, uint32_t> trees_;
};

}  // namespace

std::unique_ptr<Backend> smbBackend(Connection& connection) { return std::make_unique<Smb>(connection); }

}  // namespace remote
