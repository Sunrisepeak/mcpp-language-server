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
