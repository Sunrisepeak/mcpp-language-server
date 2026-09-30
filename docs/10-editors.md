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
their language features off for this workspace; `mcppls.detectConflicts` controls that offer. Any
time later, *Turn Off Other C++ Language Features* does it for this workspace or everywhere and
*Restore Other C++ Language Features* undoes it; the C/C++ extension's debugger keeps working. A
conflicting extension that becomes active later is named in a notice. An extension has no way to
disable another one: only their own settings are changed, and only when you choose to.

**Completion alongside Copilot.** VS Code 1.125 and later keep the completion list closed while an
inline completion (GitHub Copilot is built into VS Code) shows grey text, and open it otherwise only
once you stop typing (`editor.quickSuggestions`'s default, `{"other": "offWhenInlineCompletions"}`).
For C and C++ files the extension sets `{"other": "on"}` instead: the list opens as you type, and the
grey text shows beside it. It outranks an `editor.quickSuggestions` set for every language; setting
it under `"[cpp]"` and `"[c]"` keeps yours (the log says so, once).

**Highlighting.** `import`, `module`, `export` and module names are colored twice over: by a
grammar the extension adds (at once, as you type), and by the server's semantic tokens (module
names as the token type `module`, which themes color as a namespace unless you give it a color of
its own in `editor.semanticTokenColorCustomizations`). VS Code's own C++ grammar leaves `import`
uncolored.

**Turning it off for one workspace.** `mcppls.enable` (default `true`, per workspace) keeps mcppls
off; **C++ Modules: Turn Off in This Workspace** and **Turn On in This Workspace** set it, and the
status item reads "C++ Modules: off in this workspace". The other editors have no such setting; each
one's page below, and its README, says how to do it there.

**When something cannot be recovered.** If mcppls cannot recover by itself, one notification offers
**Report Issue…**, **Restart Server**, **Reset This Workspace's Cache**, **Turn Off in This
Workspace** and **Show Logs**, after writing a diagnostic bundle; see
[50-troubleshooting.md](50-troubleshooting.md#when-mcppls-cannot-recover-by-itself).

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

To keep mcppls off in one project, a project's `.zed/settings.json` names the server with a `!`
(and names clangd back, since a project's list replaces yours); see
[editors/zed/README.md](../editors/zed/README.md#keeping-mcppls-off-for-one-project).

## Neovim

The plugin at [`editors/nvim/`](../editors/nvim/README.md) (Neovim 0.10 or later) finds `mcppls` —
on PATH, or the payload `--install` puts in the user data directory — and starts it for C and C++
buffers through Neovim's own LSP client. Put `editors/nvim` on the runtimepath and call
`require('mcppls').setup()`; on 0.11 and later `vim.lsp.enable('mcppls')` works too. It adds
`:McpplsStatus`, `:McpplsRestart`, `:McpplsReload`, `:McpplsResetCache` (resets this workspace's
cache and prepares again) and a statusline component. Do not also start
clangd for C and C++: the plugin names a second C++ server once if one attaches, and with
`disable_conflicting = true` stops it for you. Module keywords and names come from the server's
semantic tokens (`@lsp.type.keyword`, `@lsp.type.module`; `semantic_tokens_modules = false` turns
them off). There is no per-project switch; the README shows two ways to keep it off for one project
([editors/nvim/README.md](../editors/nvim/README.md#keeping-mcppls-off-for-one-project)).

## CLion

The plugin at [`editors/clion/`](../editors/clion/README.md) registers mcppls through the IntelliJ
platform's LSP API.

`mcpp run --features clion -p devtools -- extension --editor clion --install` builds it, puts the
server in place and unpacks the plugin into every CLion's plugins directory; restart CLion. It
has not been exercised in a running CLion yet; see its README. The plugin has no settings; to keep it
off for one project, disable it for that project in Settings | Plugins
([editors/clion/README.md](../editors/clion/README.md#keeping-mcppls-off-for-one-project)).

## Any other LSP client

`mcppls serve` speaks LSP over stdio. The server needs to find a payload — see
[00-install.md](00-install.md).

## Any other MCP client

`mcppls mcp` speaks MCP over stdio and exposes the semantic tools. Several clients on one machine
can share a workspace through `mcppls mcp --daemon`. See [40-agents.md](40-agents.md).
