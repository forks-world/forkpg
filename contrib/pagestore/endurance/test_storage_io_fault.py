#!/usr/bin/env python3
"""Prove the preload is armed, scoped, reported and reversible on real fds."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', required=True, type=Path)
    args = parser.parse_args()
    for operation in ('pwrite', 'fsync', 'short-pwrite', 'wal-fsync', 'forkmeta-fsync'):
        with tempfile.TemporaryDirectory(prefix='ps-io-fault-') as temp:
            root = Path(temp)
            store = root / 'store'
            store.mkdir()
            arm, report = root / 'arm', root / 'hit.json'
            environment = dict(os.environ, LD_PRELOAD=str(args.library.resolve()),
                               PAGESTORE_IO_TEST_ROOT=str(store), PAGESTORE_IO_TEST_ARM=str(arm),
                               PAGESTORE_IO_TEST_REPORT=str(report), PAGESTORE_IO_TEST_OPERATION=operation)
            code = '''import errno,json,os,sys
from pathlib import Path
store, arm, report, operation = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4]
fd=os.open(store/('forkmeta' if operation=='forkmeta-fsync' else 'wal_1' if operation=='wal-fsync' else 'seg_00000000'), os.O_CREAT|os.O_RDWR, 0o600)
other=os.open(store/('forkmeta.bak' if operation=='forkmeta-fsync' else 'wal_invalid' if operation=='wal-fsync' else 'unrelated'), os.O_CREAT|os.O_RDWR, 0o600)
outside=os.open(store.parent/('forkmeta' if operation=='forkmeta-fsync' else 'wal_1' if operation=='wal-fsync' else 'seg_other_store'), os.O_CREAT|os.O_RDWR, 0o600)
controls=[]
if operation in ('wal-fsync','forkmeta-fsync'):
 (store/'wal_segments_1').mkdir()
 for name in ('seg_00000000','wal_1.bak','timelines','wal_segments_1/walv1_1_00000000000000000001'):
  controls.append(os.open(store/name,os.O_CREAT|os.O_RDWR,0o600))
if operation=='forkmeta-fsync':
 controls.append(os.open(store/'wal_1',os.O_CREAT|os.O_RDWR,0o600))
try:
 os.pwrite(fd,b'B'*8192 if operation=='short-pwrite' else b'baseline',0);os.fsync(fd)
 arm.touch()
 for target in (other,outside,*controls):
  os.pwrite(target,b'outside'*1024,0);os.fsync(target)
 assert not report.exists()
 if operation=='short-pwrite':
  assert os.pwrite(fd,b'H'*56,8192)==56
  assert not report.exists()
 for _ in range(2):
  try:
   if operation=='short-pwrite':
    assert os.pwrite(fd,b'F'*8192,0)==4096
    assert os.pread(fd,8192,0)==b'F'*4096+b'B'*4096
    continue
   os.pwrite(fd,b'failed',0) if operation=='pwrite' else os.fsync(fd)
  except OSError as error:
   assert error.errno == (errno.ENOSPC if operation=='pwrite' else errno.EIO)
  else: raise AssertionError('injection did not occur')
 hit=json.loads(report.read_text());assert hit['operation']==operation and hit['pid']==os.getpid()
 if operation=='short-pwrite':
  assert hit['errno']==0 and hit['requested']==8192 and hit['written']==4096
 if operation=='pwrite':assert os.pread(fd,8,0)==b'baseline'
 arm.unlink()
 os.pwrite(fd,b'healthy!',0);os.fsync(fd)
 assert os.pread(fd,8,0)==b'healthy!'
finally:
 for target in (fd,other,outside,*controls):os.close(target)
'''
            result = subprocess.run([sys.executable, '-c', code, str(store), str(arm), str(report), operation],
                                    env=environment, capture_output=True, text=True, timeout=10)
            if result.returncode:
                print(result.stderr, file=sys.stderr)
                return 1
            print(f'ok - {operation}: unarmed/scoped/error/report/disarm checks')
    return 0


if __name__ == '__main__':
    sys.exit(main())
