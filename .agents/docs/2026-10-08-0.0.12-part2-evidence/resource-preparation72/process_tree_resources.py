"""Linux resource-only observer; never use this observer's replay for latency.

PSS apportions shared mapped pages; RSS sum includes shared-page double counting.
Neither number proves the private completion cache's physical allocation budget.
Fast unsampled child lifetimes/escapes are outside periodic observation coverage.
"""
import os
import threading
import time
from pathlib import Path


class TreeChanged(RuntimeError):
    def __init__(self, message):
        super().__init__(message)
        self.observed_monotonic_ns = time.monotonic_ns()


def stat(pid):
    raw = (Path('/proc') / str(pid) / 'stat').read_text()
    fields = raw[raw.rfind(')') + 2:].split()
    return {'pid': pid, 'ppid': int(fields[1]), 'pgid': int(fields[2]),
            'start_ticks': int(fields[19]), 'state': fields[0],
            'cpu_ticks': int(fields[11]) + int(fields[12]),
            'reaped_cpu_ticks': int(fields[13]) + int(fields[14])}


def identity(row):
    return row['pid'], row['start_ticks']


class ProcessTree:
    def __init__(self, pid):
        self.root = identity(stat(pid))
        self.known = {self.root}
        self.lock = threading.Lock()

    def members(self):
        rows = {}
        for path in Path('/proc').iterdir():
            if not path.name.isdigit():
                continue
            try:
                row = stat(int(path.name))
                rows[row['pid']] = row
            except (FileNotFoundError, ProcessLookupError):
                continue
        root = rows.get(self.root[0])
        if root is None or identity(root) != self.root:
            raise TreeChanged('root exited or PID identity changed')
        owned = {pid for pid, row in rows.items() if identity(row) in self.known}
        while True:
            more = {pid for pid, row in rows.items() if row['ppid'] in owned}
            if more <= owned:
                break
            owned |= more
        members = {pid: rows[pid] for pid in owned}
        self.known.update(identity(row) for row in members.values())
        return members

    def read(self):
        with self.lock:
            began = time.monotonic_ns()
            before = self.members()
            pages = {}
            for pid, row in before.items():
                if row['state'] in ('Z', 'X'):
                    pages[pid] = {'Rss': 0, 'Pss': 0, 'Private_Clean': 0, 'Private_Dirty': 0}
                    continue
                values = {}
                for line in (Path('/proc') / str(pid) / 'smaps_rollup').read_text().splitlines():
                    if ':' in line:
                        key, value = line.split(':', 1)
                        if key in ('Rss', 'Pss', 'Private_Clean', 'Private_Dirty'):
                            values[key] = int(value.split()[0])
                if len(values) != 4:
                    raise ValueError('incomplete physical page observation')
                pages[pid] = values
            after = self.members()
            if {identity(r) for r in before.values()} != {identity(r) for r in after.values()}:
                raise TreeChanged('tree membership changed during observation')
            for pid, row in before.items():
                end = after[pid]
                if row['ppid'] != end['ppid'] or row['reaped_cpu_ticks'] != end['reaped_cpu_ticks']:
                    raise TreeChanged('reparent/reap raced the observation')
                if end['cpu_ticks'] < row['cpu_ticks']:
                    raise ValueError('CPU counter moved backwards')
            tick_ms = 1000 / os.sysconf('SC_CLK_TCK')
            return {'sample_started_monotonic_ns': began, 'monotonic_ns': time.monotonic_ns(),
                    'root_identity': list(self.root), 'members': list(after.values()),
                    'tree_cpu_lower_ms': sum(r['cpu_ticks'] + r['reaped_cpu_ticks'] for r in before.values()) * tick_ms,
                    'tree_cpu_upper_ms': (sum(r['cpu_ticks'] + r['reaped_cpu_ticks'] for r in after.values()) + 4 * len(after)) * tick_ms,
                    'tree_rss_sum_kib': sum(p['Rss'] for p in pages.values()),
                    'tree_pss_kib': sum(p['Pss'] for p in pages.values()),
                    'tree_private_kib': sum(p['Private_Clean'] + p['Private_Dirty'] for p in pages.values()),
                    'cpu_tick_ms': tick_ms}
