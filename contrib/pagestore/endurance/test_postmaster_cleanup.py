#!/usr/bin/env python3
"""Cleanup must target the owned generation, never a recycled numeric PID."""
import os
import contextlib
import io
import json
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pagestore_endurance as endurance


class CleanupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        run = SimpleNamespace(root=self.root, bin=self.root / 'bin')
        self.pg = endurance.Pg(run, 'test', self.root / 'data')
        self.handle = Mock()
        self.pg.postmaster = self.handle
        self.pg.stop = Mock()
        self.addCleanup(patch.stopall)
        self.numeric_kill = patch.object(endurance.os, 'kill', side_effect=AssertionError('numeric kill')).start()
        self.group_kill = patch.object(endurance.os, 'killpg', side_effect=AssertionError('group kill')).start()

    def test_ordinary_stop_releases_retired_generation_handle(self):
        self.handle.exited.return_value = True
        self.pg.ctl = Mock(return_value=SimpleNamespace(returncode=0))
        endurance.Pg.stop(self.pg)
        self.handle.close.assert_called_once()
        self.assertIsNone(self.pg.postmaster)

    def test_start_refuses_to_replace_live_generation(self):
        self.handle.exited.return_value = False
        self.pg.ctl = Mock()
        with patch.object(endurance.os, 'pidfd_open', create=True), \
             patch.object(endurance.signal, 'pidfd_send_signal', create=True):
            with self.assertRaisesRegex(endurance.Failure, 'still has a live postmaster'):
                self.pg.start()
        self.pg.ctl.assert_not_called()
        self.handle.close.assert_not_called()

    def test_exited_generation_with_reused_pid_never_calls_pg_ctl(self):
        self.handle.exited.return_value = True
        self.pg.pid = Mock(return_value=12345)  # could now name an unrelated process
        self.pg.halt()
        self.pg.stop.assert_not_called()
        self.handle.kill.assert_not_called()
        self.handle.close.assert_called_once()
        self.pg.pid.assert_not_called()

    def test_successful_shutdown_needs_no_kill(self):
        self.handle.exited.side_effect = [False, True, True]
        self.pg.halt()
        self.pg.stop.assert_called_once_with('immediate', check=False)
        self.handle.kill.assert_not_called()
        self.handle.close.assert_called_once()

    def test_failed_pg_ctl_uses_pinned_handle(self):
        self.handle.exited.side_effect = [False, False, True]
        self.pg.halt()
        self.handle.kill.assert_called_once()
        self.handle.close.assert_called_once()

    def test_timeout_and_exit_during_pg_ctl_do_not_signal_reused_pid(self):
        self.pg.stop.side_effect = subprocess.TimeoutExpired('pg_ctl', 300)
        self.handle.exited.side_effect = [False, True, True]
        self.pg.halt()
        self.handle.kill.assert_not_called()

    def test_timeout_with_live_postmaster_uses_handle(self):
        self.pg.stop.side_effect = subprocess.TimeoutExpired('pg_ctl', 300)
        self.handle.exited.side_effect = [False, False, True]
        self.pg.halt()
        self.handle.kill.assert_called_once()

    def test_signal_failure_retains_handle_for_retry(self):
        self.handle.exited.return_value = False
        self.handle.kill.side_effect = PermissionError('denied')
        with self.assertRaises(PermissionError):
            self.pg.halt()
        self.handle.close.assert_not_called()
        self.assertIs(self.pg.postmaster, self.handle)

    def test_partial_start_adopts_only_verified_handle(self):
        self.pg.postmaster = None
        self.pg.pid = Mock(return_value=12345)
        self.handle.exited.side_effect = [False, False, True]
        with patch.object(endurance.PostmasterHandle, 'open', return_value=self.handle) as opened:
            self.pg.halt()
        opened.assert_called_once_with(12345, self.pg.datadir, self.pg.run.bin / 'postgres')
        self.handle.kill.assert_called_once()

    def test_stale_pid_file_refusal_precedes_any_pg_ctl(self):
        self.pg.postmaster = None
        self.pg.pid = Mock(return_value=12345)
        with patch.object(endurance.PostmasterHandle, 'open',
                          side_effect=endurance.Failure('cleanup_identity', 'wrong compute')):
            with self.assertRaises(endurance.Failure):
                self.pg.halt()
        self.pg.stop.assert_not_called()

    def test_absent_compute_needs_no_handle(self):
        self.pg.postmaster = None
        self.pg.pid = Mock(return_value=None)
        self.pg.halt()
        self.pg.stop.assert_not_called()


class TeardownTests(unittest.TestCase):
    def test_cleanup_failure_is_persisted_without_losing_original_finding(self):
        for original in [None, endurance.Failure('product_bug', 'original finding')]:
            with self.subTest(original=original), tempfile.TemporaryDirectory() as temp:
                root = Path(temp) / 'run-1'
                run = Mock()
                run.async_failure = None
                run.round = 1
                run.stats = {}
                run.execute.side_effect = original
                run.teardown.side_effect = endurance.Failure('cleanup_failed', 'identity refused')
                with patch.object(endurance, 'Run', return_value=run), \
                     patch.object(endurance.subprocess, 'run', return_value=SimpleNamespace(stdout='revision')), \
                     contextlib.redirect_stdout(io.StringIO()):
                    self.assertFalse(endurance.one_run(SimpleNamespace(root=Path(temp)), 1, root))
                result = json.loads((root / 'FAILURE.json').read_text())
                self.assertFalse(result['pass'])
                self.assertEqual(result['kind'], 'product_bug' if original else 'cleanup_failed')
                self.assertIn('identity refused', result['detail'])
                self.assertTrue((Path(temp) / 'history.jsonl').exists())

    def test_identity_refusal_still_cleans_other_computes_and_daemon(self):
        run = object.__new__(endurance.Run)
        first, second = Mock(), Mock()
        first.name = 'first'
        first.halt.side_effect = endurance.Failure('cleanup_identity', 'unrelated PID')
        run.computes = Mock(return_value=[first, second])
        run.stop_sampling = Mock()
        run.daemon = Mock()
        run.daemon.poll.return_value = None
        run.shm = '/test-cleanup'
        run.events = Mock()
        with patch.object(endurance, 'unlink_shm') as unlink:
            with self.assertRaisesRegex(endurance.Failure, 'cleanup_failed'):
                run.teardown(preserve_state=False)
        second.halt.assert_called_once()
        run.daemon.send_signal.assert_called_once_with(signal.SIGKILL)
        run.daemon.wait.assert_called_once()
        unlink.assert_called_once_with(run.shm)
        run.events.close.assert_called_once()



@unittest.skipUnless(hasattr(os, 'pidfd_open') and hasattr(signal, 'pidfd_send_signal'),
                     'Linux pidfd support required for real process tests')
class RealHandleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.datadir = Path(self.temp.name)
        self.process = subprocess.Popen(
            [sys.executable, '-c', 'import time; print("ready", flush=True); time.sleep(60)',
             '-D', str(self.datadir)], stdout=subprocess.PIPE, text=True)
        self.assertEqual(self.process.stdout.readline(), 'ready\n')
        self.addCleanup(self.reap)

    def reap(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=5)
        self.process.stdout.close()

    def test_handle_kills_only_owned_process(self):
        handle = endurance.PostmasterHandle.open(self.process.pid, self.datadir, Path(sys.executable))
        try:
            handle.kill()
            self.assertTrue(handle.exited())
            self.assertEqual(self.process.wait(timeout=5), -signal.SIGKILL)
        finally:
            handle.close()

    def test_wrong_directory_and_executable_are_refused(self):
        for directory, executable in [(self.datadir / 'other', Path(sys.executable)),
                                      (self.datadir, Path('/bin/sh')),
                                      (self.datadir, self.datadir / 'missing-postgres')]:
            with self.subTest(directory=directory, executable=executable):
                with self.assertRaises(endurance.Failure):
                    endurance.PostmasterHandle.open(self.process.pid, directory, executable)
                self.assertIsNone(self.process.poll())

    def test_exit_before_signal_is_harmless(self):
        handle = endurance.PostmasterHandle.open(self.process.pid, self.datadir, Path(sys.executable))
        try:
            self.process.terminate()
            self.process.wait(timeout=5)
            handle.kill()
            self.assertTrue(handle.exited())
        finally:
            handle.close()


@unittest.skipUnless(os.environ.get('PAGESTORE_CLEANUP_TEST_BUILD'),
                     'set PAGESTORE_CLEANUP_TEST_BUILD for real PostgreSQL coverage')
class RealPostgresTests(unittest.TestCase):
    def test_timeout_cleanup_exits_backend_and_allows_restart(self):
        build = Path(os.environ['PAGESTORE_CLEANUP_TEST_BUILD']).resolve()
        pgctl = next((build / 'tmp_install').rglob('bin/pg_ctl'))
        bindir = pgctl.parent
        prefix = bindir.parent
        env = dict(os.environ, LD_LIBRARY_PATH=f"{prefix / 'lib'}:{prefix / 'lib64'}",
                   LC_ALL='C', PGCONNECT_TIMEOUT='5')
        with tempfile.TemporaryDirectory(prefix='postmaster-cleanup-') as root:
            run = SimpleNamespace(root=Path(root), bin=bindir, env=env)
            pg = endurance.Pg(run, 'cleanup', Path(root) / 'data')
            subprocess.run([str(bindir / 'initdb'), '-D', str(pg.datadir),
                            '-U', 'postgres', '-A', 'trust', '--no-sync'],
                           env=env, capture_output=True, check=True, timeout=60)
            with (pg.datadir / 'postgresql.conf').open('a') as config:
                config.write(f"\nport={pg.port}\nunix_socket_directories=''\n")
            backend = None
            backend_fd = None
            try:
                pg.start()
                backend = subprocess.Popen(
                    [str(bindir / 'psql'), '-X', '-h', '127.0.0.1', '-p', str(pg.port),
                     '-U', 'postgres', '-d', 'postgres', '-c', 'SELECT pg_sleep(60)'],
                    env=dict(env, PGAPPNAME='cleanup-backend'),
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    pid = pg.sql("SELECT pid FROM pg_stat_activity WHERE application_name='cleanup-backend'")
                    if pid:
                        backend_fd = os.pidfd_open(int(pid))
                        break
                    time.sleep(0.05)
                self.assertIsNotNone(backend_fd, 'the real backend must be running before injection')
                with patch.object(pg, 'stop', side_effect=subprocess.TimeoutExpired('pg_ctl', 300)):
                    pg.halt()
                self.assertTrue(select.select([backend_fd], [], [], 30)[0],
                                'backend must observe postmaster death')
                backend.wait(timeout=10)
                pg.start()                      # recovery after the forced crash
                self.assertEqual(pg.sql('SELECT 42'), '42')
                pg.halt()                       # the ordinary pg_ctl path
                self.assertIsNone(pg.postmaster)
            finally:
                try:
                    pg.halt()
                finally:
                    if backend_fd is not None:
                        os.close(backend_fd)
                    if backend is not None:
                        if backend.poll() is None:
                            backend.kill()
                        backend.wait(timeout=10)


if __name__ == '__main__':
    unittest.main()
