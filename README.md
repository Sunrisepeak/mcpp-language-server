# mcppls - mcpp language server

> A C++ modules language server — one set of module semantics on every compiler, for editors,
> coding agents and CI alike

**English** | [简体中文](README.zh-CN.md)

[Documentation](docs/README.md) · [Install](docs/00-install.md) · [Settings](docs/30-settings.md) ·
[For agents](docs/40-agents.md) · [Specifications](docs/specs/README.md) ·
[Releases](https://github.com/Sunrisepeak/mcpp-language-server/releases)

C++20 standardized named modules; the toolchains did not agree on the rest. BMI formats differ,
dependency scanning differs, and `import std` comes from somewhere else in each, so module code that
builds fine gives the editor nothing as soon as the compiler changes.

mcppls normalizes any build — [mcpp](https://github.com/mcpp-community/mcpp), CMake, a bare
`compile_commands.json`, or nothing at all — into one module description, drives a pinned clangd
with it, and answers the module-level requests clangd does not. Coding agents and CI get the same
answers over MCP and the command line.

<p align="center">
    <img src="https://github.com/user-attachments/assets/fb4f00c7-df49-431c-9531-4e51c8624372" alt="mcppls demo" width="800">
</p>

## Features

- **Module semantics on any compiler** — GCC, Clang, MinGW, clang-cl and MSVC, each with its own `std`
- **Zero configuration** — the build is detected, the toolchain probed, and the status says what is in use
- **What clangd lacks** — module-name navigation, `import` completion, the module graph, module diagnostics
- **Never held by what it starts** — build tools run offline, in their own process unit, under a deadline
- **Faults stay local** — a broken module gets a stand-in instead of taking the project with it
- **For agents and CI** — references and callers across imports, post-edit verification, review with evidence
- **One payload** — server, pinned clangd and semantic kit, so `import std` works with no compiler installed

## Install

### From an agent

Send this to a coding agent, and it follows one of the routes below for you:

```
Read .agents/skills/mcppls-usage/SKILL.md and docs/00-install.md of the
https://github.com/Sunrisepeak/mcpp-language-server repository,
then install mcppls for the editor I use — from the latest release, or built from source
if there is none for my platform — and check that it works on my C++ modules project.
```

### From a release

Download from the [release page](https://github.com/Sunrisepeak/mcpp-language-server/releases):

| Editor | Asset | Install |
|---|---|---|
| VS Code | the [Marketplace](https://marketplace.visualstudio.com/items?itemName=sunrisepeak.mcpp-language-server), or `mcppls-<platform>.vsix` | search *mcppls* or *C++ Modules Language Server*, or `code --install-extension mcppls-<platform>.vsix` |
| Cursor, VSCodium, Windsurf and other VS Code-compatible editors | [Open VSX](https://open-vsx.org/extension/sunrisepeak/mcpp-language-server), or `mcppls-<platform>.vsix` | search *mcppls* in the editor's extensions view, or install the `.vsix` from it |
| Zed, CLion | `mcppls-zed-<version>.tar.gz`, `mcppls-clion-<version>.zip` | see [docs/00-install.md](docs/00-install.md) |
| Neovim | `payload-<platform>.tar.gz` | put its `payload/bin/` on `PATH`, then the plugin in [editors/nvim](editors/nvim/README.md) |
| Any other LSP client | `payload-<platform>.tar.gz` | unpack, put its `payload/bin/` on `PATH`, run `mcppls serve` |

### From source

**1. Install mcpp** through [xlings](https://github.com/openxlings/xlings), once per machine:

```bash
curl -fsSL https://d2learn.org/xlings-install.sh | bash         # Linux / macOS
irm https://d2learn.org/xlings-install.ps1.txt | iex            # Windows (PowerShell)

xlings install mcpp -y -g                                       # then open a new shell
```

**2. Build the server and install it into your editor:**

```bash
mcpp build
mcpp run -p devtools -- extension --editor vscode --install
```

| Editor | Install | Remove |
|---|---|---|
| VS Code | `extension --editor vscode --install` | `uninstall --editor vscode` |
| Zed | `extension --editor zed --install`, then Zed's palette: *zed: install dev extension* (or add `--link`) | `uninstall --editor zed` |
| CLion | `mcpp run --features clion -p devtools -- extension --editor clion --install`, then restart CLion | `uninstall --editor clion` |

Coding agents and other clients: [docs/10-editors.md](docs/10-editors.md).

## How much it knows

| | Project | Module description from | You get |
|---|---|---|---|
| **L1** | **mcpp** | `mcpp emit build-database`, run offline | Every unit, its role, its arguments and the toolchain's `std` |
| **L2** | **CMake** (`FILE_SET CXX_MODULES`) | The build directory's database, or a private configure | The generator's answer, `@modmap` files expanded |
| **L3** | A **`compile_commands.json`** | The database plus scanning | Arguments per file, module roles recovered by scanning |
| **L4** | **Sources only**, or no compiler | Scanning and the bundled semantic kit | Modules resolve and `import std` works, with libc++ diagnostics |

It degrades one step at a time and always says which step it is on. An untrusted workspace is L4:
no build tool and no compiler runs.

## Documentation

- [docs/](docs/README.md) — install, editors, project kinds, settings, agents and CI, troubleshooting
- [docs/specs/](docs/specs/README.md) — the five specifications, versioned separately
- [.agents/docs/design.md](.agents/docs/design.md) — the design

**With a coding agent**, send it this to get set up:

```
Read .agents/skills/mcppls-usage/SKILL.md and the docs/ directory of the
https://github.com/Sunrisepeak/mcpp-language-server repository,
then set up mcppls for my C++ modules project and show me what it can answer.
```

## Contributing

[CONTRIBUTING.md](CONTRIBUTING.md) covers building, testing and the conformance fixtures, and
[docs/93-devtools.md](docs/93-devtools.md) lists every contributor command.

Contributions developed with coding agents are welcome; the workflow and the rules that are not
negotiable are in [.agents/skills/mcppls-contributing/SKILL.md](.agents/skills/mcppls-contributing/SKILL.md):

```
Read .agents/skills/mcppls-contributing/SKILL.md of the
https://github.com/Sunrisepeak/mcpp-language-server repository,
then follow it to help me submit a contribution.
```

## License

Apache-2.0. See [LICENSE](LICENSE), and [NOTICE](NOTICE) for what the payload bundles.
