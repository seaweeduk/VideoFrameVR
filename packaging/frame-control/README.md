# Frame Control attribution

Portions of `packaging/install/steam_client.py` are adapted from
[Frame Control](https://github.com/saphid/frame-control), Copyright 2026 saphid,
under the MIT license. [LICENSE](LICENSE) preserves the upstream copyright and
permission notice.

Upstream file:
[`frame/android/steam_shortcuts.py`](https://github.com/saphid/frame-control/blob/1b39fc171829f692807fe2d3e658905a15857122/frame/android/steam_shortcuts.py)
at revision [`1b39fc171829f692807fe2d3e658905a15857122`](https://github.com/saphid/frame-control/commit/1b39fc171829f692807fe2d3e658905a15857122),
28 September 2026.

The credited portions cover the Steam CDP/WebSocket helper, shortcut-details
subscription and artwork integration, including subscription timeout and
deferred cleanup, filling missing artwork and clearing artwork before removal.
VideoFrameVR modifies these for loopback-only debugger discovery, explicit
socket cleanup, executable-based shortcut matching, incomplete-description
handling and the signed-in account's artwork directory.

The VideoFrameVR helper was introduced in commit
`84c12a0ee1c4b36d74abed37a9809798b99e54b5`, 7 November 2026. The upstream revision
above identifies matching earlier material. Attribution was added on
10 November 2026.

Application releases include this directory at `share/licenses/frame-control/`
and list the credit in `THIRD-PARTY-NOTICES`. The installer's embedded helper
includes these files under `licenses/frame-control/`, and `steam_client.py`
also carries the complete MIT notice. Corresponding source archives include
this directory in the application tree.
