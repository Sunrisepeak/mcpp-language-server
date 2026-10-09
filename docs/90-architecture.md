# Architecture

A short map, then where the real designs are.

## The shape

```
editor / coding agent / CI
        |  LSP, MCP, or the command line
   orchestrator ......... one workspace per root: model, plan, documents, status
        |
   project .............. how is this built? mcpp, CMake, compile_commands.json, or scanning
   toolchain ............ what does that compiler do with modules? (probed, cached)
        |
   normalize ............ one engine plan: arguments every engine can take
        |
   engines .............. mcppls's own module engine, and clangd beside it
```

- **The module description is normalized once.** Whatever the build system, it becomes one S1
  document ([specs/](specs/)), and everything downstream reads only that.
- **Two engines serve one workspace.** mcppls's own engine answers module-level requests from a
  syntax index it owns; clangd answers the rest, driven by a database mcppls writes for it.
- **Nothing the server starts may hold it.** Build tools, compiler probes, CMake and git all run
  through one runner that gives each run its own process unit, bounds the reads, and writes down
  what happened.
- **A request has a budget for clangd, and an answer within it.** Completion and signature help get
  1 s, hover 2 s, go-to-definition 10 s. Past it mcppls answers from what it has — for completion the
  words of the file nearest the cursor, as an incomplete list so the editor asks again; for hover
  while modules are prepared, a line saying so — and cancels clangd's request. The report counts,
  per method, which engine answered (`requests.<method>.answeredBy`).
- **Background work happens in idle time.** Implementation units are built for clangd's index one at
  a time, and only after 10 s without typing, opening a file or a request; a restart clangd needs for
  a changed build description waits until typing has paused for 3 s (at most 60 s). A crash or a
  clangd that stopped answering restarts at once. Module preparation takes every worker but one when
  an opened file waits on it, half of them otherwise. clangd's background index uses its `background`
  priority unless `MCPPLS_ENGINE_ARGUMENTS` explicitly selects another priority. On macOS this uses
  Background QoS instead of the engine's default Utility QoS; on Linux both priorities currently use
  `SCHED_IDLE`. The full index remains enabled. This gives interactive cold module builds precedence;
  it does not guarantee a particular response time.
- **What cannot be recovered is written down at once.** When the server cannot recover by itself it
  writes a redacted diagnostic bundle (`<cache>/bundles/auto-<code>-<time>.zip`, never uploaded) and
  puts it on the status issue, so an editor can offer a filled-in report.
- **Built modules do not pile up.** Each unit keeps the BMIs of its two newest commands; older ones
  are removed in the background when clangd starts.
- **The server knows the sandbox it runs in.** Under PRoot (termux) it starts programs the way that
  sandbox answers; the report shows it as `server.sandbox`.
- **The server itself makes no network calls.** Runs it starts are offline by default; model
  features are opt-in and live behind a separate process.

## The specifications

The parts worth reusing are written as specifications with identifiers and traceability, versioned
separately from the product: S1 build database, S2 discovery, S3 LSP extensions, S4 semantic kit,
S5 semantic queries. See [specs/README.md](specs/README.md).

## The design

[`.agents/docs/design.md`](../.agents/docs/design.md) is the one design record: purpose, architecture,
robustness, the decisions that shaped the code, and an index of the labels source comments cite
("usable plan W9.3", "robustness design C6", ...).
