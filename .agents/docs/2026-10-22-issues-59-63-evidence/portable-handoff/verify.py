#!/usr/bin/env python3
"""Verify the portable PR65 evidence; optionally restore its original relative paths."""
import argparse
import hashlib
import json
from pathlib import Path
import tarfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--extract', action='store_true')
parser.add_argument('--into', type=Path, default=Path.cwd())
args = parser.parse_args()
base = Path(__file__).resolve().parent
metadata = json.loads((base / 'archive.json').read_text())
manifest = json.loads((base / 'manifest.json').read_text())
archive = base / metadata['archive']
assert hashlib.sha256(archive.read_bytes()).hexdigest() == metadata['sha256'], 'archive digest differs'
entries = {item['path']: item for item in manifest['files']}
assert len(entries) == len(manifest['files']) == metadata['files'], 'duplicate manifest paths'
root = args.into.resolve()
with tarfile.open(archive, 'r:gz') as stream:
    members = stream.getmembers()
    assert len(members) == len(entries), 'archive count differs'
    seen = set()
    for member in members:
        path = Path(member.name)
        assert member.isfile() and not path.is_absolute() and '..' not in path.parts
        assert path.parts[0] == '.payload-cache' and member.name not in seen
        seen.add(member.name)
        expected = entries[member.name]
        assert member.size == expected['bytes'], 'size differs: ' + member.name
        content = stream.extractfile(member).read()
        assert hashlib.sha256(content).hexdigest() == expected['sha256'], 'digest differs: ' + member.name
        destination = root / path
        if args.extract and destination.exists():
            assert destination.is_file() and not destination.is_symlink()
            assert hashlib.sha256(destination.read_bytes()).hexdigest() == expected['sha256'], 'refuse to overwrite: ' + str(destination)
    if args.extract:
        for member in members:
            destination = root / member.name
            assert destination.resolve().is_relative_to(root), 'path leaves extraction root'
            if destination.exists():
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            assert destination.parent.resolve().is_relative_to(root)
            with destination.open('xb') as output:
                output.write(stream.extractfile(member).read())
print(json.dumps({'verifiedFiles': len(entries), 'archiveSHA256': metadata['sha256'], 'extracted': args.extract}))
