# For coding agents and CI

An editor asks "what is under the cursor". An agent asks "what does this symbol mean, who calls it,
and did my edit break anything" — and it asks without a cursor, a window, or a human to look at the
result. These are the same answers the editor gets, shaped for that.

The protocol is specified as [S5](specs/s5-semantic-query.md); this is what it gives you.

| Capability | MCP tool | Command line |
|---|---|---|
| Symbols by name, identifier or position | `cxx_symbol` | `mcppls query symbol` |
| References and callers — searched in the symbol's own module **and every module importing it**, re-exports included | `cxx_references` | `mcppls query refs` |
| Callees | `cxx_references` | `mcppls query calls` |
| A file's outline, a module's description, the module graph | `cxx_outline`, `cxx_module` | `mcppls query outline\|module` |
| What a file is built as (arguments, standard library, context) | `cxx_build_context` | `mcppls query context` |
| Fresh diagnostics for what is on disk now | `cxx_diagnostics` | `mcppls diagnostics` |
| Verify after an edit: the changed files and everything importing them, built again | `cxx_verify` | `mcppls verify --changed` |
| Check a candidate snippet in place, without writing it | `cxx_verify` | `mcppls verify` |
| What a change affects | `cxx_impact` | `mcppls impact` |
| Review a change: semantic diff, impact, evidence-carrying rules; SARIF, LSP or Markdown | `cxx_review` | `mcppls review --base REV --format sarif` |

Two things make these worth using over grepping:

- **They follow modules, not files.** "Who calls this" means the modules that import the one
  declaring it, not the files that happen to mention the name.
- **They answer from the same model the editor uses**, so an agent and the human are looking at the
  same build.

**Model-backed review** is optional and off by default. With it off, review runs deterministic
rules only and the server makes no network calls at all. Wiring, and the shared workspace daemon,
are in [editors/agents/README.md](../editors/agents/README.md).
