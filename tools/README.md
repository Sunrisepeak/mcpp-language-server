# tools

Programs that are not the server. Each is its own member of the mcpp workspace and depends on no
server code.

| Directory | What | Run by |
|---|---|---|
| `devtools/` | `mcppls-devtools`, the one entry for work on this repository: payloads, editor extensions, releases, versions, checks, timing, benchmarks | `mcpp run -p devtools -- <command>` ([docs/93-devtools.md](../docs/93-devtools.md)) |
| `model-gateway/` | `mcppls-model`, the only program that ever talks to a model endpoint | started by the server when a model source is configured |
| `bench/` | The agent task benchmark and the review fixtures (expected findings, precision and recall per rule) | `mcpp run -p devtools -- bench tasks ...` / `bench review ...` |

The server's own extra executables — the conformance runner, the LSP protocol generator and the
mocks — are targets of the root package in [`src/bin/`](../src/bin), because they share its modules.
