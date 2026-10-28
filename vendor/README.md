# vendor

Upstream data and source this repository carries a copy of, with its licence. Nothing here is
written by this project except where a row below says it is patched.

| Directory | What | Used by |
|---|---|---|
| `lsp-metamodel/` | The Language Server Protocol 3.18 meta model (`metaModel-3.18.json`), Microsoft's machine-readable description of the protocol, with its licence | `mcppls-lspgen` generates `src/lsp/protocol.cppm` and `.cpp` from it; CI regenerates and fails if the result differs from what is committed |

The generator itself is ours and lives with the rest of the source, at `src/bin/lspgen.cpp`.
