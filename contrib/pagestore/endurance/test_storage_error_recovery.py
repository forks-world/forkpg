#!/usr/bin/env python3
"""Regression oracles for deterministic storage fault evidence and amcheck."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock

import pagestore_storage_error_test as probe


class StorageErrorTests(unittest.TestCase):
    def test_report_can_be_created_before_json_is_complete(self):
        with tempfile.TemporaryDirectory() as root:
            run = object.__new__(probe.StorageErrorRun)
            run.report = Path(root) / 'hit.json'
            self.assertIsNone(run.read_fault_report())
            for pending in ('', '{"operation":', '{"operation":"pwrite"'):
                run.report.write_text(pending)
                self.assertIsNone(run.read_fault_report())
            hit = dict(operation='pwrite', errno=28, pid=123)
            run.report.write_text(json.dumps(hit))
            self.assertEqual(run.read_fault_report(), hit)

    def test_amcheck_is_required_even_when_endurance_sampling_skips_it(self):
        run = object.__new__(probe.StorageErrorRun)
        run.writer, run.mat = object(), object()
        run.resolve_prepared = Mock()
        run.sync_materializer = Mock(return_value='0/123')
        run.rng = Mock()
        run.rng.random.return_value = 0.99  # Ordinary endurance would skip amcheck.
        run.checksums = Mock(return_value={'acct': 'same'})
        run.check_invariants = Mock()
        run.event = Mock()
        run.timing, run.stats = {}, {'verifies': 0}
        for why in ('before storage fault', 'after I/O failure and daemon crash'):
            run.verify_materializer(why)
            run.check_invariants.assert_any_call(run.writer, True)
            run.check_invariants.assert_any_call(run.mat, True)
            self.assertTrue(run.event.call_args.kwargs['amcheck'])
            run.check_invariants.reset_mock()
        run.rng.random.assert_not_called()


if __name__ == '__main__':
    unittest.main()
