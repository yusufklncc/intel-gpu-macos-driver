# SPDX-License-Identifier: MIT
"""The upstream trees a release of the Windows app ships beside the builder (upstream/<project>-<commit>/): used in
place of the cache, checked against the manifest, never replaced by a download.

  python3 -I tests/test_upstream.py
"""
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from intelxe_efibuild import pins, upstream  # noqa: E402

COMMIT = "0" * 40


class Shipped(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="upstream-test-")
        self.tree = os.path.join(self.dir, "Demo-" + COMMIT)
        os.makedirs(os.path.join(self.tree, "Scripts"))
        for rel, text in (("LICENSE", "BSD-3-Clause\n"), ("Scripts/a.py", "print('a')\n")):
            with open(os.path.join(self.tree, *rel.split("/")), "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
        manifest = upstream.manifest_text(self.tree)
        with open(os.path.join(self.dir, "Demo.sha256"), "w", encoding="utf-8", newline="\n") as f:
            f.write(manifest)
        projects = {"Demo": {"commit": COMMIT, "manifest": "Demo.sha256", "archive": "https://example.invalid/demo.tgz",
                             "manifest_sha256": pins.sha256_bytes(manifest.encode())}}
        self.patches = [mock.patch.object(upstream, "UPSTREAM_DIR", self.dir),
                        mock.patch.object(upstream, "pinned", lambda: projects),
                        mock.patch.object(pins, "http_get", side_effect=AssertionError("no download for a shipped tree"))]
        for p in self.patches:
            p.start()

    def tearDown(self):
        for p in self.patches:
            p.stop()
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_the_shipped_tree_is_used_without_a_download(self):
        cache = os.path.join(self.dir, "cache")
        self.assertEqual(upstream.fetch("Demo", cache), self.tree)
        self.assertFalse(os.path.exists(os.path.join(cache, "upstream")))

    def test_a_changed_shipped_tree_stops_and_stays(self):
        with open(os.path.join(self.tree, "Scripts", "a.py"), "a", encoding="utf-8") as f:
            f.write("print('changed')\n")
        with self.assertRaises(pins.PinError):
            upstream.fetch("Demo", os.path.join(self.dir, "cache"))
        self.assertTrue(os.path.isfile(os.path.join(self.tree, "Scripts", "a.py")))

    def test_an_extra_file_in_the_shipped_tree_stops(self):
        with open(os.path.join(self.tree, "Scripts", "json.py"), "w", encoding="utf-8") as f:
            f.write("")
        with self.assertRaises(pins.PinError):
            upstream.copy_for_build("Demo", os.path.join(self.dir, "work"), os.path.join(self.dir, "cache"))


if __name__ == "__main__":
    unittest.main()
