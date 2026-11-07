#!/usr/bin/env python3
"""Register a native non-Steam shortcut and its artwork, using only stdlib.

Steam must be closed because it rewrites shortcuts.vdf from memory on exit.
Unknown binary VDF value types are rejected rather than discarded.
"""

import argparse
from datetime import datetime, timezone
import os
from pathlib import Path
import re
import shutil
import struct
import tempfile
import zlib


def read_vdf(data):
    offset = 0

    def take(size):
        nonlocal offset
        if offset + size > len(data):
            raise ValueError("Truncated binary VDF")
        result = data[offset:offset + size]
        offset += size
        return result

    def text():
        nonlocal offset
        end = data.find(b"\0", offset)
        if end < 0:
            raise ValueError("Unterminated VDF string")
        return take(end - offset + 1)[:-1].decode("utf-8", "surrogateescape")

    def obj():
        entries = []
        while True:
            kind = take(1)[0]
            if kind == 8:
                return entries
            key = text()
            if kind == 0:
                value = obj()
            elif kind == 1:
                value = text()
            elif kind in (2, 3, 4, 6):
                value = take(4)
            elif kind in (7, 10):
                value = take(8)
            else:
                raise ValueError(f"Unsupported VDF type {kind}; no files changed")
            entries.append((kind, key, value))

    result = obj()
    if offset != len(data):
        raise ValueError("Unexpected trailing VDF data")
    return result


def write_vdf(entries):
    output = bytearray()
    for kind, key, value in entries:
        output += bytes([kind]) + key.encode("utf-8", "surrogateescape") + b"\0"
        if kind == 0:
            output += write_vdf(value)
        elif kind == 1:
            output += value.encode("utf-8", "surrogateescape") + b"\0"
        else:
            output += value
    return bytes(output) + b"\x08"


def get(entries, key, kind):
    return next((v for k, name, v in entries if name == key and k == kind), None)


def put(entries, key, kind, value):
    for i, (_, name, _) in enumerate(entries):
        if name == key:
            entries[i] = (kind, key, value)
            return
    entries.append((kind, key, value))


def steam_running():
    for proc in Path("/proc").glob("[0-9]*"):
        try:
            if proc.stat().st_uid == os.getuid() and (proc / "comm").read_text().strip() == "steam":
                return True
        except (OSError, ProcessLookupError):
            pass
    return False


def user_config(steam, account):
    if account:
        if not account.isdigit():
            raise ValueError("Steam account ID must be the numeric userdata directory name")
        config = steam / "userdata" / account / "config"
        if not config.is_dir():
            raise ValueError("That Steam account has no config directory")
        return config
    configs = [config for config in (steam / "userdata").glob("[0-9]*/config") if config.is_dir()]
    if not configs:
        raise ValueError("No Steam account has signed in here yet")
    if len(configs) == 1:
        return configs[0]
    # Several accounts have signed in here: use the one that signed in last.
    # Steam no longer writes MostRecent, but still writes each sign-in's time.
    # loginusers.vdf contains flat per-account records. Do not print its contents.
    loginusers = steam / "config/loginusers.vdf"
    ranked = []
    if loginusers.exists():
        for steam_id, body in re.findall(r'"(\d{17})"\s*\{([^{}]*)\}', loginusers.read_text(errors="replace")):
            config = steam / "userdata" / str(int(steam_id) - 76561197960265728) / "config"
            if config in configs:
                most_recent = re.search(r'"MostRecent"\s*"1"', body, re.IGNORECASE) is not None
                stamp = re.search(r'"Timestamp"\s*"(\d+)"', body, re.IGNORECASE)
                ranked.append((most_recent, int(stamp.group(1)) if stamp else 0, str(config)))
    if any(most_recent or stamp for most_recent, stamp, _ in ranked):
        return Path(max(ranked)[2])

    # Without sign-in times: the account whose settings Steam saved last.
    def saved(config):
        settings = config / "localconfig.vdf"
        return (settings if settings.exists() else config).stat().st_mtime
    return max(configs, key=saved)


def atomic_write(path, content):
    fd, name = tempfile.mkstemp(prefix=".videoframevr-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        os.replace(name, path)
    finally:
        Path(name).unlink(missing_ok=True)


ARTWORK = {"p": "cover.png", "": "wide.png", "_hero": "hero.png",
           "_logo": "logo.png", "_icon": "icon-256.png"}


def runs(entry, executable):
    """Whether a shortcut runs executable. Steam quotes the path; other tools may not."""
    exe = get(entry, "Exe", 1) or ""
    return (exe[1:-1] if len(exe) > 1 and exe[0] == exe[-1] == '"' else exe) == str(executable)


def find_shortcut(steam, account, executable):
    """The account's config folder and the appid of this launcher's shortcut, or None."""
    config = user_config(Path(steam), account)
    path = config / "shortcuts.vdf"
    tree = read_vdf(path.read_bytes() if path.exists() else b"\x00shortcuts\x00\x08\x08")
    executable = Path(executable).absolute()
    for kind, _, value in get(tree, "shortcuts", 0) or []:
        if kind == 0 and runs(value, executable):
            appid = get(value, "appid", 2)
            return config, struct.unpack("<I", appid)[0] if appid else None
    return config, None


def change_shortcut(executable, artwork, name, steam, account=None, remove=False, dry_run=False):
    """Add or update this launcher's shortcut and artwork, or remove both.

    Other shortcuts are kept as they are. Returns the account config folder and appid.
    """
    executable = Path(executable).absolute()
    exe = f'"{executable}"'
    config = user_config(Path(steam), account)
    path = config / "shortcuts.vdf"
    original = path.read_bytes() if path.exists() else b"\x00shortcuts\x00\x08\x08"
    tree = read_vdf(original)
    shortcuts = get(tree, "shortcuts", 0)
    if shortcuts is None:
        raise ValueError("shortcuts.vdf has no shortcuts object")
    ours = [entry for entry in shortcuts if entry[0] == 0 and runs(entry[2], executable)]
    grid = config / "grid"
    images = {}
    if remove:
        appid = None
        for entry in ours:
            shortcuts.remove(entry)
            appid_bytes = get(entry[2], "appid", 2)
            if appid_bytes:
                appid = struct.unpack("<I", appid_bytes)[0]
                images.update({grid / f"{appid}{suffix}.png": None for suffix in ARTWORK})
    else:
        if not executable.is_file() or not os.access(executable, os.X_OK):
            raise ValueError("Executable is missing or not executable")
        if ours:
            entry = ours[0][2]
        else:
            indexes = [int(key) for _, key, _ in shortcuts if key.isdigit()]
            entry = []
            shortcuts.append((0, str(max(indexes, default=-1) + 1), entry))
        appid_bytes = get(entry, "appid", 2)
        appid = struct.unpack("<I", appid_bytes)[0] if appid_bytes else (
            zlib.crc32((exe + name).encode()) | 0x80000000)
        for key, value in {"appid": appid, "IsHidden": 0, "AllowDesktopConfig": 1,
                           "AllowOverlay": 1, "OpenVR": 1}.items():
            put(entry, key, 2, struct.pack("<I", value))
        for key, value in {"AppName": name, "Exe": exe,
                           "StartDir": f'"{executable.parent}"',
                           "icon": str(grid / f"{appid}_icon.png")}.items():
            put(entry, key, 1, value)
        if get(entry, "LaunchOptions", 1) is None:
            put(entry, "LaunchOptions", 1, "")
        if get(entry, "tags", 0) is None:
            put(entry, "tags", 0, [(1, "0", "VR")])
        images = {grid / f"{appid}{suffix}.png": (Path(artwork) / filename).read_bytes()
                  for suffix, filename in ARTWORK.items()}
    updated = write_vdf(tree)
    if dry_run:
        return config, appid
    if steam_running():
        raise ValueError("Exit Steam completely before changing the shortcut")
    if path.exists() and path.read_bytes() != original:
        raise ValueError("Steam shortcuts changed during setup; try again with Steam closed")
    grid.mkdir(exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    if path.exists():
        shutil.copy2(path, config / f"shortcuts.vdf.videoframevr-backup-{stamp}")
    for image, content in images.items():
        if content is None:
            image.unlink(missing_ok=True)
        else:
            atomic_write(image, content)
    atomic_write(path, updated)
    return config, appid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--artwork", type=Path)
    parser.add_argument("--name", default="VideoFrameVR")
    parser.add_argument("--steam-root", type=Path, default=Path.home() / ".local/share/Steam")
    parser.add_argument("--steam-user")
    parser.add_argument("--remove", action="store_true", help="Remove the shortcut and its artwork")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if not args.remove and not args.artwork:
        parser.error("--artwork is required when adding a shortcut")
    config, appid = change_shortcut(args.executable, args.artwork, args.name, args.steam_root,
                                    args.steam_user, args.remove, args.dry_run)
    action = "remove" if args.remove else "register"
    if args.dry_run:
        print(f"Would {action} {args.name}, appid {appid}, for Steam account {config.parent.name}")
    elif args.remove:
        print(f"Removed {args.name} and its artwork")
    else:
        print(f"Registered {args.name} with icon and all Steam artwork. Appid: {appid}")
        print("Start Steam. The shortcut runs natively; leave forced Proton compatibility off.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        raise SystemExit(str(error)) from error
