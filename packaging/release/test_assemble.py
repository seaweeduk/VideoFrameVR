"""Checks for source attribution carried into release bundles."""
from pathlib import Path
import json
import shutil
import tempfile
import unittest
from unittest.mock import patch

from assemble import assemble, frameplayerx_notices


APP = Path(__file__).resolve().parents[2]


class FramePlayerXNoticesTest(unittest.TestCase):
    def test_bundle_preserves_license_notice_and_scope(self):
        with tempfile.TemporaryDirectory() as directory:
            licenses = Path(directory) / "share/licenses"
            licenses.mkdir(parents=True)
            notice = frameplayerx_notices(APP, licenses)
            destination = licenses / "frameplayerx"
            self.assertEqual({p.name for p in destination.iterdir()},
                             {"LICENSE", "NOTICE", "README.md"})
            for name in ["LICENSE", "NOTICE", "README.md"]:
                self.assertEqual((destination / name).read_bytes(),
                                 (APP / "packaging/frameplayerx" / name).read_bytes())
            for text in ["remotesojourner", "yellkell", "Apache-2.0", "Q10C",
                         "mount-path parsing", "unescaping", "removable-drive detection",
                         "SteamOS-specific automount policy", "share/licenses/frameplayerx/"]:
                self.assertIn(text, notice)

    def test_missing_attribution_fails_the_build(self):
        for missing in ["LICENSE", "NOTICE", "README.md"]:
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                app = root / "app"
                source = app / "packaging/frameplayerx"
                source.mkdir(parents=True)
                for name in ["LICENSE", "NOTICE", "README.md"]:
                    if name != missing:
                        (source / name).write_text("fixture\n")
                licenses = root / "licenses"
                licenses.mkdir()
                with self.assertRaises(FileNotFoundError):
                    frameplayerx_notices(app, licenses)

    def test_release_and_corresponding_source_include_attribution(self):
        for architecture in ["aarch64", "x86_64"]:
            with self.subTest(architecture=architecture), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                app, bundle, work, sdk, cache = [root / name for name in
                                                ["app", "bundle", "work", "sdk", "cache"]]

                def write(path, text):
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(text)

                shutil.copytree(APP / "packaging/frameplayerx", app / "packaging/frameplayerx")
                write(app / "LICENSE", "GPL fixture\n")
                write(app / "packaging/linux/launch.sh", "#!/bin/sh\n")
                write(app / "packaging/release/fonts.conf", "<fontconfig/>\n")
                write(app / "packaging/release/sources.json", json.dumps({
                    "fonts": [{"name": "OFL.txt"}], "sources": []}))
                write(cache / "downloads/OFL.txt", "Font license fixture\n")
                write(cache / "downloads/librsvg_fixture.debian.tar.xz", "Crate fixture\n")
                for name in ["glslang-dev", "spirv-tools", "spirv-headers", "libxxhash-dev"]:
                    write(sdk / "usr/share/doc" / name / "copyright", "Dependency fixture\n")
                write(sdk / "usr/share/common-licenses/GPL-3", "GPL fixture\n")
                write(sdk / "var/lib/dpkg/status", "SDK fixture\n")
                write(work / "compiler.txt", "Compiler fixture\n")
                write(work / "config/fixture", "Build configuration fixture\n")
                metadata = {"app_id": "videoframevr", "version": "0.0.0",
                            "architecture": architecture, "channel": "stable",
                            "source_commit": "fixture", "modification_date": "2026-11-10"}
                # Dependency crate extraction is unrelated to application attribution.
                with patch("assemble.crate_notices", return_value=0):
                    source = assemble(bundle, app, work, sdk, cache,
                                      {"binaries": [], "sources": []}, metadata, "source.tar.gz")

                notices = (bundle / "THIRD-PARTY-NOTICES").read_text()
                self.assertIn("FramePlayerX, Copyright 2026 remotesojourner", notices)
                self.assertIn("mount-path parsing and unescaping", notices)
                self.assertIn("removable-drive detection", notices)
                self.assertIn("SteamOS-specific automount policy", notices)
                self.assertIn("share/licenses/frameplayerx/", notices)
                self.assertIn("Combined application: GPL-3.0", notices)
                self.assertNotIn("Application and repository additions: GPL-3.0", notices)
                for name in ["LICENSE", "NOTICE", "README.md"]:
                    expected = (APP / "packaging/frameplayerx" / name).read_bytes()
                    self.assertEqual((bundle / "share/licenses/frameplayerx" / name).read_bytes(),
                                     expected)
                    self.assertEqual((source / "app/packaging/frameplayerx" / name).read_bytes(),
                                     expected)


if __name__ == "__main__":
    unittest.main()
