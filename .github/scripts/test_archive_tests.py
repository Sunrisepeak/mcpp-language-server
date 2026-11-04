"""Exercise archive merging and fail-before-reclaim checks with real test names."""

import contextlib
import gzip
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location('archive_tests', Path(__file__).with_name('archive-tests.py'))
archives = importlib.util.module_from_spec(spec)
spec.loader.exec_module(archives)
REPO = Path(__file__).resolve().parents[2]
TARGET = 'x86_64-windows-gnu'
TEST_DIRS = dict(mcppls='tests', pack='modules/pack/tests', devtools='tools/devtools/tests',
                 **{'model-gateway': 'tools/model-gateway/tests'})


class ArchiveTests(unittest.TestCase):
    def exercise(self, fault=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target_root = root / 'target' / TARGET
            bindir = target_root / 'configuration' / 'bin'
            bindir.mkdir(parents=True)
            preserved = {bindir / 'mcppls.exe': b'common program',
                         target_root / 'configuration' / 'dependency.pcm': b'keep module',
                         target_root / 'configuration' / 'module.o': b'keep object',
                         target_root / 'older' / 'bin' / 'test_answer.exe': b'other configuration'}
            for path, content in preserved.items():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(content)
            names = {package: sorted(path.stem for path in (REPO / where).glob('test_*.cpp'))
                     for package, where in TEST_DIRS.items()}
            latest, built, calls = [], [], []

            def fake_mcpp(argv, **kwargs):
                self.assertEqual(argv[:2], ['mcpp', 'test'])
                package = argv[argv.index('-p') + 1] if '-p' in argv else 'mcppls'
                if '--list' in argv:
                    discovered = names[package]
                    if fault == 'overlapping-filters':
                        discovered = ['test_answer', 'test_test_answer']
                    rows = [dict(test=name) for name in discovered] + [dict(summary=dict(total=len(discovered)))]
                    return subprocess.CompletedProcess(argv, 0, '\n'.join(map(json.dumps, rows)))
                self.assertEqual(argv[argv.index('--profile') + 1], 'dev')
                self.assertEqual(argv[argv.index('--target') + 1], TARGET)
                self.assertIn('--no-run', argv)
                word = argv[4] if '-p' in argv else argv[2]
                selected = [name for name in names[package] if word in name]
                latest.clear()
                files, tests = {}, []
                for name in selected:
                    path = bindir / package / (name + '.exe')
                    path.parent.mkdir(parents=True, exist_ok=True)
                    content = f'{package}/{name}: debug executable'.encode()
                    path.write_bytes(content)
                    latest.append(path)
                    built.append(path)
                    program = f'legs/{TARGET}/tests/{name}/{name}.exe'
                    runtime = f'legs/{TARGET}/tests/{name}/' + 'long-runtime-' * 12 + '.dll'
                    files[program] = (content, 0o755)
                    files[runtime] = (b'runtime payload', 0o644)
                    tests.append(dict(name=name, program=program, files=[runtime]))
                common = f'legs/{TARGET}/programs/mcppls.exe'
                files[common] = (b'common program', 0o755)
                manifest = dict(schema=1, package='actual-' + package, mcpp='2026.11.3.1', cwd='',
                                opaque=dict(preserve=True),
                                legs=[dict(triple=TARGET, programs={'MCPP_BIN_MCPPLS': common}, tests=tests)])
                if not calls:
                    if fault == 'omission':
                        tests.pop()
                    elif fault == 'duplicate-test':
                        tests.append(tests[0])
                    elif fault == 'wrong-executable':
                        files[tests[0]['program']] = (b'wrong payload', 0o755)
                    elif fault == 'missing-common':
                        del files[common]
                    elif fault == 'unsafe-path':
                        files['../escape'] = (b'outside', 0o644)
                if calls:
                    if fault == 'changed-common-bytes':
                        files[common] = (b'changed common program', 0o755)
                    elif fault == 'changed-common-mode':
                        files[common] = (b'common program', 0o644)
                    elif fault == 'changed-metadata':
                        manifest['cwd'] = 'different'
                part = Path(argv[argv.index('--archive') + 1])
                self.assertEqual(part.suffix, '.tar')
                old_root = 'part-' + str(len(calls))
                with tarfile.open(part, 'w:', format=tarfile.PAX_FORMAT) as output:
                    payload = {archives.MANIFEST: (json.dumps(manifest).encode(), 0o644), **files}
                    for name, (data, mode) in payload.items():
                        member = tarfile.TarInfo(f'{old_root}/{name}')
                        member.size, member.mode = len(data), mode
                        member.pax_headers = {'path': member.name}
                        output.addfile(member, io.BytesIO(data))
                        if fault == 'duplicate-entry' and not calls and name == common:
                            output.addfile(member, io.BytesIO(data))
                if fault == 'truncated-part' and not calls:
                    part.write_bytes(part.read_bytes()[:1000])
                calls.append(dict(package=package, names=selected))
                return subprocess.CompletedProcess(argv, 0)

            original_inspect = archives.inspect_archive

            def inspect(path, compressed=False):
                if compressed and fault == 'final-crc':
                    data = bytearray(path.read_bytes())
                    data[-8] ^= 1
                    path.write_bytes(data)
                elif compressed and fault == 'final-omission':
                    changed = io.BytesIO()
                    with tarfile.open(path, 'r:gz') as source, tarfile.open(fileobj=changed, mode='w:') as output:
                        removed = False
                        for member in source:
                            if member.isfile() and '/tests/' in member.name and not removed:
                                removed = True
                                continue
                            output.addfile(member, source.extractfile(member) if member.isfile() else None)
                    path.write_bytes(gzip.compress(changed.getvalue()))
                return original_inspect(path, compressed)

            previous = Path.cwd()
            try:
                os.chdir(root)
                with patch.object(archives.subprocess, 'run', fake_mcpp), patch.object(archives, 'inspect_archive', inspect), \
                        contextlib.redirect_stdout(io.StringIO()):
                    if fault:
                        with self.assertRaises((ValueError, EOFError, tarfile.TarError, OSError)):
                            self.run_main()
                    else:
                        self.run_main()
                output = root / 'test-archives'
                if fault:
                    self.assertFalse(list(output.glob('*.tar.gz')))
                    if fault not in ('final-crc', 'final-omission'):
                        self.assertTrue(all(path.exists() for path in latest), 'invalid group was reclaimed')
                else:
                    self.assertEqual(sorted(path.name for path in output.iterdir()),
                                     ['coverage.json', 'devtools.tar.gz', 'mcppls.tar.gz',
                                      'model-gateway.tar.gz', 'pack.tar.gz'])
                    receipt = json.loads((output / 'coverage.json').read_text())
                    self.assertEqual(sum(len(row['tests']) for row in receipt), sum(map(len, names.values())))
                    for package in archives.PACKAGES:
                        info = original_inspect(output / f'{package}.tar.gz', True)
                        self.assertEqual(str(info['root']), package)
                        self.assertEqual(info['manifest']['package'], 'actual-' + package)
                        self.assertEqual(info['manifest']['mcpp'], '2026.11.3.1')
                        self.assertEqual(info['manifest']['opaque'], dict(preserve=True))
                        self.assertEqual(len(info['payload']), 2 * len(names[package]) + 1)
                        self.assertEqual({t['name'] for t in info['manifest']['legs'][0]['tests']}, set(names[package]))
                    self.assertTrue(all(not path.exists() for path in built))
                    self.assertGreater(len(calls), len(archives.PACKAGES))
                for path, content in preserved.items():
                    self.assertEqual(path.read_bytes(), content)
                self.assertFalse((root / 'escape').exists())
            finally:
                os.chdir(previous)

    def run_main(self):
        with patch('sys.argv', ['archive-tests.py', '--target', TARGET, '--profile', 'dev']):
            archives.main()

    def test_all_discovered_tests_share_four_canonical_archives(self):
        self.exercise()

    def test_invalid_parts_are_refused_before_reclaiming_their_executables(self):
        for fault in ('overlapping-filters', 'omission', 'duplicate-test', 'wrong-executable',
                      'missing-common', 'unsafe-path', 'duplicate-entry', 'truncated-part',
                      'changed-common-bytes', 'changed-common-mode', 'changed-metadata'):
            with self.subTest(fault=fault):
                self.exercise(fault)

    def test_final_corruption_is_refused_before_publication(self):
        for fault in ('final-crc', 'final-omission'):
            with self.subTest(fault=fault):
                self.exercise(fault)


if __name__ == '__main__':
    unittest.main()
