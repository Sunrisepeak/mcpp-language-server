# When something is wrong

## Start here

**C++ Modules: Collect Diagnostic Report** (or `mcppls report` on the command line). It opens JSON
carrying what almost every question turns out to need:

| In the report | Answers |
|---|---|
| `project.source`, `project.level` | Where the build description came from, and how complete it is |
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
with it. The real failure is in `plan.issues` or in your build.

**The editor found a different compiler than my terminal.** `toolEnvironment.source` should say
`login-shell`. If it says `editor`, the reason is in `toolEnvironment.reason` — an editor started
from a desktop entry has none of your shell configuration. `mcppls.toolEnvironment` controls this.

**Everything is slow to start.** The second session should be fast: the model is cached with a
fingerprint of everything the build tool read, and a session that matches plans with it at once and
confirms it in the background. `project.firstOrigin` says which happened. If it is always
`producer`, the fingerprint is not matching — the report's `project.producerRun` and the build
files' timestamps are where to look.

**clangd keeps restarting.** `engines[].restarts` and the `events` journal. Restarts are gated and
spaced out on purpose; a burst of them usually means the compile arguments are changing under it.

## Filing a bug

Attach the diagnostic report. It names paths on your machine, so read it first — it carries no
environment variable values and no file contents, by design.
