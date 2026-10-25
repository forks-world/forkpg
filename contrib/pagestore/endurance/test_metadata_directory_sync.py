#!/usr/bin/env python3
"""Real POSIX metadata publication: rename, failed dir fsync, abrupt exit, retry."""
import argparse
import errno
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', required=True, type=Path)
    parser.add_argument('--probe', required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='ps-metadata-dir-') as temp:
        root = Path(temp)
        store = root / 'store'
        store.mkdir()
        arm, report = root / 'arm', root / 'hit.json'
        env = dict(os.environ, LD_PRELOAD=str(args.library.resolve()),
                   PAGESTORE_IO_TEST_ROOT=str(store), PAGESTORE_IO_TEST_ARM=str(arm),
                   PAGESTORE_IO_TEST_REPORT=str(report),
                   PAGESTORE_IO_TEST_OPERATION='metadata-dir-fsync')
        # The boundary must not accidentally mean every directory fsync.
        scope = r'''import os,sys
from pathlib import Path
store,arm,report=map(Path,sys.argv[1:])
arm.touch()
fd=os.open(store,os.O_RDONLY|os.O_DIRECTORY)
try:
 os.fsync(fd)
 (store/'unrelated.tmp').write_text('control')
 os.rename(store/'unrelated.tmp',store/'unrelated')
 os.fsync(fd)
 other=store.parent/'other-store'
 other.mkdir()
 (other/'forkmeta.rewrite.tmp').write_text('outside control')
 os.rename(other/'forkmeta.rewrite.tmp',other/'forkmeta')
 os.fsync(fd)
 assert not report.exists()
finally:
 os.close(fd)
 (store/'unrelated').unlink()
 arm.unlink()
'''
        subprocess.run([sys.executable, '-c', scope, str(store), str(arm), str(report)],
                       env=env, check=True, timeout=10)
        child = subprocess.Popen([str(args.probe.resolve()), 'publish', str(store), str(arm)],
                                 env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            out, err = child.communicate(timeout=15)
        except subprocess.TimeoutExpired:
            child.kill()
            child.communicate()
            raise
        assert child.returncode == 86, (child.returncode, out, err)
        hit = json.loads(report.read_text())
        assert hit['operation'] == 'metadata-dir-fsync' and hit['errno'] == errno.EIO, hit
        assert hit['pid'] == child.pid, hit
        assert (store/'forkmeta').read_bytes() == b'replacement-record'
        assert not (store/'forkmeta.rewrite.tmp').exists()
        arm.unlink()
        subprocess.run([str(args.probe.resolve()), 'recover', str(store), str(arm)],
                       env=env, check=True, timeout=15)
        assert (store/'forkmeta').read_bytes() == b'replacement-record+next'
        print('PASS: post-rename directory fsync EIO rejected, abrupt exit, reopen/retry/append',
              json.dumps(hit, sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
