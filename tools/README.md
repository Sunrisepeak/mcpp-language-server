# tools

Programs that are not the server. Each is its own member of the mcpp workspace and depends on no
server code.

| Directory | What | Run by |
|---|---|---|
| `devtools/` | `mcppls-devtools`, the one entry for work on this repository: payloads, editor extensions, releases, versions, checks, stress, the scenarios' distributions | `mcpp run -p devtools -- <command>` ([docs/93-devtools.md](../docs/93-devtools.md)) |
| `model-gateway/` | `mcppls-model`, the only program that ever talks to a model endpoint | started by the server when a model source is configured |

The tests -- unit, conformance and measurements, with the agent task benchmark and the review
fixtures -- are under [`tests/`](../tests). The server's own extra executables — the conformance runner, the LSP protocol generator and the
mocks — are targets of the root package in [`src/bin/`](../src/bin), because they share its modules.
