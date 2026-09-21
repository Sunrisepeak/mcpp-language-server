# src

The server's own source, C++23 modules only: no headers, no macros. Every `.cppm`
(interface) and its `.cpp` (implementation) live side by side; `src/<dir>/<name>.cppm`
is module `mcppls.<dir>.<name>` — a deeper path adds a segment, so
`src/engine/clangd/definition.cppm` is `mcppls.engine.clangd.definition`. `main.cpp`
is the one file not under a subdirectory.

| Directory | Holds |
|---|---|
| `base` | Dependency-free utilities: errors, logging, paths, globbing, sha256, text, URIs, version |
| `platform` | OS-facing operations: filesystem, process spawning, environment, networking, tool environment (platform constants themselves are `os/`, outside `src/`) |
| `lsp` | JSON-RPC transport and the generated LSP protocol types |
| `spec` | The build-database, discovery and semantic-kit specification types |
| `project` | Detecting and modeling a project's build: mcpp, CMake, `compile_commands.json`, git, inference |
| `normalize` | Turns a detected build into mcppls's own semantic plan, for the GNU and MSVC families |
| `engine` | The semantic engines: clangd-backed (`engine/clangd`) and mcppls's own native module index (`engine/native`) |
| `orchestrator` | The per-workspace kernel: documents, routing, the LSP client, journal, report |
| `server` | The LSP session entry point |
| `cli` | The `mcppls` command line and its composition root |
| `ai` | The MCP server, model gateway, and the review/verify/query features for agents |
| `toolchain` | Compiler discovery and probing, including Visual Studio |
| `bin` | The package's other executables: the conformance runner, the protocol generator, the packaging tool, and the mock producer and model the fixtures drive |
