# Changelog

Notable changes to mcpp-language-server. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); each release is one dated section, and a
release's notes are that section.

Versions are three-part semantic versions, `MAJOR.MINOR.PATCH`, and every editor plugin carries the
product version unchanged.

## [0.0.2] — 2026-09-22

A module that does not compile, an old pinned mcpp or a stale compile database no longer leaves a
project stuck: measured on xlings with its pinned mcpp 2026.8.8.4, 0.0.1 never left *preparing*,
timed requests out at 10 s and kept four cores busy; 0.0.2 is ready in 8 s with no timeouts. The
findings and the plan are in `.agents/docs/2026-09-22-real-project-experience.md`.

### Engine

- A module clangd cannot compile affects only what imports it: those files are answered at once by
  mcppls's own engine, carry one `module-failed` diagnostic on the import that leads there, and are
  not prepared or sent to clangd again until the failed module's own source or command changes.
  Everything else keeps clangd.
- A module that does not compile is never a reason to restart clangd, nor is a file clangd is slow
  on while it rebuilds after an edit (the restart waits until the edit is two minutes old and is
  still needed); restarts are capped at three in ten minutes (`engine-restart-capped`), and modules
  known to fail are not prepared again after one.
- The status settles: preparation that makes no progress for a minute ends in *degraded*, naming
  what failed (`modules-doomed`, `preparation-stalled`), instead of *preparing* for good.
- clangd's own `E[` lines are logged as warnings and its `I[`/`V[`/`D[` chatter at debug; a module
  clangd could not build is a warning; each stand-in is named in the log with its reason.
- A request on a file clangd has not been given yet waits for it instead of being answered empty,
  and can be cancelled; a late clangd publish no longer replaces a `module-failed` diagnostic.

### Project model

- An untrusted workspace no longer reads a `compile_commands.json` or build database a trusted
  session (or another build) already left on disk; it is L4 (sources only) by definition, as the
  docs always said.
- An mcpp that cannot `emit build-database` no longer means immediately configuring the project (or
  giving up): mcppls looks for a newer mcpp installed elsewhere on the machine and, if one advertises
  it, asks that one instead, read-only — the project still builds with the mcpp it pins. The status
  and `mcppls check` say "described by mcpp X (the project pins Y)".
- A compile database entry naming a file that no longer exists is dropped rather than breaking the
  model; the status notes it as stale. A module a dependency's build generates is looked for where
  builds leave it (the project's own build directory, mcpp's build-database cache) before mcppls
  falls back to an empty stand-in.
- `cxxModules/status` gained `project.tier` (spec S3-4-8, S3-4-9): which kind of source described
  the project (the README's L1..L4), independent of `project.level` (S1's own document-conformance
  number). The status bar and the Neovim plugin now show `L<tier>`, never `level`, which the two
  numbers sharing a range made easy to misread as the same thing.
- Scanning and a file opened while browsing no longer fold a nested project's own sources (a
  conformance fixture, a vendored copy, an example with its own `mcpp.toml` or `CMakeLists.txt`) into
  the workspace's model.
- The tier says how a model was obtained: an mcpp project described through mcpp's
  `compile_commands.json` is L3, not L1. A model is never replaced by one of a worse tier; the one in
  hand is kept and marked possibly stale.

### Testing

- `mcppls-conformance` has a `stress` check — seeded random use (opening and switching files, hover,
  definition, references, completion, symbols) with budgets for timeouts, p90, stalls, CPU and
  memory — and client profiles (`--client vscode|neovim|zed|plain`).
- New fixtures: a module generated at build time (with a current, an old and a negotiated mcpp), a
  100-module chain whose base does not compile, and pinned real projects: this repository and xlings,
  as committed and with its old mcpp pin. The mock mcpp can be too old for `emit build-database`.
- `mcpp run -p devtools -- stress` runs the matrix and compares against a baseline; Neovim and the
  VS Code suite each have a stress scenario; CI runs the failing-base fixture and the stress checks
  on Linux, macOS and Windows and the generated-module fixtures on Linux and macOS (their mock mcpp
  is POSIX-only), each client profile at least once per platform, and the real projects nightly and
  before a release.

### Editors

- Neovim: a Lua plugin in `editors/nvim` (Neovim 0.10 or later) that starts mcppls for C and C++
  buffers through Neovim's own LSP client, by `require('mcppls').setup()` or, on 0.11 and later,
  `vim.lsp.enable('mcppls')`; `:McpplsStatus`, `:McpplsRestart`, `:McpplsReload`, a statusline
  component, and a one-time warning when clangd or ccls attaches beside it. CI runs it on Neovim
  0.10.4, 0.11.5 and 0.12.5 on Linux, and 0.10.4 and 0.12.5 on macOS and Windows.

## [0.0.1] — 2026-09-22

The first public release. Every artifact is a file on the GitHub release page; its `MANIFEST.md` says
what each one is and how to install it, and `SHA256SUMS` covers them all. The VS Code extension is
also on the VS Code Marketplace as `sunrisepeak.mcpp-language-server`; nothing is on Open VSX or the xlings index
yet.

### Module semantics on any compiler

- mcpp, CMake (`FILE_SET CXX_MODULES`), a bare `compile_commands.json`, or sources alone are
  normalized into one module description (L1–L4), with that toolchain's `std` — GCC, Clang, MinGW,
  clang-cl and MSVC, including MSVC STL's `import std`.
- A pinned clangd 23.1 is driven with it; mcppls's own engine adds module-name navigation, `import`
  completion, the module graph, and diagnostics for unresolved, ambiguous and cross-module partition
  imports.
- One payload per platform — the server, clangd and the semantic kit — so `import std` resolves even
  with no usable compiler on the machine. Its files are checked against their hashes at startup.

### It is never held by what it starts

- Build tools, compiler probes, CMake and git run in a process unit of their own, with deadlines that
  end the whole unit and bounded reads.
- Runs the server starts itself are offline (`mcppls.buildTool`); when a download is needed, the
  status says what is missing and offers to run the build tool in your terminal.
- Build tools get the login shell's environment on POSIX. The last session's model is cached with a
  fingerprint of its inputs and used at once, then confirmed in the background.

### Faults stay where they are

- A module nothing provides gets a stand-in unit, so one broken module does not take its importers
  with it. clangd restarts only when an imported module's unit changes, behind a rate gate; a file
  clangd stops answering is set aside and answered by mcppls's own engine.
- Degradation is visible in the status, and one diagnostic report — `mcppls report`, or "Collect
  Diagnostic Report" in VS Code — carries the model, the plan, the engines and the recent external
  runs. Logs outlive the editor.

### For coding agents and CI

Semantic queries, post-edit verification and change review over MCP (`mcppls mcp`, `--daemon` to
share a workspace) and the command line: symbols, references and callers followed through module
imports, fresh diagnostics, cross-toolchain verification, and review findings with evidence as SARIF,
LSP diagnostics or Markdown. Model-backed review is optional, off by default, and never done by the
server process itself.

### Editors

- VS Code (a VSIX per platform, carrying its payload), Zed, CLion, Claude Code and GitHub Copilot CLI.
- Built from source, every plugin is one command: `mcpp run -p devtools -- extension --editor
  vscode|zed|clion --install`, and `uninstall` to remove it. Zed's recommended last step stays its
  own palette action (`--link` does it from the command line).
- The VS Code extension writes the server's log at the level of each line; a healthy start is no
  longer shown as errors.

### Specifications

S1 build database, S2 discovery, S3 LSP extensions for modules, S4 semantic kit and S5 semantic
queries, versioned separately, with rule identifiers traced to tests, conformance checks or code.
