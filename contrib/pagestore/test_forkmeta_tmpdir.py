"""Regression for a failed temporary template preserving its parent."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='cutover-tmpdir-', dir=os.getcwd()) as root:
    path = Path(root)
    while len(str(path)) < 511:
        path /= 'x' * min(100, max(1, 510 - len(str(path))))
        path.mkdir()
    sentinel = path / 'sentinel'
    sentinel.write_text('preserved')
    result = subprocess.run([binary], env={**os.environ, 'TMPDIR': str(path)},
                            capture_output=True, text=True, timeout=10)
    if result.returncode != 2 or not sentinel.is_file() or sentinel.read_text() != 'preserved':
        raise AssertionError(f'unsafe failed TMPDIR: {result.returncode}, {result.stderr}')
