// Mount-path parsing/unescaping, removable-drive detection and SteamOS-specific
// automount policy credited to FramePlayerX, Copyright 2026 remotesojourner.
// Upstream: crates/frameplayerx/src/drives.rs,
// crates/frameplayerx/src/app/tasks.rs and crates/fp-core/src/volume.rs at
// 0c43903b78e3c1312ddaee3ce1674817a2799c0b,
// Apache-2.0. https://github.com/remotesojourner/frameplayerx
// Modified for VideoFrameVR's C++ worker and GIO D-Bus integration; present by
// 2026-11-03, attribution added 2026-11-10. See packaging/frameplayerx/.

#include "removable_drives.h"

#include <gio/gio.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace {

constexpr auto kInterval = std::chrono::seconds(3);
constexpr const char* kMediaRoot = "/run/media/";
constexpr const char* kUdisks = "org.freedesktop.UDisks2";

using Kind = RemovableDrives::Kind;

// What a device (/dev/<name>) is: a microSD card, a USB drive, or neither
// (the Frame's internal disks are sd* too, so a name alone won't do).
Kind kindOf(const std::string& device) {
  const std::string name = device.substr(device.rfind('/') + 1);
  if (name.rfind("mmcblk", 0) == 0) return Kind::Card;
  if (char* real = realpath(("/sys/class/block/" + name).c_str(), nullptr)) {
    const bool usb = std::strstr(real, "/usb") != nullptr;
    std::free(real);
    if (usb) return Kind::Usb;
  }
  return Kind::Other;
}

// Whether the app mounts this kind of filesystem on this kind of drive:
// cards SteamOS leaves alone (it checks and mounts ext4 ones itself), and
// any USB drive with a filesystem other systems write.
bool wanted(Kind kind, const std::string& type) {
  static const std::set<std::string> foreign = {"exfat", "vfat", "ntfs", "ntfs3"};
  if (kind == Kind::Card) return foreign.count(type) > 0;
  if (kind == Kind::Usb) return foreign.count(type) > 0 || type == "ext4";
  return false;
}

// /proc/mounts writes spaces and a few other bytes as \ooo.
std::string unescape(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    const bool octal = s[i] == '\\' && i + 3 < s.size() &&
                       std::all_of(s.begin() + i + 1, s.begin() + i + 4,
                                   [](char c) { return c >= '0' && c <= '7'; });
    if (octal) {
      out += static_cast<char>((s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0'));
      i += 3;
    } else {
      out += s[i];
    }
  }
  return out;
}

// udisks names the mount point after the label, else after the
// filesystem's serial number or UUID (e.g. "965E-2CEB"), which says nothing.
bool looksLikeId(const std::string& name) {
  if (name.size() < 8) return false;
  bool dash = false;
  for (const char c : name) {
    if (c == '-') dash = true;
    else if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  }
  return dash || name.size() == 16;  // NTFS: 16 digits
}

std::string bytestring(GVariant* value) {
  gsize size = 0;
  const char* data = static_cast<const char*>(g_variant_get_fixed_array(value, &size, 1));
  std::string s(data, size);
  if (!s.empty() && s.back() == '\0') s.pop_back();
  return s;
}

// A filesystem udisks knows: its object path, device and type, and whether
// it is mounted.
struct Filesystem {
  std::string object, device, type;
  bool mounted = false;
};

// Asks udisks for the block devices with a filesystem it isn't told to
// ignore. Empty on any error.
std::vector<Filesystem> filesystems(GDBusConnection* bus) {
  std::vector<Filesystem> out;
  GError* error = nullptr;
  GVariant* reply = g_dbus_connection_call_sync(
      bus, kUdisks, "/org/freedesktop/UDisks2", "org.freedesktop.DBus.ObjectManager",
      "GetManagedObjects", nullptr, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, 5000,
      nullptr, &error);
  if (!reply) {
    g_clear_error(&error);
    return out;
  }
  GVariant* objects = g_variant_get_child_value(reply, 0);
  GVariantIter it;
  g_variant_iter_init(&it, objects);
  const char* path = nullptr;
  GVariant* interfaces = nullptr;
  while (g_variant_iter_loop(&it, "{&o@a{sa{sv}}}", &path, &interfaces)) {
    GVariant* block = g_variant_lookup_value(interfaces, "org.freedesktop.UDisks2.Block", nullptr);
    GVariant* filesystem =
        g_variant_lookup_value(interfaces, "org.freedesktop.UDisks2.Filesystem", nullptr);
    if (block && filesystem) {
      gboolean ignore = FALSE;
      const char* type = nullptr;
      g_variant_lookup(block, "HintIgnore", "b", &ignore);
      g_variant_lookup(block, "IdType", "&s", &type);
      GVariant* device = g_variant_lookup_value(block, "Device", G_VARIANT_TYPE_BYTESTRING);
      GVariant* mounts = g_variant_lookup_value(filesystem, "MountPoints", nullptr);
      if (!ignore && type && device && mounts)
        out.push_back({path, bytestring(device), type, g_variant_n_children(mounts) > 0});
      if (device) g_variant_unref(device);
      if (mounts) g_variant_unref(mounts);
    }
    if (block) g_variant_unref(block);
    if (filesystem) g_variant_unref(filesystem);
  }
  g_variant_unref(objects);
  g_variant_unref(reply);
  return out;
}

// Mounts through udisks where it mounts everything (/run/media/<user>/...),
// without asking for a password. Empty, or why it failed.
std::string mount(GDBusConnection* bus, const std::string& object) {
  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&options, "{sv}", "options", g_variant_new_string("noatime"));
  g_variant_builder_add(&options, "{sv}", "auth.no_user_interaction", g_variant_new_boolean(TRUE));
  GError* error = nullptr;
  GVariant* reply = g_dbus_connection_call_sync(
      bus, kUdisks, object.c_str(), "org.freedesktop.UDisks2.Filesystem", "Mount",
      g_variant_new("(a{sv})", &options), G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 60000,
      nullptr, &error);
  if (reply) {
    g_variant_unref(reply);
    return {};
  }
  g_dbus_error_strip_remote_error(error);
  std::string why = error->message;
  g_error_free(error);
  return why.empty() ? "unknown error" : why;
}

}  // namespace

std::vector<RemovableDrives::Drive> mountedDrives() {
  std::vector<RemovableDrives::Drive> drives;
  std::ifstream mounts("/proc/self/mounts");
  std::string line;
  while (std::getline(mounts, line)) {
    std::istringstream fields(line);
    std::string device, point;
    if (!(fields >> device >> point)) continue;
    point = unescape(point);
    if (point.rfind(kMediaRoot, 0) != 0 || device.rfind("/dev/", 0) != 0) continue;
    if (std::any_of(drives.begin(), drives.end(),
                    [&](const RemovableDrives::Drive& d) { return d.mountPoint == point; }))
      continue;
    RemovableDrives::Drive d;
    d.mountPoint = point;
    d.kind = kindOf(device);
    const std::string name = point.substr(point.rfind('/') + 1);
    if (!looksLikeId(name)) d.label = name;
    drives.push_back(d);
  }
  std::sort(drives.begin(), drives.end(), [](const auto& a, const auto& b) {
    return a.mountPoint < b.mountPoint;
  });
  return drives;
}

RemovableDrives::~RemovableDrives() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    quit_ = true;
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void RemovableDrives::start(bool mountOthers) {
  if (worker_.joinable()) return;
  mountOthers_ = mountOthers;
  worker_ = std::thread([this] { work(); });
}

bool RemovableDrives::poll(Change& change) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!haveChange_) return false;
  change = std::move(change_);
  change_ = {};
  haveChange_ = false;
  return true;
}

void RemovableDrives::work() {
  setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 19);
  GDBusConnection* bus = nullptr;
  if (mountOthers_) {
    GError* error = nullptr;
    bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (!bus) {
      std::printf("Removable drives: no system bus, so none are mounted (%s)\n",
                  error ? error->message : "?");
      g_clear_error(&error);
    }
  }
  std::set<std::string> tried;  // udisks objects seen mounted, or mounted once, while present
  std::vector<Drive> known = mountedDrives();
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (wake_.wait_for(lock, kInterval, [&] { return quit_; })) break;
    }
    Change change;
    if (bus) {
      const std::vector<Filesystem> present = filesystems(bus);
      // Forget drives taken out, so they mount when put back; one unmounted
      // in Steam (still in) stays so.
      for (auto it = tried.begin(); it != tried.end();)
        it = std::any_of(present.begin(), present.end(),
                         [&](const Filesystem& f) { return f.object == *it; })
                 ? std::next(it)
                 : tried.erase(it);
      for (const Filesystem& c : present) {
        const Kind kind = kindOf(c.device);
        if (!wanted(kind, c.type) || !tried.insert(c.object).second || c.mounted) continue;
        std::printf("Removable drives: mounting a %s (%s)\n",
                    kind == Kind::Card ? "microSD card" : "USB drive", c.type.c_str());
        const std::string why = mount(bus, c.object);
        if (!why.empty()) {
          std::printf("Removable drives: could not mount it: %s\n", why.c_str());
          change.failures.push_back(why);
          change.failedKinds.push_back(kind);
        }
      }
    }
    const std::vector<Drive> now = mountedDrives();
    auto has = [](const std::vector<Drive>& in, const std::string& point) {
      return std::any_of(in.begin(), in.end(),
                         [&](const Drive& d) { return d.mountPoint == point; });
    };
    for (const Drive& d : now)
      if (!has(known, d.mountPoint)) change.added.push_back(d);
    for (const Drive& d : known)
      if (!has(now, d.mountPoint)) change.removed.push_back(d.mountPoint);
    known = now;
    if (change.added.empty() && change.removed.empty() && change.failures.empty()) continue;
    std::printf("Removable drives: %zu mounted, %zu removed\n", change.added.size(),
                change.removed.size());
    std::lock_guard<std::mutex> lock(mutex_);
    for (Drive& d : change.added) change_.added.push_back(std::move(d));
    for (std::string& p : change.removed) change_.removed.push_back(std::move(p));
    for (size_t i = 0; i < change.failures.size(); ++i) {
      change_.failures.push_back(change.failures[i]);
      change_.failedKinds.push_back(change.failedKinds[i]);
    }
    haveChange_ = true;
  }
  if (bus) g_object_unref(bus);
}
