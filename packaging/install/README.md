# Installer

`build_installer.py` turns a release archive from
[packaging/release](../release/README.md) into the one-file installer that users
download:

```sh
python3 packaging/install/build_installer.py path/to/videoframevr-<version>-aarch64.tar.gz [--output DIR]
```

It prints the path of `Install-VideoFrameVR-<version>-<architecture>.desktop`.
Attach that file to the GitHub release; the in-app updater looks for that name.

The installer is a desktop entry with `installer.py` and the release archive
embedded in it, so installing needs no network. It shows its dialogs with zenity
and needs only Python 3 and zenity, which SteamOS includes. It installs to:

```
~/Applications/VideoFrameVR/
  videoframevr.sh    launcher that Steam and the menu entry run
  helper.pyz         updater, Add to Steam and Uninstall
  current -> releases/<version>-<archive hash>/
~/.local/share/applications/videoframevr.desktop
```

The Steam shortcut is added through the running Steam client when it offers its
debugging interface on `127.0.0.1:8080`, as Steam on the Steam Frame always does
(`steam_client.py`). Steam then adds it to the signed-in account, with its
artwork and the VR flag, without restarting. A shortcut added some other way
(such as Desktop Mode's **Add to Steam**) is found by its launcher path and
completed: VR flag, icon, missing artwork, and the app's name if it still has
the file's.

Otherwise the installer edits `shortcuts.vdf` for the account that signed in
last. Steam rewrites that file when it exits, so the installer can only change
it while Steam is stopped; that's why adding to Steam then restarts it.
Problems go to `~/.local/state/videoframevr-installer.log`.
