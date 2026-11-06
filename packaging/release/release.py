#!/usr/bin/env python3
"""Local-only Steam Linux Runtime release builder. No deployment or network publication."""
import argparse
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform as host_platform
import re
import shutil
import subprocess
import sys

from artifacts import PLATFORM_DIGESTS, SDK_CREATED, SDK_DIGEST, SDK_VERSION, check_age, fetch, platform, sdk, sha256
from assemble import archive, assemble, manifest, write_json
from closure import audit
from debian import supplement

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
DEFAULT_ROOT = Path(os.environ.get("VIDEOFRAMEVR_RELEASE_ROOT") or Path.home() / ".cache/videoframevr-release")


def run(command, **kwargs):
    return subprocess.run([str(x) for x in command], check=True, **kwargs)


def sandbox(root, work, downloads, source=None, writable_root=False):
    command = ["bwrap", "--die-with-parent", "--new-session", "--unshare-all",
               "--bind" if writable_root else "--ro-bind", str(root), "/",
               "--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp",
               "--bind", str(work), "/work", "--ro-bind", str(downloads), "/downloads",
               "--clearenv", "--setenv", "PATH", "/usr/bin:/bin:/usr/sbin:/sbin",
               "--setenv", "HOME", "/work/home", "--setenv", "LC_ALL", "C.UTF-8",
               "--setenv", "PYTHONDONTWRITEBYTECODE", "1"]
    if source:
        command += ["--ro-bind", str(source), "/src"]
        # /work also contains the captured app tree. Protect that alias too.
        if source.is_relative_to(work):
            command += ["--ro-bind", str(source), str(Path("/work") / source.relative_to(work))]
    return command


def doctor():
    missing = [x for x in ["bwrap", "tar", "readelf", "git", "cp"] if not shutil.which(x)]
    print("Host: " + host_platform.machine())
    print("SDK: steamrt4 " + SDK_VERSION + " sha256:" + SDK_DIGEST)
    print("Build route: local x86_64 cross-SDK, ARM64 target; no QEMU or headset build")
    if host_platform.machine() != "x86_64":
        raise RuntimeError("This recipe currently needs an x86_64 Linux build host")
    if missing:
        raise RuntimeError("Missing local tools: " + ", ".join(missing) + ". Install only after owner approval.")
    run(["bwrap", "--ro-bind", "/", "/", "--unshare-all", "--proc", "/proc", "--dev", "/dev", "/bin/true"])
    print("Rootless user/network namespaces: usable")


def download(cache, architecture, offline, recipe=HERE):
    root = sdk(cache, offline)
    runtime = platform(cache, architecture, offline)
    lock = json.loads((recipe / "sources.json").read_text())
    for item in lock["sources"] + lock["fonts"]:
        if not item.get("cooldown_waiver"):
            check_age(item["published"])
        fetch(item["url"], cache / "downloads" / item.get("file", item.get("name")),
              item["sha256"], offline=offline)
    extras = supplement(cache, "arm64" if architecture == "aarch64" else "amd64", offline,
                        recipe / "debian.json")
    return root, runtime, extras


def snapshot(destination, allow_dirty):
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip()
    state = subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=all"], cwd=REPO, text=True)
    if state and not allow_dirty:
        raise RuntimeError("Release requires a clean checkout. Use --allow-dirty only for an explicitly marked local candidate.")
    epoch = int(subprocess.check_output(["git", "show", "-s", "--format=%ct", "HEAD"], cwd=REPO))
    paths = subprocess.check_output(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], cwd=REPO).split(b"\0")
    digest = hashlib.sha256()
    destination.mkdir()
    for raw in sorted(set(paths)):
        if not raw:
            continue
        relative = Path(os.fsdecode(raw))
        path = REPO / relative
        if not path.exists() and not path.is_symlink():
            continue
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        digest.update(raw + b"\0")
        if path.is_symlink():
            link = os.readlink(path)
            if Path(link).is_absolute() or not (path.parent / link).resolve().is_relative_to(REPO):
                raise RuntimeError("Source has an external symlink: " + str(relative))
            target.symlink_to(link)
            digest.update(b"link\0" + os.fsencode(link))
        elif path.is_file():
            # Read once. The same captured bytes feed compilation and source delivery.
            data = path.read_bytes()
            target.write_bytes(data)
            target.chmod(0o755 if path.stat().st_mode & 0o111 else 0o644)
            digest.update(b"file\0" + str(target.stat().st_mode & 0o777).encode() + b"\0" + data)
        else:
            raise RuntimeError("Unsupported source entry: " + str(relative))
    after = subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=all"], cwd=REPO, text=True)
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip()
    dirty = bool(state or after or head != revision)
    if dirty and not allow_dirty:
        raise RuntimeError("Checkout changed while capturing the release source")
    return revision, epoch, "dirty-local-candidate" if dirty else "clean", digest.hexdigest()


def prepare_sdk(cache, base, extras):
    key = hashlib.sha256((SDK_DIGEST + json.dumps(extras, sort_keys=True)).encode()).hexdigest()
    directory = cache / "prepared-sdk" / key
    root = directory / "rootfs"
    if not (directory / "complete").exists():
        if root.exists():
            raise RuntimeError("Interrupted SDK preparation: " + str(directory) + ". Inspect it, then remove that directory before retrying.")
        directory.mkdir(parents=True)
        run(["cp", "-a", "--reflink=auto", str(base), str(root)])
        for name in ["work", "downloads", "src"]:
            (root / name).mkdir(exist_ok=True)
        command = sandbox(root, directory, cache / "downloads", writable_root=True)
        for item in extras["binaries"]:
            run(command + ["dpkg-deb", "-x", "/downloads/" + item["file"], "/"])
        (directory / "complete").touch()
    return root


def dependency_key(app, architecture):
    digest = hashlib.sha256((SDK_DIGEST + architecture).encode())
    paths = [app / "packaging/release/build-sdk.sh", app / "packaging/release/sources.json",
             app / "packaging/release/debian.json", app / "src/video_export.h"]
    paths += sorted((app / "packaging/frame").glob("*.patch"))
    paths += sorted((app / "packaging/mpv").glob("*.patch"))
    paths += sorted((app / "packaging/mpv/mvc").glob("*.[ch]"))
    paths += sorted((app / "packaging/libsmb2").glob("*.patch"))
    paths += sorted((app / "packaging/frame/mpv").glob("dmabuf_ubwc.*"))
    for path in paths:
        digest.update(str(path.relative_to(app)).encode() + b"\0" + path.read_bytes())
    return digest.hexdigest()


def copy_dependencies(source, destination):
    for name in ["prefix", "source", "config"]:
        run(["cp", "-a", "--reflink=auto", source / name, destination / name])
    for name in ["compiler.txt", ".dependencies-complete"]:
        shutil.copy2(source / name, destination / name)


def build(args):
    if not args.version:
        raise RuntimeError("--version is required, use the actual release tag, never 'latest'")
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,95}", args.version) or args.version.lower() == "latest":
        raise RuntimeError("Invalid release version")
    doctor()
    output = args.output.resolve()
    cache = args.cache.resolve()
    if output.is_relative_to(REPO) or cache.is_relative_to(REPO):
        raise RuntimeError("Keep output/cache outside the source repository")
    output.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    with (cache / "build.lock").open("w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("Another local release build holds " + str(cache / "build.lock")) from None
        name = f"videoframevr-{args.version}-{args.arch}"
        work = output / "work" / name
        if work.exists() or (output / "releases" / args.version / args.arch).exists():
            raise RuntimeError("Release output already exists; choose a new version or output directory: " + str(work))
        work.mkdir(parents=True)
        revision, epoch, state, source_digest = snapshot(work / "app", args.allow_dirty)
        if not (work / "app/packaging/linux/launch.sh").is_file():
            raise RuntimeError("Missing packaging/linux/launch.sh release launcher")
        base, runtime, extras = download(cache, args.arch, args.offline, work / "app/packaging/release")
        root = prepare_sdk(cache, base, extras)
        dep_key = dependency_key(work / "app", args.arch)
        dependencies = cache / "dependencies" / dep_key
        if (dependencies / ".dependencies-complete").exists():
            print("Reusing checked recipe dependencies: " + dep_key, flush=True)
            copy_dependencies(dependencies, work)
        command = sandbox(root, work, cache / "downloads", work / "app")
        runtime_debs = work / "runtime-debs"
        runtime_debs.mkdir()
        for item in extras["binaries"]:
            if item["role"] == "runtime":
                run(command + ["dpkg-deb", "-x", "/downloads/" + item["file"], "/work/runtime-debs/" + item["name"]])
        try:
            commit = revision[:12] + ("" if state == "clean" else "-dirty")
            run(command + ["--setenv", "SOURCE_DATE_EPOCH", str(epoch),
                           "--setenv", "VIDEOFRAMEVR_RELEASE_VERSION", args.version,
                           "--setenv", "VIDEOFRAMEVR_RELEASE_COMMIT", commit, "/bin/bash",
                           "/src/packaging/release/build-sdk.sh", args.arch, str(args.jobs)])
        finally:
            if (work / ".dependencies-complete").exists() and not dependencies.exists():
                dependencies.mkdir(parents=True)
                copy_dependencies(work, dependencies)
        bundle = work / "bundle"
        result = audit(bundle, runtime, args.arch)
        write_json(bundle / "BUNDLE-CLOSURE.json", result)
        if result["errors"]:
            raise RuntimeError("Bundle closure failed:\n" + "\n".join(result["errors"]))
        # Cross ARM64 execution belongs to the later disposable-profile headset
        # gate. Do not confuse a successful link/closure audit with runtime proof.
        metadata = {"app_id": "videoframevr", "version": args.version, "architecture": args.arch,
                    "channel": args.channel, "source_commit": revision, "source_state": state,
                    "source_tree_sha256": source_digest, "source_date_epoch": epoch,
                     "modification_date": (datetime.datetime.now(datetime.timezone.utc) if state != "clean" else
                                           datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc)).date().isoformat(),
                    "build_recipe_sha256": sha256(work / "app/packaging/release/build-sdk.sh"),
                    "dependency_key": dep_key,
                    "sdk_created": SDK_CREATED,
                    "platform_sysroot_sha256": PLATFORM_DIGESTS["arm64" if args.arch == "aarch64" else "amd64,i386"],
                    "compiler": (work / "compiler.txt").read_text(),
                    "patches": [{"path": str(path.relative_to(work / "app")), "sha256": sha256(path),
                                 "applied": args.arch == "aarch64"}
                                for path in sorted((work / "app/packaging/frame").glob("*.patch"))],
                    "converter_sources": [{"path": str(path.relative_to(work / "app")), "sha256": sha256(path)}
                                          for path in [work / "app/src/video_export.h"] +
                                          sorted((work / "app/packaging/frame/mpv").glob("dmabuf_ubwc.*"))],
                    "runtime_execution": "unverified; requires matching SLR and OpenXR hardware",
                    "desktop_hardware_decode": "not enabled by this initial source recipe"}
        metadata["patches"] += [{"path": str(path.relative_to(work / "app")), "sha256": sha256(path),
                                 "applied": True}
                                for path in sorted((work / "app/packaging/libsmb2").glob("*.patch"))]
        source_name = f"videoframevr-source-{args.version}-{args.arch}.tar.gz"
        source = assemble(bundle, work / "app", work, root, cache, extras, metadata, source_name)
        manifest(bundle)
        artifacts = output / "artifacts"
        artifacts.mkdir(exist_ok=True)
        # Source is archived first. A missing notice/source never yields a published binary.
        archive(source, artifacts / source_name, epoch, f"videoframevr-source-{args.version}-{args.arch}")
        archive(bundle, artifacts / (name + ".tar.gz"), epoch, name)
        sums = []
        for architecture in ["aarch64", "x86_64"]:
            for prefix in ["videoframevr", "videoframevr-source"]:
                file = artifacts / f"{prefix}-{args.version}-{architecture}.tar.gz"
                if file.exists():
                    sums.append(sha256(file) + "  " + file.name)
        (artifacts / "SHA256SUMS").write_text("\n".join(sums) + "\n")
        destination = output / "releases" / args.version / args.arch
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(bundle), destination)
        print("Staged, not activated: " + str(destination))
        print("Artifacts: " + str(artifacts))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["doctor", "fetch", "build", "check"])
    parser.add_argument("--version", help="Required release tag for build")
    parser.add_argument("--arch", choices=["aarch64", "x86_64"], default="aarch64")
    parser.add_argument("--channel", choices=["github", "steam"], default="github")
    parser.add_argument("--cache", type=Path, default=DEFAULT_ROOT / "cache")
    parser.add_argument("--output", type=Path, default=DEFAULT_ROOT / "output")
    parser.add_argument("--bundle", type=Path, help="Extracted release directory to check")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    parser.add_argument("--offline", action="store_true", help="Use cached, checksum-verified downloads only")
    parser.add_argument("--allow-dirty", action="store_true", help="Capture and mark an unreleased local candidate")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if args.cache.resolve().is_relative_to(REPO) or args.output.resolve().is_relative_to(REPO):
        parser.error("Keep cache/output outside the source repository")
    if args.command == "doctor":
        doctor()
    elif args.command == "fetch":
        doctor()
        args.cache.mkdir(parents=True, exist_ok=True)
        with (args.cache / "build.lock").open("w") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise RuntimeError("Another local fetch/build holds the cache lock") from None
            download(args.cache.resolve(), args.arch, args.offline)
    elif args.command == "check":
        if not args.bundle:
            parser.error("check requires --bundle")
        result = audit(args.bundle, platform(args.cache, args.arch, args.offline), args.arch)
        print(json.dumps(result, indent=2))
        return bool(result["errors"])
    else:
        build(args)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.CalledProcessError, OSError) as error:
        print("release: " + str(error), file=sys.stderr)
        sys.exit(1)
