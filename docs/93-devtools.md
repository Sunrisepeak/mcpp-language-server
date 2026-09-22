# Working on mcppls: mcpp, mcppls and mcppls-devtools

Everything this repository asks of a contributor goes through three commands. No script path has
to be remembered, and no interpreter has to be installed: every program here is C++ built by mcpp
on openkal, the same source for Linux, macOS and Windows.

| Command | What it answers | Examples |
|---|---|---|
| `mcpp` | Build, test, run, and the environment | `mcpp build`, `mcpp test --workspace`, `mcpp run -p devtools -- …` |
| `mcppls` | What a user debugging their own machine needs | `mcppls cache`, `mcppls check FILE`, `mcppls report`, `mcppls print-environment` |
| `mcppls-devtools` | What only someone working on this repository needs | packaging, releases, repository checks, benchmarks |

**Which one a new command belongs to.** If a user looking at their own problem would run it, it is
the server's. If only someone working on this repository would, it is devtools'. If mcpp already
does it, it is neither. devtools never wraps an mcpp verb and never repeats a server diagnostic.

## Quick start

```bash
mcpp build                                                   # the server
mcpp test && mcpp test --workspace                           # its tests, then every member's
mcpp run -p devtools -- extension --editor vscode --install  # payload, VS Code extension, installed
```

`mcpp run -p devtools -- <command>` provisions what the command needs (Node, Rust, Gradle — see
[Environment](#environment)) and then runs it. The built binary can also be run directly, which is
what CI does; then the tools must already be on PATH, and a missing one is named with the
declaration that provides it.

## The repository

The root `mcpp.toml` is a workspace rooted in the server. It is split by **dependency set**: mcpp
links every module of a package into every executable of that package, so anything a tool needs
and the server must not carry lives in a member the server does not depend on.

| Member | What | Depends on |
|---|---|---|
| `.` (root) | `mcppls`, and its own test and generation programs: `mcppls-conformance`, `mcppls-lspgen`, the two mocks | base, platform |
| `modules/base` | errors, paths, text, globbing, sha256, URIs, logging, the product version | openkal |
| `modules/platform` | files, processes, environment, directories, loopback sockets — the only package that talks to openkal | base |
| `modules/pack` | verified downloads, archives, payload and kit assembly, release checks | base, platform, tinyhttps, libarchive |
| `tools/devtools` | `mcppls-devtools` | base, platform, pack |
| `tools/model-gateway` | `mcppls-model`, the reference model gateway | openkal, tinyhttps |

`mcpp build -p <member>` and `mcpp test -p <member>` reach one member; `--workspace` reaches all of
them (the root package is reached by the plain command). `modules/os/*` hold the six platform
constants, and are the only place a platform may differ (`mcppls-devtools check os-surface`).

## Commands by task

### Develop

| Task | Command |
|---|---|
| Build the server | `mcpp build` |
| Build for another platform | `mcpp build --target aarch64-macos` / `--target x86_64-windows-gnu` |
| Regenerate `src/lsp` from the LSP meta model | `mcpp run mcppls-lspgen -- generate --meta-model vendor/lsp-metamodel/metaModel-3.18.json --out src/lsp` |
| Change the product version everywhere | `mcpp run -p devtools -- version --set 0.0.2` |

### Test

| Task | Command |
|---|---|
| Unit tests | `mcpp test`, `mcpp test --workspace` |
| Repository invariants | `mcpp run -p devtools -- check all` |
| A conformance fixture | `mcppls-conformance run --server … --payload … --fixture conformance/fixtures/<name>` ([conformance/README.md](../conformance/README.md)) |
| Review fixtures (precision, recall) | `mcpp run -p devtools -- bench review --server … --payload … --mock-model …` |
| Agent task benchmark, baseline and reference | `mcpp run -p devtools -- bench tasks validate` |
| The specifications' schemas | `python3 docs/specs/tools/validate.py` — the one Python left, see [below](#what-is-not-c) |
| Real-project stress testing, a matrix of fixtures and client profiles | `mcpp run -p devtools -- stress --payload … --fixture module-faults --client vscode --client neovim` |

### Debug

| Task | Command |
|---|---|
| What the module caches hold, and clearing one | `mcppls cache [--modules] [--format json]`, `mcppls cache --clean <name>` |
| What the server makes of one file | `mcppls check FILE` |
| Everything a bug report needs | `mcppls report` |
| Startup timings over several runs, optionally against a budget | `mcpp run -p devtools -- measure summary DIR [--max-cold S] [--max-warm S]` |

### Package and release

| Task | Command |
|---|---|
| A payload for this host | `mcpp run -p devtools -- payload` |
| A payload for another platform | `mcpp run -p devtools -- payload --platform win32-x64 --server PATH` |
| Check a payload | `mcpp run -p devtools -- payload --verify DIR` |
| The semantic kit alone | `mcpp run -p devtools -- kit --platform linux-x64 --out DIR` |
| Editor extensions | `mcpp run -p devtools -- extension [--editor vscode\|zed\|clion\|all] [--install [--link]]` |
| Install a plugin you already have (a release's) | `... -- extension --editor zed\|clion --install --plugin PATH --payload DIR` |
| Remove an installed extension | `mcpp run -p devtools -- uninstall --editor vscode\|zed\|clion\|all` |
| Check a staged release | `mcppls-devtools release check --version V --dir release [--render]` |
| The xlings artifacts of a release | `mcppls-devtools release xlings --payloads DIR --out DIR` |

## Environment

devtools installs nothing. What it runs is declared in the root `mcpp.toml`, and `mcpp run -p
devtools` provisions it first:

| Tool | For | Declared in |
|---|---|---|
| Node (npm, npx) | the VS Code extension | `[xlings.workspace]`, `when = "dev"` |
| Rust (cargo, rustup) | the Zed extension | `[xlings.workspace]`, `when = "dev"` |
| Gradle and its JDK | the CLion plugin | `[feature-xlings.clion]`: `mcpp run -p devtools --features clion -- extension --editor clion` |
| cmake, ninja | the semantic kit's libc++ configure | the host (CI installs them) |

`when = "dev"` means these are provisioned for `mcpp run`, never for `mcpp build` or `mcpp test`,
and never for a consumer.

## CI does what you do

Every CI step calls `mcpp` or `mcppls-devtools`; any of them can be run locally with the same
arguments. The workflow ([.github/workflows/ci.yml](../.github/workflows/ci.yml)) builds devtools
once per host and hands the binary to later jobs, including the release job, which has no
toolchain.

## What is not C++

| What | Why | Until |
|---|---|---|
| `editors/vscode` (TypeScript), `editors/clion` (Kotlin), `editors/zed` (Rust → WebAssembly) | an editor decides its extension's language | permanent; devtools drives their builds |
| `docs/specs/tools/validate.py` | JSON Schema 2020-12 validation; no C++ validator for it is in the mcpp index | one is, or the schemas can say the same in draft-07 |

`mcppls-devtools check scripts` fails on any other script; the list and the reason for each entry
are in `tools/devtools/scripts.allow`.

## Adding a command

- The logic goes in a library module — `modules/pack` for packaging data, `tools/devtools/src` for
  the rest — with a test beside it (`mcpp test -p pack`, `mcpp test -p devtools`). The command
  itself parses arguments and calls it.
- One module per command in `tools/devtools/src/`, exporting a `cmdline::App`; `main.cpp` adds it
  with one line.
- `--json` for anything CI or an agent reads. Errors are one sentence that says what to do.
- Files, processes and the environment go through `mcppls.platform`, never the C library or the
  operating system directly; a platform difference is one of the six constants or nothing.
- Update the tables above in the same change (`mcppls-devtools check docs` compares them with
  `--help`).
