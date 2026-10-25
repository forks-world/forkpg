#!/usr/bin/env python3
"""Scope selector-directory fsync failure to a successful manifest renameat."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='ps-selector-dir-') as temp:
        root = Path(temp)
        store = root/'store'
        snapshots = store/'forkmeta_snapshots'
        other = root/'other-store'/'forkmeta_snapshots'
        snapshots.mkdir(parents=True)
        other.mkdir(parents=True)
        arm, report = root/'arm', root/'hit.json'
        env = dict(os.environ, LD_PRELOAD=str(args.library.resolve()),
                   PAGESTORE_IO_TEST_ROOT=str(store), PAGESTORE_IO_TEST_ARM=str(arm),
                   PAGESTORE_IO_TEST_REPORT=str(report),
                   PAGESTORE_IO_TEST_OPERATION='selector-dir-fsync')
        code = r'''import errno,json,os,sys
from pathlib import Path
store,snapshots,other,arm,report=map(Path,sys.argv[1:])
manifest='forkmeta_manifest_v1'
temporary=manifest+'.tmp.123.0'
fds=[os.open(p,os.O_RDONLY|os.O_DIRECTORY) for p in (store,snapshots,other)]
rootfd,snapfd,otherfd=fds
try:
 arm.touch()
 for fd in fds: os.fsync(fd)
 try: os.rename(temporary,manifest,src_dir_fd=snapfd,dst_dir_fd=snapfd)
 except FileNotFoundError: pass
 else: raise AssertionError('missing source renamed')
 os.fsync(snapfd)
 (snapshots/'unrelated.tmp').write_bytes(b'control')
 os.rename('unrelated.tmp','unrelated',src_dir_fd=snapfd,dst_dir_fd=snapfd)
 os.fsync(snapfd)
 (other/temporary).write_bytes(b'other store')
 os.rename(temporary,manifest,src_dir_fd=otherfd,dst_dir_fd=otherfd)
 os.fsync(otherfd);os.fsync(snapfd)
 assert not report.exists()
 (snapshots/temporary).write_bytes(b'complete selector')
 os.rename(temporary,manifest,src_dir_fd=snapfd,dst_dir_fd=snapfd)
 os.fsync(rootfd);os.fsync(otherfd)
 fd=os.open(snapshots/manifest,os.O_RDONLY)
 try: os.fsync(fd)
 finally: os.close(fd)
 for _ in range(2):
  try: os.fsync(snapfd)
  except OSError as error: assert error.errno==errno.EIO
  else: raise AssertionError('directory failure missing')
 assert (snapshots/manifest).read_bytes()==b'complete selector'
 hit=json.loads(report.read_text())
 assert hit['operation']=='selector-dir-fsync' and hit['errno']==errno.EIO
 assert hit['pid']==os.getpid()
 arm.unlink();os.fsync(snapfd)
 print('PASS: selector renameat/fsync scope, persistent EIO, disarm',json.dumps(hit,sort_keys=True))
finally:
 for fd in fds: os.close(fd)
'''
        result = subprocess.run([sys.executable, '-c', code, str(store), str(snapshots),
                                 str(other), str(arm), str(report)], env=env,
                                capture_output=True, text=True, timeout=15)
        if result.returncode:
            print(result.stderr, file=sys.stderr)
            return 1
        print(result.stdout, end='')
    return 0


if __name__ == '__main__':
    sys.exit(main())
