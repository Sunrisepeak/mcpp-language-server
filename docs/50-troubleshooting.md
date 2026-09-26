# When something is wrong

## Start here

**C++ Modules: Collect Diagnostic Report** (or `mcppls report` on the command line). It opens JSON
carrying what almost every question turns out to need:

| In the report | Answers |
|---|---|
| `project.source`, `project.tier`, `project.level` | Where the build description came from (`tier`, the README's L1..L4), and how complete its document is (`level`, S1's own number — a different question from `tier`) |
| `project.notices` | Facts worth knowing that cost no feature: a stale database entry dropped, a generated module recovered instead of stubbed, a producer negotiated in place of the project's pinned mcpp |
| `project.origin` / `firstOrigin` | Whether this session started from the cache, from the build tool, or from scanned sources |
| `project.producer`, `producerRun` | Which build tool ran, how long it took, how it ended, whether it was offline |
| `toolEnvironment` | Which environment build tools were started in, and the **names** of the variables that differ from the editor's (never the values) |
| `toolRuns` | The last twenty external runs, each with its command, duration and outcome |
| `plan` | What the engine was given: entries, stand-ins, what was left out and why |
| `engines` | clangd's state, restarts, files set aside, and `workarounds`: the clangd defects this server works around for this version |
| `events` | A journal of the session |
| `logTail` | The end of the log |

Log files outlive the editor: the report names the path, and they are kept under the cache
directory with timestamps.

The report is made to be shared: your home directory is `~` in it, your user and machine names are
`<user>` and `<host>`, and anything that looks like a secret (a token, a password, an API key, an
e-mail address) is `<redacted>`. The project's own paths are kept — they are what it is read for.

**C++ Modules: Export Diagnostic Bundle** goes further: one zip, written under the cache directory's
`bundles/` (the newest five are kept) and never uploaded, with everything a problem usually needs —

| In the bundle | What it is |
|---|---|
| `report.json` | The report above |
| `environment.json` | System, editor and extension versions, the other C/C++ extensions, your mcppls settings, the payload, the toolchains found, and a few environment variables (`PATH`, `LANG`, `LC_*`, `MCPP_*`, `XLINGS_*`) — no other |
| `logs/` | The server's logs of the last three sessions and any other of the last day, and the extension's own log |
| `incidents/` | What the server wrote down when clangd crashed, hung or was set aside |
| `engine/` | The database clangd was given, and the plan behind it |
| `manifest.json` | Every file with its size and SHA-256, and how many replacements each redaction rule made |

— with the same replacements in every file. Before anything is written, the bundle is searched for
your home directory, user name and host name in every spelling; if any is left, **no bundle is
written** and the message says which file, and *Retry with Project Paths Hidden* replaces the
project's paths too. Source files are never included; an incident carries only the lines it is
about. Other editors run the same command as `workspace/executeCommand` `mcppls.exportBundle`, and
on the command line:

```bash
mcppls report --bundle problem.zip --root path/to/project   # --hide-project-paths, --no-source-excerpts
```

Crash dumps are left out unless asked for (`--include-dumps`): they hold memory, which cannot be
redacted. `--no-redact` keeps everything as it is, for looking at a problem on your own machine; it
is not offered in the editor.

## Symptoms

**Nothing works — no go-to-definition anywhere.** Look at `project.source` in the report. If it is
`inferred` on a project that has a build system, the build tool did not answer; `project.issues`
and the last `toolRuns` entry say why. A common cause is the build tool needing a download: the
status bar offers to run it in your terminal.

**Go-to-definition works, completion does not, or the standard library is missing.** Look at
`profile`. A semantic kit means no usable compiler was found; `import std` still resolves, but
diagnostics come from libc++ rather than from your toolchain.

**It was fine, then a module stopped resolving.** `plan.standIns` lists modules nothing provides —
mcppls gives them stand-in units so that one broken module does not take the rest of the project
with it, and names each one in the log with the reason. The real failure is in `plan.issues` or in
your build.

**A module does not compile.** Only what imports it, directly or not, is affected: those files are
answered at once by mcppls's own engine (module navigation, symbols, `import` completion), carry one
`module-failed` diagnostic on the import that leads to the failure, and are not sent to clangd until
the failed module's own source or command changes; everything else keeps clangd. This is a problem
in the code, so it is told where it is, as diagnostics: the status stays *ready* (listing
`modules-doomed`, category `code`) and is never *preparing* for good. The engine's `doomedModules`
and `filesRoutedToOwnEngine` in the report list them.

**What the status says, and what it does not.** A mistake in your code (a missing `;`, an import
of a module nothing provides, a module that does not compile) is a diagnostic where it is, in the
Problems list; the status stays *ready*. *degraded* means the server lost something it would
otherwise give you, and names what and where: "clangd stopped responding on main.cpp",
"the workspace is not trusted", "the macOS SDK was not found". Each status issue carries a
`category` (`code`, `engine`, `environment`, `project`) saying whose problem it is; only the ones
other than `code` make the state *degraded*, and only once that has lasted three seconds, so a
condition that passes by itself never reaches the status bar.

**The editor froze while typing an `import` (0.0.3 and earlier).** clangd 23.1 never finishes a
file in which a module name ends in `.` at the end of its line (`import hello.`, `export module a.`),
and every later version of the file waits behind it: typing any dotted import went through that
text. mcppls 0.0.4 gives clangd the line with `;` right after the dot instead, which clangd reports
at once (workaround `WA-CLANGD-001`); `engines[].details.workarounds` in the report lists it.

**While typing an import with autosave on, the file loses clangd for a few seconds.** clangd reads a
file's imports from its text on disk, not from the editor (0.0.4 and earlier could stall there for
good: every request for the file unanswered after `import hello.` was autosaved). When a save puts
something on disk clangd would stall on — a module name ending in `.`, or an import of a module the
project does not have (yet) — the file is answered by mcppls's own engine until it is saved again;
the status lists it as `file-unsafe-on-disk`, category `code`, with the reason, and stays *ready*.
A module nothing provides gets a stand-in within a second of the save, and the file goes back to
clangd once clangd has read the database with it, about six seconds later.

**"Import directive must end with a ';'" on the wrong line, or "module X not found" for an import you
just typed.** clangd reports a directive missing its `;` on the code after it; mcppls moves the
diagnostic back onto the directive (workaround `WA-CLANGD-006`). An import typed and not saved yet is
not built by clangd until the file is saved (it reads imports from disk); while the module is in the
project, that is an information-level "module 'X' is in the project; clangd loads it once the file is
saved", not an error (`WA-CLANGD-007`).

**"clangd would not finish main.cpp".** A file's build ran past its budget — five times its own
last build, never under 20 s — while the editor waited on it: clangd will not finish it, busy or
not. The file is answered by mcppls's own engine, with module-level features, until its text
changes (the exact text clangd stopped on is never given back to it), and clangd is restarted at
once without it. The `events` journal has an `engine-spin` entry with the numbers.

**Is a workaround still needed?** `--disable-workaround WA-CLANGD-<n>` (repeatable) turns one off;
the log's first lines name the ones in use. Each has a canary in the conformance suite
(`workaround-canaries`) that fails once a clangd update fixes its defect.

**The editor found a different compiler than my terminal.** `toolEnvironment.source` should say
`login-shell`. If it says `editor`, the reason is in `toolEnvironment.reason` — an editor started
from a desktop entry has none of your shell configuration. `mcppls.toolEnvironment` controls this.

**Everything is slow to start.** The second session should be fast: the model is cached with a
fingerprint of everything the build tool read, and a session that matches plans with it at once and
confirms it in the background. `project.firstOrigin` says which happened. If it is always
`producer`, the fingerprint is not matching — the report's `project.producerRun` and the build
files' timestamps are where to look.

**clangd keeps restarting.** `engines[].restarts`, `engines[].details.restartBudget` and the `events`
journal. Each reason has its own budget of three restarts in ten minutes: the engine database
changing (`plan`), recovering a clangd that stopped answering or spun (`recovery`), and clangd
exiting (`crash`). Past it, the next restart of that kind waits one, two, four, then eight minutes
— it is backed off, not refused, so a stuck clangd always comes back — and the status says so
(`engine-restart-capped`) with a **Restart clangd** button (`mcppls.restartClangd`), which restarts
at once and is never counted. Switching the toolchain, the profile or the context is never counted
either, and a module that does not compile is never a reason to restart. Every change of the engine
database is logged with what it changed (`engine database changed: … compiled otherwise (main.cpp:
argument 3: -O0 -> -O2)`), so a burst names its cause.

**"clangd crashed while building NormalJsonTranslator.Core.cpp".** clangd names the file it crashed
on (its crash context), and that file is set aside: answered by mcppls's own engine while clangd is
restarted without it. `engines[].details.lastExit` in the report has the exit code, the file, what
clangd was doing and, on Windows, the exception code. Five exits in five minutes and clangd is given
up until the next server start; the status offers **Export Diagnostic Bundle**.

**"clangd rejected the compile command for module scanning".** Before it builds a module, clangd
scans each unit of the database for its imports, with that unit's compile command; a command the
compiler driver rejects fails the scan, and no module is built — issue #23's `LTO requires
-fuse-ld=lld` was one. The status names the first rejection in the driver's own words (category
`environment`); `engines[].details.scanFailures` counts them. A header the command cannot find is
told the same way (category `project`). A scan of a file you are typing fails all the time and is
only counted.

**"C++26 was disabled in precompiled file".** A module was built with one C++ standard and imported
under another; clang refuses that. mcppls reads the module units of a context with one standard,
the newest they name (`plan.languageStandard` in the report, `standard` in the status profile), so
this should only come from a module clangd built before 0.0.5 — it rebuilds on the next change — or
from the build itself mixing standards, which the build tool's own compiler will refuse too.

**Right after opening a project, only module-level features for up to a minute.** A project whose
build system was found gets clangd once its build tool has described it — up to a minute, the
build tool's own limit — rather than with a guess from its sources that clangd would then have to
unlearn; mcppls's own engine answers module navigation, hover on imports and `import` completion
meanwhile. A second session starts from the cached model at once.

**Where the server writes down what went wrong.** A crash, a stuck or spinning clangd, a file set
aside, a restart held back and a broken workaround premise each leave an *incident*: a directory
under the workspace's cache (`incidents/<UTC time>-<kind>/`, the newest twenty, for a week) with
what led up to it, clangd's latest log lines (clangd logs at `info` into memory, never to the default
log), each file's lines where the editor's text and the disk's differ, and which of clangd's threads
used the CPU. The diagnostic bundle carries them.

**"clangd stopped making progress; it was restarted".** clangd left a request unanswered, answered
nothing else meanwhile, and used next to no CPU for five seconds: it was waiting for something that
was not coming, not compiling (a long compile keeps a core busy, and is left alone). The `events`
journal has an `engine-stuck` entry with the numbers. clangd 23.1 has been seen to do this after a
module's source changed twice within a second. On Windows, where the server cannot read clangd's CPU
time, this is not detected; files clangd stops answering for are still set aside one by one.

**"The bundled clangd cannot run on this system".** clangd did not start at all: the system's
program loader refused it, and its message is in the status and the log (for example
``version `GLIBCXX_3.4.30' not found``). No restart can change that, so none is tried; mcppls's own
engine answers module navigation, `import` completion and module diagnostics meanwhile. On Linux
arm64 the bundled clangd needs glibc 2.34 and a GCC 12 libstdc++ — the systems it runs on are listed
in [the install guide](00-install.md). Elsewhere it usually means a musl system (Alpine) or a damaged
payload. An editor that starts `mcppls` itself can give it a clangd of your own, 23.1 or later, with
`--clangd PATH`.

## Filing a bug

Attach the diagnostic bundle (**C++ Modules: Export Diagnostic Bundle**, or `mcppls report --bundle`),
or at least the diagnostic report. Both have your user name, home directory, host name and secrets
replaced, and neither carries the contents of your files; read them before attaching all the same.
