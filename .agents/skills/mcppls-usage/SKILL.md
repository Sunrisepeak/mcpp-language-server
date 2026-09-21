---
name: mcppls-usage
description: Use when helping someone install or use mcpp-language-server (mcppls) — the compiler-agnostic C++ modules language server — or when working inside a C++ modules project that has it, including its MCP tools and command line for agents.
---

# Using mcppls

## Overview

mcppls gives an editor, a coding agent or CI the same C++ module semantics whatever the project is
built with. It normalizes any build into one module description, drives a pinned clangd with it,
and answers module-level requests itself.

- Repository: https://github.com/Sunrisepeak/mcpp-language-server
- Documentation: `docs/` (start at `docs/00-install.md`)
- Specifications: `docs/specs/` (S1 build database, S2 discovery, S3 LSP extensions, S4 kit, S5 queries)

## Quick reference

| Command | Purpose |
|---|---|
| `mcppls serve` | The language server, LSP over stdio (what an editor starts) |
| `mcppls mcp [--root DIR] [--daemon]` | MCP over stdio: the semantic tools below |
| `mcppls query symbol\|refs\|calls\|outline\|module\|context` | Semantic queries, S5 JSON out |
| `mcppls diagnostics <file>...` | Fresh diagnostics for what is on disk now |
| `mcppls verify [--changed]` | Build the changed files and everything importing them |
| `mcppls impact` / `mcppls review [--base REV]` | What a change affects; a review of it |
| `mcppls check <file>` | The model, the semantic profile, module diagnostics, then `clangd --check` |
| `mcppls report` | Everything a bug report needs, as JSON |
| `mcppls model --export s1\|compile-commands\|engine` | The project model in each form |

Global options worth knowing: `--build-tool offline|online|off`, `--tool-environment auto|editor`,
`--producer-timeout SECONDS`, `--untrusted`, `--payload DIR`.

## The MCP tools

`cxx_symbol`, `cxx_references`, `cxx_outline`, `cxx_module`, `cxx_build_context`,
`cxx_diagnostics`, `cxx_verify`, `cxx_impact`, `cxx_review`.

Two things make them worth using over grep: references and callers are searched in the symbol's own
module **and every module importing it** (re-exports included), and every answer comes from the same
model the editor is using.

## What to expect per project kind

| Project | What mcppls does |
|---|---|
| mcpp | `mcpp emit build-database --format json`, run offline; that toolchain's `std` |
| CMake with `FILE_SET CXX_MODULES` | Reads the build directory's database, or configures a private one |
| Only `compile_commands.json` | Scans sources for module roles, probes the compilers it names |
| Sources only, or no compiler | Infers from sources; falls back to the bundled semantic kit |
| Untrusted workspace | Runs no build tool and no compiler at all |

## Things that surprise people

- **Runs the server starts itself are offline.** If a project's dependencies are not on the machine,
  the build tool says so and the status offers to run it in the user's own terminal — that is where
  a hand-set proxy is. `mcppls.buildTool = online` changes it.
- **The tool environment is the login shell's**, not the editor's. An editor started from a desktop
  entry has none of the user's shell configuration, so without this the build tool it finds may not
  be the one the terminal finds.
- **A module nothing provides gets a stand-in unit**, so one broken module does not remove every
  unit that imports it.
- **`mcppls report` is the first thing to ask for** when something is wrong. It carries where the
  model came from, what ran, how long it took, and the end of the log.

## Diagnosing

1. `mcppls report` (or the editor command "C++ Modules: Collect Diagnostic Report").
2. `project.source` — did the build description come from the build tool, or from scanning?
3. `project.producerRun` — did the build tool run, how long, how did it end, was it offline?
4. `toolEnvironment.source` — `login-shell` or `editor`, and why.
5. `plan.standIns` / `plan.issues` — which modules are missing and what was left out.

`docs/50-troubleshooting.md` maps each symptom to the field that explains it.
