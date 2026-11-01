#!/usr/bin/env python3
import json
from pathlib import Path
import tempfile
import unittest

from pagestore_endurance import Failure
from pagestore_fixed_replay import assert_separate, tree_hash, validate_fixture


class FixtureIntegrityTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / "fixture"
        self.root.mkdir()
        for name in ("store", "materializer"):
            (self.root / name).mkdir()
            (self.root / name / "input").write_bytes(name.encode())
        self.write_manifest()

    def tearDown(self):
        self.tmp.cleanup()

    def write_manifest(self):
        meta = {"version": 2, "target_lsn": "0/1234", "scale": 1, "checksums": {},
                "files": {name: tree_hash(self.root / name) for name in ("store", "materializer")}}
        (self.root / "fixture.json").write_text(json.dumps(meta))

    def test_exact_file_set_and_contents(self):
        self.assertEqual(validate_fixture(self.root)["target_lsn"], "0/1234")
        (self.root / "store" / "input").write_bytes(b"changed")
        with self.assertRaises(Failure):
            validate_fixture(self.root)
        self.write_manifest()
        (self.root / "materializer" / "extra").write_bytes(b"unexpected WAL")
        with self.assertRaises(Failure):
            validate_fixture(self.root)

    def test_empty_directories_and_permissions_are_part_of_fixture(self):
        (self.root / "store" / "empty").mkdir()
        with self.assertRaises(Failure):
            validate_fixture(self.root)
        self.write_manifest()
        (self.root / "store" / "empty").chmod(0o500)
        with self.assertRaises(Failure):
            validate_fixture(self.root)

    def test_links_and_invalid_lsn_are_rejected(self):
        (self.root / "store" / "link").symlink_to(self.root / "materializer", target_is_directory=True)
        with self.assertRaises(Failure):
            validate_fixture(self.root)
        (self.root / "store" / "link").unlink()
        meta = json.loads((self.root / "fixture.json").read_text())
        meta["target_lsn"] = "0/1234'; SELECT 1; --"
        (self.root / "fixture.json").write_text(json.dumps(meta))
        with self.assertRaises(Failure):
            validate_fixture(self.root)

    def test_work_directory_cannot_overlap_fixture(self):
        with self.assertRaises(Failure):
            assert_separate(self.root, self.root / "inside")
        with self.assertRaises(Failure):
            assert_separate(self.root, self.root.parent)
        assert_separate(self.root, self.root.parent / "other")


if __name__ == "__main__":
    unittest.main()
