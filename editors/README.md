# editors

The four ways mcppls reaches a client, each started as `mcppls serve` (LSP) or
`mcppls mcp` (MCP) over stdio — none of them talk to each other; each
subdirectory is self-contained.

| Directory | What it is |
|---|---|
| `zed` | The Zed extension: finds `mcppls` on PATH and starts it; built to WebAssembly |
| `vscode` | The VS Code extension ("C++ Modules"): a full npm package, marketplace `README.md`/`CHANGELOG.md`, and a bundled payload (server + clangd + semantic kit) |
| `claude-code` | A Claude Code plugin marketplace (`.claude-plugin/marketplace.json`) holding one plugin, `mcppls-lsp`, that registers the server as an LSP and MCP source |
| `copilot-cli` | Two JSON config fragments (`lsp.json`, `mcp-config.json`) a user copies or merges into GitHub Copilot CLI's own config — no build, nothing installed from here |
| `agents` | Not an integration itself: the overview doc for driving mcppls from any coding agent, covering the `PATH` requirement, all three integrations above, and the MCP tool surface (`cxx_symbol`, `cxx_review`, ...) directly |

See `agents/README.md` first if you are wiring up a new agent; the other three
are consumed by a specific client.
