# Install

> Releases come from the GitHub release page, and the VS Code extension is also on the VS Code
> Marketplace. Nothing is on Open VSX or the xlings index yet.

## VS Code, from the Marketplace

Search the Extensions view for **C++ Modules Language Server**, or run
`code --install-extension sunrisepeak.mcpp-language-server` ([Marketplace page](https://marketplace.visualstudio.com/items?itemName=sunrisepeak.mcpp-language-server)). VS Code picks the
build for your platform — `linux-x64`, `darwin-arm64` or `win32-x64`; there is none for other
platforms yet.

## VS Code, from a release

Download `mcppls-<platform>.vsix` from the [release page][releases] and install it:

- **In the editor**: Extensions view → the `…` menu → *Install from VSIX…*
- **On the command line**: `code --install-extension mcppls-linux-x64.vsix`

The VSIX carries everything it needs: the server, a pinned clangd, and the semantic kit. Open a C++
project and the status bar says what it found. Take the file matching your machine —
`linux-x64`, `darwin-arm64` or `win32-x64` — since each carries its own platform's payload.

The extension is `sunrisepeak.mcpp-language-server`, and it is a different extension from **mcpp**
(`mcpp-community.mcpp-vscode`), which handles building, toolchains and project operations. Both are
worth having; see [10-editors.md](10-editors.md).

## Another editor, from a release

Download `payload-<platform>.tar.gz`, unpack it, and put its `payload/bin/` on `PATH`. That gives you
`mcppls`, which speaks LSP over stdio (`mcppls serve`) and MCP (`mcppls mcp`). The Zed and CLion
plugins are separate archives on the same release page; [10-editors.md](10-editors.md) has both.
The Neovim plugin is [`editors/nvim`](../editors/nvim/README.md) in this repository.

Every asset on a release is listed in its `MANIFEST.md`, with what it is and how to install it, and
`SHA256SUMS` covers all of them.

[releases]: https://github.com/Sunrisepeak/mcpp-language-server/releases

## From source

The server is C++23 modules on [openkal](https://github.com/mcpplibs/openkal), built with
[mcpp](https://github.com/mcpp-community/mcpp):

```bash
mcpp build                                        # the server
mcpp run -p devtools -- extension --install       # payload, extension, installed into VS Code
```

The second command assembles the payload (the
server, a trimmed clangd and the semantic kit), builds the extension and installs it, naming each
step and how long it took. If something it needs is missing it says which and how to get it.

| Variation | Command |
|---|---|
| A release build | `mcpp run --release -p devtools -- extension --editor vscode --install` |
| Zed | `mcpp run -p devtools -- extension --editor zed --install`, then Zed's palette: *zed: install dev extension* (or add `--link`) |
| CLion | `mcpp run --features clion -p devtools -- extension --editor clion --install`, then restart CLion |
| All three, packaged but not installed | `mcpp run -p devtools -- extension` |
| Remove what was installed | `mcpp run -p devtools -- uninstall --editor vscode\|zed\|clion\|all` |
| Only the payload, for another editor | `mcpp run -p devtools -- payload` |
| Reuse a clangd or kit you already built | `... -- payload --clangd DIR --kit DIR` |
| Another platform's server | `mcpp build --target aarch64-macos` / `--target x86_64-windows-gnu` |

**Zero setup.** What packaging needs is declared in `mcpp.toml`'s `[xlings.workspace]` — Node for
the VS Code extension, Rust for Zed — so `mcpp run` provisions it before the tool starts. The
payload itself (clangd, the semantic kit) is assembled by the tool in C++; no interpreter is
involved. Every contributor command is in [93-devtools.md](93-devtools.md). Gradle and its JDK are behind the `clion` feature
(`mcpp run --features clion ...`), so the plugin nobody else builds costs nobody else a download. Nothing to install by hand, and
nothing the tool installs behind your back: the manifest is the one place that says what this
needs.

VS Code has a command line for installing and removing an extension; Zed and CLion do not. For
them `--install` leaves on disk exactly what their own UI would, and nothing else: for Zed it stops
one step short and names the palette action, the recommended route, unless `--link` asks it to make
that same link itself; for CLion it unpacks the plugin as *Install Plugin from Disk* does. Both
start `mcppls` from PATH, or else the server `--install` puts at `<user data>/mcppls/payload`
(`$XDG_DATA_HOME` or `~/.local/share` on Linux, `~/Library/Application Support` on macOS,
`%LOCALAPPDATA%` on Windows); `uninstall` removes that server with the last of the two.

The first run compiles the semantic kit from libc++ sources, which takes a while; it is cached
under `.payload-cache` afterwards. What the steps actually are, and the payload layout they
produce, is in [packaging/README.md](../packaging/README.md).

To point the extension at a payload without repackaging, put it at `editors/vscode/payload` or set
`MCPPLS_PAYLOAD`.

## What gets installed where

| Part | What it is | Why it is pinned |
|---|---|---|
| `mcppls` | The server | — |
| clangd 23.1.0 | The engine it drives for everything but module-level requests | Module support differs between clangd releases; the arguments mcppls generates are matched to this one |
| `mcppls-kit` | A semantic kit: libc++ built as modules, with a module manifest | It is what makes `import std` resolve on a machine whose compiler cannot provide it |

The server looks for a payload named by `--payload`, then for one containing its own executable.
`--clangd` and `--kit` replace one part of it; a part still missing is looked for outside a payload
(clangd on `PATH`, the kit where xlings installs it).
