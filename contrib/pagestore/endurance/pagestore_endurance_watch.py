#!/usr/bin/env python3
"""Supervise one endurance command and report its exit through an explicit hook.

The hook is executed without a shell and receives the absolute status-file path
as its final argument. See ENDURANCE.md for delivery and crash limitations.
"""
from __future__ import annotations

import argparse
import fcntl
import json
import math
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import time
import uuid


def write_status(path: Path, status: dict) -> None:
    temporary = path.with_name(path.name + '.tmp')
    try:
        with temporary.open('w') as stream:
            json.dump(status, stream, indent=2)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        temporary.unlink(missing_ok=True)


def notify(command: list[str], path: Path, timeout: float) -> dict:
    try:
        # Hook output may contain credentials; retain only exit/timeout status.
        result = subprocess.run([*command, str(path)], stdin=subprocess.DEVNULL,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                timeout=timeout)
        return {'status': 'command_succeeded' if result.returncode == 0 else 'command_failed',
                'returncode': result.returncode}
    except subprocess.TimeoutExpired:
        return {'status': 'timeout'}
    except OSError as error:
        return {'status': 'launch_failed', 'errno': error.errno}


def supervise(command: list[str], path: Path, hook: list[str], timeout: float,
              interval: float, notify_success: bool = False, label: str = "forkpg endurance") -> int:
    started = time.time()
    status = {'schema': 1, 'label': label, 'run_id': str(uuid.uuid4()), 'watcher_pid': os.getpid(),
              'started_at': started, 'phase': 'starting', 'heartbeat_at': started,
              'notification': {'status': 'not_attempted'}}
    write_status(path, status)
    child = None
    interrupted = False
    previous = {}

    def stop(signum, _frame):
        nonlocal interrupted
        interrupted = True
        # The loop sends one interrupt; the driver owns pidfd compute cleanup.

    for signum in (signal.SIGINT, signal.SIGTERM):
        previous[signum] = signal.signal(signum, stop)
    try:
        try:
            # Let this watcher forward one interrupt rather than also letting a
            # terminal process-group SIGINT interrupt driver cleanup twice.
            child = subprocess.Popen(command, start_new_session=True)
        except OSError as error:
            status.update(phase='failed', returncode=127, launch_errno=error.errno)
            result = 127
        else:
            status.update(phase='running', child_pid=child.pid)
            stop_sent = False
            while child.poll() is None:
                if interrupted and not stop_sent:
                    # Send once: repeated interrupts could interrupt driver cleanup.
                    child.send_signal(signal.SIGINT)
                    stop_sent = True
                    status['phase'] = 'stopping'
                status['heartbeat_at'] = time.time()
                write_status(path, status)
                time.sleep(interval)
            # After interruption the driver gets to finish its normal cleanup.
            result = child.wait()
            status.update(phase='interrupted' if interrupted else 'passed' if result == 0 else 'failed',
                          returncode=result)
        status.update(finished_at=time.time(), heartbeat_at=time.time())
        write_status(path, status)  # Failure evidence exists before running a hook.
        if status['phase'] != 'passed' or notify_success:
            status['notification'] = notify(hook, path, timeout)
            write_status(path, status)
        print(json.dumps(status), flush=True)
        if status['notification']['status'] in ('command_failed', 'timeout', 'launch_failed'):
            return result if result > 0 else 1
        return 130 if interrupted else result if result >= 0 else 128 - result
    finally:
        # Also stop the driver if writing status or an unexpected exception fails.
        # Do not attempt to signal arbitrary descendants by numeric process group.
        if child is not None and child.poll() is None:
            child.send_signal(signal.SIGINT)
            child.wait()
        for signum, handler in previous.items():
            signal.signal(signum, handler)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--status-file', required=True, type=Path)
    parser.add_argument('--notify-command', required=True,
                        help='shell-style words, executed without a shell; receives status path as final arg')
    parser.add_argument('--notify-timeout', type=float, default=10)
    parser.add_argument('--heartbeat-interval', type=float, default=5)
    parser.add_argument('--label', default='forkpg endurance', help='human-readable run identifier')
    parser.add_argument('--notify-on-success', action='store_true')
    parser.add_argument('--test-notification', action='store_true',
                        help='send a probe notification without starting a driver')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    for field in ('notify_timeout', 'heartbeat_interval'):
        if not math.isfinite(getattr(args, field)) or getattr(args, field) <= 0:
            parser.error(f'--{field.replace("_", "-")} must be finite and positive')
    try:
        hook = shlex.split(args.notify_command)
    except ValueError as error:
        parser.error(str(error))
    if not hook:
        parser.error('--notify-command must contain an executable')
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command and not args.test_notification:
        parser.error('provide the endurance command after --')
    if command and args.test_notification:
        parser.error('--test-notification cannot start an endurance command')
    path = args.status_file.resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.with_name(path.name + '.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            parser.error('another watcher owns this status file')
        if args.test_notification:
            status = {'schema': 1, 'label': args.label, 'phase': 'notification_probe', 'heartbeat_at': time.time(),
                      'notification': {'status': 'not_attempted'}}
            write_status(path, status)
            status['notification'] = notify(hook, path, args.notify_timeout)
            write_status(path, status)
            return 0 if status['notification']['status'] == 'command_succeeded' else 1
        return supervise(command, path, hook, args.notify_timeout,
                         args.heartbeat_interval, args.notify_on_success, args.label)


if __name__ == '__main__':
    sys.exit(main())
