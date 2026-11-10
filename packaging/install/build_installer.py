#!/usr/bin/env python3
"""Turn a release archive into the single installer file users download.

The file is a .desktop entry, so Dolphin can open it after KDE's trust prompt.
Its comment lines carry the installer code and the archive in base64.
"""

import argparse
import base64
from datetime import datetime, timezone
import hashlib
import io
import json
from pathlib import Path
import tarfile
import zipfile

HERE = Path(__file__).resolve().parent

# Runs from the Exec line: unpack the helper and start the install dialogs.
BOOTSTRAP = '''\
import base64, hashlib, json, os, subprocess, sys, tempfile
path = sys.argv[1]
lines = [line for line in open(path) if line[:2] in ("#@", "#H")]
metadata = json.loads(lines[0][2:])
helper = b"".join(base64.b64decode(line[2:]) for line in lines[1:])
if hashlib.sha256(helper).hexdigest() != metadata["helper_sha256"]:
    sys.exit(subprocess.call(["zenity", "--error", "--title", "VideoFrameVR",
                              "--text", "The installer file is damaged. Download it again."]))
with tempfile.TemporaryDirectory() as temporary:
    pyz = os.path.join(temporary, "helper.pyz")
    with open(pyz, "wb") as output:
        output.write(helper)
    sys.exit(subprocess.call([sys.executable, pyz, "gui", path]))
'''

EXEC = "python3 -c \"import sys;exec(''.join(l[2:] for l in open(sys.argv[1]) if l[:2]=='#>'))\" %k"


def encoded(data, prefix):
    # 3072 bytes per line: each line decodes on its own.
    return "".join(prefix + base64.b64encode(data[i:i + 3072]).decode() + "\n"
                   for i in range(0, len(data), 3072))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path, help="Release archive from packaging/release")
    parser.add_argument("--output", type=Path, help="Output folder; defaults to the archive's folder")
    args = parser.parse_args()
    config = json.loads((HERE / "config.json").read_text())
    with tarfile.open(args.archive) as tar:
        member = next(m for m in tar if m.name.count("/") == 1 and m.name.endswith("/RELEASE.json"))
        release = json.load(tar.extractfile(member))
    if release["app_id"] != config["app_id"] or release["channel"] != "github":
        parser.error("The archive is not a GitHub release of " + config["app_name"])

    helper = io.BytesIO()
    helper.write(b"#!/usr/bin/env python3\n")
    with zipfile.ZipFile(helper, "w", zipfile.ZIP_DEFLATED) as package:
        package.writestr("__main__.py", "import installer\ninstaller.main()\n")
        package.write(HERE / "installer.py", "installer.py")
        package.write(HERE / "config.json", "config.json")
        package.write(HERE / "steam_client.py", "steam_client.py")
        for name in ["LICENSE", "README.md"]:
            package.write(HERE.parent / "frame-control" / name, "licenses/frame-control/" + name)
        package.write(HERE.parents[1] / "tools/install_steam_shortcut.py", "install_steam_shortcut.py")
    helper = helper.getvalue()
    archive = args.archive.read_bytes()
    metadata = {"schema": 1, "app_id": config["app_id"], "version": release["version"],
                "architecture": release["architecture"],
                "built_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                "helper_sha256": hashlib.sha256(helper).hexdigest(),
                "archive_sha256": hashlib.sha256(archive).hexdigest(), "archive_size": len(archive)}

    name = config["app_name"]
    output = (args.output or args.archive.parent) / f"Install-{name}-{release['version']}-{release['architecture']}.desktop"
    with output.open("w") as out:
        # A leading #! line stops KDE rewriting this large file when it marks it trusted.
        out.write("#!/usr/bin/env xdg-open\n[Desktop Entry]\nType=Application\n"
                  f"Name=Install {name}\nComment=Installs {name} {release['version']}\n"
                  f"Icon=system-software-install\nExec={EXEC}\nTerminal=false\n")
        out.write("#@" + json.dumps(metadata) + "\n")
        out.write("".join("#>" + line + "\n" for line in BOOTSTRAP.splitlines()))
        out.write(encoded(helper, "#H"))
        out.write(encoded(archive, "#A"))
    print(output)


if __name__ == "__main__":
    main()
