#!/usr/bin/env python3
"""Release metadata, notices, fonts and matching source delivery."""
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import tarfile
import tomllib

from artifacts import SDK_DIGEST, SDK_REPOSITORY, SDK_VERSION, sha256
from closure import rooted


def archive(directory, output, epoch, root_name):
    """Stable ordering, metadata and gzip header. No host usernames or timestamps."""
    def normalize(info):
        info.uid = info.gid = 0
        info.uname = info.gname = ""
        info.mtime = epoch
        info.mode = 0o755 if info.isdir() or info.mode & 0o111 else 0o644
        return info
    part = output.with_name(output.name + ".part")
    with part.open("wb") as file, gzip.GzipFile(filename="", mode="wb", fileobj=file, mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as tar:
            tar.add(directory, arcname=root_name, filter=normalize)
    part.rename(output)


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


# Permissive grants that must travel with binaries, as they appear in file headers.
PERMISSIVE = ("Redistribution and use in source and binary forms", "Permission is hereby granted",
              "Permission to use, copy, modify", "Independent JPEG Group")
SOURCE_SUFFIXES = {".c", ".h", ".cpp", ".m", ".S", ".asm", ".inc", ".py", ".glsl", ".comp"}


def leading_comment(path):
    """A source file's header comment: its first /* */ block or its leading //, ; or # lines."""
    text = path.read_text(errors="replace")[:16384]
    lines, block = [], False
    for line in text.splitlines():
        stripped = line.strip()
        if block:
            lines.append(line)
            block = "*/" not in stripped
        elif stripped.startswith("/*"):
            lines.append(line)
            block = "*/" not in stripped[2:]
        elif stripped.startswith(("//", ";", "#!", "# ")) or stripped in ("#", ""):
            lines.append(line)
        else:
            break
    return "\n".join(lines).strip()


def per_file_notices(source, output):
    """Every header in a source tree that carries a permissive grant, with its path."""
    entries = []
    for path in sorted(source.rglob("*")):
        if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
            continue
        header = leading_comment(path)
        if any(marker in " ".join(header.split()) for marker in PERMISSIVE):
            entries.append("== " + str(path.relative_to(source)) + "\n" + header)
    output.write_text("Per-file notices from this component's source tree, as written in each file.\n"
                      "Files without a permissive grant are under the component's main licence.\n\n"
                      + "\n\n".join(entries) + "\n")
    return len(entries)


def crate_notices(debian_archive, destination):
    """The licence files and declared licence of each Rust crate vendored into librsvg."""
    destination.mkdir(parents=True)
    declared = {}
    with tarfile.open(debian_archive) as tar:
        for member in tar:
            parts = member.name.split("/")
            if len(parts) != 4 or parts[1] != "missing-sources" or not member.isfile():
                continue
            crate, name = parts[2], parts[3]
            if name == "Cargo.toml":
                package = tomllib.loads(tar.extractfile(member).read().decode(errors="replace")).get("package", {})
                declared[crate] = {key: package[key] for key in ("version", "license") if isinstance(package.get(key), str)}
            elif name.upper().startswith(("LICENSE", "LICENCE", "COPYING", "NOTICE", "COPYRIGHT")):
                (destination / crate).mkdir(exist_ok=True)
                (destination / crate / name).write_bytes(tar.extractfile(member).read())
    if not declared:
        raise RuntimeError("No vendored crates found in " + debian_archive.name)
    (destination / "CRATES.txt").write_text(
        "Rust crates compiled into librsvg (Debian's missing-sources), their versions and declared\n"
        "licences. Each crate's own licence files are in the folder of the same name.\n\n"
        + "".join(f"{crate} {info.get('version', '?')}: {info.get('license', 'see its licence files')}\n"
                  for crate, info in sorted(declared.items())))
    return len(declared)


def assemble(bundle, app, work, sdk, cache, supplement, metadata, source_name):
    licenses = bundle / "share/licenses"
    licenses.mkdir(parents=True)
    shutil.copy2(app / "LICENSE", bundle / "LICENSE")
    launcher = app / "packaging/linux/launch.sh"
    if not launcher.is_file():
        raise RuntimeError("Release launcher is missing: packaging/linux/launch.sh")
    shutil.copy2(launcher, bundle / "launch.sh")
    (bundle / "launch.sh").chmod(0o755)
    fonts = bundle / "share/videoframevr/fonts"
    fonts.mkdir(parents=True)
    lock = json.loads((app / "packaging/release/sources.json").read_text())
    for font in lock["fonts"]:
        target = fonts / font["name"]
        shutil.copy2(cache / "downloads" / font["name"], target)
    shutil.copy2(app / "packaging/release/fonts.conf", fonts / "fonts.conf")
    (licenses / "noto").mkdir()
    shutil.copy2(fonts / "OFL.txt", licenses / "noto/OFL.txt")
    notices = [
        "VideoFrameVR third-party notices",
        "Forked from tuxVRdome by PandaSoda45: https://github.com/tzyvkq9eoyrg/tuxVRdome",
        "Original import 4c14ef86e88ca7132998dd35d13aed8e29429204. Application and repository additions: GPL-3.0.",
        "This build includes fork modifications as of " + metadata["modification_date"] + ".",
        "Source and library-replacement directions: SOURCE.txt.",
        "Noto Sans Condensed Medium/SemiBold, Noto Sans Mono Medium: notofonts/noto-fonts "
        "ffebf8c1ee449e544955a7e813c54f9b73848eac; SIL OFL-1.1; unmodified.",
    ]
    patterns = {
        "libsmb2": ["COPYING", "LICENCE-LGPL-2.1.txt"],
        "libssh": ["COPYING", "BSD"],
        "libudfread": ["COPYING"],
        "libbluray": ["COPYING"],
        "json": ["LICENSE.MIT"],
        "mpv": ["LICENSE*", "Copyright"],
        "ffmpeg": ["COPYING*", "LICENSE*"],
        # No licence file: each header carries its MIT notice.
        "nv-codec-headers": ["README", "include/ffnvcodec/*.h"],
    }
    descriptions = {
        "libsmb2": "LGPL-2.1-or-later with per-file public-domain/RSA notices; shared, replaceable; Kerberos/GSSAPI/examples disabled; local patches, if any, recorded in BUILD-INFO.json",
        "libssh": "LGPL-2.1-or-later with BSD-2-Clause parts (BSD); shared, replaceable; unmodified; server, GSSAPI, zlib and examples disabled; OpenSSL from the Steam Linux Runtime",
        "libudfread": "LGPL-2.1-or-later; shared, replaceable; unmodified; examples disabled",
        "libbluray": ("LGPL-2.1-or-later; shared, replaceable; unmodified; used to read disc playlists only; "
                      "BD-J (Java), tools, FreeType, fontconfig and libxml2 disabled; no AACS or BD+ library bundled"),
        "json": "MIT; compiled-in header library, unmodified",
        "mpv": ("upstream GPL-2.0-or-later, combined under GPLv3; both builds modified by mpv-mvc.patch and the "
                "packaging/mpv/mvc sources (3D Blu-ray, GPL-3.0); Frame build also by mpv-v4l2-reset.patch, "
                "mpv-v4l2-hardware-only.patch, mpv-v4l2-interrupt.patch, mpv-export-release.patch and dmabuf_ubwc sources"),
        "ffmpeg": ("upstream LGPL-2.1-or-later fork. Desktop build: unmodified, LGPL-2.1-or-later. "
                   "Frame build: modified by ffmpeg-iris.patch, ffmpeg-main10.patch, ffmpeg-capture-pool.patch, "
                   "ffmpeg-drain-seek.patch, ffmpeg-staged-start.patch and ffmpeg-capture-interrupt.patch "
                   "(GPL-3.0), so this "
                   "modified FFmpeg is distributed under GPL-3.0-or-later as a whole (LGPL-2.1 section 3; "
                   "configured with --enable-gpl --enable-version3)"),
        "nv-codec-headers": "MIT (in each header); desktop build only, compiled into FFmpeg's NVIDIA decoding, unmodified",
    }
    for item in lock["sources"]:
        if item["name"] == "nv-codec-headers" and metadata["architecture"] != "x86_64":
            continue
        source = work / "source" / item["directory"]
        destination = licenses / item["name"]
        destination.mkdir()
        copied = 0
        for pattern in patterns[item["name"]]:
            for file in source.glob(pattern):
                target = destination / file.name
                if file.is_dir():
                    shutil.copytree(file, target)
                else:
                    shutil.copy2(file, target)
                copied += 1
        if not copied:
            raise RuntimeError("Missing licence files for " + item["name"])
        if item["name"] == "libsmb2":
            shutil.copy2(app / "packaging/libsmb2/NOTICE", destination / "NOTICE")
        if item["name"] in ("mpv", "ffmpeg"):
            per_file_notices(source, destination / "PER-FILE-NOTICES")
        notices.append(f"{item['name']} {item['version']}: {item['url']}; {descriptions[item['name']]}")
    # Debian's copyright includes librsvg's vendored Rust grants. Preserve the
    # complete file rather than inventing one top-level licence for all crates.
    packages = {x["name"]: x for x in supplement["binaries"] if x["role"] == "runtime"}
    for name in ["glslang-dev", "spirv-tools", "spirv-headers", "libxxhash-dev"]:
        packages.setdefault(name, {"version": "see SDK package inventory", "homepage": "https://sources.debian.org/"})
    for name, item in sorted(packages.items()):
        copyright_file = rooted(sdk, Path("usr/share/doc") / name / "copyright")
        if not copyright_file.is_file():
            raise RuntimeError("Missing dependency copyright: " + name)
        destination = licenses / name
        destination.mkdir()
        shutil.copy2(copyright_file, destination / "copyright")
        notices.append(f"{name} {item['version']}: {item['homepage']}; unmodified Debian/SDK code; terms in share/licenses/{name}/copyright")
    rsvg = sorted((cache / "downloads").glob("librsvg_*.debian.tar.xz"))
    if len(rsvg) != 1:
        raise RuntimeError("Expected one librsvg Debian source archive, found " + str(len(rsvg)))
    crate_notices(rsvg[0], licenses / "librsvg2-2/crates")
    notices.append("librsvg's vendored Rust crates (including ICU4X under Unicode-3.0): each crate's licence "
                   "files and declared licence in share/licenses/librsvg2-2/crates/.")
    notices.append("mpv and FFmpeg files with their own BSD, MIT, ISC or IJG grants: share/licenses/mpv/PER-FILE-NOTICES "
                   "and share/licenses/ffmpeg/PER-FILE-NOTICES.")
    notices.append("This software is based in part on the work of the Independent JPEG Group.")
    common = sdk / "usr/share/common-licenses"
    shutil.copytree(common, licenses / "common-licenses")
    (bundle / "THIRD-PARTY-NOTICES").write_text("\n\n".join(notices) + "\n")
    release = {key: metadata[key] for key in ["app_id", "version", "architecture", "channel", "source_commit"]}
    release.update(schema=1, runtime="steamrt4")
    write_json(bundle / "RELEASE.json", release)
    metadata.update(sdk_version=SDK_VERSION, sdk_image=SDK_REPOSITORY + "@sha256:" + SDK_DIGEST,
                    sources=lock, debian=supplement)
    write_json(bundle / "BUILD-INFO.json", metadata)
    (bundle / "BUILD-INFO").write_text("\n".join(f"{k}={v}" for k, v in metadata.items() if isinstance(v, (str, int))) + "\n")
    (bundle / "SOURCE.txt").write_text(
        "Matching complete source: " + source_name + "\n"
        "Distributors must publish that archive beside this binary, at no additional charge.\n"
        "It contains the exact application tree, dependency archives, Debian source packages\n"
        "including librsvg's vendor sources, local patches, recipe and build metadata.\n"
        "Build from app/ using packaging/release/build.sh. The README's source-archive\n"
        "rebuild section explains Git initialization and offline download-cache setup.\n"
        "Shared libraries in lib/ and bin/videoframevr may be replaced by compatible modified builds.\n"
        "No signing keys, active-release switch or binary allowlist is required. Keep LGPL\n"
        "libraries replaceable. Run launch.sh in the matching Steam Linux Runtime 4.0.\n")
    source = work / "corresponding-source"
    source.mkdir()
    shutil.copytree(app, source / "app", symlinks=True)
    upstream = source / "upstream"
    upstream.mkdir()
    for item in lock["sources"]:
        shutil.copy2(cache / "downloads" / item["file"], upstream / item["file"])
    for item in lock["fonts"]:
        shutil.copy2(cache / "downloads" / item["name"], upstream / item["name"])
    for item in supplement["sources"]:
        for file in item["files"]:
            shutil.copy2(cache / "downloads" / file["file"], upstream / file["file"])
    shutil.copytree(work / "config", source / "build-config")
    shutil.copy2(work / "compiler.txt", source / "build-config/compiler.txt")
    shutil.copy2(sdk / "var/lib/dpkg/status", source / "build-config/sdk-packages")
    shutil.copy2(bundle / "BUILD-INFO.json", source / "BUILD-INFO.json")
    return source


def manifest(bundle):
    records = []
    for path in sorted(bundle.rglob("*")):
        if path.is_symlink():
            records.append({"path": str(path.relative_to(bundle)), "symlink": str(path.readlink())})
        elif path.is_file() and path.name != "FILES.json":
            records.append({"path": str(path.relative_to(bundle)), "sha256": sha256(path), "size": path.stat().st_size})
    write_json(bundle / "FILES.json", {"schema": 1, "files": records})
