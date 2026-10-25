#!/usr/bin/env python3
"""Space budgets must include compute-local WAL, not only the page store."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import pagestore_endurance as endurance


class SpaceBudgetTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.store = self.root / "store"
        self.store.mkdir()
        self.args = SimpleNamespace(min_free_gb=0.001, max_run_gb=0.01, max_store_gb=0.01,
                                    max_rss_mb=0, max_fds=0)

    def test_compute_wal_trips_run_budget_while_store_is_small(self):
        wal = self.root / "writer" / "pg_wal"
        wal.mkdir(parents=True)
        (wal / "segment").write_bytes(b"x" * (2 << 20))
        self.args.max_run_gb = 0.001
        with self.assertRaises(endurance.Failure) as caught:
            endurance.space_snapshot(self.root, self.store, self.args)
        self.assertEqual(caught.exception.kind, "run_size")

    def test_free_space_checked_before_size_scan(self):
        with patch.object(endurance.shutil, "disk_usage", return_value=SimpleNamespace(free=0)), \
             patch.object(endurance, "directory_kb") as scan:
            with self.assertRaises(endurance.Failure) as caught:
                endurance.space_snapshot(self.root, self.store, self.args)
        self.assertEqual(caught.exception.kind, "disk_free")
        scan.assert_not_called()

    def test_store_limit_remains_independent(self):
        (self.store / "segment").write_bytes(b"x" * (2 << 20))
        self.args.max_store_gb = 0.001
        with self.assertRaises(endurance.Failure) as caught:
            endurance.space_snapshot(self.root, self.store, self.args)
        self.assertEqual(caught.exception.kind, "store_size")

    def test_provisioning_without_store_is_measured(self):
        self.store.rmdir()
        (self.root / ".hidden").write_bytes(b"x" * 8192)
        result = endurance.space_snapshot(self.root, self.store, self.args)
        self.assertGreaterEqual(result["run_kb"], 8)
        self.assertEqual(result["store_total_kb"], 0)
        json.dumps(result)

    def test_unmeasurable_directory_does_not_become_zero(self):
        with patch.object(endurance.subprocess, "run", return_value=SimpleNamespace(
                returncode=1, stdout="", stderr="permission denied")):
            with self.assertRaises(ValueError):
                endurance.directory_kb(self.root)

    def test_sampler_fails_closed_after_three_bad_measurements(self):
        run = object.__new__(endurance.Run)
        run.root, run.store, run.args = self.root, self.store, self.args
        run.round = 0
        run.stats = {}
        run.daemon = None
        run.daemon_expected_up = False
        run.async_failure = None
        run.computes = lambda: []
        run.stop_sampler = SimpleNamespace(wait=lambda _: False)
        self.args.sample_interval = 0.01
        with patch.object(endurance, "space_snapshot", side_effect=ValueError("du failed")) as scan:
            run.sample_loop()
        self.assertEqual(scan.call_count, 3)
        self.assertEqual(run.async_failure.kind, "space_unmeasurable")
        records = [json.loads(line) for line in (self.root / "metrics.jsonl").read_text().splitlines()]
        self.assertEqual(len(records), 3)
        self.assertTrue(all("space_error" in record for record in records))

    def test_free_space_rechecked_after_successful_sample(self):
        with patch.object(endurance.shutil, "disk_usage", side_effect=[
                SimpleNamespace(free=1 << 30), SimpleNamespace(free=0)]):
            endurance.space_snapshot(self.root, self.store, self.args)
            with self.assertRaises(endurance.Failure) as caught:
                endurance.space_snapshot(self.root, self.store, self.args)
        self.assertEqual(caught.exception.kind, "disk_free")

    def test_invalid_budget_rejected_before_provisioning(self):
        for option, value in [("max-run-gb", "nan"), ("min-free-gb", "-1"),
                              ("sample-interval", "0"), ("max-store-gb", "inf")]:
            result = subprocess.run([sys.executable, endurance.__file__, "--build", "unused",
                                     "--root", str(self.root), f"--{option}={value}"],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertIn("finite and greater than zero", result.stderr)


if __name__ == "__main__":
    unittest.main()
