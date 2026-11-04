#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name('capture_rotating_logs.py')
spec = importlib.util.spec_from_file_location('capture', SCRIPT)
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


class CaptureTests(unittest.TestCase):
    def test_unlinked_rotations_remain_readable(self):
        with tempfile.TemporaryDirectory(prefix='mcpp-885-capture-test-') as temp:
            root = Path(temp)
            logs = root / 'logs'
            logs.mkdir()
            observer = capture.Capture(logs)
            path = logs / 'server-session.log'
            for index in range(4):
                path.write_text(f'rotation {index}\n')
                observer.scan()
                previous = logs / 'server-session.log.1'
                path.replace(previous)
                previous.unlink()
            records = observer.export(root / 'out')
            self.assertEqual(len(records), 4)
            self.assertEqual([(root/'out'/r['artifact']).read_text() for r in records],
                             [f'rotation {i}\n' for i in range(4)])

    def test_growth_is_copied_after_capture(self):
        with tempfile.TemporaryDirectory(prefix='mcpp-885-capture-test-') as temp:
            root = Path(temp)
            path = root / 'server-one.log'
            path.write_text('first\n')
            observer = capture.Capture(root)
            observer.scan()
            with path.open('a') as stream:
                stream.write('last\n')
            observer.scan()
            records = observer.export(root / 'out')
            self.assertEqual(len(records), 1)
            self.assertEqual((root/'out'/records[0]['artifact']).read_text(), 'first\nlast\n')

    def test_symlink_files_and_directories_are_not_followed(self):
        with tempfile.TemporaryDirectory(prefix='mcpp-885-capture-test-') as temp:
            root = Path(temp)
            logs = root / 'logs'
            logs.mkdir()
            outside = root / 'outside.txt'
            outside.write_text('outside')
            (logs/'server-link.log').symlink_to(outside)
            observer = capture.Capture(logs)
            observer.scan()
            self.assertEqual(observer.export(root/'out'), [])
            alias = root/'alias'
            alias.symlink_to(logs)
            observer = capture.Capture(alias)
            for _ in range(20):
                observer.scan()
            self.assertEqual(observer.export(root/'alias-out'), [])
            self.assertGreater(len(observer.errors), 0)
            self.assertLessEqual(len(observer.errors), 8)

    def test_file_and_byte_limits_are_explicit(self):
        with tempfile.TemporaryDirectory(prefix='mcpp-885-capture-test-') as temp:
            root = Path(temp)
            (root/'server-a.log').write_text('abcdef')
            (root/'server-b.log').write_text('abcdef')
            observer = capture.Capture(root, files=1, bytes_each=3)
            observer.scan()
            records = observer.export(root/'out')
            self.assertTrue(observer.limited)
            self.assertEqual(len(records), 1)
            self.assertTrue(records[0]['truncated'])
            self.assertEqual(records[0]['bytes'], 3)

    def test_child_exit_and_command_are_preserved(self):
        with tempfile.TemporaryDirectory(prefix='mcpp-885-capture-test-') as temp:
            root = Path(temp)
            command = [sys.executable, '-c', 'raise SystemExit(7)']
            result = subprocess.run([sys.executable, str(SCRIPT), '--cache-dir', str(root/'cache'),
                                     '--output', str(root/'out'), '--', *command])
            self.assertEqual(result.returncode, 7)
            manifest = json.loads((root/'out/manifest.json').read_text())
            self.assertEqual(manifest['command'], command)
            self.assertEqual(manifest['exit_code'], 7)


if __name__ == '__main__':
    unittest.main()
