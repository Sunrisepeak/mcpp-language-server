#!/usr/bin/env python3
"""Issue 86: clangd --check on the reporter's file, as a matrix of experiments.

Each experiment is one `okrun clangd.okb --check=<file>` with a variant of the file, of the arguments,
of the environment or of the project's .clangd, and records how clangd ended. An experiment that
ends by an exception (an NTSTATUS exit on Windows, a signal elsewhere) is run again under cdb, which
writes the registers, the bytes at the instruction pointer, the frame-pointer chain, the raw stack
and a full dump; symbolize.py then names the frames from the image's own symbol table.

The variants are the reporter's reduction (issue 86, comment 6816991554), so the run confirms or
refutes each of its rows on a clean machine, plus the switches that tell which part of clangd traps.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

GUARDED = "#ifndef ISSUE86_SYNTHETIC_HPP\n#define ISSUE86_SYNTHETIC_HPP\nstruct Issue86Synthetic {};\n#endif\n"
UNGUARDED = "struct Issue86Synthetic {};\n"
PRAGMA_ONCE = "#pragma once\nstruct Issue86Synthetic {};\n"
HEADER_NAME = "issue86_synthetic.hpp"


def unit(include, imports, body):
    return "module;\n" + include + "\nexport module systop:monitor_view;\n" + imports + "\nexport namespace monitor {\n" + body + "\n}\n"


FTXUI = "#include <ftxui/screen/box.hpp>"
SYNTHETIC = f'#include "{HEADER_NAME}"'
STD = "import std;"
PARTITION = "import :monitor_state;"

# name -> (file text or None for the project's own, synthetic header text or None, why)
VARIANTS = {
    "original": (None, None, "the project's own src/ui/monitor_view.cppm"),
    "ftxui-used": (unit(FTXUI, STD, "    auto f(ftxui::Box&) -> int;"), None, "one ftxui header, import std, a purview use of its type"),
    "ftxui-unused": (unit(FTXUI, STD, "    auto f() -> int;"), None, "the same without the purview use"),
    "guarded-used": (unit(SYNTHETIC, STD, "    auto f(Issue86Synthetic&) -> int;"), GUARDED, "a one-struct header with an include guard, used"),
    "guarded-unused": (unit(SYNTHETIC, STD, "    auto f() -> int;"), GUARDED, "the guarded header, not used"),
    "unguarded-used": (unit(SYNTHETIC, STD, "    auto f(Issue86Synthetic&) -> int;"), UNGUARDED, "the header without its guard, used"),
    "pragma-once-used": (unit(SYNTHETIC, STD, "    auto f(Issue86Synthetic&) -> int;"), PRAGMA_ONCE, "the header under #pragma once, used"),
    "guarded-used-partition-import": (unit(SYNTHETIC, PARTITION, "    auto f(Issue86Synthetic&) -> int;"), GUARDED, "imports a partition of the project, not std"),
    "guarded-used-no-import": (unit(SYNTHETIC, "", "    auto f(Issue86Synthetic&) -> int;"), GUARDED, "imports nothing: the textual-preamble path"),
    "guarded-used-in-variable": (unit(SYNTHETIC, STD, "    inline Issue86Synthetic value;"), GUARDED, "the type used by a variable, not a parameter"),
    "std-type-used": (unit(SYNTHETIC, STD, "    auto f(std::string&) -> int;"), GUARDED, "the guarded header included, a std type used"),
}

NO_INCLUDE_ANALYSIS = "Diagnostics:\n  UnusedIncludes: None\n  MissingIncludes: None\n"

# name -> (variant, extra arguments, arguments removed, environment, .clangd text or None, why)
SWITCHES = {
    "no-include-analysis": ("{crash}", [], [], {}, NO_INCLUDE_ANALYSIS, ".clangd turns unused/missing include analysis off"),
    "no-config": ("{crash}", ["--enable-config=0"], [], {}, None, "no configuration files read"),
    "no-clang-tidy": ("{crash}", ["--clang-tidy=0"], [], {}, None, "clang-tidy off"),
    "no-modules-support": ("{crash}", [], ["--experimental-modules-support"], {}, None, "without --experimental-modules-support"),
    "pch-memory": ("{crash}", ["--pch-storage=memory"], [], {}, None, "the preamble kept in memory"),
    "pch-disk": ("{crash}", ["--pch-storage=disk"], [], {}, None, "the preamble kept on disk"),
    "in-process-modules": ("{crash}", ["--modules-builder-worker-policy=in-process"], [], {}, None, "module builds in clangd's own process"),
    "okrun-no-snapshot": ("{crash}", [], [], {"OKRUN_SNAPSHOT": "0"}, None, "the runner maps no initialiser snapshot"),
    "okrun-place-anywhere": ("{crash}", [], [], {"OKRUN_PLACE": "0"}, None, "the runner places the image anywhere"),
    "fresh-module-cache": ("{crash}", [], [], {}, None, "the module cache of the database removed first"),
}

CDB_SCRIPT = r"""sxd av
sxe ii
g
.echo ===BREAK===
.lastevent
.exr -1
.echo ===REGS===
r
.echo ===ADDR===
!address @rip
.echo ===BYTES===
db @rip-0x40 L0x80
.echo ===DISASM===
ub @rip L12
u @rip L6
.echo ===K===
kn 0x40
.echo ===STACK===
dps @rsp L0x600
.echo ===MODS===
lm
.echo ===THREADS===
~
.echo ===DUMP===
{dump}
.echo ===CHAIN===
r $t1 = 0
.for (r $t0 = @rbp; (@$t0 != 0) & (@$t1 < 0x100); r $t0 = poi(@$t0); r $t1 = @$t1 + 1) { .printf "FRAME %p RET %p\n", @$t0, poi(@$t0+8) }
.echo ===END===
q
"""


# The same questions asked of a dump (Windows Error Reporting's, or procdump's): no `g`, the exception's own context.
CDB_DUMP_SCRIPT = CDB_SCRIPT.replace("sxd av\nsxe ii\ng\n", ".ecxr\n").replace("{dump}", ".echo a dump already")


def status_text(code):
    if code is None:
        return "timeout"
    if os.name == "nt":
        unsigned = code & 0xFFFFFFFF
        return f"0x{unsigned:08X}" if unsigned >= 0x80000000 else str(code)
    return f"signal {-code}" if code < 0 else str(code)


def is_exception(code):
    if code is None:
        return False
    if os.name == "nt":
        return (code & 0xFFFFFFFF) >= 0x80000000
    return code < 0 or code >= 128


def tail(path, count):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            return handle.read().splitlines()[-count:]
    except OSError:
        return []


class Matrix:
    def __init__(self, args):
        self.args = args
        self.out = os.path.abspath(args.out)
        os.makedirs(self.out, exist_ok=True)
        self.file = os.path.abspath(os.path.join(args.project, args.file))
        self.header = os.path.join(os.path.dirname(self.file), HEADER_NAME)
        self.config = os.path.join(os.path.abspath(args.project), ".clangd")
        with open(self.file, "r", encoding="utf-8", newline="") as handle:
            self.original = handle.read()
        self.base_arguments = [f"--check={self.file}", f"--compile-commands-dir={args.database}", "--experimental-modules-support", "--log=verbose"]
        self.results = []

    def command(self, extra, removed):
        arguments = [argument for argument in self.base_arguments if argument not in removed] + list(extra)
        return [self.args.okrun, self.args.image] + arguments

    def stage(self, variant, config):
        text, header, _why = VARIANTS[variant]
        with open(self.file, "w", encoding="utf-8", newline="") as handle:
            handle.write(self.original if text is None else text)
        if os.path.exists(self.header):
            os.remove(self.header)
        if header is not None:
            with open(self.header, "w", encoding="utf-8", newline="") as handle:
                handle.write(header)
        if os.path.exists(self.config):
            os.remove(self.config)
        if config is not None:
            with open(self.config, "w", encoding="utf-8", newline="") as handle:
                handle.write(config)

    def restore(self):
        with open(self.file, "w", encoding="utf-8", newline="") as handle:
            handle.write(self.original)
        for path in (self.header, self.config):
            if os.path.exists(path):
                os.remove(path)

    def run(self, name, variant, extra=(), removed=(), environment=None, config=None, why="", repeat=1):
        self.stage(variant, config)
        if name == "fresh-module-cache":
            shutil.rmtree(os.path.join(self.args.database, ".cache"), ignore_errors=True)
        env = dict(os.environ)
        env.update(environment or {})
        command = self.command(extra, removed)
        runs = []
        for attempt in range(repeat):
            log = os.path.join(self.out, f"{name}.{attempt}.log")
            started = time.time()
            try:
                with open(log, "wb") as sink:
                    done = subprocess.run(command, cwd=self.args.project, env=env, stdin=subprocess.DEVNULL, stdout=sink, stderr=subprocess.STDOUT, timeout=self.args.timeout)
                code = done.returncode
            except subprocess.TimeoutExpired:
                code = None
            lines = tail(log, 400)
            runs.append({
                "exit": status_text(code), "exception": is_exception(code), "seconds": round(time.time() - started, 1), "log": os.path.basename(log),
                "preamble": next((line for line in reversed(lines) if "Built preamble of size" in line), "")[:200],
                "lastStage": next((line for line in reversed(lines) if any(stage in line for stage in ("Building preamble", "Building AST", "Indexing AST", "Building inlay", "Testing features", "All checks completed"))), "")[:160],
                "errors": [line[:240] for line in lines if line.startswith("E[")][:12],
                "tail": [line[:240] for line in lines[-6:]],
            })
        result = {"name": name, "variant": variant, "why": why or VARIANTS[variant][2], "extra": list(extra), "removed": list(removed), "environment": environment or {},
                  "config": config, "command": command, "runs": runs}
        self.results.append(result)
        print(f"[{name}] variant={variant} -> " + ", ".join(f"{run['exit']} ({run['seconds']} s; {run['lastStage'][15:] or 'no stage'})" for run in runs), flush=True)
        return result

    def debug(self, name, variant, extra=(), removed=(), environment=None, config=None, dump=False):
        """The same run under cdb: what the trap is, and where."""
        if not self.args.cdb:
            return None
        self.stage(variant, config)
        output = os.path.join(self.out, f"{name}.cdb.txt")
        script = os.path.join(self.out, f"{name}.cdb.script")
        dump_path = os.path.join(self.args.dumps, f"{name}.dmp")
        os.makedirs(self.args.dumps, exist_ok=True)
        with open(script, "w", encoding="ascii", newline="\r\n") as handle:
            handle.write(CDB_SCRIPT.replace("{dump}", f".dump /ma {dump_path}" if dump else ".echo no dump for this run"))
        env = dict(os.environ)
        env.update(environment or {})
        env["_NT_SYMBOL_PATH"] = ""
        command = [self.args.cdb, "-cf", script] + self.command(extra, removed)
        started = time.time()
        try:
            with open(output, "wb") as sink:
                done = subprocess.run(command, cwd=self.args.project, env=env, stdin=subprocess.DEVNULL, stdout=sink, stderr=subprocess.STDOUT, timeout=self.args.timeout * 3)
            code = status_text(done.returncode)
        except subprocess.TimeoutExpired:
            code = "timeout"
        stack = os.path.join(self.out, f"{name}.stack.txt")
        symbolized = None
        if self.args.elf:
            symbolizer = os.path.join(os.path.dirname(os.path.abspath(__file__)), "symbolize.py")
            done = subprocess.run([sys.executable, symbolizer, "--elf", self.args.elf, "--cdb-output", output, "--out", stack], capture_output=True, text=True)
            symbolized = done.returncode
            print(f"--- {name}: the trap, named from the image's symbol table (cdb ended {code} in {round(time.time() - started, 1)} s) ---", flush=True)
            print(done.stdout[-12000:], flush=True)
            if done.stderr:
                print(done.stderr[-2000:], flush=True)
        return {"name": name, "cdb": os.path.basename(output), "stack": os.path.basename(stack), "cdbExit": code, "symbolized": symbolized,
                "dump": os.path.basename(dump_path) if dump and os.path.exists(dump_path) else None}

    def procdump(self, name, variant):
        """Without cdb: procdump writes a full dump at the first illegal instruction, for analysis elsewhere."""
        if not self.args.procdump:
            return None
        self.stage(variant, None)
        os.makedirs(self.args.dumps, exist_ok=True)
        output = os.path.join(self.out, f"{name}.procdump.txt")
        command = [self.args.procdump, "-accepteula", "-ma", "-e", "1", "-f", "C000001D", "-x", self.args.dumps] + self.command((), ())
        try:
            with open(output, "wb") as sink:
                done = subprocess.run(command, cwd=self.args.project, stdin=subprocess.DEVNULL, stdout=sink, stderr=subprocess.STDOUT, timeout=self.args.timeout * 3)
            code = status_text(done.returncode)
        except subprocess.TimeoutExpired:
            code = "timeout"
        print(f"--- {name}: procdump ended {code} ---", flush=True)
        print("\n".join(tail(output, 30)), flush=True)
        return {"name": name, "procdump": os.path.basename(output), "exit": code}

    def analyze_dumps(self):
        """Dumps Windows Error Reporting wrote for okrun.exe during the replayed session: the same capture, from each."""
        found = []
        if not self.args.cdb or not self.args.wer or not os.path.isdir(self.args.wer):
            return found
        dumps = sorted(os.path.join(self.args.wer, entry) for entry in os.listdir(self.args.wer) if entry.lower().endswith(".dmp"))
        for index, dump in enumerate(dumps[:3]):
            name = f"wer-{index}"
            output = os.path.join(self.out, f"{name}.cdb.txt")
            script = os.path.join(self.out, f"{name}.cdb.script")
            with open(script, "w", encoding="ascii", newline="\r\n") as handle:
                handle.write(CDB_DUMP_SCRIPT)
            env = dict(os.environ)
            env["_NT_SYMBOL_PATH"] = ""
            try:
                with open(output, "wb") as sink:
                    subprocess.run([self.args.cdb, "-z", dump, "-cf", script], env=env, stdin=subprocess.DEVNULL, stdout=sink, stderr=subprocess.STDOUT, timeout=600)
            except subprocess.TimeoutExpired:
                pass
            stack = os.path.join(self.out, f"{name}.stack.txt")
            if self.args.elf:
                symbolizer = os.path.join(os.path.dirname(os.path.abspath(__file__)), "symbolize.py")
                done = subprocess.run([sys.executable, symbolizer, "--elf", self.args.elf, "--cdb-output", output, "--out", stack], capture_output=True, text=True)
                print(f"--- {name}: {os.path.basename(dump)} ({os.path.getsize(dump)} bytes), a clangd of the replayed session ---", flush=True)
                print(done.stdout[-12000:], flush=True)
            found.append({"name": name, "dump": os.path.basename(dump), "cdb": os.path.basename(output), "stack": os.path.basename(stack)})
        return found

    def server_arguments(self):
        """The arguments the server gave the clangd of the replayed session (drive_lsp.py's processes), for --check."""
        try:
            with open(self.args.session, "r", encoding="utf-8") as handle:
                lines = list(json.load(handle).get("processes", {}))
        except (OSError, ValueError, TypeError):
            return None
        for line in lines:
            if "clangd.okb" not in line or "--compile-commands-dir" not in line:
                continue
            if os.name == "nt":
                import ctypes
                count = ctypes.c_int(0)
                ctypes.windll.shell32.CommandLineToArgvW.restype = ctypes.POINTER(ctypes.c_wchar_p)
                parsed = ctypes.windll.shell32.CommandLineToArgvW(ctypes.c_wchar_p(line), ctypes.byref(count))
                words = [parsed[i] for i in range(count.value)]
            else:
                import shlex
                words = shlex.split(line)
            arguments = [word for word in words[2:] if word not in ("--background-index", "--pretty=false") and not word.startswith("--compile-commands-dir")]
            return arguments
        return None

    def main(self):
        debugged = []
        crashed = []
        subject = "original"
        try:
            baseline = self.run("original", "original", repeat=3)
            if any(run["exception"] for run in baseline["runs"]):
                crashed.append("original")
            for variant in ([] if self.args.only_original else VARIANTS):
                if variant == "original":
                    continue
                result = self.run(variant, variant, repeat=2)
                if any(run["exception"] for run in result["runs"]):
                    crashed.append(variant)
            # The switches are thrown on the smallest variant that traps, else on the project's own file.
            preferred = [name for name in ("guarded-used", "ftxui-used", "original") if name in crashed]
            subject = preferred[0] if preferred else (crashed[0] if crashed else "original")
            for name, (variant, extra, removed, environment, config, why) in ({} if self.args.only_original and not crashed else SWITCHES).items():
                self.run(name, subject if variant == "{crash}" else variant, extra=extra, removed=removed, environment=environment, config=config, why=why, repeat=2)
            given = self.server_arguments()
            if given is not None:
                kept = self.base_arguments
                self.base_arguments = [kept[0], kept[1]] + given
                try:
                    self.run("as-the-server-starts-it", subject, why="clangd's arguments as the server gave them in the replayed session", repeat=2)
                    self.run("as-the-server-starts-it-original", "original", why="the same, on the project's own file", repeat=2)
                finally:
                    self.base_arguments = kept
            # Under the debugger: the project's own file (with a dump), and the smallest variant that traps.
            for index, name in enumerate(dict.fromkeys(["original"] + preferred[:1] + crashed[:2])):
                if name in crashed or name == "original":
                    # A full dump only of a run that traps: one at a normal exit says nothing.
                    found = self.debug(name, name, dump=(name in crashed and index == 0))
                    if found:
                        debugged.append(found)
            if not self.args.cdb and crashed:
                self.procdump(crashed[0], crashed[0])
        finally:
            self.restore()
        debugged += self.analyze_dumps()
        summary = {"file": self.file, "database": self.args.database, "image": self.args.image, "crashed": crashed, "subject": subject,
                   "experiments": self.results, "debugged": debugged}
        with open(os.path.join(self.out, "matrix.json"), "w", encoding="utf-8") as handle:
            json.dump(summary, handle, indent=1)
        lines = [f"## clangd --check on `{self.args.file}` (issue 86)", "", f"Database `{self.args.database}`.", "",
                 "| experiment | variant | what it changes | exits | last stage | preamble | first error |", "|---|---|---|---|---|---|---|"]
        for result in self.results:
            exits = ", ".join(run["exit"] for run in result["runs"])
            stage = result["runs"][-1]["lastStage"].split("] ", 1)[-1]
            preamble = result["runs"][-1]["preamble"].split("Built preamble of size ", 1)[-1].split(" for file", 1)[0]
            error = (result["runs"][-1]["errors"] or [""])[0].split("] ", 1)[-1][:110].replace("|", "/")
            lines.append(f"| {result['name']} | {result['variant']} | {result['why']} | {exits} | {stage} | {preamble} | {error} |")
        lines += ["", f"Variants that ended by an exception: {', '.join(crashed) or 'none'}.", ""]
        for found in debugged:
            lines += [f"### {found['name']} under cdb", "", "```"]
            lines += tail(os.path.join(self.out, found["stack"]), 400)[:90]
            lines += ["```", ""]
        with open(os.path.join(self.out, "matrix.md"), "w", encoding="utf-8") as handle:
            handle.write("\n".join(lines) + "\n")
        return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--okrun", required=True)
    parser.add_argument("--image", required=True, help="clangd.okb")
    parser.add_argument("--elf", help="the image's ELF for this architecture (x86_64/clangd.oke)")
    parser.add_argument("--project", required=True)
    parser.add_argument("--file", default="src/ui/monitor_view.cppm")
    parser.add_argument("--database", required=True, help="the directory of the engine database (compile_commands.json)")
    parser.add_argument("--session", default="", help="drive_lsp.py's lsp-summary.json, for the arguments the server gave clangd")
    parser.add_argument("--cdb", default="")
    parser.add_argument("--procdump", default="")
    parser.add_argument("--wer", default="", help="the directory Windows Error Reporting writes okrun.exe's dumps to")
    parser.add_argument("--dumps", default="dumps")
    parser.add_argument("--out", required=True)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--only-original", action="store_true", help="the file as it is, no variants; the switches only when it traps")
    args = parser.parse_args()
    args.project = os.path.abspath(args.project)
    args.database = os.path.abspath(args.database)
    args.dumps = os.path.abspath(args.dumps)
    return Matrix(args).main()


if __name__ == "__main__":
    sys.exit(main())
