#!/usr/bin/env python3
"""Run the tiny timing canary and retain bounded evidence outside its measured checks."""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import stat
import subprocess

MAX_LOGS = 8
LOG_BYTES = 128 * 1024


def identity(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return {'path': str(path.resolve()), 'sha256': digest.hexdigest()}


def capture_logs(cache, destination):
    """Only direct regular log files; never traverse the cache or follow symlinks."""
    captured = []
    logs = cache / 'logs'
    if not logs.is_dir() or logs.is_symlink():
        return captured
    with os.scandir(logs) as entries:
        for entry in itertools.islice(entries, 32):
            if len(captured) == MAX_LOGS:
                break
            if not entry.is_file(follow_symlinks=False):
                continue
            expected = entry.stat(follow_symlinks=False)
            fd = os.open(entry.path, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0))
            with os.fdopen(fd, 'rb') as stream:
                info = os.fstat(stream.fileno())
                if (not stat.S_ISREG(info.st_mode) or
                        (info.st_dev, info.st_ino) != (expected.st_dev, expected.st_ino)):
                    continue
                prefix = stream.read(LOG_BYTES // 2)
                offset = max(len(prefix), info.st_size - LOG_BYTES // 2)
                stream.seek(offset)
                tail = stream.read(LOG_BYTES // 2)
            output = f'log-{len(captured)}.txt'
            (destination / output).write_bytes(prefix + (b'\n--- omitted middle ---\n' if offset > len(prefix) else b'') + tail)
            captured.append({'source': entry.path, 'device': info.st_dev, 'inode': info.st_ino,
                             'size': info.st_size, 'mtime_ns': info.st_mtime_ns, 'artifact': output,
                             'prefix_bytes': len(prefix), 'tail_offset': offset, 'tail_bytes': len(tail),
                             'retained_sha256': hashlib.sha256((destination / output).read_bytes()).hexdigest()})
    return captured


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('a conformance command is required')
    def value(option):
        return command[command.index(option) + 1]
    fixture = Path(value('--fixture'))
    if json.loads((fixture / 'scenario.json').read_text())['name'] != 'timing':
        parser.error('only the generated tiny timing fixture is supported')
    args.output.mkdir(parents=True, exist_ok=True)
    command += ['--timing-evidence', str(args.output / 'report.json')]
    manifest = {'command': command, 'cwd': os.getcwd(), 'logs': [],
                'limits': {'log_files': MAX_LOGS, 'log_bytes_each': LOG_BYTES,
                           'directory_entries_examined': 32, 'report_bytes': 1024 * 1024},
                'collection_excluded_from_navigation_budget': True}
    status = subprocess.call(command)
    manifest['exit_code'] = status
    try:
        manifest['binaries'] = [identity(Path(command[0])), identity(Path(value('--server')))]
    except OSError as error:
        manifest['identity_error'] = str(error)
    try:
        manifest['logs'] = capture_logs(Path(value('--cache-dir')), args.output)
    except OSError as error:
        manifest['capture_error'] = str(error)
    manifest['report_present'] = (args.output / 'report.json').is_file()
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return status


if __name__ == '__main__':
    raise SystemExit(main())
