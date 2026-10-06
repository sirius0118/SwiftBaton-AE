#!/usr/bin/env python3
"""Exercise flattened-download repair without Git or executable script modes."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("metadata", ROOT / "scripts/restore-source-metadata.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class MetadataTest(unittest.TestCase):
    def test_flattened_links_modes_and_missing_parent(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "headers").mkdir()
            (root / "headers/fpu.h").write_text("fixture\n")
            (root / "asm").write_text("headers\n")
            (root / "build.sh").write_text("#!/bin/sh\nexit 0\n")
            (root / "build.sh").chmod(0o644)
            metadata = {"symlinks": {"asm": "headers", "uapi/common": "../headers"},
                        "executables": ["build.sh"]}
            changed, errors = MODULE.restore(root, metadata)
            self.assertEqual(len(changed), 3)
            self.assertFalse(errors)
            self.assertFalse((root / "asm").is_symlink())
            changed, errors = MODULE.restore(root, metadata, repair=True)
            self.assertFalse(errors)
            self.assertEqual((root / "asm/fpu.h").read_text(), "fixture\n")
            self.assertEqual((root / "uapi/common/fpu.h").read_text(), "fixture\n")
            self.assertEqual((root / "build.sh").stat().st_mode & 0o111, 0o111)
            self.assertEqual(MODULE.restore(root, metadata, repair=True), ([], []))

    def test_does_not_replace_modified_file(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "asm").write_text("important source\n")
            changed, errors = MODULE.restore(root, {"symlinks": {"asm": "headers"}, "executables": []}, True)
            self.assertTrue(errors)
            self.assertFalse(changed)
            self.assertEqual((root / "asm").read_text(), "important source\n")

    def test_does_not_write_through_external_parent(self):
        with tempfile.TemporaryDirectory() as temp, tempfile.TemporaryDirectory() as outside:
            root = Path(temp)
            (root / "parent").symlink_to(outside)
            metadata = {"symlinks": {"parent/link": "inside"}, "executables": []}
            changed, errors = MODULE.restore(root, metadata, True)
            self.assertTrue(errors)
            self.assertFalse(changed)
            self.assertFalse((Path(outside) / "link").exists())


if __name__ == "__main__":
    unittest.main()
