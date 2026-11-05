#include "run_log.h"

#include "platform.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <execinfo.h>
#include <fcntl.h>
#include <link.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <poll.h>
#include <sys/utsname.h>
#include <pwd.h>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

volatile sig_atomic_t crashFd = -1;

// What the logs leave out (docs/alpha/diagnostics.md): the home folder,
// written as ~, and the user's name where it is a folder in a path. Set
// before the copying starts, then only read (also by the crash handler).
char home[512];
size_t homeSize = 0;
char user[128];
size_t userSize = 0;

bool nameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '-' || c == '.';
}

bool startsAt(const char* in, size_t n, size_t at, const char* what, size_t size) {
  if (!size || at + size > n) return false;
  for (size_t i = 0; i < size; ++i)
    if (in[at + i] != what[i]) return false;
  return at + size == n || !nameChar(in[at + size]);
}

// Copies in to out (at least 4 * n + 8 bytes) without the home folder or
// the user's name; returns the size written. No allocation: the crash
// handler uses it.
size_t redact(const char* in, size_t n, char* out) {
  size_t o = 0;
  for (size_t i = 0; i < n;) {
    // The home folder only where a path starts ("/var/home/x" is not "~").
    if ((i == 0 || !nameChar(in[i - 1])) && startsAt(in, n, i, home, homeSize)) {
      out[o++] = '~';
      i += homeSize;
    } else if (in[i] == '/' && startsAt(in, n, i + 1, user, userSize)) {
      for (const char* p = "/<user>"; *p; ++p) out[o++] = *p;
      i += 1 + userSize;
    } else {
      out[o++] = in[i++];
    }
  }
  return o;
}

// An os-release file's NAME (or PRETTY_NAME) with its version and build.
std::string osRelease(const char* path) {
  std::ifstream in(path);
  std::string line, pretty, name, version, build;
  while (std::getline(in, line)) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string value = line.substr(eq + 1);
    if (value.size() >= 2 && (value[0] == '"' || value[0] == '\''))
      value = value.substr(1, value.size() - 2);
    const std::string key = line.substr(0, eq);
    if (key == "PRETTY_NAME") pretty = value;
    else if (key == "NAME") name = value;
    else if (key == "VERSION_ID") version = value;
    else if (key == "BUILD_ID") build = value;
  }
  std::string text = !pretty.empty() ? pretty : name;
  if (text.empty()) return text;
  if (!version.empty() && text.find(version) == std::string::npos) text += " " + version;
  if (!build.empty()) text += " (build " + build + ")";
  return text;
}

// The program's GNU build ID in hex ("" without one): it names the exact
// binary, whose kept debug file turns the crash's offsets into functions.
char buildId[2 * 64 + 1];

void takeBuildId() {
  dl_iterate_phdr(
      [](dl_phdr_info* info, size_t, void*) {
        for (int i = 0; i < info->dlpi_phnum; ++i) {
          const ElfW(Phdr)& ph = info->dlpi_phdr[i];
          if (ph.p_type != PT_NOTE) continue;
          const char* p = reinterpret_cast<const char*>(info->dlpi_addr + ph.p_vaddr);
          const char* end = p + ph.p_memsz;
          while (p + sizeof(ElfW(Nhdr)) <= end) {
            const auto* note = reinterpret_cast<const ElfW(Nhdr)*>(p);
            const char* name = p + sizeof(ElfW(Nhdr));
            const char* desc = name + ((note->n_namesz + 3) & ~3u);
            if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 &&
                std::memcmp(name, "GNU", 4) == 0 && note->n_descsz <= 64) {
              for (size_t k = 0; k < note->n_descsz; ++k)
                std::snprintf(buildId + 2 * k, 3, "%02x", static_cast<unsigned char>(desc[k]));
              return 1;
            }
            p = desc + ((note->n_descsz + 3) & ~3u);
          }
        }
        return 1;  // the first object is the program itself
      },
      nullptr);
}

// The newest log before this run's ended in a crash (its "Crash:" block).
bool crashedBefore = false;

bool endedInCrash(const std::filesystem::path& log) {
  std::ifstream in(log, std::ios::binary);
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  const std::streamoff from = std::max<std::streamoff>(0, size - 65536);
  in.seekg(from);
  std::string tail(static_cast<size_t>(size - from), '\0');
  in.read(tail.data(), static_cast<std::streamsize>(tail.size()));
  return tail.find("\nCrash: SIG") != std::string::npos;
}

// hideInRunLog's words: the copying thread reads them under the lock.
std::mutex hiddenLock;
std::vector<std::pair<std::string, std::string>> hidden;

bool wordChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '-';
}

void hideWords(std::string& line) {
  std::lock_guard<std::mutex> lock(hiddenLock);
  for (const auto& [word, shownAs] : hidden)
    for (size_t at = 0; (at = line.find(word, at)) != std::string::npos;) {
      const size_t end = at + word.size();
      if ((at > 0 && wordChar(line[at - 1])) || (end < line.size() && wordChar(line[end]))) {
        at = end;
        continue;
      }
      line.replace(at, word.size(), shownAs);
      at += shownAs.size();
    }
}

void takeNames() {
  static bool taken = false;
  if (taken) return;
  taken = true;
  if (const char* h = std::getenv("HOME")) {
    homeSize = std::strlen(h);
    while (homeSize > 1 && h[homeSize - 1] == '/') --homeSize;
    if (homeSize <= 1 || homeSize >= sizeof(home)) homeSize = 0;  // "/" or too long: kept
    std::memcpy(home, h, homeSize);
  }
  passwd entry{}, *found = nullptr;
  char buffer[4096];
  const char* name = getpwuid_r(getuid(), &entry, buffer, sizeof(buffer), &found) == 0 && found
                         ? entry.pw_name
                         : std::getenv("USER");
  if (name && *name && std::strlen(name) < sizeof(user) && std::strcmp(name, "root") != 0) {
    userSize = std::strlen(name);
    std::memcpy(user, name, userSize);
  }
}

void writeAll(int fd, const char* data, size_t size) {
  while (size) {
    const ssize_t n = write(fd, data, size);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    data += n;
    size -= n;
  }
}

void crash(int sig) {
  const char* name = "\nCrash: SIGSEGV\n";
  switch (sig) {
    case SIGABRT: name = "\nCrash: SIGABRT\n"; break;
    case SIGBUS: name = "\nCrash: SIGBUS\n"; break;
    case SIGILL: name = "\nCrash: SIGILL\n"; break;
    case SIGFPE: name = "\nCrash: SIGFPE\n"; break;
  }
  size_t size = 0;
  while (name[size]) ++size;
  writeAll(crashFd, name, size);
  if (buildId[0]) {
    size = 0;
    while (buildId[size]) ++size;
    writeAll(crashFd, "Build ID: ", 10);
    writeAll(crashFd, buildId, size);
    writeAll(crashFd, "\n", 1);
  }
  void* frames[32];
  const int count = backtrace(frames, 32);
  // The frames name the program's file, under the home folder when
  // installed there: through a pipe, to leave it out.
  static char raw[32768], clean[4 * sizeof(raw) + 8];
  int through[2];
  if (pipe(through) == 0) {
    backtrace_symbols_fd(frames, count, through[1]);
    close(through[1]);
    size_t got = 0;
    for (ssize_t n; got < sizeof(raw) && (n = read(through[0], raw + got, sizeof(raw) - got)) != 0;)
      if (n > 0) got += n;
      else if (errno != EINTR) break;
    close(through[0]);
    writeAll(crashFd, clean, redact(raw, got, clean));
  } else {
    backtrace_symbols_fd(frames, count, crashFd);
  }
  // SA_RESETHAND restored the default; delivery after this handler dumps core.
  raise(sig);
}

class RunLog {
 public:
  RunLog() {
    if (!open()) std::fprintf(stderr, "Could not start run log\n");
  }

  ~RunLog() { end(); }

  void end() {
    if (ended_.exchange(true)) return;
    std::fflush(stdout);
    std::fflush(stderr);
    for (int i = 0; i < 2; ++i) {
      if (saved_[i] >= 0) dup2(saved_[i], i + 1);
      if (pipes_[i][1] >= 0) close(pipes_[i][1]);
    }
    if (thread_.joinable()) thread_.join();
    for (int i = 0; i < 2; ++i) {
      if (saved_[i] >= 0) close(saved_[i]);
      if (pipes_[i][0] >= 0) close(pipes_[i][0]);
    }
    crashFd = -1;
    if (file_ >= 0) close(file_);
  }

 private:
  bool open() {
    namespace fs = std::filesystem;
    takeNames();
    takeBuildId();
    const fs::path dir = runLogDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return false;
    fs::path path;
    do {
      const auto now = std::chrono::system_clock::now();
      const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                              now.time_since_epoch()).count() % 1000000;
      const std::time_t time = std::chrono::system_clock::to_time_t(now);
      std::tm local{};
      localtime_r(&time, &local);
      char stamp[32], name[64];
      std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &local);
      std::snprintf(name, sizeof(name), "%s-%06ld.log", stamp, static_cast<long>(micros));
      path = dir / name;
      file_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0600);
    } while (file_ < 0 && errno == EEXIST);
    if (file_ < 0) return false;

    std::vector<fs::path> logs;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
      if (it->path() != path && it->path().extension() == ".log") logs.push_back(it->path());
    std::sort(logs.begin(), logs.end());
    crashedBefore = !logs.empty() && endedInCrash(logs.back());
    for (size_t i = 0; i + 9 < logs.size(); ++i) fs::remove(logs[i], ec);

    for (int i = 0; i < 2; ++i) {
      saved_[i] = fcntl(i + 1, F_DUPFD_CLOEXEC, 3);
      if (saved_[i] < 0 || pipe2(pipes_[i], O_CLOEXEC) != 0) return false;
      // Absorb bursts without involving the render thread in file I/O.
      fcntl(pipes_[i][1], F_SETPIPE_SZ, 1024 * 1024);
    }
    try {
      thread_ = std::thread([this] { copy(); });
    } catch (const std::system_error&) {
      return false;
    }
    for (int i = 0; i < 2; ++i) {
      if (dup2(pipes_[i][1], i + 1) < 0) return false;
      close(pipes_[i][1]);
      pipes_[i][1] = -1;
    }

    // Load the unwinder before a signal: its first use can allocate/dlopen.
    void* frames[1];
    const int count = backtrace(frames, 1);
    const int sink = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (sink >= 0) {
      backtrace_symbols_fd(frames, count, sink);
      close(sink);
    }
    crashFd = file_;
    struct sigaction action{};
    action.sa_handler = crash;
    sigfillset(&action.sa_mask);
    action.sa_flags = SA_RESETHAND;
    for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE}) sigaction(sig, &action, nullptr);
    std::printf("Log: %s\n", path.c_str());
    return true;
  }

  // Whole lines, without the home folder or the user's name, to the log and
  // the console; a line longer than the buffer goes in parts.
  void write(int stream, std::string& pending, bool all) {
    size_t start = 0;
    for (size_t end; (end = pending.find('\n', start)) != std::string::npos; start = end + 1)
      writeLine(stream, pending.data() + start, end + 1 - start);
    if (all || pending.size() - start > 65536) {
      writeLine(stream, pending.data() + start, pending.size() - start);
      start = pending.size();
    }
    pending.erase(0, start);
  }

  void writeLine(int stream, const char* line, size_t size) {
    if (!size) return;
    clean_.resize(4 * size + 8);
    clean_.resize(redact(line, size, clean_.data()));
    hideWords(clean_);
    // Kernel-buffered batches, no fsync or disk writes on the frame loop.
    writeAll(file_, clean_.data(), clean_.size());
    writeAll(saved_[stream], clean_.data(), clean_.size());
  }

  void copy() {
    pollfd fds[2] = {{pipes_[0][0], POLLIN, 0}, {pipes_[1][0], POLLIN, 0}};
    char buffer[16384];
    std::string pending[2];
    int active = 2;
    while (active) {
      if (poll(fds, 2, -1) < 0) {
        if (errno == EINTR) continue;
        break;
      }
      for (int i = 0; i < 2; ++i) {
        if (!fds[i].revents) continue;
        const ssize_t n = read(fds[i].fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
          write(i, pending[i], true);
          fds[i].fd = -1;
          --active;
          continue;
        }
        pending[i].append(buffer, n);
        write(i, pending[i], false);
      }
    }
  }

  std::string clean_;
  int file_ = -1;
  int saved_[2] = {-1, -1};
  int pipes_[2][2] = {{-1, -1}, {-1, -1}};
  std::thread thread_;
  std::atomic<bool> ended_{false};
};

RunLog* runLog = nullptr;

}  // namespace

void startRunLog() {
  static RunLog log;
  runLog = &log;
}

void endRunLog() {
  if (runLog) runLog->end();
}

bool previousRunCrashed() { return crashedBefore; }

std::filesystem::path runLogDir() {
  const char* home = std::getenv("HOME");
  std::filesystem::path base = std::filesystem::path(home ? home : ".") / ".local/state";
  if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg == '/') base = xdg;
  return base / kAppName / "logs";
}

std::string anonymised(const std::string& text) {
  takeNames();
  std::string clean(4 * text.size() + 8, '\0');
  clean.resize(redact(text.data(), text.size(), clean.data()));
  // Words are hidden line by line, as the log does.
  std::string out;
  out.reserve(clean.size());
  for (size_t start = 0; start < clean.size();) {
    size_t end = clean.find('\n', start);
    end = end == std::string::npos ? clean.size() : end + 1;
    std::string line = clean.substr(start, end - start);
    hideWords(line);
    out += line;
    start = end;
  }
  return out;
}

void printRunHeader() {
  const char* arch =
#if defined(__aarch64__)
      "aarch64";
#elif defined(__x86_64__)
      "x86_64";
#else
      "other";
#endif
  const char* commit = VIDEOFRAMEVR_COMMIT;
  std::printf("%s %s%s%s, %s build, %s\n", kAppTitle, VIDEOFRAMEVR_VERSION, *commit ? " " : "", commit,
              kStandaloneHeadset ? "Steam Frame" : "desktop", arch);
  // Inside the Steam Linux Runtime the host's file is under /run/host.
  const std::string host = osRelease("/run/host/os-release");
  const std::string own = osRelease("/etc/os-release");
  utsname u{};
  uname(&u);
  if (buildId[0]) std::printf("Build ID: %s\n", buildId);
  std::printf("System: %s, kernel %s%s%s\n", host.empty() ? own.c_str() : host.c_str(), u.release,
              host.empty() ? "" : "; runtime ", host.empty() ? "" : own.c_str());
}

void hideInRunLog(const std::string& word, const std::string& shownAs) {
  if (word.empty()) return;
  std::lock_guard<std::mutex> lock(hiddenLock);
  for (const auto& h : hidden)
    if (h.first == word) return;
  hidden.emplace_back(word, shownAs);
}

std::string urlHost(const std::string& url) {
  size_t start = url.find("://");
  start = start == std::string::npos ? 0 : start + 3;
  std::string host = url.substr(start, url.find_first_of("/?#", start) - start);
  if (const size_t at = host.rfind('@'); at != std::string::npos) host.erase(0, at + 1);
  if (!host.empty() && host[0] == '[') return host.substr(1, host.find(']') - 1);  // IPv6
  return host.substr(0, host.find(':'));
}
