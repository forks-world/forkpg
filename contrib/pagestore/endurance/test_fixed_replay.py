#!/usr/bin/env python3
import json
from pathlib import Path
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from pagestore_endurance import Failure
from pagestore_fixed_replay import assert_separate, installed_module, tree_hash, validate_fixture, workload


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

    def test_module_uses_pg_config_package_directory(self):
        lib = self.root / "lib" / "postgresql"
        lib.mkdir(parents=True)
        module = lib / "pagestore.so"
        module.write_bytes(b"module")
        run = SimpleNamespace(bin=self.root, env={})
        with patch("pagestore_fixed_replay.subprocess.run", return_value=SimpleNamespace(stdout=str(lib) + "\n")):
            self.assertEqual(installed_module(run), module)

    def test_workload_terminates_and_reaps_on_health_failure(self):
        class Process:
            returncode = None
            def poll(self):
                return self.returncode
            def terminate(self):
                self.returncode = -15
            def wait(self, timeout=None):
                self.waited = True
        process = Process()
        def failed():
            raise Failure("disk_free", "budget exceeded")
        run = SimpleNamespace(bin=self.root, writer=SimpleNamespace(port=1),
                              args=SimpleNamespace(clients=1), naccts=1, ndocs=1,
                              root=self.root, env={}, check_health=failed)
        with patch("pagestore_fixed_replay.subprocess.Popen", return_value=process):
            with self.assertRaises(Failure):
                workload(run, 1)
        self.assertEqual(process.returncode, -15)
        self.assertTrue(process.waited)


if __name__ == "__main__":
    unittest.main()
