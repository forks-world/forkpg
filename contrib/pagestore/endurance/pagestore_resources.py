"""Read-only Linux process-tree sampling for the endurance driver's computes."""
from __future__ import annotations

import os
from pathlib import Path
import re
from typing import Callable


def identity(pid: int) -> tuple[int, int]:
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(') ', 1)[1].split()
    return int(fields[1]), int(fields[19])  # parent PID and start-time ticks


def snapshot(roots: dict[str, tuple[int, Callable[[], bool]]]) -> dict:
    identities = {}
    errors = {}
    for path in Path('/proc').iterdir():
        if not path.name.isdecimal():
            continue
        pid = int(path.name)
        try:
            identities[pid] = identity(pid)
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
    children = {}
    for pid, (parent, _) in identities.items():
        children.setdefault(parent, []).append(pid)
    groups, procs, samples = {}, {}, {}
    for name, (root, alive) in roots.items():
        if not alive():
            continue  # the pinned generation exited during a coordinated restart
        if root not in identities:
            errors[name] = 'live root identity unavailable'
            continue
        pending, seen, measured = [root], set(), {}
        while pending:
            pid = pending.pop()
            if pid in seen:
                continue
            seen.add(pid)
            try:
                expected = identities[pid]
                if identity(pid) != expected:
                    continue
                status = Path(f'/proc/{pid}/status').read_text()
                match = re.search(r'VmRSS:\s+(\d+)', status)
                if match is None:  # exited/zombie task has no resident mapping
                    continue
                value = {'rss_kb': int(match.group(1)),
                         'fds': len(os.listdir(f'/proc/{pid}/fd')),
                         'start_ticks': expected[1]}
                if identity(pid) == expected:
                    measured[pid] = value
                    pending.extend(children.get(pid, ()))
            except (FileNotFoundError, ProcessLookupError):
                continue  # short-lived backend left during sampling
            except (PermissionError, ValueError, IndexError) as error:
                errors[f'{name}:{pid}'] = type(error).__name__
        if not alive():
            # Do not associate a recycled PID's sample with the old pidfd/Popen.
            continue
        if root not in measured:
            errors[name] = 'live root resource sample unavailable'
            continue
        procs[name] = dict(measured[root], pid=root)
        groups[name] = {'root_pid': root, 'root_start_ticks': identities[root][1],
                        'processes': len(measured),
                        'rss_kb': sum(v['rss_kb'] for v in measured.values()),
                        'fds': sum(v['fds'] for v in measured.values())}
        samples.update(measured)  # count a process once even if roots overlap
    totals = {'processes': len(samples), 'rss_kb': sum(v['rss_kb'] for v in samples.values()),
              'fds': sum(v['fds'] for v in samples.values())}
    return {'procs': procs, 'resource_groups': groups, 'resource_totals': totals,
            'resource_errors': errors}
