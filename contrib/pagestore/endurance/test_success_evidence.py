#!/usr/bin/env python3
"""Passing runs retain bounded diagnostic context before cleanup."""
from pathlib import Path
import tempfile
import unittest

import pagestore_endurance as endurance


class SuccessEvidenceTests(unittest.TestCase):
    def test_early_warning_survives_large_tail_and_logs_are_bounded(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "run"
            keep = Path(tmp) / "passed"
            root.mkdir()
            keep.mkdir()
            (root / "writer.log").write_bytes(
                b"WARNING: archive paused: WAL index behind\n" + b"x" * (600 * 1024) + b"\nend\n")
            (root / "daemon.log").write_bytes(b"refused historical recovery write\n")
            endurance.preserve_success_logs(root, keep)
            self.assertEqual((keep / "writer.log.tail").stat().st_size, 512 * 1024)
            self.assertIn(b"archive paused", (keep / "writer.log.warnings").read_bytes())
            self.assertIn(b"refused historical", (keep / "daemon.log.warnings").read_bytes())
            self.assertFalse((keep / "materializer.log.tail").exists())
            (root / "writer.log").write_text("clean\n")
            (root / "daemon.log").unlink()
            endurance.preserve_success_logs(root, keep)
            self.assertFalse((keep / "writer.log.warnings").exists())
            self.assertFalse((keep / "daemon.log.tail").exists())

    def test_warning_count_and_branch_count_are_bounded(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "run"
            keep = Path(tmp) / "passed"
            root.mkdir()
            keep.mkdir()
            (root / "writer.log").write_bytes(b"WARNING: " + b"x" * 2048 + b"\n")
            with (root / "writer.log").open("ab") as stream:
                stream.write(b"WARNING: repeated\n" * 1100)
            for i in range(8):
                (root / f"branch{i}.log").write_text("branch\n")
            endurance.preserve_success_logs(root, keep)
            self.assertEqual(len((keep / "writer.log.warnings").read_bytes().splitlines()), 1024)
            self.assertEqual(len(list(keep.glob("branch*.log.tail"))), 4)


if __name__ == "__main__":
    unittest.main()
