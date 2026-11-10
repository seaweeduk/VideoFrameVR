# FramePlayerX attribution

VideoFrameVR includes material adapted from
[FramePlayerX](https://github.com/remotesojourner/frameplayerx), Copyright 2026
remotesojourner, under the Apache License, Version 2.0. [LICENSE](LICENSE) and
[NOTICE](NOTICE) preserve the upstream license and notices. The combined
VideoFrameVR application remains GPLv3; this does not remove the upstream
Apache-2.0 notices or conditions.

## Q10C support

Upstream revision: [`0358020e13bc3185fe66a3ebf88d5ad9c50f24f6`](https://github.com/remotesojourner/frameplayerx/commit/0358020e13bc3185fe66a3ebf88d5ad9c50f24f6),
18 October 2026.

| VideoFrameVR file | Upstream file | Credited material |
| --- | --- | --- |
| `packaging/frame/mpv/dmabuf_ubwc.c` | [`crates/fp-render/src/q10c.rs`](https://github.com/remotesojourner/frameplayerx/blob/0358020e13bc3185fe66a3ebf88d5ad9c50f24f6/crates/fp-render/src/q10c.rs) | Carrier images, TP10 descriptor rewriting, band splitting, reference-layout validation and associated comments |
| `packaging/frame/mpv/dmabuf_ubwc.comp` | [`crates/fp-render/shaders/q10c.wgsl`](https://github.com/remotesojourner/frameplayerx/blob/0358020e13bc3185fe66a3ebf88d5ad9c50f24f6/crates/fp-render/shaders/q10c.wgsl) | TP10-to-P010 conversion and associated comments |
| `src/direct_video.cpp` | `crates/fp-render/src/q10c.rs`, linked above | Carrier-image and TP10 descriptor-rewriting machinery |

VideoFrameVR modified the conversion implementation for C/mpv integration,
DMA-BUF import, packed buffer output and synchronization. Its direct Q10C path
imports decoder buffers and samples TP10 in the scene shaders, rather than
copying the picture into renderer-owned memory and converting it to P010 first.
The direct path also uses overlapping luma bands for texture sampling. This
credit is for the underlying carrier/descriptor machinery, not the entire
renderer or zero-copy pipeline.

## Removable drives

Upstream revision: [`0c43903b78e3c1312ddaee3ce1674817a2799c0b`](https://github.com/remotesojourner/frameplayerx/commit/0c43903b78e3c1312ddaee3ce1674817a2799c0b),
21 October 2026.

FramePlayerX is credited for mount-path parsing and unescaping, removable-drive
detection and the SteamOS-specific automount policy in
`src/removable_drives.cpp` and `src/removable_drives.h`.

| Upstream file | Credited material |
| --- | --- |
| [`crates/fp-core/src/volume.rs`](https://github.com/remotesojourner/frameplayerx/blob/0c43903b78e3c1312ddaee3ce1674817a2799c0b/crates/fp-core/src/volume.rs) | Mount-list parsing, octal unescaping and associated comments |
| [`crates/frameplayerx/src/drives.rs`](https://github.com/remotesojourner/frameplayerx/blob/0c43903b78e3c1312ddaee3ce1674817a2799c0b/crates/frameplayerx/src/drives.rs) | Card/USB identification, filesystem eligibility, UDisks mounting options and SteamOS-specific explanations |
| [`crates/frameplayerx/src/app/tasks.rs`](https://github.com/remotesojourner/frameplayerx/blob/0c43903b78e3c1312ddaee3ce1674817a2799c0b/crates/frameplayerx/src/app/tasks.rs) | Polling and attempted-mount tracking |

The unescaping helper includes closely matching wording and code structure.
The broader credit recognizes the matching drive-identification rules,
filesystem allowlist, ext4 handling, mounting options and SteamOS rationale.
These routines also appeared in earlier FramePlayer versions before being split
into the files above.

VideoFrameVR uses GIO D-Bus to enumerate UDisks objects and request mounts, with
its own C++ worker and change-delivery interface. FramePlayerX enumerates sysfs
and udev records and invokes `udisksctl` to mount drives.

## Dates and distribution

The credited material was present in VideoFrameVR's initial public commit
`9e33deea479855bdc2d189870eb4b00d4dd01e27`, authored 3 November 2026. The
references above identify matching upstream revisions, not a verified record of
which revisions were consulted during development. Attribution was added on
10 November 2026.

Releases include this directory at `share/licenses/frameplayerx/` and list the
credit in `THIRD-PARTY-NOTICES`. Corresponding source archives include it in the
application tree. The final paragraph of the preserved upstream NOTICE refers
to FramePlayerX's own third-party bundle, not VideoFrameVR's bundle; VideoFrameVR's
other bundled components are listed separately in its `THIRD-PARTY-NOTICES`.
