#!/usr/bin/env bash
# Release entry point. Copy beside bin/, lib/ and share/ in the SLR 4 bundle.
# This deliberately does not share the experimental Frame launcher's profile.
set -euo pipefail

app_id=videoframevr
fail() {
  printf '%s\n' "$app_id: $*" >&2 || :
  # Steam shows no output, so without a terminal also say it in a window.
  if [[ ! -t 2 ]] && command -v zenity >/dev/null; then
    zenity --error --no-markup --title VideoFrameVR --width 460 --timeout 60 --text "$*" 2>/dev/null || true
  fi
  exit 1
}
os_value() {
  # Read data, not shell code. os-release values used here contain no escapes.
  sed -n "s/^$2=//p" "$1" 2>/dev/null | tr -d '\"' | head -n 1
}

launcher=$(readlink -f -- "${BASH_SOURCE[0]}")
bundle=${launcher%/*}
# RELEASE.json is the builder's flat schema-1 record. Consume only simple
# identifier fields, never evaluate JSON as shell code. Escaped identifiers are
# deliberately unsupported. No JSON interpreter is needed in the Platform.
[[ -r $bundle/RELEASE.json ]] || fail 'Release metadata is missing.'
metadata=$(tr '\n' ' ' < "$bundle/RELEASE.json")
json_identifier() {
  printf '%s\n' "$metadata" | sed -nE "s/.*\"$1\"[[:space:]]*:[[:space:]]*\"([A-Za-z0-9_.-]+)\".*/\\1/p"
}
[[ $metadata =~ \"schema\"[[:space:]]*:[[:space:]]*1[[:space:]]*[,}] ]] || fail 'Unsupported release metadata schema.'
release_app=$(json_identifier app_id)
[[ $release_app =~ ^[a-z][a-z0-9_-]*$ ]] || fail 'Invalid release application identifier.'
app_id=$release_app
[[ $(json_identifier runtime) == steamrt4 ]] || fail 'This launcher requires a Steam Runtime 4 release.'
channel=$(json_identifier channel)
case $channel in
  github)
    if [[ ${VIDEOFRAMEVR_UPDATE_HELPER:-} != /* || ! -x ${VIDEOFRAMEVR_UPDATE_HELPER:-} ]]; then
      unset VIDEOFRAMEVR_UPDATE_HELPER
    fi
    ;;
  steam) unset VIDEOFRAMEVR_UPDATE_HELPER ;;
  *) fail 'Unsupported release channel.' ;;
esac
binary="$bundle/bin/$app_id"
[[ -x $binary ]] || fail 'Release executable is missing or not executable.'
case $(uname -m) in
  aarch64|arm64) arch=aarch64; runtime_name=SteamLinuxRuntime_4-arm64; runtime_app=4185400 ;;
  x86_64) arch=x86_64; runtime_name=SteamLinuxRuntime_4; runtime_app=4183110 ;;
  *) fail 'This release requires an ARM64 or x86-64 Linux host.' ;;
esac
# runtime_title is the tool's name in the Steam library.
case $arch in
  aarch64) runtime_variant=com.valvesoftware.steamruntime.platform-arm64-steamrt4
           runtime_title='Steam Linux Runtime 4.0 - Arm64' ;;
  x86_64) runtime_variant=com.valvesoftware.steamruntime.platform-amd64_i386-steamrt4
          runtime_title='Steam Linux Runtime 4.0' ;;
esac
[[ $(json_identifier architecture) == "$arch" ]] || fail 'Release metadata does not match the host architecture.'
# Fail before entering a container when a bundle of the other architecture was
# unpacked. ELF e_machine is little-endian on both supported architectures.
machine=$(od -An -tu1 -j18 -N2 -- "$binary" | xargs)
case "$arch:$machine" in
  aarch64:'183 0'|x86_64:'62 0') ;;
  *) fail 'Release executable does not match the host architecture.' ;;
esac

os=/usr/lib/os-release
[[ -r $os ]] || os=/etc/os-release
if [[ ${container:-} == pressure-vessel ]]; then
  [[ $(os_value "$os" ID) == steamrt && $(os_value "$os" VERSION_ID) == 4 ]] ||
    fail 'This release needs Steam Linux Runtime 4.0. Do not force an older compatibility tool.'
  [[ $(os_value "$os" VARIANT_ID) == "$runtime_variant" ]] ||
    fail 'The active Steam Linux Runtime platform does not match this release architecture.'
else
  [[ ${VIDEOFRAMEVR_SLR_ENTERED:-0} != 1 ]] ||
    fail 'Steam Linux Runtime did not enter the expected container.'
  [[ -z ${container:-} ]] || fail 'Start this release outside other containers.'

  # Any 4.0 runtime will do, however old: Steam can leave it weeks behind, and
  # the release build checks the bundle against the first public 4.0. What was
  # entered is checked above, from inside.
  runtime=
  if [[ -n ${VIDEOFRAMEVR_SLR_ROOT:-} ]]; then
    [[ -x $VIDEOFRAMEVR_SLR_ROOT/run ]] || fail 'VIDEOFRAMEVR_SLR_ROOT is not a Steam Linux Runtime installation.'
    runtime=$VIDEOFRAMEVR_SLR_ROOT
  else
    roots=("${STEAM_COMPAT_CLIENT_INSTALL_PATH:-}" "$HOME/.steam/root" "$HOME/.steam/steam"
      "${XDG_DATA_HOME:-$HOME/.local/share}/Steam" "$HOME/.local/share/Steam")
    libraries=()
    for root in "${roots[@]}"; do
      [[ -d $root ]] || continue
      libraries+=("$root")
      # libraryfolders.vdf is data. Never source it or eval a path from it.
      for vdf in "$root/steamapps/libraryfolders.vdf" "$root/config/libraryfolders.vdf"; do
        [[ -r $vdf ]] || continue
        while IFS= read -r line; do
          if [[ $line =~ ^[[:space:]]*\"(path|[0-9]+)\"[[:space:]]*\"(([^\"\\]|\\.)*)\" ]]; then
            library=${BASH_REMATCH[2]}
            library=${library//\\\\/\\}
            library=${library//\\\"/\"}
            [[ $library == /* ]] && libraries+=("$library")
          fi
        done < "$vdf"
      done
    done
    for library in "${libraries[@]}"; do
      candidate="$library/steamapps/common/$runtime_name"
      if [[ -x $candidate/run ]]; then runtime=$candidate; break; fi
    done
  fi
  [[ -n $runtime ]] || fail "\"$runtime_title\" is not installed. Install it from your Steam library (steam://install/$runtime_app), then start again."
  # Only the payload gets our library directory. pressure-vessel must start with
  # its own libraries. Keep Steam's process ancestry, overlay and app identity.
  export VIDEOFRAMEVR_SLR_ENTERED=1
  exec "$runtime/run" -- "$launcher" "$@"
fi

unset VIDEOFRAMEVR_SLR_ENTERED
export LD_LIBRARY_PATH="$bundle/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
font_config="$bundle/share/$app_id/fonts/fonts.conf"
[[ -r $font_config ]] || fail 'Bundled font configuration is missing.'
export FONTCONFIG_FILE="$font_config"

# Restrict the proven SteamOS GL-cap workaround and decoder/mirror defaults to
# the native Frame. An ARM64 desktop must keep its runtime and display choices.
host_os=/run/host/os-release
[[ -r $host_os ]] || host_os=/run/host/usr/lib/os-release
[[ -r $host_os ]] || host_os=/run/host/etc/os-release
frame=0
if [[ $arch == aarch64 && $(os_value "$host_os" ID) == steamos && $(os_value "$host_os" VARIANT_ID) == vr ]]; then
  frame=1
  unset MESA_GL_VERSION_OVERRIDE MESA_GLSL_VERSION_OVERRIDE
fi

# Leave OpenXR's normal discovery alone, including explicit caller overrides.
# With a disposable XDG_CONFIG_HOME on Frame, SteamVR's default manifest remains
# visible at /opt. Use it only if no active-runtime registration is present.
if [[ $frame == 1 && ! ${XR_RUNTIME_JSON+x} ]]; then
  registered=0
  config_dirs=("${XDG_CONFIG_HOME:-$HOME/.config}")
  IFS=: read -r -a system_config_dirs <<< "${XDG_CONFIG_DIRS:-/etc/xdg}"
  config_dirs+=("${system_config_dirs[@]}" /etc)
  for dir in "${config_dirs[@]}"; do
    [[ $dir == /* ]] || continue
    for suffix in ".${arch}" ''; do
      # A broken registration is a user/runtime error, not permission to replace
      # their runtime with SteamVR silently.
      manifest="$dir/openxr/1/active_runtime$suffix.json"
      [[ -e $manifest || -L $manifest ]] && registered=1
    done
  done
  if [[ $registered == 0 && -r /opt/steamvr/steamxr_linuxarm64.json ]]; then
    export XR_RUNTIME_JSON=/opt/steamvr/steamxr_linuxarm64.json
  fi
fi

# XDG_CONFIG_HOME, XDG_DATA_HOME, XDG_CACHE_HOME, DISPLAY, XDG_RUNTIME_DIR,
# XDG_DATA_DIRS and all overlay/layer variables are inherited, never rewritten.
# The app supplies standard XDG defaults. The release launcher writes no files.
defaults=()
[[ $frame == 0 ]] || defaults=(--hwdec=v4l2m2m --preview-hwdec=no --no-mirror)
exec "$binary" "${defaults[@]}" "$@"
