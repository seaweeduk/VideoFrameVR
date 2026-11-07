"""Install, update and uninstall VideoFrameVR for one user. Python standard library only.

build_installer.py packs this file, steam_client.py, install_steam_shortcut.py and
config.json into helper.pyz and embeds it, with one release archive, in a single
.desktop file.
"""

import argparse
import base64
from contextlib import contextmanager
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import pkgutil
import platform
import shlex
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import time
import traceback
import urllib.request
import uuid

import install_steam_shortcut as steam
import steam_client

CONFIG = json.loads(pkgutil.get_data(__name__, "config.json"))
APP_ID = CONFIG["app_id"]
APP_NAME = CONFIG["app_name"]
REPOSITORY = CONFIG["repository"]
HOME = Path.home()
ROOT = HOME / "Applications" / APP_NAME
DATA = Path(os.environ.get("XDG_DATA_HOME") or HOME / ".local/share")
MENU_ENTRY = DATA / "applications" / (APP_ID + ".desktop")
LOG = Path(os.environ.get("XDG_STATE_HOME") or HOME / ".local/state") / (APP_ID + "-installer.log")
LOG_NAME = str(LOG).replace(str(HOME), "~", 1)


def find_steam():
    """Steam's folder: the one ~/.steam/root points to, else a usual place (Ubuntu's package uses its own)."""
    places = [HOME / ".steam/root", HOME / ".steam/steam", HOME / ".local/share/Steam",
              HOME / ".steam/debian-installation"]
    for place in places:
        if (place / "userdata").is_dir():
            return place.resolve()
    return HOME / ".local/share/Steam"


STEAM = find_steam()
# Flatpak Steam's sandbox can't open apps in ~/Applications, so it isn't supported.
FLATPAK_STEAM = HOME / ".var/app/com.valvesoftware.Steam/.local/share/Steam"
LAUNCHER = ROOT / (APP_ID + ".sh")
HELPER = ROOT / "helper.pyz"
ARTWORK = ROOT / "current/share" / APP_ID / "steam"
# Steam Linux Runtime 4.0, which launch.sh enters.
RUNTIME_APPS = {"aarch64": "4185400", "x86_64": "4183110"}
UID = os.getuid()
# The Frame's Desktop Mode is a nested Plasma session with its own runtime
# directory and session bus, so reach the user's systemd manager explicitly.
SYSTEMD_ENV = {**os.environ, "XDG_RUNTIME_DIR": f"/run/user/{UID}",
               "DBUS_SESSION_BUS_ADDRESS": f"unix:path=/run/user/{UID}/bus"}


class InstallError(Exception):
    """A message for the person installing, without technical detail."""


def log(*lines):
    LOG.parent.mkdir(parents=True, exist_ok=True)
    if LOG.exists() and LOG.stat().st_size > 1024 ** 2:
        LOG.unlink()
    stamp = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
    with LOG.open("a") as output:
        for line in lines:
            output.write(f"{stamp} {line}\n")


def machine():
    return {"arm64": "aarch64", "aarch64": "aarch64", "amd64": "x86_64"}.get(
        platform.machine().lower(), platform.machine().lower())


def atomic_write(path, content, mode=0o644):
    fd, temporary = tempfile.mkstemp(prefix=".tmp-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(content)
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


# Installer file: a .desktop entry whose comment lines carry the payload.
#   #@ metadata JSON    #H helper.pyz in base64    #A release archive in base64

def read_metadata(path):
    with open(path) as source:
        for line in source:
            if line.startswith("#@"):
                return json.loads(line[2:])
    raise InstallError("This file is not a VideoFrameVR installer. Download it again.")


def decode(path, prefix, output, digest, progress=None, total=1):
    sha = hashlib.sha256()
    done = 0
    with open(path) as source:
        for line in source:
            if line.startswith(prefix):
                block = base64.b64decode(line[2:])
                sha.update(block)
                output.write(block)
                done += len(block)
                if progress:
                    progress(done / total)
    if sha.hexdigest() != digest:
        raise InstallError("The installer file is damaged. Download it again.")


@contextmanager
def locked():
    ROOT.mkdir(parents=True, exist_ok=True)
    with (ROOT / ".lock").open("w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise InstallError(APP_NAME + " is already being installed or updated. Wait for it to finish.") from None
        yield


def safe_extract(archive, staging, progress):
    """Extract regular files first, then symlinks that stay inside the release. Never extractall."""
    with tarfile.open(archive, "r:gz") as tar:
        members = tar.getmembers()
        top = PurePosixPath(members[0].name).parts[0] if members else None
        total = sum(member.size for member in members) or 1
        for member in members:
            name = PurePosixPath(member.name)
            if (name.is_absolute() or ".." in name.parts or not name.parts or name.parts[0] != top
                    or not (member.isfile() or member.isdir() or member.issym())):
                raise InstallError("The installer file is damaged. Download it again.")
            if member.issym():
                target = (staging / member.name).parent / member.linkname
                if PurePosixPath(member.linkname).is_absolute() or not os.path.normpath(target).startswith(str(staging / top) + "/"):
                    raise InstallError("The installer file is damaged. Download it again.")
        if shutil.disk_usage(staging).free < total + 64 * 1024 ** 2:
            raise InstallError("There is not enough free space. Free some storage and try again.")
        done = 0
        for member in members:
            destination = staging / member.name
            if member.isdir():
                destination.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                destination.parent.mkdir(parents=True, exist_ok=True)
                with tar.extractfile(member) as source, destination.open("xb") as output:
                    shutil.copyfileobj(source, output)
                destination.chmod(0o755 if member.mode & 0o111 else 0o644)
                done += member.size
                progress(done / total)
        for member in members:
            if member.issym():
                (staging / member.name).symlink_to(member.linkname)
        return staging / top


def release_info(directory, metadata):
    info = json.loads((directory / "RELEASE.json").read_text())
    if (info.get("app_id") != APP_ID or info.get("version") != metadata["version"]
            or info.get("architecture") != metadata["architecture"]
            or not os.access(directory / "launch.sh", os.X_OK)):
        raise InstallError("The installer file is damaged. Download it again.")
    return info


def in_use(directory):
    prefix = str(directory) + "/"
    for proc in Path("/proc").glob("[0-9]*"):
        try:
            if os.readlink(proc / "exe").startswith(prefix):
                return True
        except OSError:
            continue
    return False


def installed():
    """The installed version and build time, or None."""
    try:
        return json.loads((ROOT / "install.json").read_text())
    except (OSError, ValueError):
        return None


def install(path, progress=lambda fraction: None):
    """Install the release inside an installer file and make it current."""
    metadata = read_metadata(path)
    if metadata.get("app_id") != APP_ID or metadata.get("schema") != 1:
        raise InstallError("This file is not a VideoFrameVR installer. Download it again.")
    if metadata["architecture"] != machine():
        kind = {"aarch64": "the Steam Frame", "x86_64": "desktop PCs"}.get(metadata["architecture"],
                                                                         metadata["architecture"] + " computers")
        raise InstallError("This installer is for " + kind + ". It can't install on this computer.")
    log("Installing " + metadata["version"] + " from " + str(path))
    with locked():
        releases = ROOT / "releases"
        releases.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".stage-", dir=ROOT) as temporary:
            staging = Path(temporary)
            helper = staging / "helper.pyz"
            with helper.open("wb") as output:
                decode(path, "#H", output, metadata["helper_sha256"])
            archive = staging / "release.tar.gz"
            with archive.open("wb") as output:
                decode(path, "#A", output, metadata["archive_sha256"],
                       lambda f: progress(0.3 * f), metadata["archive_size"])
            extracted = safe_extract(archive, staging, lambda f: progress(0.3 + 0.65 * f))
            release_info(extracted, metadata)
            destination = releases / (metadata["version"] + "-" + metadata["archive_sha256"][:16])
            if destination.exists():
                # Reinstalling the same release repairs it, unless it is running.
                if in_use(destination):
                    destination = destination.with_name(destination.name + "-" + uuid.uuid4().hex[:8])
                else:
                    shutil.rmtree(destination)
            extracted.rename(destination)
            helper.chmod(0o755)
            os.replace(helper, HELPER)
        # Resolve current once, so an update never mixes two releases in one run.
        atomic_write(LAUNCHER, ('#!/bin/sh\nrelease=$(readlink -f -- ' + shlex.quote(str(ROOT / "current")) + ')\n'
                                'export VIDEOFRAMEVR_UPDATE_HELPER=' + shlex.quote(str(HELPER)) + '\n'
                                'export VIDEOFRAMEVR_LAUNCHER=' + shlex.quote(str(LAUNCHER)) + '\n'
                                'exec "$release/launch.sh" "$@"\n').encode(), 0o755)
        link = ROOT / (".current-" + uuid.uuid4().hex)
        link.symlink_to("releases/" + destination.name)
        os.replace(link, ROOT / "current")
        atomic_write(ROOT / "install.json", json.dumps(
            {"version": metadata["version"], "built_at": metadata["built_at"]}).encode())
        for old in releases.iterdir():
            if old != destination and not in_use(old):
                shutil.rmtree(old)
        write_menu_entry()
        progress(1)
    log("Installed " + metadata["version"])
    return metadata["version"]


def write_menu_entry():
    def run(*arguments):
        return " ".join('"' + str(argument) + '"' for argument in arguments)
    MENU_ENTRY.parent.mkdir(parents=True, exist_ok=True)
    atomic_write(MENU_ENTRY, (
        "[Desktop Entry]\nType=Application\nName=" + APP_NAME + "\nGenericName=VR Media Player\n"
        "Exec=" + run(LAUNCHER) + "\nIcon=" + str(ROOT / "current/share/icons/hicolor/256x256/apps" / (APP_ID + ".png"))
        + "\nTerminal=false\nCategories=AudioVideo;Video;Player;\nActions=add-to-steam;save-diagnostics;uninstall;\n\n"
        "[Desktop Action add-to-steam]\nName=Add to Steam library\nExec=" + run("python3", HELPER, "add-to-steam") + "\n\n"
        "[Desktop Action save-diagnostics]\nName=Save diagnostics\nExec=" + run("python3", HELPER, "save-diagnostics") + "\n\n"
        "[Desktop Action uninstall]\nName=Uninstall\nExec=" + run("python3", HELPER, "uninstall") + "\n").encode())


def runtime_installed():
    """Whether Steam has the runtime that launch.sh needs. A missing manifest is enough to ask Steam."""
    libraries = [STEAM]
    folders = STEAM / "steamapps/libraryfolders.vdf"
    if folders.exists():
        libraries += [Path(line.split('"')[3]) for line in folders.read_text().splitlines()
                      if line.strip().startswith('"path"')]
    app = RUNTIME_APPS.get(machine())
    return any((library / "steamapps" / f"appmanifest_{app}.acf").exists() for library in libraries)


# Steam shortcut. When the running Steam offers its client interface (always on
# the Steam Frame), Steam itself adds or removes the shortcut, for the account
# that is signed in, without a restart (steam_client.py).
#
# Otherwise shortcuts.vdf is edited. Steam keeps shortcuts in memory and rewrites
# that file when it exits, so the file is changed only while Steam is stopped. On
# SteamOS the desktop runs inside Steam's service, so a separate systemd job stops
# Steam, changes the file and starts Steam again; the desktop closes meanwhile. On
# other desktops Steam is an ordinary program: it is asked to quit and started
# again, only when the person has agreed to that.

def require_steam():
    if (STEAM / "userdata").is_dir():
        return
    if (FLATPAK_STEAM / "userdata").is_dir():
        raise InstallError(APP_NAME + " needs the regular Steam. The Flatpak version of Steam can't open apps "
                           "installed outside its sandbox. Install Steam from your distribution or from "
                           "store.steampowered.com, then try again.")
    raise InstallError("Steam was not found. Open Steam and sign in, then try again.")


def in_steam():
    try:
        if steam_client.available():
            return steam_client.find(LAUNCHER) is not None
        return steam.find_shortcut(STEAM, None, LAUNCHER)[1] is not None
    except Exception as error:
        log("Steam shortcut lookup failed: " + str(error))
        return False


def through_steam(remove=False):
    """Add (or complete) or remove the shortcut through the running Steam. False
    when Steam doesn't offer that or it failed; the file is changed instead then."""
    if not steam_client.available():
        return False
    try:
        if remove:
            steam_client.remove(LAUNCHER)
            log("Removed the Steam shortcut through Steam")
        else:
            appid, added = steam_client.add(LAUNCHER, APP_NAME, ARTWORK, STEAM)
            log(("Added" if added else "Completed") + " the Steam shortcut through Steam: appid " + str(appid))
        return True
    except Exception:
        log("Changing the Steam shortcut through Steam failed:", *traceback.format_exc().splitlines())
        return False


def steam_service_active():
    try:
        return subprocess.run(["systemctl", "--user", "is-active", "--quiet", "steam.service"],
                              env=SYSTEMD_ENV).returncode == 0
    except OSError:
        return False


def steam_restart():
    """What changing the shortcut does to Steam: None (Steam is closed), "service"
    (SteamOS: Steam and the desktop restart) or "desktop" (Steam restarts)."""
    if not steam.steam_running():
        return None
    return "service" if steam_service_active() else "desktop"


def restart_note(remove=False):
    goal = "remove it from" if remove else "add it to"
    kind = steam_restart()
    if kind == "service":
        return "Steam restarts to " + goal + " your library, which closes this desktop."
    if kind == "desktop":
        return ("Steam needs to restart to " + goal + " your library. "
                "This also closes SteamVR and any game that's running.")
    return ""


def change_steam(remove):
    """Check everything that can fail before Steam is stopped, then run the job."""
    require_steam()
    try:
        steam.change_shortcut(LAUNCHER, ARTWORK, APP_NAME, STEAM, remove=remove, dry_run=True)
    except (OSError, ValueError) as error:
        log("Steam shortcut check failed: " + str(error))
        if not remove:
            raise InstallError("Unable to add " + APP_NAME + " to Steam. Details are in " + LOG_NAME + ".") from None
        raise InstallError("Unable to remove " + APP_NAME + " from Steam. Details are in " + LOG_NAME + ".") from None
    command = [sys.executable, str(HELPER), "steam-job", "remove" if remove else "add"]
    kind = steam_restart()
    if kind is None:
        return subprocess.run(command).returncode == 0
    if kind == "desktop":
        if not shutil.which("steam"):
            raise InstallError("Close Steam, then try again.")
        with progress("Restarting Steam…"):
            return subprocess.run(command).returncode == 0
    # --wait reports failures that happen before Steam stops. When Steam stops,
    # this process may end with the desktop; the job continues on its own.
    return subprocess.run(["systemd-run", "--user", "--quiet", "--collect", "--wait",
                           "--unit", APP_ID + "-steam-" + uuid.uuid4().hex[:8]] + command,
                          env=SYSTEMD_ENV).returncode == 0


def steam_job(remove):
    """Stop Steam, change the shortcut, start Steam. On SteamOS this runs outside the desktop session."""
    action = "Removing the Steam shortcut" if remove else "Adding the Steam shortcut"
    log(action)
    running = steam_restart()
    try:
        if running == "service":
            subprocess.run(["systemctl", "--user", "stop", "steam.service"], env=SYSTEMD_ENV,
                           check=True, timeout=60)
        elif running:
            subprocess.run(["steam", "-shutdown"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=60)
            log("Asked Steam to quit")
        until = time.monotonic() + 60
        while running and steam.steam_running() and time.monotonic() < until:
            time.sleep(0.5)
        if running and steam.steam_running():
            raise RuntimeError("Steam did not quit within 60 seconds")
        config, appid = steam.change_shortcut(LAUNCHER, ARTWORK, APP_NAME, STEAM, remove=remove)
        log(action + " done: account " + config.parent.name + ", appid " + str(appid))
    except Exception:
        log(action + " failed:", *traceback.format_exc().splitlines())
        raise SystemExit(1)
    finally:
        if running == "service":
            subprocess.run(["systemctl", "--user", "start", "steam.service"], env=SYSTEMD_ENV, timeout=60)
            log("Started Steam")
        elif running and not steam.steam_running():
            # Detached, so Steam outlives this job and the installer.
            subprocess.Popen(["steam"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, start_new_session=True)
            log("Started Steam")


def uninstall():
    if (ROOT / "current").exists() and in_use((ROOT / "current").resolve()):
        raise InstallError("Close " + APP_NAME + ", then try again.")
    log("Uninstalling")
    with locked():
        MENU_ENTRY.unlink(missing_ok=True)
        for path in ROOT.iterdir():
            if path.name in (".lock", HELPER.name, LAUNCHER.name):
                continue
            if path.is_dir() and not path.is_symlink():
                shutil.rmtree(path)
            else:
                path.unlink()
    log("Removed the app files")


# In-app updates: the newest published GitHub release that has an installer file.

def github(url, destination=None):
    request = urllib.request.Request(url, headers={"User-Agent": APP_ID + "-updater"})
    with urllib.request.urlopen(request, timeout=20) as response:
        if not response.url.startswith("https://"):
            raise InstallError("The update address is not secure.")
        if destination is None:
            return json.loads(response.read(4 * 1024 ** 2))
        shutil.copyfileobj(response, destination)


def newest_release():
    name = "Install-" + APP_NAME + "-{}-" + machine() + ".desktop"
    releases = [release for release in github(f"https://api.github.com/repos/{REPOSITORY}/releases?per_page=20")
                if not release.get("draft") and release.get("published_at")]
    for release in sorted(releases, key=lambda release: release["published_at"], reverse=True):
        for asset in release.get("assets", []):
            if asset.get("name") == name.format(release["tag_name"]):
                return release, asset["browser_download_url"]
    raise InstallError("No published release was found.")


def helper_command(args):
    # The app stops a running helper with SIGTERM; exit through the cleanup path.
    signal.signal(signal.SIGTERM, lambda signum, frame: sys.exit(1))
    current = installed()
    version = current["version"] if current else ""
    try:
        if not current:
            raise InstallError(APP_NAME + " is not installed.")
        release, url = newest_release()
        # Publication follows the build, so an older release never replaces a newer build.
        newer = release["tag_name"] != version and release["published_at"] > current["built_at"]
        if args.command == "check":
            status = "available" if newer else "current"
            version = release["tag_name"] if newer else version
        else:
            if newer:
                with tempfile.TemporaryDirectory(prefix=".download-", dir=ROOT) as temporary:
                    download = Path(temporary) / "installer.desktop"
                    with download.open("wb") as output:
                        github(url, output)
                    version = install(download)
            status = "ready"
        print(json.dumps({"status": status, "version": version, "message": ""}))
    except Exception as error:
        log(args.command + " failed")
        print(json.dumps({"status": "error", "version": version, "message": failure(error)}))
        raise SystemExit(1)


# Dialogs use zenity, which SteamOS includes.

def zenity(*arguments, **options):
    return subprocess.run(["zenity", "--title", APP_NAME, "--width", "460"] + list(arguments),
                          capture_output=True, text=True, **options)


def message(text, error=False):
    zenity("--error" if error else "--info", "--no-markup", "--text", text)


def ask(text, action, cancel, extra=None, default_cancel=False):
    """Returns 'ok', 'cancel' or 'extra'."""
    arguments = ["--question", "--no-markup", "--text", text, "--ok-label", action, "--cancel-label", cancel]
    arguments += (["--extra-button", extra] if extra else []) + (["--default-cancel"] if default_cancel else [])
    result = zenity(*arguments)
    if result.returncode == 0:
        return "ok"
    return "extra" if extra and result.stdout.strip() == extra else "cancel"


@contextmanager
def progress(text):
    window = subprocess.Popen(["zenity", "--progress", "--title", APP_NAME, "--width", "460", "--text", text,
                               "--no-cancel", "--auto-close", "--percentage", "0"],
                              stdin=subprocess.PIPE, text=True)
    shown = [0]

    def report(fraction):
        if int(fraction * 100) != shown[0]:
            shown[0] = int(fraction * 100)
            window.stdin.write(f"{shown[0]}\n")
            window.stdin.flush()
    try:
        yield report
    finally:
        window.stdin.close()
        window.wait()


def failure(error):
    if isinstance(error, InstallError):
        log("Stopped: " + str(error))
        return str(error)
    log("Failed:", *traceback.format_exc().splitlines())
    return "Something went wrong. Details are in " + LOG_NAME + "."


ADD_LATER = ("To add it to Steam later, right-click " + APP_NAME + " in the application menu and choose "
             "Add to Steam library. If Steam is closed then, it doesn't need to restart.")


def add_to_steam():
    if not change_steam(remove=False):
        raise InstallError("Unable to add " + APP_NAME + " to Steam. Details are in " + LOG_NAME + ".")
    message("Done. " + APP_NAME + " is in your Steam library.")


def ask_add_to_steam(text, later):
    """Added straight away through the running Steam, or with Steam closed. Asks
    first only when Steam has to restart for it."""
    require_steam()
    if through_steam():
        return message("Done. " + APP_NAME + " is in your Steam library.")
    note = restart_note()
    if note and ask(text + "\n\n" + note, "Restart Steam" if steam_restart() == "desktop" else "Add to Steam",
                    "Later") != "ok":
        return message(later)
    add_to_steam()


def gui_install(path):
    metadata = read_metadata(path)
    current = installed()
    if current and current["version"] == metadata["version"]:
        text = APP_NAME + " " + metadata["version"] + " is already installed. Install it again?"
    elif current:
        text = "Update " + APP_NAME + " from " + current["version"] + " to " + metadata["version"] + "?"
    else:
        text = "Install " + APP_NAME + " " + metadata["version"] + "?"
    text += "\n\nYour settings and history are kept."
    choice = ask(text, "Install", "Cancel", "Uninstall" if current else None)
    if choice == "extra":
        return gui_uninstall()
    if choice != "ok":
        return
    require_steam()
    if not runtime_installed():
        subprocess.Popen(["steam", "steam://install/" + RUNTIME_APPS[machine()]],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        return message(APP_NAME + " needs Steam Linux Runtime 4.0, which Steam now offers to download.\n\n"
                       "When the download finishes, open this installer again.")
    with progress("Installing " + APP_NAME + "…") as report:
        version = install(path, report)
    if in_steam():
        # Completes a shortcut added some other way: VR flag, icon and artwork.
        through_steam()
        return message("Done. " + APP_NAME + " " + version + " is in your Steam library.")
    ask_add_to_steam(APP_NAME + " " + version + " is installed. Add it to your Steam library?",
                     "Done. Open " + APP_NAME + " from the application menu.\n\n" + ADD_LATER)


def gui_add_to_steam():
    if in_steam():
        through_steam()
        return message(APP_NAME + " is already in your Steam library.")
    ask_add_to_steam("Add " + APP_NAME + " to your Steam library?", ADD_LATER)


def gui_uninstall():
    steam_entry = in_steam()
    by_steam = steam_entry and steam_client.available()
    kind = steam_restart() if steam_entry and not by_steam else None
    text = "Uninstall " + APP_NAME + "?\n\nYour settings and history are kept."
    if kind == "service":
        text += "\n\n" + restart_note(remove=True)
    if ask(text, "Uninstall", "Cancel", default_cancel=True) != "ok":
        return
    if kind == "desktop" and ask(restart_note(remove=True) + " Restart Steam now?",
                                 "Restart Steam", "Later") != "ok":
        steam_entry = False
    uninstall()
    still_listed = kind == "desktop" and not steam_entry
    if by_steam:
        still_listed = not through_steam(remove=True)
    elif steam_entry and not change_steam(remove=True):
        raise InstallError("Unable to remove " + APP_NAME + " from Steam. Details are in " + LOG_NAME + ".")
    finish_uninstall()
    text = APP_NAME + " is uninstalled. Your settings and history are kept."
    if still_listed:
        text += ("\n\nIt's still listed in Steam. To remove it, right-click it in your Steam library and "
                 "choose Manage, then Remove non-Steam game from your library.")
    message(text)


def finish_uninstall():
    """Remove the helper and launcher last; the Steam job needs them."""
    shutil.rmtree(ROOT, ignore_errors=True)


def gui_save_diagnostics():
    """The app's anonymised logs and settings in one zip in Downloads, and its folder opened."""
    result = subprocess.run([str(LAUNCHER), "--save-diagnostics"], capture_output=True, text=True, timeout=120)
    lines = [line for line in result.stdout.splitlines() if line.startswith("/")]
    if result.returncode or not lines:
        raise RuntimeError("Could not save diagnostics: " + (result.stderr.strip() or "no file was written"))
    path = Path(lines[-1])
    subprocess.Popen(["xdg-open", str(path.parent)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    message("Saved " + path.name + " in " + str(path.parent) + ".\n\nAttach it to your report. "
            "It's anonymised: no file names, folders, server addresses or user names.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("gui", "install", "check", "update", "add-to-steam",
                                            "save-diagnostics", "uninstall", "steam-job"))
    parser.add_argument("argument", nargs="?", help="gui, install: installer file; steam-job: add or remove")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    if args.command in ("check", "update"):
        return helper_command(args)
    if args.command == "install":
        # Without dialogs (scripted installs): the same install, the
        # Steam shortcut left as it is.
        print("Installed " + install(Path(args.argument)))
        return
    if args.command == "steam-job":
        steam_job(args.argument == "remove")
        if args.argument == "remove":
            finish_uninstall()
        return
    try:
        if args.command == "gui":
            gui_install(args.argument)
        elif args.command == "add-to-steam":
            gui_add_to_steam()
        elif args.command == "save-diagnostics":
            gui_save_diagnostics()
        else:
            gui_uninstall()
    except Exception as error:
        message(failure(error), error=True)
        raise SystemExit(1)
