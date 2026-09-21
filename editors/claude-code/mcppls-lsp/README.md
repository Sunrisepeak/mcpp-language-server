# mcppls (C/C++ Modules) — Claude Code plugin

Registers `mcppls`, the compiler-agnostic C++ modules language server, as Claude Code's
language server for C and C++ sources: `.cpp`, `.cc`, `.cxx`, `.c++`, `.cppm`, `.ccm`,
`.cxxm`, `.c++m`, `.ixx`, `.mpp`, `.mxx`, `.h`, `.hh`, `.hpp`, `.hxx`, `.inl` (language id
`cpp`) and `.c` (language id `c`). The server is started as `mcppls serve` over stdio, the
same command a plain editor client would use.

With this plugin enabled and `mcppls` on `PATH`, Claude Code gets automatic diagnostics
after every edit and code navigation (go to definition, references, hover, document
symbols, implementations, call hierarchy) for C++20/23 named modules — including
cross-module and cross-partition navigation — the same way it does for any other LSP
plugin (see the "Code intelligence" section of the Claude Code plugins documentation).

## Requirements

`mcppls` must be on `PATH`. Build it with `mcpp build` from this repository (binary under
`target/x86_64-linux-gnu/<fingerprint>/bin/mcppls`, or the matching triple for your
platform), or install it once published — see `../../agents/README.md` for both routes.

## Do not enable alongside `clangd-lsp`

This plugin **replaces** the official `clangd-lsp` code-intelligence plugin for a C/C++
project: both configure a language server for the same file extensions, and a project
should use only one. Disable or uninstall `clangd-lsp` before enabling `mcppls-lsp`, and
do not enable both for the same project at the same time.

## Install

From a local path — a clone of this repository, pointed at this marketplace's own
subdirectory (`.claude-plugin/marketplace.json` lives under `editors/claude-code/`, not
at the repository root):

```
git clone https://github.com/Sunrisepeak/mcpp-language-server
/plugin marketplace add mcpp-language-server/editors/claude-code
/plugin install mcppls-lsp@mcppls
```

Or from a raw URL straight at the repository, without cloning it:

```
/plugin marketplace add https://raw.githubusercontent.com/Sunrisepeak/mcpp-language-server/main/editors/claude-code/.claude-plugin/marketplace.json
/plugin install mcppls-lsp@mcppls
```

See `../../agents/README.md` for the complete walkthrough, including the non-interactive
`claude plugin install` form.

## Validate without installing

```
claude plugin validate editors/claude-code/mcppls-lsp --strict
claude plugin validate editors/claude-code --strict
```
