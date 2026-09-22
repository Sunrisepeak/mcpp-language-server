# Editors and agents

Every extension here can also be built from source, and one tool builds all of them:

```bash
mcpp run -p devtools -- extension --editor vscode|zed|clion|all   # build (all is the default)
mcpp run -p devtools -- extension --editor vscode|zed|clion --install   # and install it
mcpp run -p devtools -- uninstall --editor vscode|zed|clion|all   # remove it again
```

What packaging needs — Node and Rust — is declared in `mcpp.toml`'s `[xlings.workspace]`,
so `mcpp run` provisions it before the tool starts. Gradle and the JDK it runs on sit behind the
`clion` feature, so only someone building that plugin downloads them:
`mcpp run --features clion -p devtools -- extension --editor clion`. Zed and CLion have no
command line for installing a plugin, so `--install` leaves on disk exactly what their own UI would:
for Zed it stops one step short and names the palette action, the recommended route (`--link`
takes that step too); for CLion it unpacks the plugin as *Install Plugin from Disk* does. Both
start `mcppls` from PATH, or else the server `--install` puts at `<user data>/mcppls/payload`.

## VS Code

Install **C++ Modules** (`sunrisepeak.mcpp-language-server`). It carries the server, a pinned
clangd and the semantic kit; nothing else to install and nothing to configure. Settings and
commands are in [30-settings.md](30-settings.md); the extension's own page is
[editors/vscode/README.md](../editors/vscode/README.md).

**Alongside the mcpp extension.** `mcpp-community.mcpp-vscode` ("mcpp") and this one are separate
and both are worth having:

| | Handles |
|---|---|
| **mcpp** | Building, toolchains, project operations |
| **C++ Modules** (this one) | C++ module semantics, driving its own pinned clangd |

**Alongside other C++ extensions.** Microsoft's C/C++ extension and the official clangd extension
both want to be the language server for the same files. On first run mcppls offers, once, to turn
their language features off for this workspace; `mcppls.detectConflicts` controls that offer.

## Claude Code

A plugin registers `mcppls serve` as the language server for C, C++ and the module extensions
(`.cppm`, `.ccm`, `.cxxm`, `.c++m`, `.ixx`, `.mpp`, `.mxx`). It replaces the official clangd plugin
for a project rather than running beside it. See
[editors/claude-code/mcppls-lsp/README.md](../editors/claude-code/mcppls-lsp/README.md).

## GitHub Copilot CLI

`editors/copilot-cli/` has an LSP entry and an MCP entry to drop into its configuration.

## Zed

The extension at [`editors/zed/`](../editors/zed/README.md) finds `mcppls` on PATH and starts it.
`mcpp run -p devtools -- extension --editor zed --install` builds it and the server, then leaves
one step: command palette → "zed: install dev extension" → `editors/zed`. Add `--link` to have the
tool make that same link itself. See [editors/zed/README.md](../editors/zed/README.md).

Zed ships clangd for C and C++, and running both over one file means two engines answering the same
question, so put mcppls first: `"languages": {"C++": {"language_servers": ["mcppls", "!clangd"]}}`.
mcppls starts clangd itself with a module database clangd would not otherwise have.

## Neovim

The plugin at [`editors/nvim/`](../editors/nvim/README.md) (Neovim 0.10 or later) finds `mcppls` —
on PATH, or the payload `--install` puts in the user data directory — and starts it for C and C++
buffers through Neovim's own LSP client. Put `editors/nvim` on the runtimepath and call
`require('mcppls').setup()`; on 0.11 and later `vim.lsp.enable('mcppls')` works too. It adds
`:McpplsStatus`, `:McpplsRestart`, `:McpplsReload` and a statusline component. Do not also start
clangd for C and C++: the plugin names a second C++ server once if one attaches.

## CLion

The plugin at [`editors/clion/`](../editors/clion/README.md) registers mcppls through the IntelliJ
platform's LSP API.

`mcpp run --features clion -p devtools -- extension --editor clion --install` builds it, puts the
server in place and unpacks the plugin into every CLion's plugins directory; restart CLion. It
has not been exercised in a running CLion yet; see its README.

## Any other LSP client

`mcppls serve` speaks LSP over stdio. The server needs to find a payload — see
[00-install.md](00-install.md).

## Any other MCP client

`mcppls mcp` speaks MCP over stdio and exposes the semantic tools. Several clients on one machine
can share a workspace through `mcppls mcp --daemon`. See [40-agents.md](40-agents.md).
