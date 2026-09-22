# mcppls with coding agents

mcppls integrates with coding agents through standard protocols: LSP, for
editor-style navigation and post-edit diagnostics, and MCP, for
structured, symbol-oriented queries. This page covers what is available now and what
to expect next. Background: [`.agents/docs/design.md`](../../.agents/docs/design.md) §4.

## Requirement: `mcppls` on PATH

Every integration below starts the server as `mcppls serve` (LSP over stdio) — the
`mcppls` binary must resolve on `PATH` wherever the agent runs.

- **From a release**: download `payload-<platform>.tar.gz` (`linux-x64`, `darwin-arm64`,
  `win32-x64`) from the [release page](https://github.com/Sunrisepeak/mcpp-language-server/releases),
  unpack it, and put its `payload/bin/` on `PATH`. It carries the server, its pinned clangd and the
  semantic kit.
- **Once published to the xlings index**: `xlings install mcpp-language-server -y -g` (xlings
  package `mcpp-language-server`, program `mcppls`). Not published there yet.
- **Or build it locally**: `mcpp build` from a clone of this repository, binary at
  `target/<triple>/<fingerprint>/bin/mcppls` (for example
  `target/x86_64-linux-gnu/<fingerprint>/bin/mcppls` on Linux); put that `bin/` directory
  on `PATH`.
- **Or assemble a runnable payload** (server + clangd + a semantic kit) as described in
  `../../packaging/README.md`, and put its `bin/` on `PATH`.

## Claude Code

The plugin at `../claude-code/mcppls-lsp` (marketplace `mcppls`, `editors/claude-code`)
registers `mcppls serve` as the language server for C and C++ sources, including the
C++20/23 module extensions (`.cppm`, `.ccm`, `.cxxm`, `.c++m`, `.ixx`, `.mpp`, `.mxx`) next
to the traditional ones. It **replaces** the official `clangd-lsp` code-intelligence
plugin for a project — the two should not both be enabled; see that plugin's own README.

From a clone of this repository:

```
git clone https://github.com/Sunrisepeak/mcpp-language-server
/plugin marketplace add mcpp-language-server/editors/claude-code
/plugin install mcppls-lsp@mcppls
```

If this repository is already the workspace open in Claude Code, the marketplace path is
just `editors/claude-code`. Either way, the same steps work non-interactively:

```
claude plugin marketplace add editors/claude-code
claude plugin install mcppls-lsp@mcppls
```

See `../claude-code/mcppls-lsp/README.md` for a raw-URL add that skips cloning, and for
`claude plugin validate`, which checks both manifests without installing anything.

Once enabled and `mcppls` is on `PATH`, Claude Code gets automatic diagnostics after every
edit and LSP-based navigation (definition, references, hover, document symbols,
implementations, call hierarchy) across modules and partitions, the same way it does for
any other code-intelligence plugin.

## GitHub Copilot CLI

`../copilot-cli/lsp.json` registers `mcppls serve` the same way, in Copilot CLI's own
schema (`lspServers.<name>.command` / `args` / `fileExtensions` / ...), documented at
<https://docs.github.com/en/copilot/how-tos/copilot-cli/set-up-copilot-cli/add-lsp-servers>.
Use it one of two ways:

- **Per project**: copy it to `.github/lsp.json` in the project you want mcppls in.

  ```
  cp editors/copilot-cli/lsp.json /path/to/project/.github/lsp.json
  ```

- **For yourself, everywhere**: merge its `lspServers.mcppls` entry into
  `~/.copilot/lsp-config.json`.

Once the directory is trusted, Copilot CLI starts `mcppls` in the background and uses it
for definitions, references, hover, rename, document symbols, workspace symbols,
implementations and call hierarchy — the same eight operations it uses any other language
server for.

## MCP: the agent tools

`mcppls mcp` serves the Model Context Protocol on standard input and output (S5, section 6).
It answers what an agent otherwise pieces together with grep and builds, from the
compiler's own view of the workspace:

| Tool | What it answers |
|---|---|
| `cxx_symbol` | Symbols by name, id or position: declaration, definition, signature, documentation, module |
| `cxx_references` | References, callers or callees, searched in the symbol's module and everything importing it |
| `cxx_outline` | A file's declarations |
| `cxx_module` | A module's units, partitions, imports, importers and interface; the module graph |
| `cxx_build_context` | How a file is built: sets, role, compiler, standard library, standard, macros |
| `cxx_diagnostics` | Diagnostics of files as they are on disk now |
| `cxx_verify` | An edit checked: changed files and their importers, a snippet before it is written, or several toolchains |
| `cxx_impact` | What a change does to module interfaces and what it can break |
| `cxx_review` | A review of a change: rule findings with evidence; `model: "agent"` adds the context for you to judge |

Every tool is read-only. Lines and columns start at 1.

**Claude Code**: the `mcppls-lsp` plugin above also registers the MCP server (`.mcp.json`).
Without the plugin, add it to a project's `.mcp.json`:

```json
{ "mcpServers": { "mcppls": { "command": "mcppls", "args": ["mcp"] } } }
```

**GitHub Copilot CLI**: merge `../copilot-cli/mcp-config.json` into `~/.copilot/mcp-config.json`,
or pass it for one session with `copilot --additional-mcp-config @editors/copilot-cli/mcp-config.json`.

**Any other MCP client**: start `mcppls mcp` in the workspace root (or pass `--root DIR`).
`--payload`, `--clangd`, `--kit` and `--engine` work as for `mcppls serve`.

**Sharing one warm session**: with `mcppls mcp --daemon`, every connection to a workspace — several agents,
or an agent that restarts its MCP servers — reaches one workspace daemon whose engines have already loaded the
project and built its modules; the first connection starts it. `mcppls daemon status` and `mcppls daemon stop`
manage it, and it exits by itself after 30 idle minutes.

A model for `cxx_review` beyond the agent itself is enabled only where the server is started
(design §11): `mcppls mcp --model-source gateway --model-gateway PATH --model-name NAME`
(the gateway is `tools/model-gateway/`), or `--model-source mcp-sampling` for a client that supports
MCP sampling.

## Command line and hooks

The same queries are commands, for scripts, CI and agent hooks; each prints S5 JSON
(`--format text` for people) and exits 0, 1 (nothing found, errors, error findings) or 2:

```
mcppls query symbol hello::greet
mcppls query refs hello::greet
mcppls diagnostics src/main.cpp
mcppls verify --changed                  # after an edit: the changed files and their importers
mcppls review --base origin/main --format sarif --output review.sarif
```
