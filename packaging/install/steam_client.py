# Portions adapted from saphid's Frame Control, frame/android/steam_shortcuts.py
# at 1b39fc171829f692807fe2d3e658905a15857122.
# https://github.com/saphid/frame-control
# Modified for VideoFrameVR's loopback discovery, shortcut matching, socket
# cleanup and account-specific artwork; present by 2026-11-07, credited 2026-11-10.
# See packaging/frame-control/ for provenance and the upstream license.
#
# MIT License
#
# Copyright (c) 2026 saphid
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

"""Change non-Steam shortcuts through the running Steam client, without restarting it.

Steam on the Steam Frame always runs with CEF remote debugging on loopback, and
its SharedJSContext page has the client's own shortcut functions. Steam then
adds the shortcut to the account that is signed in and saves shortcuts.vdf
itself. On other computers this is usually off; available() is then False and
the installer edits shortcuts.vdf instead. Python standard library only.
"""

import base64
import json
import os
from pathlib import Path
import socket
import struct
import urllib.request

DEVTOOLS = "http://127.0.0.1:8080/json"
SHORTCUT_APP_TYPE = 1073741824
# Steam's custom artwork types and the files Steam keeps for them in grid/.
ARTWORK = {0: ("cover.png", "p"), 1: ("hero.png", "_hero"), 2: ("logo.png", "_logo"), 3: ("wide.png", "")}
STEAM_ID64_BASE = 76561197960265728


class ClientError(Exception):
    pass


def target():
    """The SharedJSContext debugger address, or None when Steam doesn't offer one."""
    try:
        with urllib.request.urlopen(DEVTOOLS, timeout=3) as response:
            targets = json.loads(response.read(1024 ** 2))
    except Exception:
        return None
    # Anything else may listen on this port: accept only Steam's page.
    for entry in targets if isinstance(targets, list) else []:
        if not isinstance(entry, dict) or entry.get("title") != "SharedJSContext":
            continue
        url = entry.get("webSocketDebuggerUrl")
        if isinstance(url, str) and url.startswith("ws://127.0.0.1:8080/"):
            return url
    return None


def available():
    return target() is not None


class Socket:
    """A WebSocket client for one request and its reply on loopback (RFC 6455)."""

    def __init__(self, url, timeout):
        path = url[len("ws://127.0.0.1:8080"):]
        self.connection = socket.create_connection(("127.0.0.1", 8080), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        self.connection.sendall((f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nUpgrade: websocket\r\n"
                                 f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                                 "Sec-WebSocket-Version: 13\r\n\r\n").encode())
        reply = b""
        while b"\r\n\r\n" not in reply:
            chunk = self.connection.recv(4096)
            if not chunk:
                raise ClientError("Steam closed the connection")
            reply += chunk
        head, self.pending = reply.split(b"\r\n\r\n", 1)
        if b" 101 " not in head.split(b"\r\n", 1)[0]:
            raise ClientError("Steam refused the connection")

    def close(self):
        self.connection.close()

    def read(self, size):
        while len(self.pending) < size:
            chunk = self.connection.recv(65536)
            if not chunk:
                raise ClientError("Steam closed the connection")
            self.pending += chunk
        data, self.pending = self.pending[:size], self.pending[size:]
        return data

    def send(self, text):
        data = text.encode()
        size = len(data)
        if size < 126:
            head = struct.pack(">BB", 0x81, 0x80 | size)
        elif size < 65536:
            head = struct.pack(">BBH", 0x81, 0x80 | 126, size)
        else:
            head = struct.pack(">BBQ", 0x81, 0x80 | 127, size)
        mask = os.urandom(4)
        masked = bytes(byte ^ mask[i % 4] for i, byte in enumerate(data))
        self.connection.sendall(head + mask + masked)

    def receive(self):
        message = b""
        while True:
            first, second = self.read(2)
            size = second & 0x7F
            if size == 126:
                size = struct.unpack(">H", self.read(2))[0]
            elif size == 127:
                size = struct.unpack(">Q", self.read(8))[0]
            payload = self.read(size)
            opcode = first & 0x0F
            if opcode == 8:
                raise ClientError("Steam closed the connection")
            if opcode in (0, 1, 2):
                message += payload
                if first & 0x80:
                    return message.decode()


def evaluate(script, timeout=30):
    """Run JavaScript in Steam's SharedJSContext and return its (awaited) value."""
    url = target()
    if url is None:
        raise ClientError("Steam's client interface is not available")
    connection = Socket(url, timeout)
    try:
        connection.send(json.dumps({"id": 1, "method": "Runtime.evaluate", "params": {
            "expression": script, "awaitPromise": True, "returnByValue": True}}))
        while True:
            reply = json.loads(connection.receive())
            if reply.get("id") == 1:
                break
    finally:
        connection.close()
    if "error" in reply:
        raise ClientError("Steam: " + str(reply["error"])[:300])
    result = reply.get("result", {})
    if "exceptionDetails" in result:
        details = result["exceptionDetails"]
        raise ClientError("Steam: " + str(details.get("exception", {}).get("description") or details.get("text"))[:300])
    return result.get("result", {}).get("value")


# Defines find(): the appid of the shortcut that runs EXE, or null. Steam keeps the
# program path in quotes; compare without them to also match other ways of adding it.
# A shortcut whose details don't arrive could be this one, so that is an error.
FIND = """
const unquote = s => (s || "").replace(/^"(.*)"$/, "$1");
const details = id => appDetailsStore.GetAppDetails(id) || new Promise(done => {
  let registration;
  const timer = setTimeout(() => { if (registration) registration.unregister(); done(null); }, 3000);
  registration = SteamClient.Apps.RegisterForAppDetails(id, value => {
    clearTimeout(timer); setTimeout(() => registration && registration.unregister()); done(value); });
});
async function find() {
  const shortcuts = appStore.allApps.filter(app => app.app_type === %d);
  const all = await Promise.all(shortcuts.map(async app => [app.appid, await details(app.appid)]));
  const match = all.find(([, value]) => value && unquote(value.strShortcutExe) === EXE);
  if (match) return match[0];
  if (all.some(([, value]) => !value)) throw new Error("Steam didn't describe every shortcut");
  return null;
}
""" % SHORTCUT_APP_TYPE


def find(executable):
    """The appid of the shortcut that runs executable, or None."""
    return evaluate("(async () => {\nconst EXE = %s;\n%s\nreturn await find();\n})()"
                    % (json.dumps(str(executable)), FIND))


def account_grid(steam):
    """The signed-in account's grid folder, where Steam keeps custom artwork."""
    steam_id = evaluate("App.m_CurrentUser.strSteamID")
    return Path(steam) / "userdata" / str(int(steam_id) - STEAM_ID64_BASE) / "config/grid"


def missing_artwork(grid, appid):
    return [kind for kind, (_, suffix) in ARTWORK.items()
            if not any((grid / f"{appid}{suffix}.{extension}").exists() for extension in ("png", "jpg", "jpeg"))]


def add(executable, name, artwork, steam):
    """Add the shortcut, or complete one that exists: VR flag, icon, artwork Steam
    doesn't have, and this name if it still has the file's. Returns (appid, added)."""
    executable = Path(executable)
    icon = Path(artwork) / "icon-256.png"
    appid = find(executable)
    added = appid is None
    if added:
        appid = evaluate("SteamClient.Apps.AddShortcut(%s, %s, '', '')"
                         % (json.dumps(name), json.dumps(str(executable))))
        if not isinstance(appid, int):
            raise ClientError("Steam didn't add the shortcut")
    appid &= 0xFFFFFFFF
    try:
        kinds = list(ARTWORK) if added else missing_artwork(account_grid(steam), appid)
    except (OSError, ValueError, TypeError):
        kinds = list(ARTWORK)
    images = [[kind, base64.b64encode((Path(artwork) / ARTWORK[kind][0]).read_bytes()).decode()] for kind in kinds]
    done = evaluate("""(async () => {
      const id = %d, name = %s, file = %s, added = %s;
      const overview = appStore.GetAppOverviewByAppID(id);
      if (added || (overview && [file, ""].includes(overview.display_name)))
        SteamClient.Apps.SetShortcutName(id, name);
      SteamClient.Apps.SetShortcutStartDir(id, %s);
      SteamClient.Apps.SetShortcutIcon(id, %s);
      SteamClient.Apps.SetShortcutIsVR(id, true);
      for (const [kind, data] of %s)
        await SteamClient.Apps.SetCustomArtworkForApp(id, data, "png", kind);
      return true;
    })()""" % (appid, json.dumps(name), json.dumps(executable.name), json.dumps(added),
               json.dumps(f'"{executable.parent}"'), json.dumps(str(icon)), json.dumps(images)), timeout=60)
    if done is not True:
        raise ClientError("Steam didn't finish setting up the shortcut")
    return appid, added


def remove(executable):
    """Remove the shortcut and its custom artwork. Returns False when there was none."""
    appid = find(executable)
    if appid is None:
        return False
    done = evaluate("""(async () => {
      const id = %d;
      for (const kind of [0, 1, 2, 3])
        try { await SteamClient.Apps.ClearCustomArtworkForApp(id, kind); } catch (error) {}
      SteamClient.Apps.RemoveShortcut(id);
      return true;
    })()""" % appid)
    if done is not True:
        raise ClientError("Steam didn't remove the shortcut")
    return True
