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

    def test_refresh_replaces_exited_controller_generation(self):
        self.handle.exited.return_value = True
        replacement = Mock()
        self.pg.pid = Mock(return_value=67890)
        with patch.object(endurance.PostmasterHandle, 'open', return_value=replacement):
            self.pg.refresh_postmaster()
        self.handle.wait_children.assert_called_once()
        self.handle.close.assert_called_once()
        self.assertIs(self.pg.postmaster, replacement)

    def test_slow_children_are_waited_before_halt_returns(self):
        self.handle.exited.side_effect = [False, True]
        self.pg.halt()
        self.handle.wait_children.assert_called_once()
        self.handle.close.assert_called_once()

    def test_child_timeout_retains_handles_for_retry(self):
        self.handle.exited.side_effect = [False, True]
        self.handle.wait_children.side_effect = endurance.Failure('cleanup_timeout', 'slow child')
        with self.assertRaises(endurance.Failure):
            self.pg.halt()
        self.handle.close.assert_not_called()
        self.assertIs(self.pg.postmaster, self.handle)

    def test_ordinary_stop_releases_retired_generation_handle(self):
        self.handle.exited.return_value = True
        self.pg.ctl = Mock(return_value=SimpleNamespace(returncode=0))
        endurance.Pg.stop(self.pg)
        self.handle.close.assert_called_once()
        self.assertIsNone(self.pg.postmaster)

    def test_successful_pg_ctl_stop_waits_for_late_process_exit(self):
        self.handle.exited.return_value = False
        self.pg.ctl = Mock(return_value=SimpleNamespace(returncode=0))
        endurance.Pg.stop(self.pg)
        self.handle.wait_exit.assert_called_once()
        self.handle.wait_children.assert_called_once()
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

    def test_successful_cleanup_retry_allows_shared_resource_teardown(self):
        run = object.__new__(endurance.Run)
        compute = Mock()
        compute.name = 'retry'
        compute.halt.side_effect = [endurance.Failure('cleanup_timeout', 'slow child'), None]
        run.computes = Mock(return_value=[compute])
        run.stop_sampling = Mock()
        run.daemon = Mock()
        run.daemon.poll.return_value = None
        run.shm = '/test-retry'
        run.events = Mock()
        with patch.object(endurance, 'unlink_shm') as unlink:
            with self.assertRaisesRegex(endurance.Failure, 'cleanup_failed'):
                run.teardown(preserve_state=False)
        self.assertEqual(compute.halt.call_count, 2)
        run.daemon.send_signal.assert_called_once_with(signal.SIGKILL)
        run.daemon.wait.assert_called_once()
        unlink.assert_called_once_with(run.shm)
        run.events.close.assert_called_once()

    def test_identity_refusal_cleans_other_computes_but_preserves_shared_resources(self):
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
        first.halt.assert_has_calls([unittest.mock.call(), unittest.mock.call()])
        run.daemon.send_signal.assert_not_called()
        run.daemon.wait.assert_not_called()
        unlink.assert_not_called()
        run.events.close.assert_called_once()



class ChildExitRaceTests(unittest.TestCase):
    def test_procfs_esrch_requires_pinned_child_exit(self):
        for exited in (True, False):
            with self.subTest(exited=exited):
                handle = endurance.PostmasterHandle(7, 123)
                handle.exited = Mock(return_value=False)
                def proc_read(path, *args, **kwargs):
                    if path.name == 'children':
                        return '456'
                    raise ProcessLookupError('child exited while procfs stat was read')
                with patch.object(Path, 'read_text', new=proc_read), \
                     patch.object(endurance.os, 'pidfd_open', return_value=8), \
                     patch.object(endurance.select, 'select', return_value=([8] if exited else [], [], [])), \
                     patch.object(endurance.os, 'close') as close:
                    if exited:
                        handle.pin_children()
                    else:
                        with self.assertRaises(ProcessLookupError):
                            handle.pin_children()
                    close.assert_called_once_with(8)
                    self.assertFalse(handle.children)


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

    def test_halt_waits_for_a_child_with_delayed_parent_death_exit(self):
        child_code = ("import os,time; parent=os.getppid(); print('ready',flush=True); "
                      "exec('while os.getppid()==parent: time.sleep(0.01)'); time.sleep(0.3)")
        parent_code = ("import subprocess,sys,time; "
                       f"child=subprocess.Popen([sys.executable,'-c',{child_code!r}],stdout=subprocess.PIPE); "
                       "child.stdout.readline(); print(child.pid,flush=True); time.sleep(60)")
        parent = subprocess.Popen([sys.executable, '-c', parent_code, '-D', str(self.datadir)],
                                  stdout=subprocess.PIPE, text=True)
        child_pid = int(parent.stdout.readline())
        child_fd = os.pidfd_open(child_pid)
        handle = endurance.PostmasterHandle.open(parent.pid, self.datadir, Path(sys.executable))
        pg = endurance.Pg(SimpleNamespace(root=self.datadir, bin=Path(sys.executable).parent),
                          'slow-child', self.datadir)
        pg.postmaster = handle
        pg.stop = Mock(side_effect=subprocess.TimeoutExpired('pg_ctl', 300))
        started = time.monotonic()
        try:
            pg.halt()
            self.assertTrue(select.select([child_fd], [], [], 0)[0])
            self.assertGreaterEqual(time.monotonic() - started, 0.25)
        finally:
            if parent.poll() is None:
                parent.kill()
            parent.wait(timeout=5)
            parent.stdout.close()
            os.close(child_fd)
            if pg.postmaster is not None:
                pg.postmaster.close()

    def test_kernel_without_procfs_children_uses_parentage_fallback(self):
        read_text = Path.read_text
        def missing_children(path, *args, **kwargs):
            if path.name == 'children':
                raise FileNotFoundError('CONFIG_CHECKPOINT_RESTORE disabled')
            return read_text(path, *args, **kwargs)
        with patch.object(Path, 'read_text', new=missing_children):
            self.test_halt_waits_for_a_child_with_delayed_parent_death_exit()

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
                self.assertTrue(select.select([backend_fd], [], [], 0)[0],
                                'backend must observe postmaster death')
                backend.wait(timeout=10)
                pg.start()                      # recovery after the forced crash
                original = pg.postmaster
                pg.ctl('-m', 'immediate', '-w', 'stop')
                pg.ctl('-l', str(pg.log), '-w', 'start')
                pg.refresh_postmaster()         # external branch controller restart
                self.assertIsNot(pg.postmaster, original)
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


class BurstDeadlineTests(unittest.TestCase):
    def test_ddl_does_not_start_after_burst_deadline(self):
        run = Mock()
        stop = Mock()
        with patch.object(endurance.time, 'monotonic', return_value=10):
            endurance.Run.ddl_chaos(run, stop, Mock(), Mock(), 10)
        stop.wait.assert_not_called()
        run.tables.assert_not_called()

    def test_ddl_sleep_is_capped_and_rechecks_deadline(self):
        run = Mock()
        stop = Mock()
        stop.wait.return_value = False
        rng = Mock()
        rng.uniform.return_value = 2
        with patch.object(endurance.time, 'monotonic', side_effect=[9.75, 10]):
            endurance.Run.ddl_chaos(run, stop, Mock(), rng, 10)
        stop.wait.assert_called_once_with(0.25)
        run.tables.assert_not_called()

    def test_inflight_ddl_finishes_without_starting_another_operation(self):
        run = Mock()
        run.tables.return_value = []
        stop = Mock()
        stop.wait.return_value = False
        rng = Mock()
        rng.uniform.return_value = 0.3
        rng.choice.return_value = 'checkpoint'
        with patch.object(endurance.time, 'monotonic', side_effect=[9, 9.3, 11]):
            endurance.Run.ddl_chaos(run, stop, Mock(), rng, 10)
        run.writer.sql.assert_called_once_with('CHECKPOINT;', timeout=900)
        stop.wait.assert_called_once_with(0.3)


class RetentionLogTests(unittest.TestCase):
    def test_rejected_retention_progress_is_a_finding(self):
        with tempfile.TemporaryDirectory() as root:
            run = SimpleNamespace(root=Path(root), bin=Path(root), killed_pids=set())
            pg = endurance.Pg(run, 'materializer', Path(root) / 'data')
            for warning in (
                'pagestore materializer retention advance was rejected by the daemon',
                'pagestore materializer retention advance failed: request timed out',
            ):
                with self.subTest(warning=warning):
                    pg.log_scanned = 0
                    pg.log.write_text('2026-10-24 03:03:39 WARNING:  ' + warning + '\n')
                    with self.assertRaises(endurance.Failure) as raised:
                        pg.scan_log()
                    self.assertEqual(raised.exception.kind, 'bad_log')
                    self.assertIn(warning, raised.exception.detail)


if __name__ == '__main__':
    unittest.main()
