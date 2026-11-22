#include "decoder_holders.h"

#include <signal.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <thread>

namespace decoder_holders {
namespace {

namespace fs = std::filesystem;

std::string firstLine(const fs::path& path) {
  std::ifstream in(path);
  std::string line;
  std::getline(in, line);
  return line;
}

// Calls `each` for the entries of `dir`, never throwing (processes come and
// go while /proc is read): whether all were read.
template <class Each>
bool eachEntry(const fs::path& dir, Each each) {
  std::error_code error;
  for (fs::directory_iterator it(dir, error), end; !error && it != end; it.increment(error))
    if (!each(*it)) return true;
  return !error;
}

// The decoder's device number, from its video4linux name ("qcom-iris-decoder").
bool decoderDevice(dev_t& device) {
  bool found = false;
  eachEntry("/sys/class/video4linux", [&](const fs::directory_entry& node) {
    if (firstLine(node.path() / "name") != "qcom-iris-decoder") return true;
    unsigned major = 0, minor = 0;
    if (std::sscanf(firstLine(node.path() / "dev").c_str(), "%u:%u", &major, &minor) != 2) return true;
    device = makedev(major, minor);
    found = true;
    return false;
  });
  return found;
}

enum class Holds { No, Yes, Unknown };

// Whether the process has the decoder open. Inside the Steam Runtime's
// container, where the app runs on the Frame, the files other processes
// have open are listed but not where they lead: Unknown.
Holds holds(const fs::path& process, dev_t device) {
  Holds holds = Holds::No;
  const bool all = eachEntry(process / "fd", [&](const fs::directory_entry& fd) {
    char target[64];
    const ssize_t n = readlink(fd.path().c_str(), target, sizeof target - 1);
    if (n <= 0) {
      if (errno == EACCES || errno == EPERM) holds = Holds::Unknown;
      return true;
    }
    if (std::string_view(target, n).rfind("/dev/video", 0) != 0) return true;
    struct stat st;
    if (stat(fd.path().c_str(), &st) != 0 || !S_ISCHR(st.st_mode) || st.st_rdev != device) return true;
    holds = Holds::Yes;
    return false;
  });
  return holds == Holds::Yes || all ? holds : Holds::Unknown;
}

// Chromium (Steam's web helper) decodes on V4L2 devices with a thread of
// this name ("V4L2DevicePoller", cut to 15 characters) per session; thread
// names can be read where open files can't.
bool decodesInChromium(const fs::path& process) {
  bool decodes = false;
  eachEntry(process / "task", [&](const fs::directory_entry& task) {
    decodes = firstLine(task.path() / "comm").rfind("V4L2DevicePoll", 0) == 0;
    return !decodes;
  });
  return decodes;
}

}  // namespace

std::vector<Holder> find() {
  std::vector<Holder> holders;
  dev_t device = 0;
  if (!decoderDevice(device)) return holders;
  const pid_t self = getpid();
  const uid_t user = getuid();
  eachEntry("/proc", [&](const fs::directory_entry& entry) {
    const std::string name = entry.path().filename().string();
    if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) return true;
    const pid_t pid = static_cast<pid_t>(std::stol(name));
    struct stat st;
    if (pid == self || stat(entry.path().c_str(), &st) != 0 || st.st_uid != user) return true;
    Holder h;
    h.pid = pid;
    h.name = firstLine(entry.path() / "comm");
    h.steam = h.name == "steamwebhelper";
    const Holds open = holds(entry.path(), device);
    if (open == Holds::Yes || (open == Holds::Unknown && h.steam && decodesInChromium(entry.path())))
      holders.push_back(h);
    return true;
  });
  return holders;
}

bool freeFromSteam() {
  using Clock = std::chrono::steady_clock;
  // Only those asked to end are waited for and forced: Steam starts a new
  // helper at once, which is left alone.
  std::vector<pid_t> asked;
  for (const Holder& h : find()) {
    if (!h.steam) continue;
    std::printf("Decoder: ending Steam's %s (%d), which holds the video decoder\n", h.name.c_str(),
                static_cast<int>(h.pid));
    kill(h.pid, SIGTERM);
    asked.push_back(h.pid);
  }
  auto stillThere = [&] {
    std::vector<pid_t> left;
    for (const Holder& h : find())
      if (std::find(asked.begin(), asked.end(), h.pid) != asked.end()) left.push_back(h.pid);
    return left;
  };
  const Clock::time_point start = Clock::now();
  std::vector<pid_t> left = asked;
  bool forced = false;
  while (!left.empty()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    left = stillThere();
    const double waited = std::chrono::duration<double>(Clock::now() - start).count();
    if (!forced && waited > 2) {
      for (pid_t pid : left) kill(pid, SIGKILL);
      forced = true;
    } else if (waited > 3) {
      break;
    }
  }
  std::printf("Decoder: %s after %.0f ms\n", left.empty() ? "free of Steam" : "still held by Steam",
              std::chrono::duration<double, std::milli>(Clock::now() - start).count());
  return left.empty();
}

}  // namespace decoder_holders
