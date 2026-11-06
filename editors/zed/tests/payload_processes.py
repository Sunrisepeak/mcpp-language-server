#!/usr/bin/env python3
"""Find live Linux payload processes by executable, image and direct parent."""
import argparse
import json
import os
from pathlib import Path


def matching_processes(executable, image=None, parents=None):
    executable = Path(executable).resolve(strict=True)
    image = os.fsencode(Path(image).resolve(strict=True)) if image else None
    found = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            status = dict(line.split(":", 1) for line in (entry / "status").read_text().splitlines())
            if status["State"].strip().startswith(("Z", "X")):
                continue
            if parents is not None and int(status["PPid"]) not in parents:
                continue
            if (entry / "exe").resolve(strict=True) != executable:
                continue
            arguments = (entry / "cmdline").read_bytes().split(b"\0")
            if image is not None and (len(arguments) < 2 or arguments[1] != image):
                continue
            found.append(int(entry.name))
        except (OSError, ValueError, KeyError):
            # A process can exit between any two reads.
            continue
    return sorted(found)


def payload_processes(root, platform, kind):
    root = Path(root).resolve(strict=True)
    payload = json.loads((root / "payload.json").read_text())
    host = payload["platforms"][platform]
    server = payload["server"]["path"]
    runner = host.get("runner")
    parents = matching_processes(root / (runner or server), root / server if runner else None)
    if kind == "server":
        return parents
    engine = host["engines"]["clangd"]
    runner = engine.get("runner")
    return matching_processes(root / (runner or engine["path"]),
                              root / engine["path"] if runner else None, set(parents))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("payload", type=Path)
    parser.add_argument("platform")
    parser.add_argument("kind", choices=("server", "engine"))
    args = parser.parse_args()
    found = payload_processes(args.payload, args.platform, args.kind)
    for pid in found:
        print(pid)
    return 0 if found else 1


if __name__ == "__main__":
    raise SystemExit(main())
