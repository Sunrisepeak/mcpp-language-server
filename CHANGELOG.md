# Changelog

Notable changes to mcpp-language-server. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); each release is one dated section, and a
release's notes are that section.

Versions are three-part semantic versions, `MAJOR.MINOR.PATCH`, and every editor plugin carries the
product version unchanged.

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
