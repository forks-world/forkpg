#!/usr/bin/env python3
"""A completed timeline deletion must not hide growing space or owner debt."""
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import Mock

import pagestore_endurance as endurance


class BranchReclaimTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.run = object.__new__(endurance.Run)
        self.run.root = Path(self.temp.name)
        self.run.args = SimpleNamespace(branch_reclaim_cycles=2,
                                        branch_reclaim_slack_mb=1, sync_timeout=1)
        self.run.writer, self.run.mat = Mock(), Mock()
        self.run.writer.sql.return_value = 't'
        self.run.check_health = Mock()
        self.run.checksums = Mock(return_value={'acct': 'same'})
        self.run.event = Mock()
        self.run.sync_materializer = Mock()
        self.run.branches = []
        self.next_timeline = 0
        def create(_):
            self.next_timeline += 1
            self.run.branches = [(self.next_timeline, None, None)]
        self.run.create_branch = Mock(side_effect=create)
        self.run.retire_branch = Mock(side_effect=lambda: self.run.branches.clear())
        self.run.store_kb = Mock(side_effect=[10, 100, 20, 10, 100, 30])
        self.owners = dict(owner_count=3, page_history_owners=2, wal_owners=3,
                           wal_index_owners=2, retention_poisoned=False)
        self.run.inspect = Mock(return_value=json.dumps(self.owners))
        def wait(what, fn, timeout):
            if not fn():
                raise endurance.Failure('timeout', what)
        self.run.wait_for = wait

    def test_repeated_deletion_within_warmup_bounds(self):
        self.run.branch_reclaim_cycles()
        self.assertEqual(self.run.retire_branch.call_count, 2)
        completed = [c.kwargs for c in self.run.event.call_args_list
                     if c.args[0] == 'branch_reclaim_complete']
        self.assertEqual([c['warmup'] for c in completed], [True, False])
        self.assertEqual([c['baseline_store_kb'] for c in completed], [20, 20])

    def test_deleted_timeline_still_fails_for_excess_residual_allocation(self):
        self.run.store_kb.side_effect = [10, 100, 20, 10, 100, 2000]
        with self.assertRaises(endurance.Failure):
            self.run.branch_reclaim_cycles()
        self.assertEqual(self.run.retire_branch.call_count, 2)

    def test_deleted_timeline_still_fails_for_leaked_owner(self):
        self.run.inspect.side_effect = [json.dumps(self.owners),
                                       json.dumps(dict(self.owners, owner_count=4))]
        with self.assertRaises(endurance.Failure):
            self.run.branch_reclaim_cycles()

    def test_missing_delete_api_is_not_a_successful_churn_run(self):
        self.run.writer.sql.return_value = 'f'
        with self.assertRaises(endurance.Failure):
            self.run.branch_reclaim_cycles()
        self.run.create_branch.assert_not_called()


if __name__ == '__main__':
    unittest.main()
