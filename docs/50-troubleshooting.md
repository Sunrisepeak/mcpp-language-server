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
| `engines` | clangd's state, restarts, files set aside |
| `events` | A journal of the session |
| `logTail` | The end of the log |

Log files outlive the editor: the report names the path, and they are kept under the cache
directory with timestamps.

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
the failed module's own source or command changes; everything else keeps clangd. The status says
*degraded* with "N modules cannot be prepared because M failed", never *preparing* for good. The
engine's `doomedModules` and `filesRoutedToOwnEngine` in the report list them.

**The editor found a different compiler than my terminal.** `toolEnvironment.source` should say
`login-shell`. If it says `editor`, the reason is in `toolEnvironment.reason` — an editor started
from a desktop entry has none of your shell configuration. `mcppls.toolEnvironment` controls this.

**Everything is slow to start.** The second session should be fast: the model is cached with a
fingerprint of everything the build tool read, and a session that matches plans with it at once and
confirms it in the background. `project.firstOrigin` says which happened. If it is always
`producer`, the fingerprint is not matching — the report's `project.producerRun` and the build
files' timestamps are where to look.

**clangd keeps restarting.** `engines[].restarts` and the `events` journal. Restarts are spaced out
and capped at three in ten minutes, after which the status says so (`engine-restart-capped`) and
mcppls's own engine answers what a restart would have tried to fix; a module that does not compile
is never a reason to restart. A burst usually means the compile arguments are changing under it.

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

Attach the diagnostic report. It names paths on your machine, so read it first — it carries no
environment variable values and no file contents, by design.
