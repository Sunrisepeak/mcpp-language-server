#!/usr/bin/env python3
"""Pin bounded log inodes while a diagnostic runs; copy them after it exits."""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import threading


class Capture:
    def __init__(self, directory, files=64, bytes_each=6 * 1024 * 1024):
        self.directory = Path(directory)
        self.limit = files
        self.bytes_each = bytes_each
        self.held = {}
        self.limited = False
        self.errors = []

    def scan(self):
        directory_fd = None
        try:
            directory_fd = os.open(self.directory, os.O_RDONLY | os.O_DIRECTORY |
                                   os.O_CLOEXEC | os.O_NOFOLLOW)
            with os.scandir(directory_fd) as entries:
                for index, entry in enumerate(itertools.islice(entries, 65)):
                    if index == 64:
                        self.limited = True
                        break
                    if not re.fullmatch(r'server-.*\.log(?:\.[0-9]+)?', entry.name):
                        continue
                    if not entry.is_file(follow_symlinks=False):
                        continue
                    expected = entry.stat(follow_symlinks=False)
                    key = (expected.st_dev, expected.st_ino)
                    if key in self.held:
                        continue
                    if len(self.held) >= self.limit:
                        self.limited = True
                        continue
                    flags = os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC | os.O_NOFOLLOW
                    fd = os.open(entry.name, flags, dir_fd=directory_fd)
                    current = os.fstat(fd)
                    if not stat.S_ISREG(current.st_mode) or key != (current.st_dev, current.st_ino):
                        os.close(fd)
                        continue
                    self.held[key] = (fd, entry.name)
        except OSError as error:
            if error.errno != 2 and len(self.errors) < 8:
                self.errors.append({'errno': error.errno})
        finally:
            if directory_fd is not None:
                os.close(directory_fd)

    def export(self, destination):
        destination = Path(destination)
        destination.mkdir(parents=True, exist_ok=True)
        records = []
        try:
            for index, ((device, inode), (fd, name)) in enumerate(self.held.items()):
                size = os.fstat(fd).st_size
                os.lseek(fd, 0, os.SEEK_SET)
                output = destination / f'log-{index:03d}.txt'
                remaining = min(size, self.bytes_each)
                digest = hashlib.sha256()
                with output.open('xb') as stream:
                    while remaining:
                        block = os.read(fd, min(65536, remaining))
                        if not block:
                            break
                        stream.write(block)
                        digest.update(block)
                        remaining -= len(block)
                records.append({'device': device, 'inode': inode, 'first_name': name,
                                'size': size, 'bytes': output.stat().st_size,
                                'sha256': digest.hexdigest(), 'artifact': output.name,
                                'truncated': size > self.bytes_each,
                                'changed_after_exit': os.fstat(fd).st_size != size})
        finally:
            for fd, _ in self.held.values():
                os.close(fd)
            self.held.clear()
        return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('a diagnostic command is required')
    args.output.mkdir(parents=True, exist_ok=False)
    capture = Capture(args.cache_dir / 'logs')
    stop = threading.Event()

    def follow():
        while not stop.is_set():
            capture.scan()
            stop.wait(0.05)

    observer = threading.Thread(target=follow)
    observer.start()
    try:
        result = subprocess.call(command)
    finally:
        stop.set()
        observer.join()
    capture.scan()
    records = capture.export(args.output)
    manifest = {'command': command, 'exit_code': result, 'logs': records,
                'limits': {'files': capture.limit, 'bytes_each': capture.bytes_each,
                           'directory_entries_per_poll': 65, 'poll_seconds': 0.05},
                'limit_reached': capture.limited, 'errors': capture.errors,
                'scope': 'Diagnostic only. Pins regular inodes during the unchanged command; '
                         'reads/copies contents only after exit. Gaps before a poll remain possible; '
                         'limits and capture errors never imply complete history or qualification.'}
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return result


if __name__ == '__main__':
    raise SystemExit(main())
