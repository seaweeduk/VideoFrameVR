#!/usr/bin/env python3
"""Content-addressed downloads and the pinned, rootless Steam Runtime SDK."""
import datetime
import hashlib
import json
from pathlib import Path
import subprocess
import urllib.parse
import urllib.request

SDK_VERSION = "4.0.20261007.263973"
SDK_REPOSITORY = "steamrt/steamrt4/sdk/arm64-on-amd64"
SDK_DIGEST = "713141e5e6644bae33260477d42e71d596e451b58c54274ae4b181fca8c7840c"
SDK_CREATED = "2026-10-07T19:59:51.855740+00:00"
REGISTRY = "https://registry.gitlab.steamos.cloud/v2/" + SDK_REPOSITORY
# The oldest runtime a release must start on: the first public Steam Linux
# Runtime 4.0. Steam can leave a runtime weeks behind, so launch.sh accepts any
# 4.0 and the closure audit checks each bundle against this Platform instead.
PLATFORM_VERSION = "4.0.20251111.181352"
IMAGE_BASE = "https://repo.steampowered.com/steamrt4/images/" + PLATFORM_VERSION + "/"
PLATFORM_DIGESTS = {
    "arm64": "a318183de381537c45fdc9bc67a7e1c3a6e1cf198fec37ce2a9fad1b76bb13f3",
    "amd64,i386": "f890e1f5964bb44054fc26872547f4e184cf5f5be934b03c4396d4640df13eac",
}


def sha256(path):
    with open(path, "rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def check_age(published):
    date = datetime.datetime.fromisoformat(published.replace("Z", "+00:00"))
    if datetime.datetime.now(datetime.timezone.utc) - date < datetime.timedelta(days=10):
        raise RuntimeError("Dependency has not cleared 10 days: " + published)


def fetch(url, path, digest, headers=None, offline=False):
    path = Path(path)
    if path.exists():
        if sha256(path) != digest:
            raise RuntimeError("Cached download checksum mismatch: " + str(path))
        return path
    if offline:
        raise RuntimeError("Not cached for offline build: " + str(path))
    path.parent.mkdir(parents=True, exist_ok=True)
    part = path.with_name(path.name + ".part")
    print("Downloading " + url, flush=True)
    request = urllib.request.Request(url, headers=headers or {})
    with urllib.request.urlopen(request, timeout=120) as response, part.open("wb") as output:
        while block := response.read(1024 * 1024):
            output.write(block)
    if sha256(part) != digest:
        raise RuntimeError("Download checksum mismatch: " + str(part))
    part.rename(path)
    return path


def sdk(cache, offline=False):
    """Extract only the reviewed image. Never run image entrypoints or root hooks."""
    check_age(SDK_CREATED)
    cache = Path(cache)
    root = cache / "sdk" / SDK_DIGEST / "rootfs"
    if (root.parent / "complete").exists():
        return root
    headers = {"Accept": "application/vnd.docker.distribution.manifest.v2+json"}
    if not offline:
        query = urllib.parse.urlencode({"service": "container_registry",
                                       "scope": "repository:" + SDK_REPOSITORY + ":pull"})
        with urllib.request.urlopen("https://gitlab.steamos.cloud/jwt/auth?" + query) as response:
            headers["Authorization"] = "Bearer " + json.load(response)["token"]
    manifest = fetch(REGISTRY + "/manifests/sha256:" + SDK_DIGEST,
                     cache / "downloads" / (SDK_DIGEST + ".json"), SDK_DIGEST, headers, offline)
    image = json.loads(manifest.read_text())
    layers = []
    for layer in image["layers"]:
        digest = layer["digest"].removeprefix("sha256:")
        layers.append(fetch(REGISTRY + "/blobs/" + layer["digest"],
                            cache / "downloads" / (digest + ".tar.gz"), digest, headers, offline))
    # The reviewed image has no whiteouts. Refuse an unexpected layer format,
    # rather than silently retaining files that an OCI layer meant to remove.
    root.mkdir(parents=True, exist_ok=True)
    for layer in layers:
        listing = subprocess.check_output(["tar", "-tzf", str(layer)], text=True)
        if any(Path(name).name.startswith(".wh.") for name in listing.splitlines()):
            raise RuntimeError("Unexpected OCI whiteout; use a container engine to inspect the SDK")
        subprocess.run(["tar", "--extract", "--gzip", "--file", str(layer), "--directory", str(root),
                        "--no-same-owner", "--no-same-permissions", "--exclude=./dev/*"], check=True)
    (root.parent / "manifest.json").write_text(manifest.read_text())
    (root.parent / "complete").touch()
    return root


def platform(cache, architecture, offline=False):
    check_age(SDK_CREATED)
    name = "arm64" if architecture == "aarch64" else "amd64,i386"
    digest = PLATFORM_DIGESTS[name]
    root = cache / "platform" / digest
    if (root / ".complete").exists():
        return root
    file = "com.valvesoftware.SteamRuntime.Platform-" + name + "-steamrt4-sysroot.tar.gz"
    archive = fetch(IMAGE_BASE + file, cache / "downloads" / (PLATFORM_VERSION + "-" + file), digest, offline=offline)
    root.mkdir(parents=True, exist_ok=True)
    subprocess.run(["tar", "-xzf", str(archive), "-C", str(root), "--no-same-owner", "--no-same-permissions",
                    "--anchored", "--exclude=./dev/*", "--exclude=dev/*"], check=True)
    (root / ".complete").touch()
    return root


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cache", type=Path)
    parser.add_argument("--offline", action="store_true")
    args = parser.parse_args()
    print(sdk(args.cache, args.offline))
