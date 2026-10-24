#!/usr/bin/env python3
"""Archive bounds must not be enabled before a durable recovery marker."""
from pathlib import Path
from types import SimpleNamespace
import tempfile
import subprocess
import sys
import unittest
from unittest.mock import Mock

import pagestore_endurance as endurance


class ArchiveLagTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.config = self.root / 'postgresql.conf'
        self.config.write_text('# bootstrap\n')
        self.run = object.__new__(endurance.Run)
        self.run.args = SimpleNamespace(materializer_max_lag_mb=32,
                                        wal_index_max_lag_mb=8, sync_timeout=30)
        self.run.writer = Mock(datadir=self.root)
        self.run.mat = Mock()
        self.run.sync_materializer = Mock()
        self.run.wait_for = Mock()
        self.run.event = Mock()

    def test_invalid_limit_rejected_before_provisioning(self):
        for option, value in [("materializer-max-lag-mb", "-1"),
                              ("wal-index-max-lag-mb", "2147483648")]:
            result = subprocess.run([sys.executable, endurance.__file__, "--build", "unused",
                                     "--root", str(self.root), f"--{option}={value}"],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn("must be between", result.stderr)

    def test_missing_marker_leaves_bootstrap_writer_unchanged(self):
        self.run.wait_for.side_effect = endurance.Failure('timeout', 'no durable marker')
        with self.assertRaises(endurance.Failure):
            self.run.configure_archive_lag_limits()
        self.assertEqual(self.config.read_text(), '# bootstrap\n')
        self.run.writer.stop.assert_not_called()
        self.run.writer.start.assert_not_called()

    def test_disabled_limits_do_not_restart_writer(self):
        self.run.args.materializer_max_lag_mb = 0
        self.run.args.wal_index_max_lag_mb = 0
        self.run.configure_archive_lag_limits()
        self.run.sync_materializer.assert_not_called()
        self.run.writer.stop.assert_not_called()
        self.assertEqual(self.config.read_text(), '# bootstrap\n')

    def test_wrong_effective_settings_are_a_finding(self):
        self.run.writer.sql.return_value = '0|8'
        with self.assertRaises(endurance.Failure) as caught:
            self.run.configure_archive_lag_limits()
        self.assertEqual(caught.exception.kind, 'archive_lag_config')
        self.run.event.assert_not_called()


if __name__ == '__main__':
    unittest.main()
