#include "diagnostics.h"

#include "media_server.h"
#include "platform.h"
#include "run_log.h"
#include "app_dirs.h"
#include "settings.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

// The Downloads folder: XDG's user-dirs.dirs, else ~/Downloads.
fs::path downloadDir() {
  const char* homeEnv = std::getenv("HOME");
  const std::string home = homeEnv ? homeEnv : ".";
  std::ifstream dirs(configDir().parent_path() / "user-dirs.dirs");
  for (std::string line; std::getline(dirs, line);) {
    if (line.rfind("XDG_DOWNLOAD_DIR=", 0) != 0) continue;
    std::string value = line.substr(17);
    if (value.size() >= 2 && value.front() == '"') value = value.substr(1, value.size() - 2);
    if (value.rfind("$HOME", 0) == 0) value = home + value.substr(5);
    if (!value.empty() && value[0] == '/' && value != home) return value;
  }
  return fs::path(home) / "Downloads";
}

// settings.conf without what could identify someone: folders, addresses and
// keys.
std::string anonymisedSettings(const std::string& text) {
  std::istringstream in(text);
  std::string out;
  for (std::string line; std::getline(in, line);) {
    const size_t eq = line.find('=');
    if (eq != std::string::npos && eq + 1 < line.size()) {
      const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
      bool secret = value.find('/') != std::string::npos;
      for (const char* word : {"key", "url", "path", "folder", "password", "user", "host", "share", "address"})
        secret = secret || key.find(word) != std::string::npos;
      if (secret) line = key + "=<removed>";
    }
    out += line + "\n";
  }
  return anonymised(out);
}

// A zip of stored (uncompressed) files: no compression library needed, and
// every system opens it.
class Zip {
 public:
  void add(const std::string& name, const std::string& data, std::time_t when) {
    std::tm t{};
    localtime_r(&when, &t);
    Entry e;
    e.name = name;
    e.crc = crc32(data);
    e.size = static_cast<uint32_t>(data.size());
    e.time = static_cast<uint16_t>(t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec / 2);
    e.date = static_cast<uint16_t>(std::max(0, t.tm_year - 80) << 9 | (t.tm_mon + 1) << 5 | t.tm_mday);
    e.offset = static_cast<uint32_t>(out_.size());
    u32(0x04034b50);
    common(e);
    out_ += name;
    out_ += data;
    entries_.push_back(e);
  }

  std::string finish() {
    const uint32_t start = static_cast<uint32_t>(out_.size());
    for (const Entry& e : entries_) {
      u32(0x02014b50);
      u16(0x031e);  // made on Unix, zip 3.0
      common(e);
      u16(0);  // comment
      u16(0);  // disk
      u16(0);  // internal attributes
      u32(0100644u << 16);
      u32(e.offset);
      out_ += e.name;
    }
    const uint32_t size = static_cast<uint32_t>(out_.size()) - start;
    u32(0x06054b50);
    u16(0);
    u16(0);
    u16(static_cast<uint16_t>(entries_.size()));
    u16(static_cast<uint16_t>(entries_.size()));
    u32(size);
    u32(start);
    u16(0);
    return std::move(out_);
  }

 private:
  struct Entry {
    std::string name;
    uint32_t crc = 0, size = 0, offset = 0;
    uint16_t time = 0, date = 0;
  };

  void u16(uint16_t v) {
    out_ += static_cast<char>(v & 0xff);
    out_ += static_cast<char>(v >> 8);
  }
  void u32(uint32_t v) {
    u16(static_cast<uint16_t>(v & 0xffff));
    u16(static_cast<uint16_t>(v >> 16));
  }
  // What the local and central headers share, from "version needed" on.
  void common(const Entry& e) {
    u16(20);
    u16(0x0800);  // UTF-8 names
    u16(0);       // stored
    u16(e.time);
    u16(e.date);
    u32(e.crc);
    u32(e.size);
    u32(e.size);
    u16(static_cast<uint16_t>(e.name.size()));
    u16(0);  // extra
  }

  static uint32_t crc32(const std::string& data) {
    static const std::vector<uint32_t> table = [] {
      std::vector<uint32_t> t(256);
      for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
      }
      return t;
    }();
    uint32_t c = 0xffffffffu;
    for (const unsigned char b : data) c = table[(c ^ b) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
  }

  std::string out_;
  std::vector<Entry> entries_;
};

}  // namespace

std::string saveDiagnostics(std::string& error) {
  // Logs from before the media server was hidden still name it.
  const Settings settings = loadSettings();
  hideInRunLog(urlHost(settings.stash.url), kHiddenServer);
  for (const StashSettings& other : settings.otherServers)
    hideInRunLog(urlHost(other.url), kHiddenServer);

  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d-%H%M%S", &local);

  Zip zip;
  zip.add("README.txt",
          std::string(kAppTitle) + " " + VIDEOFRAMEVR_VERSION + " diagnostics, saved " + stamp +
              ".\n\n"
              "logs/          the last runs' logs, newest last\n"
              "settings.conf  the settings\n\n"
              "Anonymised: the home folder shows as ~, the user name as <user>, servers as\n"
              "<media server>, files and folders as <path> or <video>. File names, folders,\n"
              "titles, server addresses and keys are left out.\n",
          now);
  std::error_code ec;
  std::string settingsText = readFile(configDir() / "settings.conf");
  if (!settingsText.empty()) zip.add("settings.conf", anonymisedSettings(settingsText), now);
  std::vector<fs::path> logs;
  for (fs::directory_iterator it(runLogDir(), ec), end; !ec && it != end; it.increment(ec))
    if (it->path().extension() == ".log") logs.push_back(it->path());
  std::sort(logs.begin(), logs.end());
  for (const fs::path& log : logs) {
    const auto written = fs::last_write_time(log, ec);
    const std::time_t when =
        ec ? now
           : std::chrono::system_clock::to_time_t(
                 std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                     written - fs::file_time_type::clock::now() + std::chrono::system_clock::now()));
    zip.add("logs/" + log.filename().string(), anonymised(readFile(log)), when);
  }

  const fs::path dir = downloadDir();
  fs::create_directories(dir, ec);
  const fs::path path = dir / (std::string(kAppName) + "-diagnostics-" + stamp + ".zip");
  const fs::path part = path.string() + ".part";
  {
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    const std::string data = zip.finish();
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) {
      fs::remove(part, ec);
      error = "could not write to " + dir.string();
      return {};
    }
  }
  fs::rename(part, path, ec);
  if (ec) {
    fs::remove(part, ec);
    error = "could not write to " + dir.string();
    return {};
  }
  return path.string();
}

DiagnosticsExport::~DiagnosticsExport() {
  if (worker_.joinable()) worker_.join();
}

void DiagnosticsExport::start() {
  if (state_ == State::Saving) return;
  if (worker_.joinable()) worker_.join();
  state_ = State::Saving;
  done_ = false;
  worker_ = std::thread([this] {
    std::string error;
    const std::string path = saveDiagnostics(error);
    if (path.empty()) std::fprintf(stderr, "Diagnostics: %s\n", error.c_str());
    else std::printf("Diagnostics saved: %s\n", path.c_str());
    std::lock_guard<std::mutex> lock(lock_);
    result_ = path;
    done_ = true;
  });
}

void DiagnosticsExport::poll() {
  if (state_ != State::Saving) return;
  std::lock_guard<std::mutex> lock(lock_);
  if (!done_) return;
  state_ = result_.empty() ? State::Failed : State::Saved;
  fileName_ = fs::path(result_).filename().string();
}
