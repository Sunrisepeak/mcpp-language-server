"""Read-only clangd review probes. Outputs JSON; edits travel over LSP only.

python3 direct_probe.py CLANGD QT_PROJECT CDB
"""
import json
import pathlib
import queue
import subprocess
import sys
import threading
import time


class Client:
    def __init__(self, command):
        self.proc = subprocess.Popen(command, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.responses = queue.Queue()
        self.serial = 0
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        stream = self.proc.stdout
        while True:
            headers = {}
            line = stream.readline()
            if not line:
                return
            while line.strip():
                key, _, value = line.decode().partition(":")
                headers[key.lower()] = value.strip()
                line = stream.readline()
            self.responses.put(json.loads(stream.read(int(headers["content-length"]))))

    def send(self, method, params=None, notification=False):
        message = {"jsonrpc": "2.0", "method": method, "params": params}
        if not notification:
            self.serial += 1
            message["id"] = self.serial
        data = json.dumps(message).encode()
        self.proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(data) + data)
        self.proc.stdin.flush()
        if notification:
            return
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            answer = self.responses.get(timeout=max(.1, deadline - time.monotonic()))
            if answer.get("id") == self.serial:
                return answer
        raise TimeoutError(method)

    def close(self):
        self.send("shutdown")
        self.send("exit", notification=True)
        self.proc.stdin.close()
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()


def main():
    binary, project, cdb = sys.argv[1:]
    project = pathlib.Path(project)
    result = {"binary": binary, "version": subprocess.check_output(
        [binary, "--version"], text=True).splitlines()[0]}
    client = Client([binary, "--experimental-modules-support", "--background-index",
                     "--header-insertion=never", "-j=4", "--compile-commands-dir=" + cdb])
    client.send("initialize", {"processId": None, "rootUri": project.as_uri(), "capabilities": {}})
    client.send("initialized", {}, True)
    path = project / "src/main.cpp"
    raw = path.read_text()
    uri = path.as_uri()
    client.send("textDocument/didOpen", {"textDocument": {
        "uri": uri, "languageId": "cpp", "version": 1, "text": raw}}, True)
    time.sleep(5)
    cases = []
    for version, prefix in enumerate(["nlohmann::j", "std::ve", "cli.", "cli.ad"], 2):
        marker = "    cli.process(app);"
        injection = "\n    " + prefix
        text = raw.replace(marker, marker + injection, 1)
        offset = text.index(injection) + len(injection)
        before = text[:offset]
        client.send("textDocument/didChange", {"textDocument": {"uri": uri, "version": version},
                    "contentChanges": [{"text": text}]}, True)
        time.sleep(.5)
        samples = []
        for _ in range(3):
            start = time.monotonic()
            answer = client.send("textDocument/completion", {"textDocument": {"uri": uri},
                "position": {"line": before.count("\n"), "character": len(before.rsplit("\n", 1)[-1])}})
            data = answer.get("result") or {}
            items = data.get("items", []) if isinstance(data, dict) else data
            samples.append({"ms": round((time.monotonic() - start) * 1000, 1),
                            "labels": [item["label"] for item in items], "error": answer.get("error")})
        cases.append({"prefix": prefix, "samples": samples})
    client.close()
    result["completion"] = cases
    client = Client([binary, "--background-index=false"])
    client.send("initialize", {"processId": None, "rootUri": project.as_uri(), "capabilities": {}})
    client.send("initialized", {}, True)
    uri = (project / "range-boundary-review.cpp").as_uri()
    client.send("textDocument/didOpen", {"textDocument": {
        "uri": uri, "languageId": "cpp", "version": 1,
        "text": "int before = 0; int inside = 1; int after = 2;\n"}}, True)
    result["range"] = client.send("textDocument/semanticTokens/range", {
        "textDocument": {"uri": uri}, "range": {
            "start": {"line": 0, "character": 20}, "end": {"line": 0, "character": 34}}})
    client.close()
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
