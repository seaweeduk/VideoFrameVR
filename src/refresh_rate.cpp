#include "refresh_rate.h"

#include "platform.h"

#include <glib.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

std::string appId() {
  const char* id = std::getenv("SteamAppId");
  if (!id || !*id || std::string(id) == "0") return {};
  for (const char* c = id; *c; ++c)
    if (*c < '0' || *c > '9') return {};
  return id;
}

std::string settingKey() { return "steam.app." + appId() + ".preferredRefreshRate"; }

// vrcmd with its own libraries, not the app's; false if it failed. `output`:
// what it prints, else that is dropped.
bool vrcmd(std::vector<std::string> args, std::string* output) {
  gchar** env = g_get_environ();
  env = g_environ_unsetenv(env, "LD_PRELOAD");
  env = g_environ_setenv(env, "LD_LIBRARY_PATH", kVrcmdDir, TRUE);
  args.insert(args.begin(), kVrcmd);
  std::vector<gchar*> argv;
  for (std::string& arg : args) argv.push_back(arg.data());
  argv.push_back(nullptr);
  const auto flags = static_cast<GSpawnFlags>(
      G_SPAWN_STDERR_TO_DEV_NULL | (output ? G_SPAWN_DEFAULT : G_SPAWN_STDOUT_TO_DEV_NULL));
  gchar* out = nullptr;
  gint status = 0;
  GError* error = nullptr;
  const bool ok = g_spawn_sync(nullptr, argv.data(), env, flags, nullptr, nullptr, output ? &out : nullptr,
                               nullptr, &status, &error) &&
                  g_spawn_check_wait_status(status, &error);
  if (!ok) std::fprintf(stderr, "Refresh rate: vrcmd failed: %s\n", error->message);
  if (output && out) *output = out;
  g_free(out);
  g_clear_error(&error);
  g_strfreev(env);
  return ok;
}

}  // namespace

bool appRefreshRateAvailable() {
  return kStandaloneHeadset && !appId().empty() && access(kVrcmd, X_OK) == 0;
}

int readAppRefreshRate() {
  std::string out;
  if (!vrcmd({"--settings-float", settingKey()}, &out)) return 0;
  // "<key>=120.000000"; nothing when the app has none.
  const size_t equals = out.find('=');
  return equals == std::string::npos ? 0 : static_cast<int>(std::lround(std::atof(out.c_str() + equals + 1)));
}

void writeAppRefreshRate(int hz) { vrcmd({"--set-settings-float", settingKey(), std::to_string(hz)}, nullptr); }
