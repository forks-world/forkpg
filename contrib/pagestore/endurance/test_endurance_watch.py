#!/usr/bin/env python3
"""Exercise notification delivery with local hooks; never contact a service."""
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import tempfile
import time
import unittest

WATCH = Path(__file__).with_name('pagestore_endurance_watch.py')


class WatchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.status = self.root / 'watch.json'
        self.hook = self.root / 'hook.py'
        self.hook.write_text('''import json,sys
from pathlib import Path
p=Path(sys.argv[1])
with p.with_name('notices.jsonl').open('a') as f:
 f.write(json.dumps(json.loads(p.read_text()))+'\\n')
''')

    def argv(self, code=None, extra=()):
        args = [sys.executable, str(WATCH), '--status-file', str(self.status),
                '--notify-command', shlex.join([sys.executable, str(self.hook)]),
                '--heartbeat-interval', '0.02', *extra]
        if code is not None:
            args += ['--', sys.executable, '-c', code]
        return args

    def run_watch(self, code=None, extra=()):
        return subprocess.run(self.argv(code, extra), capture_output=True, text=True, timeout=10)

    def notices(self):
        p = self.root / 'notices.jsonl'
        return [json.loads(line) for line in p.read_text().splitlines()] if p.exists() else []

    def wait_running(self, process):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.status.exists():
                # Concurrent readers must never see a partial JSON document.
                value = json.loads(self.status.read_text())
                if value['phase'] == 'running':
                    return value
            if process.poll() is not None:
                self.fail('watcher exited before running')
            time.sleep(.01)
        self.fail('watcher never reached running')

    def test_failure_notified_once_after_persisting_result(self):
        result = self.run_watch('raise SystemExit(7)')
        self.assertEqual(result.returncode, 7, result.stderr)
        notice, = self.notices()
        self.assertEqual(notice['phase'], 'failed')
        self.assertEqual(notice['returncode'], 7)
        self.assertEqual(notice['notification']['status'], 'not_attempted')
        self.assertEqual(json.loads(self.status.read_text())['notification']['status'], 'command_succeeded')

    def test_launch_failure_is_reported(self):
        args = self.argv()
        args += ['--', str(self.root / 'missing-driver')]
        result = subprocess.run(args, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 127)
        self.assertEqual(self.notices()[0]['phase'], 'failed')

    def test_status_path_is_passed_as_one_literal_argument(self):
        self.status = self.root / 'status;touch injected'
        result = self.run_watch('raise SystemExit(7)')
        self.assertEqual(result.returncode, 7)
        self.assertEqual(len(self.notices()), 1)
        self.assertFalse((self.root / 'injected').exists())

    def test_killed_driver_is_reported(self):
        result = self.run_watch('import os,signal; os.kill(os.getpid(), signal.SIGKILL)')
        self.assertEqual(result.returncode, 137)
        self.assertEqual(self.notices()[0]['returncode'], -signal.SIGKILL)

    def test_success_does_not_alert_by_default(self):
        self.assertEqual(self.run_watch('pass').returncode, 0)
        self.assertEqual(self.notices(), [])
        self.assertEqual(json.loads(self.status.read_text())['phase'], 'passed')

    def test_probe_without_driver(self):
        self.assertEqual(self.run_watch(extra=['--test-notification']).returncode, 0)
        self.assertEqual(self.notices()[0]['phase'], 'notification_probe')

    def test_notification_timeout_preserves_driver_failure(self):
        self.hook.write_text('import time;time.sleep(2)')
        result = self.run_watch('raise SystemExit(7)', ['--notify-timeout', '.05'])
        self.assertEqual(result.returncode, 7)
        status = json.loads(self.status.read_text())
        self.assertEqual(status['phase'], 'failed')
        self.assertEqual(status['notification']['status'], 'timeout')

    def test_notification_error_is_not_a_success(self):
        self.hook.write_text('raise SystemExit(3)')
        result = self.run_watch('pass', ['--notify-on-success'])
        self.assertEqual(result.returncode, 1)
        self.assertEqual(json.loads(self.status.read_text())['notification']['status'], 'command_failed')

    def test_duplicate_watcher_does_not_overwrite_owner_state(self):
        process = subprocess.Popen(self.argv('import time;time.sleep(.5)'),
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        try:
            state = self.wait_running(process)
            duplicate = self.run_watch('raise SystemExit(7)')
            self.assertEqual(duplicate.returncode, 2)
            self.assertEqual(json.loads(self.status.read_text())['run_id'], state['run_id'])
            process.wait(timeout=5)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stderr.close()
        self.assertEqual(self.notices(), [])

    def test_interrupt_allows_driver_cleanup_and_notifies(self):
        ready = self.root / 'ready'
        clean = self.root / 'clean'
        code = ('import signal,time;from pathlib import Path;'
                f'signal.signal(signal.SIGINT, lambda *_: (Path({str(clean)!r}).touch(), time.sleep(.2), exit(0)));'
                f'Path({str(ready)!r}).touch();time.sleep(5)')
        process = subprocess.Popen(self.argv(code), stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        try:
            self.wait_running(process)
            deadline = time.monotonic() + 3
            while not ready.exists() and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertTrue(ready.exists())
            current = json.loads(self.status.read_text())
            self.assertNotEqual(os.getpgid(process.pid), os.getpgid(current['child_pid']))
            process.send_signal(signal.SIGTERM)
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                current = json.loads(self.status.read_text())
                if current['phase'] == 'stopping':
                    break
                time.sleep(.005)
            self.assertEqual(current['phase'], 'stopping')
            self.assertEqual(process.wait(timeout=5), 130)
            self.assertTrue(clean.exists())
            self.assertEqual(self.notices()[0]['phase'], 'interrupted')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stderr.close()


if __name__ == '__main__':
    unittest.main()
