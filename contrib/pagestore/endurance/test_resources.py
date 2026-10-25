#!/usr/bin/env python3
"""Measure real child processes and handle restart/sampling races explicitly."""
import os
import tempfile
from types import SimpleNamespace
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import Mock, patch

import pagestore_resources as resources
import pagestore_endurance as endurance


class ResourceTests(unittest.TestCase):
    def setUp(self):
        self.child = subprocess.Popen([sys.executable, '-c',
            "import os,time; files=[open('/dev/null') for _ in range(40)]; "
            "data=bytearray(12*1024*1024);print('ready',flush=True);time.sleep(60)"],
            stdout=subprocess.PIPE, text=True)
        self.addCleanup(self.cleanup_child)
        self.assertEqual(self.child.stdout.readline(), 'ready\n')

    def cleanup_child(self):
        if self.child.poll() is None:
            self.child.kill()
        self.child.wait(timeout=5)
        self.child.stdout.close()

    def test_backend_memory_and_descriptors_are_included(self):
        result = resources.snapshot({'compute': (os.getpid(), lambda: True)})
        self.assertFalse(result['resource_errors'])
        group = result['resource_groups']['compute']
        self.assertGreaterEqual(group['processes'], 2)
        self.assertGreater(group['rss_kb'], result['procs']['compute']['rss_kb'] + 12 * 1024)
        self.assertGreaterEqual(group['fds'], result['procs']['compute']['fds'] + 40)
        self.assertGreater(result['procs']['compute']['start_ticks'], 0)

    def test_exited_generation_is_not_a_recycled_pid_sample(self):
        result = resources.snapshot({'old': (os.getpid(), lambda: False)})
        self.assertEqual(result['resource_totals']['processes'], 0)
        self.assertFalse(result['resource_errors'])

    def test_exit_during_sampling_discards_whole_group(self):
        result = resources.snapshot({'old': (os.getpid(), Mock(side_effect=[True, False]))})
        self.assertEqual(result['resource_totals']['processes'], 0)

    def test_live_root_permissions_are_not_silently_zero_usage(self):
        original = os.listdir
        def denied(path):
            if str(path) == f'/proc/{self.child.pid}/fd':
                raise PermissionError('denied')
            return original(path)
        with patch.object(resources.os, 'listdir', side_effect=denied):
            result = resources.snapshot({'child': (self.child.pid, lambda: True)})
        self.assertTrue(result['resource_errors'])
        self.assertNotIn('child', result['resource_groups'])

    def test_transient_backend_esrch_does_not_stop_root_sampling(self):
        original = resources.identity
        count = 0
        def identity(pid):
            nonlocal count
            if pid == self.child.pid:
                count += 1
                if count > 1:
                    raise ProcessLookupError('child exited during read')
            return original(pid)
        with patch.object(resources, 'identity', side_effect=identity):
            result = resources.snapshot({'compute': (os.getpid(), lambda: True)})
        self.assertIn('compute', result['procs'])
        self.assertFalse(result['resource_errors'])

    def test_overlapping_roots_are_not_double_counted_in_total(self):
        result = resources.snapshot({'a': (self.child.pid, lambda: True),
                                     'b': (self.child.pid, lambda: True)})
        self.assertEqual(result['resource_totals']['processes'], 1)
        self.assertEqual(result['resource_totals']['fds'], result['resource_groups']['a']['fds'])


class BudgetSamplingTests(unittest.TestCase):
    def test_pidfd_closed_during_restart_is_not_a_sampler_crash(self):
        handle = Mock()
        pg = SimpleNamespace(expected_up=True, postmaster=handle)
        def closing():
            pg.expected_up = False
            raise OSError('pidfd closed during stop')
        handle.exited.side_effect = closing
        self.assertFalse(endurance.compute_resources_alive(pg, handle))

    def test_pidfd_failure_on_still_owned_generation_is_not_ignored(self):
        handle = Mock()
        handle.exited.side_effect = OSError('unexpected pidfd error')
        pg = SimpleNamespace(expected_up=True, postmaster=handle)
        with self.assertRaises(OSError):
            endurance.compute_resources_alive(pg, handle)

    def test_repeated_live_sampling_failure_is_not_ignored_with_limits(self):
        with tempfile.TemporaryDirectory() as temp:
            run = object.__new__(endurance.Run)
            run.root, run.store = Path(temp), Path(temp) / 'store'
            run.args = SimpleNamespace(sample_interval=.01, max_rss_mb=1, max_fds=0)
            run.round, run.stats, run.async_failure = 0, {}, None
            run.daemon, run.daemon_expected_up = None, False
            run.computes = lambda: []
            run.stop_sampler = SimpleNamespace(wait=lambda _: False)
            sample = dict(procs={}, resource_groups={},
                          resource_totals=dict(rss_kb=100, fds=0, processes=1),
                          resource_errors={'daemon': 'live root unavailable'})
            with patch.object(endurance, 'space_snapshot', return_value={}), \
                 patch.object(resources, 'snapshot', return_value=sample) as scan:
                run.sample_loop()
            self.assertEqual(scan.call_count, 3)
            self.assertEqual(run.async_failure.kind, 'resource_unmeasurable')


if __name__ == '__main__':
    unittest.main()
