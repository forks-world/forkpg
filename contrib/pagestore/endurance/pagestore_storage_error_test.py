#!/usr/bin/env python3
"""Real SQL recovery after daemon-only segment ENOSPC, fsync EIO or short writes.

Compile storage_io_fault.c as a Linux preload library. Arguments after -- are
normal endurance-driver arguments; use a fresh root and explicit space budgets.
"""
import argparse
import errno
import hashlib
import json
from pathlib import Path
import re
import shlex
import signal
import shutil
import sys
import threading
import time

import pagestore_endurance as driver


class StorageErrorRun(driver.Run):
    library: Path
    fault_operation: str

    def __init__(self, args, seed, root):
        super().__init__(args, seed, root)
        self.arm = root / 'io-fault.arm'
        self.report = root / 'io-fault-hit.json'
        self.injecting = False
        binary = self.daemon_bin
        wrapper = root / 'daemon-io-wrapper'
        environment = {'LD_PRELOAD': str(self.library),
                       'PAGESTORE_IO_TEST_ROOT': str(self.store),
                       'PAGESTORE_IO_TEST_ARM': str(self.arm),
                       'PAGESTORE_IO_TEST_REPORT': str(self.report),
                       'PAGESTORE_IO_TEST_OPERATION': self.fault_operation}
        if self.fault_operation == 'metadata-dir-fsync':
            environment['PAGESTORE_FORKMETA_HIGH_WATER_BYTES'] = '65536'
            environment['PAGESTORE_FORKMETA_CATCH_UP_BYTES'] = '32768'
        wrapper.write_text('#!/bin/sh\n' + '\n'.join(
            f'export {key}={shlex.quote(value)}' for key, value in environment.items()) +
            '\nexec ' + shlex.quote(str(binary)) + ' "$@"\n')
        wrapper.chmod(0o700)
        self.daemon_bin = wrapper

    def verify_materializer(self, why):
        # Storage recovery probes require structural checks for every seed.
        return super().verify_materializer(why, amcheck=True)

    def read_fault_report(self):
        # The preload hook creates the pathname before writing its JSON.
        # A missing or incomplete report is pending, never successful evidence.
        try:
            return json.loads(self.report.read_text())
        except (FileNotFoundError, json.JSONDecodeError):
            return None

    def fault_response(self, text):
        # Replay promotes storage errors to FATAL and exits with status 1.
        # Only this explicit metadata fault permits that controlled outcome.
        severity = r'(?:ERROR|FATAL)' if self.fault_operation == 'forkmeta-fsync' else 'ERROR'
        return re.search(severity + r':.*pagestore localsvc: daemon reported error[^\n]*', text)

    def check_health(self):
        if not self.injecting:
            return super().check_health()
        # During the explicit fault window, RPC errors/recovery exits are the
        # expected outcome. Budgets and unexpected daemon exit remain findings.
        if self.async_failure:
            raise self.async_failure
        if self.daemon.poll() is not None:
            raise driver.Failure('daemon_died', 'daemon exited during injected storage error')

    def execute(self):
        self.event('space_budget', **driver.space_snapshot(self.root, self.store, self.args))
        self.sampler_thread = threading.Thread(target=self.sampler, daemon=True)
        self.sampler_thread.start()
        self.provision()
        self.verify_materializer('before storage fault')
        self.mat.stop('fast')
        wal_fault = self.fault_operation == 'wal-fsync'
        if wal_fault:
            fault_offsets = {pg.name: pg.log.stat().st_size for pg in self.computes()}
            self.injecting = True
            self.arm.touch()
            self.event('storage_fault_armed', operation=self.fault_operation)
        # Commit and archive the writer's independent local-WAL truth before
        # preventing the materializer from publishing its new page images.
        self.writer.sql("BEGIN; UPDATE acct SET bal=bal-1 WHERE id=1; "
                        "UPDATE acct SET bal=bal+1 WHERE id=2; COMMIT; "
                        "CREATE TABLE storage_error_probe AS SELECT g, md5(g::text) AS v "
                        "FROM generate_series(1,5000) g; CHECKPOINT;")
        if self.fault_operation == 'metadata-dir-fsync':
            self.writer.sql("DO $$ BEGIN FOR i IN 1..128 LOOP "
                            "EXECUTE format('CREATE TABLE metadata_churn_%s (id int PRIMARY KEY)', i); "
                            "EXECUTE format('DROP TABLE metadata_churn_%s', i); "
                            "END LOOP; END $$; CHECKPOINT;")
        expected = self.checksums(self.writer)
        if wal_fault:
            switch = self.writer.sql('SELECT pg_switch_wal();')
            segment = self.writer.sql(f"SELECT pg_walfile_name('{switch}'::pg_lsn - 1);")
            archive_done = self.writer.datadir / 'pg_wal/archive_status' / (segment + '.done')
        else:
            self.archive_current_wal()
            fault_offsets = {pg.name: pg.log.stat().st_size for pg in self.computes()}
            self.injecting = True
            self.arm.touch()
            self.event('storage_fault_armed', operation=self.fault_operation)
        try:
            self.mat.start()
        except driver.Failure as error:
            if error.kind != 'start_failed' or not self.report.exists():
                raise
        deadline = time.monotonic() + 30
        hit = self.read_fault_report()
        while time.monotonic() < deadline and hit is None:
            self.check_health()
            # A restartpoint forces page-segment fsync after the replayed
            # writer checkpoint, even when the workload is too small for a
            # timed restartpoint. A storage error may abort this connection.
            try:
                self.mat.sql('CHECKPOINT;', timeout=2)
            except (driver.SqlError, driver.subprocess.TimeoutExpired):
                pass
            time.sleep(.1)
            hit = self.read_fault_report()
        if hit is None:
            raise driver.Failure('fault_unreached', 'no complete storage I/O fault report')
        expected_errno = {'pwrite': errno.ENOSPC, 'fsync': errno.EIO,
                          'short-pwrite': 0, 'wal-fsync': errno.EIO, 'forkmeta-fsync': errno.EIO,
                          'metadata-dir-fsync': errno.EIO}[self.fault_operation]
        if (hit.get('operation') != self.fault_operation or hit.get('errno') != expected_errno or
                hit.get('pid') != self.daemon.pid):
            raise driver.Failure('fault_identity', f'unexpected I/O hit: {hit}')
        if self.fault_operation == 'short-pwrite' and not (
                hit.get('requested', 0) >= 512 and
                0 < hit.get('written', -1) < hit.get('requested', 0)):
            raise driver.Failure('fault_identity', f'no actual positive short write: {hit}')
        # A connection failure alone is not proof that storage errors surfaced.
        # Require the actual daemon-error response in the compute's fault window.
        fault_compute = self.writer if wal_fault else self.mat
        compute_error = None
        publication_state = None
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            self.check_health()
            if self.fault_operation == 'metadata-dir-fsync':
                state = json.loads(self.inspect('gc'))
                if state.get('forkmeta_poisoned') is True:
                    publication_state = state
                    break
                time.sleep(.1)
                continue
            text = fault_compute.log.read_bytes()[fault_offsets[fault_compute.name]:].decode(errors='replace')
            response = self.fault_response(text)
            archive_failure = (not wal_fault or
                               f'archiving write-ahead log file "{segment}" failed' in text)
            if response and archive_failure:
                compute_error = response.group(0)
                break
            try:
                self.mat.sql('CHECKPOINT;', timeout=2)
            except (driver.SqlError, driver.subprocess.TimeoutExpired):
                pass
            time.sleep(.1)
        if compute_error is None and publication_state is None:
            raise driver.Failure('fault_not_surfaced', 'no compute daemon-error response after I/O failure')
        if wal_fault:
            if archive_done.exists():
                raise driver.Failure('false_archive_success', 'faulted WAL was marked archived')
            if not (self.writer.datadir / 'pg_wal' / segment).is_file():
                raise driver.Failure('wal_retry_source_lost', 'faulted WAL missing from writer')
            self.event('wal_archive_rejected', segment=segment, retry_source_retained=True)
        if publication_state is not None:
            selector = self.store / 'forkmeta_snapshots/forkmeta_manifest_v1'
            if not selector.is_file() or selector.stat().st_size == 0:
                raise driver.Failure('selector_missing', 'no selected snapshot after source rewrite')
        self.event('storage_fault_observed', compute_error=compute_error,
                   publication_state=publication_state, **hit)
        # Stop computes immediately, then crash daemon while fault is still
        # armed. No successful segment sync is allowed to mask the error.
        for pg in self.computes():
            pg.halt()
        self.stop_daemon(signal.SIGKILL)
        for pg in self.computes():
            text = pg.log.read_text(errors='replace')
            if re.search(r'TRAP:|signal (6|7|11)\b|invalid page in block|stack smashing', text):
                raise driver.Failure('unexpected_corruption', f'{pg.name} fault window: {text[-1500:]}')
            # Expected fault-window RPC/PANIC messages remain in the log;
            # resume the ordinary strict log oracle at the recovery boundary.
            pg.log_scanned = pg.log.stat().st_size
        # The ordinary driver deletes successful run directories; retain fault
        # reports and logs alongside its PASS/events/metrics artifacts.
        evidence = self.args.root / 'passed' / str(self.seed) / ('storage-error-' + self.fault_operation)
        evidence.mkdir(parents=True, exist_ok=True)
        shutil.copy2(self.report, evidence / self.report.name)
        if publication_state is not None:
            shutil.copytree(self.store / 'forkmeta_snapshots', evidence / 'forkmeta_snapshots')
            shutil.copy2(self.store / 'forkmeta', evidence / 'forkmeta')
        for pg in self.computes():
            shutil.copy2(pg.log, evidence / pg.log.name)
        shutil.copy2(self.root / 'daemon.log', evidence / 'daemon.log')
        (evidence / 'scope.json').write_text(json.dumps({
            'operation': self.fault_operation, 'hit': hit, 'compute_error': compute_error,
            'publication_state': publication_state,
            'library_sha256': hashlib.sha256(self.library.read_bytes()).hexdigest(),
            'model': ('post-rename store-directory fsync EIO during SQL snapshot cutover'
                      if self.fault_operation == 'metadata-dir-fsync' else
                      'pre-syscall fsync EIO on daemon forkmeta append log'
                      if self.fault_operation == 'forkmeta-fsync' else
                      'pre-syscall fsync EIO on daemon legacy WAL logs wal_<timeline>'
                      if self.fault_operation == 'wal-fsync' else
                      'positive short pwrite on daemon page segments'
                      if self.fault_operation == 'short-pwrite' else
                      'pre-syscall failure on daemon page-segment files only'),
            'not_covered': ['torn writes after successful return', 'immutable WAL segment failures', 'other metadata boundaries',
                            'slow I/O', 'power loss']},
            indent=2) + '\n')
        self.arm.unlink()
        self.start_daemon()
        self.writer.start()
        self.mat.start()
        self.injecting = False
        self.verify_materializer('after I/O failure and daemon crash')
        if publication_state is not None:
            recovered = json.loads(self.inspect('gc'))
            if recovered.get('forkmeta_poisoned') is not False:
                raise driver.Failure('selector_unrecovered', f'poison remains after reopen: {recovered}')
            self.event('snapshot_selector_recovered', gc=recovered)
        if wal_fault:
            if not archive_done.exists() and (self.writer.datadir / 'pg_wal' / segment).exists():
                raise driver.Failure('wal_retry_incomplete', 'faulted WAL still awaiting archive')
            self.event('wal_archive_recovered', segment=segment)
        if self.checksums(self.writer) != expected:
            raise driver.Failure('committed_data_lost', 'writer lost committed fault-window data')
        for pg in reversed(self.computes()):
            pg.stop('fast')
            pg.scan_log()
        self.stop_daemon()
        self.event('storage_fault_recovery_pass', operation=self.fault_operation)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', required=True, type=Path)
    parser.add_argument('--operation', required=True, choices=('pwrite', 'fsync', 'short-pwrite', 'wal-fsync', 'forkmeta-fsync', 'metadata-dir-fsync'))
    parser.add_argument('driver_args', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    library = args.library.resolve()
    if not library.is_file():
        parser.error('preload library does not exist')
    StorageErrorRun.library = library
    StorageErrorRun.fault_operation = args.operation
    driver.Run = StorageErrorRun
    forwarded = args.driver_args[1:] if args.driver_args[:1] == ['--'] else args.driver_args
    sys.argv = [sys.argv[0], *forwarded]
    return driver.main()


if __name__ == '__main__':
    sys.exit(main())
