# Install

> Releases come from the GitHub release page, and the VS Code extension is also on the VS Code
> Marketplace and on Open VSX. Nothing is on the xlings index yet. Termux on Android runs the
> `linux-arm64` build ([below](#android-under-termux)).

## VS Code, from the Marketplace

Search the Extensions view for **mcppls** or **C++ Modules Language Server**, or run
`code --install-extension sunrisepeak.mcpp-language-server` ([Marketplace page](https://marketplace.visualstudio.com/items?itemName=sunrisepeak.mcpp-language-server)). VS Code picks the
build for your platform — `linux-x64`, `linux-arm64`, `darwin-arm64` or `win32-x64`; there is none
for other platforms yet. `darwin-arm64` needs macOS 12 or later; on Linux, see [which systems each
architecture runs on](#supported-linux-systems).

## Cursor, VSCodium, Windsurf and other VS Code-compatible editors, from Open VSX

These editors install extensions from [Open VSX](https://open-vsx.org/extension/sunrisepeak/mcpp-language-server):
search their extensions view for **mcppls**. It is the same extension, built from the same release,
with the same platforms. A release reaches Open VSX when it is published, and the VS Code Marketplace
shortly after.

## VS Code, from a release

Download `mcppls-<platform>.vsix` from the [release page][releases] and install it:

- **In the editor**: Extensions view → the `…` menu → *Install from VSIX…*
- **On the command line**: `code --install-extension mcppls-linux-x64.vsix`

The VSIX carries everything it needs: the server, a pinned clangd, and the semantic kit. Open a C++
project and the status bar says what it found. Take the file matching your machine —
`linux-x64`, `linux-arm64`, `darwin-arm64` or `win32-x64` — since each carries its own platform's clangd
and kit. The server in every one of them is the same file, `mcppls.okb` (the server's image for x86_64
and for arm64), started by the platform's runner `okrun` that the VSIX carries beside it.

The extension is `sunrisepeak.mcpp-language-server`, and it is a different extension from **mcpp**
(`mcpp-community.mcpp-vscode`), which handles building, toolchains and project operations. Both are
worth having; see [10-editors.md](10-editors.md).

## Another editor, from a release

Two kinds of file on the release page serve every editor but VS Code:

- **The payload**, `payload-<platform>.tar.gz`: the server, a pinned clangd and the semantic kit, as
  the VSIX carries them. The server is `payload/bin/mcppls.okb`, started by this platform's runner:
  `payload/bin/okrun payload/bin/mcppls.okb serve` speaks LSP over stdio, and `… mcp` MCP; it finds
  the clangd and the kit beside it. Unpack it into your user data directory as `mcppls/payload` —
  `~/.local/share/mcppls/payload` on Linux (`$XDG_DATA_HOME` when it is set),
  `~/Library/Application Support/mcppls/payload` on macOS, `%LOCALAPPDATA%\mcppls\payload` on
  Windows — and the Zed, CLion and Neovim plugins start it with nothing else configured.
- **The server as a command**, `mcppls`: `mcppls-<version>-openkal.zip` is one package for Linux,
  macOS and Windows on x86_64 and arm64. Unpack it and put its `bin/` on `PATH`; it carries the
  runner of each of those systems, so nothing else is installed. Or take the single file of your
  system, `mcppls-<version>-<system>-<arch>` (`linux-x86_64`, `linux-aarch64`, `macos-aarch64`,
  `windows-x86_64.exe`), and put it on `PATH` as `mcppls`: one executable, nothing to unpack. The
  plugins start an `mcppls` on `PATH` before an installed payload. Given `--payload <dir>` it uses
  that payload's clangd and kit; without one, a clangd on `PATH` and the kit xlings installs.

The Zed and CLion plugins are separate archives on the same release page;
[10-editors.md](10-editors.md) has both. The Neovim plugin is
[`editors/nvim`](../editors/nvim/README.md) in this repository.

Every asset on a release is listed in its `MANIFEST.md`, with what it is and how to install it, and
`SHA256SUMS` covers all of them.

[releases]: https://github.com/Sunrisepeak/mcpp-language-server/releases

## Supported Linux systems

The server and its runner need nothing of the system and run on any Linux of their architecture. The
floor is set by the clangd the payload carries:

| Platform | clangd | Needs | Runs on |
|---|---|---|---|
| `linux-x64` | mcppls-clangd 23.1.0, built on Ubuntu 20.04 with libstdc++ and zlib linked in | glibc 2.31 | Ubuntu 20.04, Debian 11, RHEL and Rocky Linux 9, openEuler 22.03 LTS and later |
| `linux-arm64` | the same engine, built the same way for arm64 | glibc 2.31 | the same systems, on arm64 |

On an older system — Ubuntu 18.04, Debian 10, RHEL and Rocky Linux 8, Amazon Linux 2 — the bundled
clangd cannot start. mcppls then keeps its own module-level features (module navigation, import
completion, module diagnostics), and its status says why; `mcppls.clangd` can name a clangd that
does run there. Alpine and other musl distributions run neither architecture's clangd.

### Android, under Termux

An Android phone or tablet with Termux runs the `linux-arm64` build; there is no separate package
for it. Termux's PRoot, which many setups run a Linux system under, is a supported environment:

- **The server** is the `linux-arm64` payload's: unpack `payload-linux-arm64.tar.gz` where the
  plugins look, as in [Another editor, from a release](#another-editor-from-a-release), and start it
  from the editor you run there (Neovim, say); or put `mcppls-<version>-linux-aarch64` on `PATH` as
  `mcppls`; or install `mcppls-linux-arm64.vsix` in a VS Code-compatible editor running inside Termux.
- **clangd** is the same glibc program as on any `linux-arm64` system, so the floor in the table
  applies: CI runs it in a Debian that `proot-distro` installs in Termux. Where it cannot run (a glibc
  below the floor) the status says `engine-incompatible`, and where the sandbox refuses to start it,
  `engine-start-failed` with the reason; either way mcppls keeps its module-level features.
- **What mcppls does for PRoot.** It detects the sandbox it runs in (`mcppls report` shows it as
  `server.sandbox`, `proot` there) and starts programs the way PRoot answers. Two PRoot behaviors
  used to stop it: Termux's PRoot refuses the call that starts a program from a directory handle, so
  the server said "not supported" when it started clangd, and PRoot's fast path left a register
  rewritten after `openat`, so reads failed with "outside every preopened directory" (issue #32).
  Nothing needs configuring.

## From source

The server is C++23 modules on [mcpplibs.os](https://github.com/mcpplibs/os) over
[openkal](https://github.com/mcpplibs/openkal), built with [mcpp](https://github.com/mcpp-community/mcpp)
2026.10.27.1 or later. The workspace's one platform, openkal-platform 0.17.3, states the C library and
the C++ run-time of every target, so no member declares either:

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
| Another platform's server | `mcpp build --target aarch64-macos` / `--target x86_64-windows-gnu` / `--target aarch64-linux-musl` |
| The server as an ok image, one per architecture | `mcpp build --release --target x86_64-openkal` (or `aarch64-openkal`); `mcpp run --release --target x86_64-openkal mcppls -- serve` runs it through the platform's runner, on Linux, macOS and Windows |
| The okb every payload carries: both images in one file | `mcpp pack --release --target x86_64-openkal --pkg-format okb` |
| One package for every system and architecture | `mcpp pack --release --target x86_64-openkal --mode self-contained --pkg-format zip` (the runners of the systems `--systems` names, built over the platform's hosted arrangement on this machine; CI packs Linux's and Windows' on Linux and macOS's on macOS, and joins the two with `mcpp pack --merge`) |
| One file for this system | `mcpp pack --release --target x86_64-openkal --pkg-format single-file` |
| Build programs as ok images | `mcpp build --host-platform openkal-platform@0.17.3` |

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
start `mcppls` from PATH, or else the server of the payload `--install` puts at `<user data>/mcppls/payload`
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
| `mcppls.okb` and `okrun` | The server, as the images of both architectures, and the platform's runner of this system that starts it | — |
| clangd 23.1.0 | The engine it drives for everything but module-level requests | Module support differs between clangd releases; the arguments mcppls generates are matched to this one |
| `mcppls-kit` | A semantic kit: libc++ built as modules, with a module manifest | It is what makes `import std` resolve on a machine whose compiler cannot provide it |

The server looks for a payload named by `--payload`, then for one containing its own image or
executable.
`--clangd` and `--kit` replace one part of it; a part still missing is looked for outside a payload
(clangd on `PATH`, the kit where xlings installs it).
