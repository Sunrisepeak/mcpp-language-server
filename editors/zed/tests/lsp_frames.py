#!/usr/bin/env python3
"""What one side of an LSP conversation said, from a byte-for-byte copy of it.

    lsp_frames.py <file>

smoke.sh keeps a copy of everything Zed sent mcppls (`tee` on its stdin) and everything mcppls
answered (`tee` on its stdout). This reads such a copy as Content-Length frames and prints one fact
per line, so the shell side needs no JSON:

    client <clientInfo.name>          from `initialize`
    didopen <uri>                     from `textDocument/didOpen`
    diagnostics <uri> <count>         from `textDocument/publishDiagnostics`

A frame still being written at the end of the file is ignored; it shows up on the next read.
"""
import json
import re
import sys


def frames(data):
    position = 0
    header = re.compile(rb"Content-Length:\s*(\d+)\r\n(?:[^\r\n]+\r\n)*\r\n", re.IGNORECASE)
    while True:
        match = header.search(data, position)
        if match is None:
            return
        end = match.end() + int(match.group(1))
        if end > len(data):
            return
        try:
            yield json.loads(data[match.end():end])
        except ValueError:
            pass
        position = end


def main():
    with open(sys.argv[1], "rb") as handle:
        data = handle.read()
    for message in frames(data):
        method = message.get("method")
        params = message.get("params") or {}
        if method == "initialize":
            print("client", (params.get("clientInfo") or {}).get("name", ""))
        elif method == "textDocument/didOpen":
            print("didopen", (params.get("textDocument") or {}).get("uri", ""))
        elif method == "textDocument/publishDiagnostics":
            print("diagnostics", params.get("uri", ""), len(params.get("diagnostics") or []))


main()
