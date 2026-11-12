#!/usr/bin/env python3
"""Issue 86: the reporter's session, without an editor.

Starts the released server the way the VS Code extension does (`okrun mcppls.okb serve --payload ...`),
opens the project and the files the reporter had open, and then behaves as an editor at rest does:
semantic tokens, document symbols, folding ranges and links for each open file, and a small edit now
and then. It watches the server's own log for clangd's exits and starts, records the command line of
every okrun process (the clangd the server started, with its arguments), and, when asked, restarts the
engine in a loop until the sandbox's grant table refuses a start.

What it leaves in --out: the LSP trace, the diagnostics each file ended with, the processes seen, and a
summary; the server's logs, incidents and bundles stay in the cache directory for the workflow to copy.
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys
import threading
import time
from urllib.parse import quote

# The reporter's initialization options (environment.json of bundle auto-engine-start-failed-20261112T065515Z).
INITIALIZATION_OPTIONS = {
    "answer.agent": "cached", "answer.budget": "450", "answer.cache": "on", "answer.prefetch": True,
    "buildDiscovery.askBeforeDownload": True, "buildDiscovery.mcppTarget": "auto", "buildDiscovery.mode": "auto",
    "buildDiscovery.providers": ["mcpp", "cmake", "xmake", "meson", "compile-commands"], "buildProgram.apply": "auto",
    "buildTool": "offline", "compiler": None, "completion": {"provenance": "subtle", "triggerOnSpace": True},
    "conflictArbitration": "client", "engine.confine": False, "engine.diagnosticsWhileTyping": "auto", "engine.name": "clangd",
    "engine.preambleStorage": "auto", "engine.workers": "auto", "extensionVersion": "0.22.0", "format.fallbackStyle": "auto",
    "grants": [], "index.primeImplementationUnits": "auto", "plugin": {"name": "vscode", "version": "0.22.0"},
    "resources": "balanced", "semanticKit": "auto", "semanticTokens": {"moduleType": True, "modules": True}, "toolEnvironment": "auto",
}

CAPABILITIES = {
    "workspace": {"configuration": True, "workspaceFolders": True, "didChangeWatchedFiles": {"dynamicRegistration": True},
                  "semanticTokens": {"refreshSupport": True}, "executeCommand": {"dynamicRegistration": True}},
    "window": {"workDoneProgress": True, "showMessage": {}, "showDocument": {"support": True}},
    "textDocument": {
        "synchronization": {"didSave": True},
        "publishDiagnostics": {"relatedInformation": True, "versionSupport": True, "codeDescriptionSupport": True, "dataSupport": True},
        "completion": {"completionItem": {"snippetSupport": True, "insertReplaceSupport": True, "labelDetailsSupport": True}},
        "hover": {"contentFormat": ["markdown", "plaintext"]},
        "documentSymbol": {"hierarchicalDocumentSymbolSupport": True},
        "foldingRange": {"lineFoldingOnly": True},
        "documentLink": {"tooltipSupport": True},
        "semanticTokens": {"requests": {"full": {"delta": True}, "range": True}, "formats": ["relative"],
                           "tokenTypes": ["namespace", "type", "class", "enum", "interface", "struct", "typeParameter", "parameter", "variable",
                                          "property", "enumMember", "event", "function", "method", "macro", "keyword", "modifier", "comment",
                                          "string", "number", "regexp", "operator", "decorator"],
                           "tokenModifiers": ["declaration", "definition", "readonly", "static", "deprecated", "abstract", "async",
                                              "modification", "documentation", "defaultLibrary"]},
    },
}


def uri_of(path):
    path = os.path.abspath(path).replace("\\", "/")
    if os.name == "nt" and len(path) > 1 and path[1] == ":":
        # VS Code's spelling: a lower-case drive letter and an encoded colon.
        return "file:///" + path[0].lower() + "%3A" + quote(path[2:])
    return "file://" + quote(path)


class Session:
    def __init__(self, args):
        self.args = args
        self.out = os.path.abspath(args.out)
        os.makedirs(self.out, exist_ok=True)
        self.trace = open(os.path.join(self.out, "lsp-trace.jsonl"), "w", encoding="utf-8")
        self.lock = threading.Lock()
        self.next_id = 1
        self.pending = {}
        self.diagnostics = {}
        self.messages = []
        self.processes = {}
        self.started = time.time()
        self.closed = False
        env = dict(os.environ)
        env["MCPPLS_CACHE_DIR"] = os.path.abspath(args.cache)
        if args.engine_arguments:
            env["MCPPLS_ENGINE_ARGUMENTS"] = args.engine_arguments
        command = [args.okrun, args.server, "serve", "--payload", os.path.abspath(args.payload), "--log-level", "debug"]
        print("server:", " ".join(command), flush=True)
        self.process = subprocess.Popen(command, cwd=args.root, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=open(os.path.join(self.out, "server-stderr.log"), "wb"))
        threading.Thread(target=self.read, daemon=True).start()

    def record(self, direction, message):
        text = json.dumps(message, ensure_ascii=False)
        with self.lock:
            self.trace.write(json.dumps({"t": round(time.time() - self.started, 3), "dir": direction, "len": len(text), "msg": text[:4000]}) + "\n")
            self.trace.flush()

    def send(self, message):
        body = json.dumps(message).encode("utf-8")
        self.record("out", message)
        try:
            with self.lock:
                self.process.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
                self.process.stdin.flush()
        except (OSError, ValueError):
            self.closed = True

    def request(self, method, params):
        with self.lock:
            identifier = self.next_id
            self.next_id += 1
            self.pending[identifier] = method
        self.send({"jsonrpc": "2.0", "id": identifier, "method": method, "params": params})
        return identifier

    def notify(self, method, params):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def read(self):
        stream = self.process.stdout
        while True:
            header = b""
            while not header.endswith(b"\r\n\r\n"):
                byte = stream.read(1)
                if not byte:
                    self.closed = True
                    return
                header += byte
            length = 0
            for line in header.split(b"\r\n"):
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":", 1)[1])
            try:
                message = json.loads(stream.read(length))
            except ValueError:
                continue
            self.record("in", message)
            method = message.get("method")
            if method and "id" in message:
                # A request of the server's: configuration is answered with nothing set, the rest with null.
                result = None
                if method == "workspace/configuration":
                    result = [None for _ in message.get("params", {}).get("items", [])]
                self.send({"jsonrpc": "2.0", "id": message["id"], "result": result})
            elif method == "textDocument/publishDiagnostics":
                params = message["params"]
                self.diagnostics[params["uri"]] = [
                    {"line": d["range"]["start"]["line"], "severity": d.get("severity"), "code": d.get("code"), "source": d.get("source"), "message": d.get("message", "")[:300]}
                    for d in params.get("diagnostics", [])]
            elif method in ("window/showMessage", "window/logMessage"):
                self.messages.append({"t": round(time.time() - self.started, 1), "method": method, "type": message["params"].get("type"), "message": message["params"].get("message", "")[:600]})
            elif method and method.startswith("mcppls/"):
                self.messages.append({"t": round(time.time() - self.started, 1), "method": method, "params": json.dumps(message.get("params"))[:600]})
            elif "id" in message:
                with self.lock:
                    self.pending.pop(message["id"], None)

    def log_counts(self):
        """What the server's own log says so far: clangd's starts and exits, a refused start."""
        counts = {"starts": 0, "exits": 0, "refused": 0, "setAside": 0, "exitCodes": [], "refusal": ""}
        for path in glob.glob(os.path.join(os.path.abspath(self.args.cache), "logs", "*.log")):
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as handle:
                    for line in handle:
                        if "is given" in line and "directories" in line:
                            counts["starts"] += 1
                        match = re.search(r"clangd exited with (-?\d+)", line)
                        if match:
                            counts["exits"] += 1
                            counts["exitCodes"].append(int(match.group(1)))
                        if "could not be started" in line:
                            counts["refused"] += 1
                            counts["refusal"] = line.strip()[:700]
                        if "aside from clangd" in line:
                            counts["setAside"] += 1
            except OSError:
                pass
        return counts

    def snapshot_processes(self):
        """The command line of every okrun process: the clangd the server started, with its arguments."""
        try:
            if os.name == "nt":
                script = "Get-CimInstance Win32_Process -Filter \"Name='okrun.exe'\" | Select-Object ProcessId,ParentProcessId,CommandLine | ConvertTo-Json -Compress"
                done = subprocess.run(["powershell", "-NoProfile", "-Command", script], capture_output=True, text=True, timeout=30)
                found = json.loads(done.stdout) if done.stdout.strip() else []
                found = found if isinstance(found, list) else [found]
                for process in found:
                    line = process.get("CommandLine") or ""
                    if line and line not in self.processes:
                        self.processes[line] = {"pid": process.get("ProcessId"), "parent": process.get("ParentProcessId"), "t": round(time.time() - self.started, 1)}
            else:
                done = subprocess.run(["ps", "-eo", "pid,ppid,args"], capture_output=True, text=True, timeout=30)
                for row in done.stdout.splitlines():
                    pid, parent, line = (row.split(None, 2) + ["", "", ""])[:3]
                    if line.split(" ", 1)[0].endswith("/okrun") and ".okb" in line:
                        if line not in self.processes:
                            self.processes[line] = {"pid": int(pid), "parent": int(parent), "t": round(time.time() - self.started, 1)}
        except (OSError, ValueError, subprocess.SubprocessError):
            pass

    def run(self):
        args = self.args
        root = os.path.abspath(args.root)
        self.request("initialize", {
            "processId": os.getpid(), "clientInfo": {"name": "Visual Studio Code", "version": "1.146.0"}, "locale": "en",
            "rootPath": root, "rootUri": uri_of(root), "workspaceFolders": [{"uri": uri_of(root), "name": os.path.basename(root)}],
            "capabilities": CAPABILITIES, "initializationOptions": INITIALIZATION_OPTIONS, "trace": "off"})
        deadline = time.time() + 60
        while "initialize" in self.pending.values() and time.time() < deadline and not self.closed:
            time.sleep(0.1)
        self.notify("initialized", {})
        documents = {}
        for relative in args.open:
            path = os.path.join(root, relative)
            with open(path, "r", encoding="utf-8") as handle:
                text = handle.read()
            documents[relative] = {"uri": uri_of(path), "version": 1, "text": text}
            self.notify("textDocument/didOpen", {"textDocument": {"uri": uri_of(path), "languageId": "cpp", "version": 1, "text": text}})
            time.sleep(0.3)

        edited = args.open[0] if args.open else None
        last_edit = time.time()
        last_report = 0.0
        last_snapshot = 0.0
        end = time.time() + args.duration
        counts = self.log_counts()
        while time.time() < end and not self.closed:
            for relative, document in documents.items():
                identifier = {"textDocument": {"uri": document["uri"]}}
                self.request("textDocument/semanticTokens/full", identifier)
                self.request("textDocument/documentSymbol", identifier)
                self.request("textDocument/foldingRange", identifier)
                self.request("textDocument/documentLink", identifier)
            if edited and time.time() - last_edit > args.edit_every:
                # A person's edit: a comment at the end of the file, replaced each time (full-document sync is always accepted).
                document = documents[edited]
                document["version"] += 1
                text = document["text"] + f"\n// issue 86: edit {document['version']}\n"
                self.notify("textDocument/didChange", {"textDocument": {"uri": document["uri"], "version": document["version"]}, "contentChanges": [{"text": text}]})
                last_edit = time.time()
            if time.time() - last_snapshot > 4:
                self.snapshot_processes()
                last_snapshot = time.time()
            counts = self.log_counts()
            if time.time() - last_report > 15:
                print(f"[{int(time.time() - self.started):4d} s] clangd starts {counts['starts']}, exits {counts['exits']} {counts['exitCodes'][-5:]}, set aside {counts['setAside']}, refused {counts['refused']}", flush=True)
                last_report = time.time()
            if counts["refused"] or (args.stop_after_exits and counts["exits"] >= args.stop_after_exits):
                break
            time.sleep(args.poll)

        # The grant table: restart the engine until a start is refused, or the count is spent.
        restarts = 0
        while restarts < args.restarts and not self.closed and not counts["refused"]:
            self.request("workspace/executeCommand", {"command": "mcppls.restartEngine", "arguments": []})
            restarts += 1
            waited = time.time() + args.restart_wait
            before = counts["starts"]
            while time.time() < waited:
                time.sleep(1)
                counts = self.log_counts()
                if counts["refused"] or counts["starts"] > before:
                    break
            time.sleep(2)
            print(f"restart {restarts}: clangd starts {counts['starts']}, exits {counts['exits']}, refused {counts['refused']}", flush=True)
        counts = self.log_counts()
        self.snapshot_processes()

        self.request("workspace/executeCommand", {"command": "mcppls.exportBundle", "arguments": []})
        time.sleep(20)
        self.request("shutdown", None)
        time.sleep(3)
        self.notify("exit", None)
        try:
            self.process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.process.kill()
        summary = {"seconds": round(time.time() - self.started, 1), "counts": counts, "restartsAsked": restarts, "serverExit": self.process.poll(),
                   "diagnostics": self.diagnostics, "messages": self.messages[-200:], "processes": self.processes, "unanswered": sorted(set(self.pending.values()))}
        with open(os.path.join(self.out, "lsp-summary.json"), "w", encoding="utf-8") as handle:
            json.dump(summary, handle, indent=1, ensure_ascii=False)
        lines = ["## The reporter's session, replayed (issue 86)", "",
                 f"- {summary['seconds']} s; clangd starts **{counts['starts']}**, exits **{counts['exits']}** (codes the server recorded: {sorted(set(counts['exitCodes']))}), files set aside {counts['setAside']}, engine restarts asked {restarts}",
                 f"- a start refused: **{'yes' if counts['refused'] else 'no'}** {('`' + counts['refusal'] + '`') if counts['refusal'] else ''}", ""]
        for uri, found in self.diagnostics.items():
            lines.append(f"- `{uri.rsplit('/', 1)[-1]}`: {len(found)} diagnostics" + (": " + "; ".join(f"L{d['line'] + 1} {d['code']} {d['message'][:80]}" for d in found[:4]) if found else ""))
        lines.append("")
        for line in list(self.processes)[:8]:
            lines.append(f"- process: `{line[:1500]}`")
        with open(os.path.join(self.out, "lsp-summary.md"), "w", encoding="utf-8") as handle:
            handle.write("\n".join(lines) + "\n")
        print("\n".join(lines), flush=True)
        return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--okrun", required=True)
    parser.add_argument("--server", required=True, help="payload/bin/mcppls.okb")
    parser.add_argument("--payload", required=True)
    parser.add_argument("--root", required=True)
    parser.add_argument("--cache", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--open", nargs="*", default=[])
    parser.add_argument("--duration", type=int, default=300)
    parser.add_argument("--poll", type=float, default=2.0)
    parser.add_argument("--edit-every", type=float, default=20.0)
    parser.add_argument("--stop-after-exits", type=int, default=0)
    parser.add_argument("--restarts", type=int, default=0)
    parser.add_argument("--restart-wait", type=int, default=25)
    parser.add_argument("--engine-arguments", default="")
    return Session(parser.parse_args()).run()


if __name__ == "__main__":
    sys.exit(main())
