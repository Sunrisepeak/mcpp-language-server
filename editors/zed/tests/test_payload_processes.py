"""The server and engine can share an okrun executable without sharing an image."""
import json
import os
import signal
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

from payload_processes import matching_processes, payload_processes


@unittest.skipUnless(sys.platform == "linux", "Linux /proc observer")
class PayloadProcessesTest(unittest.TestCase):
    def test_image_identity_and_direct_parent(self):
        with tempfile.TemporaryDirectory(prefix="zed payload processes ") as temporary:
            root = Path(temporary)
            engine = root / "engine.py"
            engine.write_text("import time; time.sleep(60)\n")
            server = root / "server.py"
            server.write_text(
                "import subprocess,sys\n"
                "child=subprocess.Popen([sys.executable,sys.argv[1]])\n"
                "print(child.pid,flush=True)\n"
                "child.wait()\n")
            # Real processes, both with the same executable and different images.
            parent = subprocess.Popen([sys.executable, str(server), str(engine)], stdout=subprocess.PIPE, text=True)
            child_pid = int(parent.stdout.readline())
            peer = subprocess.Popen([sys.executable, str(engine)])
            try:
                manifest = {"server": {"path": str(server)}, "platforms": {"linux-x64": {
                    "runner": sys.executable, "engines": {"clangd": {
                        "runner": sys.executable, "path": str(engine)}}}}}
                (root / "payload.json").write_text(json.dumps(manifest))
                deadline = time.monotonic() + 5
                while payload_processes(root, "linux-x64", "engine") != [child_pid]:
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(0.01)
                self.assertEqual(payload_processes(root, "linux-x64", "server"), [parent.pid])
                self.assertNotIn(peer.pid, payload_processes(root, "linux-x64", "engine"))
                self.assertEqual(matching_processes(sys.executable, engine, {peer.pid}), [])
                # An initialized engine that has died cannot satisfy the live-process check.
                os.kill(parent.pid, signal.SIGSTOP)
                os.kill(child_pid, signal.SIGKILL)
                deadline = time.monotonic() + 5
                while payload_processes(root, "linux-x64", "engine"):
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(0.01)
            finally:
                os.kill(parent.pid, signal.SIGCONT)
                try:
                    parent.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.kill(child_pid, signal.SIGKILL)
                    parent.wait(timeout=5)
                parent.stdout.close()
                peer.terminate()
                peer.wait()

    def test_native_engine(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = shutil.which("sleep")
            child = subprocess.Popen([executable, "60"])
            try:
                deadline = time.monotonic() + 5
                while child.pid not in matching_processes(executable, parents={os.getpid()}):
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(0.01)
                self.assertEqual(matching_processes(executable, parents={child.pid}), [])
            finally:
                child.terminate()
                child.wait()


if __name__ == "__main__":
    unittest.main()
