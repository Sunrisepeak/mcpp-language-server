#!/usr/bin/env python3
"""Retain bounded text after one stability round; preserve its runner verdict."""
import argparse
from collections import deque
import hashlib
import json
import os
from pathlib import Path
import stat
import shutil
import subprocess
import time

from timing_evidence import capture_logs, identity

TEXT_BYTES = 128 * 1024
MEASURE_BYTES = 1024 * 1024
MAX_DATABASES = 8
MAX_ENTRIES = 512
MAX_DIRECTORIES = 64
MAX_DEPTH = 6
COLLECT_SECONDS = 5


def link_or_reparse(path):
    info = path.lstat()
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, 'st_file_attributes', 0) & 0x400)


def checked_path(path):
    """Reject symlink/junction components rather than resolving external inputs."""
    for component in reversed((path, *path.parents)):
        if link_or_reparse(component):
            raise OSError('symlink/reparse component rejected: ' + str(component))


def capture_text(path, output, limit):
    checked_path(path)
    expected = path.stat(follow_symlinks=False)
    fd = os.open(path, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0))
    with os.fdopen(fd, 'rb') as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or (info.st_dev, info.st_ino) != (expected.st_dev, expected.st_ino):
            raise OSError('not the expected regular file: ' + str(path))
        half = (limit - 32) // 2  # Reserve space for the omission marker.
        prefix = stream.read(half)
        offset = max(len(prefix), info.st_size - half)
        stream.seek(offset)
        tail = stream.read(half)
    checked_path(path)
    marker = b'\n--- omitted middle ---\n' if offset > len(prefix) else b''
    output.write_bytes(prefix + marker + tail)
    return {'source': str(path), 'artifact': output.name, 'size': info.st_size,
            'mtime_ns': info.st_mtime_ns, 'device': info.st_dev, 'inode': info.st_ino,
            'truncated': bool(marker), 'prefix_bytes': len(prefix),
            'tail_offset': offset, 'tail_bytes': len(tail),
            'retained_sha256': hashlib.sha256(output.read_bytes()).hexdigest()}


def capture_databases(roots, output):
    queue = deque((root, 0) for root in roots)
    captured, errors = [], []
    directories = entries = 0
    stop_reason = None
    depth_limited = False
    deadline = time.monotonic() + COLLECT_SECONDS
    while queue and directories < MAX_DIRECTORIES and entries < MAX_ENTRIES and len(captured) < MAX_DATABASES:
        if time.monotonic() >= deadline:
            stop_reason = 'time'
            break
        directory, depth = queue.popleft()
        try:
            checked_path(directory)
            directories += 1
            with os.scandir(directory) as children:
                for child in children:
                    if entries >= MAX_ENTRIES or len(captured) >= MAX_DATABASES or time.monotonic() >= deadline:
                        stop_reason = ('entries' if entries >= MAX_ENTRIES else
                                       'databases' if len(captured) >= MAX_DATABASES else 'time')
                        break
                    entries += 1
                    path = Path(child.path)
                    if link_or_reparse(path):
                        continue
                    if child.is_dir(follow_symlinks=False) and depth < MAX_DEPTH:
                        if child.name not in ('logs', '.git', 'node_modules', 'copies', 'instances', 'modules', 'bmi'):
                            queue.append((path, depth + 1))
                    elif child.is_dir(follow_symlinks=False):
                        depth_limited = True
                    elif child.name in ('compile_commands.json', 'build_database.json') and child.is_file(follow_symlinks=False):
                        captured.append(capture_text(path, output / f'database-{len(captured)}.txt', TEXT_BYTES))
        except OSError as error:
            if len(errors) < 8:
                errors.append(str(error))
    if stop_reason is None:
        stop_reason = ('entries' if entries >= MAX_ENTRIES else
                       'databases' if len(captured) >= MAX_DATABASES else
                       'directories' if queue and directories >= MAX_DIRECTORIES else
                       'depth' if depth_limited else None)
    return {'files': captured, 'errors': errors, 'directories_examined': directories,
            'entries_examined': entries, 'incomplete': bool(queue or stop_reason or errors),
            'stop_reason': stop_reason, 'limit_reached': stop_reason is not None,
            'depth_limited': depth_limited,
            'time_budget_reached': time.monotonic() >= deadline}


def collect(command, output):
    def value(option):
        return command[command.index(option) + 1]
    manifest = {'command': command, 'cwd': os.getcwd(),
                'limits': {'logs': 8, 'log_bytes_each_with_marker': TEXT_BYTES + 32,
                           'databases': MAX_DATABASES, 'database_bytes_each': TEXT_BYTES,
                           'walk_entries': MAX_ENTRIES, 'walk_directories': MAX_DIRECTORIES,
                           'walk_depth': MAX_DEPTH, 'walk_time_budget_seconds': COLLECT_SECONDS,
                           'measurement_bytes': MEASURE_BYTES, 'manifest_bytes': MEASURE_BYTES},
                'scope': 'Text-only collection after the runner; no BMI/payload upload. Truncated databases are not complete JSON.'}
    try:
        status = subprocess.call(command)
    except OSError as error:
        status = 127
        manifest['launch_error'] = str(error)
    manifest['exit_code'] = status
    try:
        manifest['binaries'] = [identity(Path(command[0])), identity(Path(value('--server')))]
    except OSError as error:
        manifest['identity_error'] = str(error)
    try:
        cache = Path(value('--cache-dir')).absolute()
        checked_path(cache / 'logs')
        manifest['logs'] = capture_logs(cache, output)
    except OSError as error:
        manifest['log_error'] = str(error)
    try:
        manifest['databases'] = capture_databases(
            [Path(value('--cache-dir')).absolute(), Path(value('--workspace-dir')).absolute()], output)
        manifest['measurement'] = capture_text(Path(value('--measure')).absolute(), output / 'measure.json', MEASURE_BYTES)
    except OSError as error:
        manifest['capture_error'] = str(error)
    data = (json.dumps(manifest, indent=2) + '\n').encode()
    if len(data) > MEASURE_BYTES:
        data = (json.dumps({'exit_code': status, 'capture_error': 'manifest exceeded byte limit'}) + '\n').encode()
    (output / 'manifest.json').write_bytes(data)
    return status


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--scratch', type=Path, required=True)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    def value(option):
        return command[command.index(option) + 1]
    if not command or '--keep' not in command:
        parser.error('a conformance command with --keep is required')
    for option in ('--workspace-dir', '--cache-dir', '--measure', '--server', '--fixture', '--timeout'):
        if option not in command or command.index(option) + 1 >= len(command):
            parser.error('a value is required for ' + option)
    scratch = args.scratch.absolute()
    expected = {'--workspace-dir': scratch / 'workspace', '--cache-dir': scratch / 'cache',
                '--measure': scratch / 'measure.json'}
    if any(Path(value(option)).absolute() != path for option, path in expected.items()):
        parser.error('workspace/cache/measure must use the owned scratch paths')
    if args.output.absolute().is_relative_to(scratch):
        parser.error('evidence output must be outside scratch')
    args.output.mkdir(parents=True, exist_ok=False)
    scratch.mkdir(parents=True, exist_ok=False)
    try:
        return collect(command, args.output)
    finally:
        try:
            shutil.rmtree(scratch)
        except OSError as error:
            # Diagnostic cleanup cannot change the runner's verdict.
            print('stability evidence scratch cleanup failed:', error)


if __name__ == '__main__':
    raise SystemExit(main())
