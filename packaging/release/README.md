# Release builds

These scripts build the release archives in Valve's Steam Linux Runtime 4.0 SDK,
with every dependency pinned in `sources.json` and `debian.json`. They run on an
x86_64 Linux PC and build for `x86_64` (desktop) and `aarch64` (Steam Frame).

You only need them to reproduce a release. For a development build, see
[BUILDING.md](../../BUILDING.md).

## Requirements

Python 3.11 or newer, Bubblewrap, GNU tar, GNU cp, Git and readelf. `doctor`
checks that Bubblewrap can create user namespaces.

## Build

From a clean checkout of a release tag:

```sh
bash packaging/release/build.sh doctor
bash packaging/release/build.sh fetch --arch aarch64
bash packaging/release/build.sh build --version v0.1.0 --arch aarch64 --offline
```

Use the tag as `--version`. Downloads, dependency builds and output go under
`~/.cache/videoframevr-release/` (change it with `VIDEOFRAMEVR_RELEASE_ROOT`, or
`--cache` and `--output`). The finished files are in `output/artifacts/`:

- `videoframevr-<version>-<architecture>.tar.gz`, the app
- `videoframevr-source-<version>-<architecture>.tar.gz`, its complete source
- `SHA256SUMS`

To check an extracted app folder without running it:

```sh
bash packaging/release/build.sh check --bundle /path/to/release --arch aarch64 --offline
```

The check, which every build also runs, compares the app's libraries with the
first public Steam Linux Runtime 4.0 (`PLATFORM_VERSION` in `artifacts.py`), not
with the SDK's own version. The app starts on any 4.0 runtime, so a dependency
that an old one lacks has to fail here.

## Rebuilding from a source archive

A source archive holds the app's source in `app/`, every third-party source in
`upstream/`, and a record of the original build in `build-config/` and
`BUILD-INFO.json`. It has no `.git` directory, so make a commit in `app/` first:

```sh
cd app
git init
git add -f .
git -c user.name='Source rebuild' -c user.email='rebuild@localhost' commit -m 'Import delivered source'
```

Then copy `upstream/*` into your cache's `downloads/` folder, run `fetch` to get
the SDK, and run `build --offline`.

Don't copy `build-config/` over the recipe; it's a record, not an input. The
rebuild records your local commit instead of the original one, which stays in
`BUILD-INFO.json`.
