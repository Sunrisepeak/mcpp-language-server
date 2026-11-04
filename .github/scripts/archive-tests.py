"""Archive bounded Windows dev test groups as one verified container per package."""

import argparse
import collections
import copy
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile


PACKAGES = ('mcppls', 'pack', 'devtools', 'model-gateway')
MANIFEST = 'mcpp-tests.json'


def require(condition, detail):
    if not condition:
        raise ValueError(detail)


def digest(stream):
    return hashlib.file_digest(stream, 'sha256').hexdigest()


def safe_path(name):
    path = PurePosixPath(name)
    require(name and not path.is_absolute() and '..' not in path.parts
            and '\\' not in name and ':' not in name and path.parts, f'unsafe archive path: {name}')
    return path


def inspect_archive(path, compressed=False):
    """Hash every payload in forward order, including the final gzip trailer."""
    entries, manifests = {}, []
    with tarfile.open(path, 'r:gz' if compressed else 'r:') as source:
        for member in source:
            name = str(safe_path(member.name))
            require(name not in entries, f'duplicate archive entry: {name}')
            require(member.isfile() or member.isdir(), f'non-file archive entry: {name}')
            checksum = None
            if member.isfile():
                content = source.extractfile(member)
                if PurePosixPath(name).name == MANIFEST:
                    data = content.read(4 * 1024 * 1024 + 1)
                    require(len(data) <= 4 * 1024 * 1024, 'oversized test manifest')
                    checksum = hashlib.sha256(data).hexdigest()
                    manifests.append((copy.copy(member), json.loads(data)))
                else:
                    checksum = digest(content)
            entries[name] = (member.isdir(), member.mode, member.size, checksum)
        # Tar iteration ends at its zero blocks, before gzip's CRC and size.
        while source.fileobj.read(1024 * 1024):
            pass
    require(len(manifests) == 1, 'expected one test manifest')
    member, manifest = manifests[0]
    root = safe_path(member.name).parent
    require(len(root.parts) == 1, 'expected one top-level archive directory')
    require(all(safe_path(name).is_relative_to(root) for name in entries), 'multiple archive roots')
    payload = {str(safe_path(name).relative_to(root)): value for name, value in entries.items()
               if name != str(root / MANIFEST)}
    return dict(root=root, manifest=manifest, manifest_member=member, payload=payload)


def validate_group(info, target, expected):
    manifest = info['manifest']
    require(manifest['schema'] == 1 and len(manifest['legs']) == 1, 'unsupported test manifest')
    require(isinstance(manifest['package'], str) and manifest['package'], 'missing package identity')
    leg = manifest['legs'][0]
    require(leg['triple'] == target, 'test target mismatch')
    require(collections.Counter(t['name'] for t in leg['tests']) == collections.Counter(expected),
            'test group is incomplete or duplicated')
    required = list(leg['programs'].values())
    for test in leg['tests']:
        require(PurePosixPath(test['program']).name == test['name'] + '.exe', 'unexpected test executable')
        required.extend([test['program'], *test['files']])
    for name in required:
        name = str(safe_path(name))
        require(name in info['payload'] and not info['payload'][name][0], f'missing payload: {name}')
    return leg


def reclaimable(info, leg, target_root):
    verified = {}
    for test in leg['tests']:
        executable = PurePosixPath(test['program']).name
        checksum = info['payload'][test['program']][3]
        matches = []
        for path in target_root.glob(f'*/bin/**/{executable}'):
            require(not path.is_symlink() and path.resolve().is_relative_to(target_root),
                    f'test executable leaves target directory: {path}')
            with path.open('rb') as content:
                if digest(content) == checksum:
                    matches.append(path)
        require(matches, f'no built executable matches archived {test["name"]}')
        for path in matches:
            verified[path] = path.stat().st_size
    return verified


class HashedReader:
    def __init__(self, stream):
        self.stream = stream
        self.hash = hashlib.sha256()
        self.size = 0

    def read(self, size=-1):
        data = self.stream.read(size)
        self.hash.update(data)
        self.size += len(data)
        return data


def renamed(member, root, package):
    result = copy.copy(member)
    relative = safe_path(member.name).relative_to(root)
    result.name = str(PurePosixPath(package) / relative)
    # A PAX path overrides TarInfo.name, including names from a previous part.
    result.pax_headers = {key: value for key, value in member.pax_headers.items()
                          if key not in ('path', 'linkpath')}
    return result


def merge_part(archive, info, package, output, inventory, common):
    """Copy each unique payload once; repeated common programs must match exactly."""
    seen = set()
    with tarfile.open(archive, 'r:') as source:
        for member in source:
            relative = str(safe_path(member.name).relative_to(info['root']))
            require(relative not in seen, f'duplicate part payload: {relative}')
            seen.add(relative)
            if relative == MANIFEST:
                require(json.load(source.extractfile(member)) == info['manifest'], 'manifest changed while merging')
                continue
            require(relative in info['payload'], 'part changed while merging')
            expected = info['payload'][relative]
            duplicate = relative in inventory
            if duplicate:
                require(member.isdir() or relative in common, f'overlapping test payload: {relative}')
                require(inventory[relative] == expected, f'common payload changed: {relative}')
            if member.isfile():
                reader = HashedReader(source.extractfile(member))
                if duplicate:
                    while reader.read(1024 * 1024):
                        pass
                else:
                    output.addfile(renamed(member, info['root'], package), reader)
                require((False, member.mode, reader.size, reader.hash.hexdigest()) == expected,
                        f'payload changed while merging: {relative}')
            else:
                require((True, member.mode, member.size, None) == expected, 'directory changed while merging')
                if not duplicate:
                    output.addfile(renamed(member, info['root'], package))
            inventory[relative] = expected
    require(seen == set(info['payload']) | {MANIFEST}, 'part payload disappeared while merging')


def test_groups(selection):
    listed = subprocess.run(['mcpp', 'test', *selection, '--list', '--message-format', 'json'],
                            check=True, text=True, capture_output=True)
    rows = [json.loads(line) for line in listed.stdout.splitlines() if line.strip()]
    names = [row['test'] for row in rows if 'test' in row]
    summaries = [row['summary']['total'] for row in rows if 'summary' in row]
    require(names and len(names) == len(set(names)) and summaries == [len(names)], 'invalid test discovery')
    groups = {word: sorted(name for name in names if word in name) for word in sorted({name[:7] for name in names})}
    # Positional filters are substrings. Prove disjoint, complete selection.
    require(collections.Counter(name for group in groups.values() for name in group) == collections.Counter(names),
            'test filters overlap or omit a test')
    require(max(map(len, groups.values())) <= 4, 'test group exceeds the executable space bound')
    return names, groups


def archive_package(package, target, profile, target_root, archives):
    selection = [] if package == 'mcppls' else ['-p', package]
    names, groups = test_groups(selection)
    final = archives / f'{package}.tar.gz'
    pending = archives / f'.{package}.tar.gz.pending'
    part = archives / f'.{package}-part.tar'
    require(not any(path.exists() for path in (final, pending, part)), 'archive output already exists')
    combined, header, leg_header, inventory, receipts = None, None, None, {}, []
    with tarfile.open(pending, 'w:gz', compresslevel=6, format=tarfile.PAX_FORMAT) as output:
        for word, expected in groups.items():
            print(f'::group::{package}: {", ".join(expected)}', flush=True)
            subprocess.run(['mcpp', 'test', *selection, word, '--profile', profile, '--target', target,
                            '--no-run', '--archive', str(part)], check=True)
            info = inspect_archive(part)
            leg = validate_group(info, target, expected)
            current_header = {key: value for key, value in info['manifest'].items() if key != 'legs'}
            current_leg_header = {key: value for key, value in leg.items() if key != 'tests'}
            if combined is None:
                combined = copy.deepcopy(info['manifest'])
                combined['legs'][0]['tests'] = []
                header, leg_header = current_header, current_leg_header
                manifest_member = renamed(info['manifest_member'], info['root'], package)
            require((header, leg_header) == (current_header, current_leg_header), 'package metadata changed between groups')
            verified = reclaimable(info, leg, target_root)
            merge_part(part, info, package, output, inventory, set(leg['programs'].values()))
            combined['legs'][0]['tests'].extend(copy.deepcopy(leg['tests']))
            output.fileobj.flush()
            # Only this verified group's executable bytes are reclaimed. All
            # modules, objects, common programs and other configurations remain.
            for path in verified:
                path.unlink()
            part.unlink()
            receipts.append(dict(package=package, filter=word, tests=expected,
                                 reclaimed_bytes=sum(verified.values()), free_bytes=shutil.disk_usage('.').free))
            print(json.dumps(receipts[-1]), flush=True)
            print('::endgroup::', flush=True)
        require(collections.Counter(t['name'] for t in combined['legs'][0]['tests']) == collections.Counter(names),
                'combined archive does not cover test discovery')
        data = (json.dumps(combined, indent=2) + '\n').encode()
        manifest_member.size = len(data)
        # mcpp extracts the complete container before reading its manifest.
        output.addfile(manifest_member, io.BytesIO(data))
    checked = inspect_archive(pending, compressed=True)
    require(checked['root'] == PurePosixPath(package), 'final archive root mismatch')
    require(checked['manifest'] == combined and checked['payload'] == inventory, 'final archive verification failed')
    validate_group(checked, target, names)
    with pending.open('rb') as content:
        checksum = digest(content)
    pending.replace(final)
    result = dict(package=package, archive=str(final), sha256=checksum, bytes=final.stat().st_size,
                  tests=names, payload_files=sum(not value[0] for value in inventory.values()), groups=receipts)
    print(json.dumps(result), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', required=True)
    parser.add_argument('--profile', required=True)
    args = parser.parse_args()
    target_root = Path('target', args.target).resolve(strict=True)
    archives = Path('test-archives')
    archives.mkdir()
    receipts = [archive_package(package, args.target, args.profile, target_root, archives) for package in PACKAGES]
    (archives / 'coverage.json').write_text(json.dumps(receipts, indent=2) + '\n')


if __name__ == '__main__':
    main()
