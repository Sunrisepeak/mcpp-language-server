# mcppls specifications

This directory holds the normative specifications that let C++ named modules be understood by editors and tools independently of the compiler a project builds with. They are implemented by mcppls and are written to be implementable by any build system, language server or editor.

| Spec | Title | Version | Status | Schema |
|---|---|---|---|---|
| [S1](s1-build-database.md) | C++ Build Database: IDE Profile | profile-version 0.2.0 | Draft | [s1-build-database.schema.json](schema/s1-build-database.schema.json) |
| [S2](s2-discovery.md) | Build Database Discovery Protocol | 0.2.0 | Draft | [s2-discovery.schema.json](schema/s2-discovery.schema.json) |
| [S3](s3-lsp-extensions.md) | Language Server Protocol Extensions for C++ Modules | protocol version 1 | Draft | TypeScript interfaces in the text |
| [S4](s4-semantic-kit.md) | Semantic Kit | kit-version 1 | Draft | [s4-kit.schema.json](schema/s4-kit.schema.json) |
| [S5](s5-semantic-query.md) | Semantic Queries for C++ Code | 0.1.0 | Draft | TypeScript interfaces in the text |

## How the specifications fit together

```
build system (mcpp, CMake, ...)
    │  writes an S1 database; S2 tells a consumer where it is and when it changes
    ▼
language server (mcppls)  ◄──  S4 semantic kit, when no compiler is available
    │  standard LSP, plus S3 for module status, graph and contexts
    ▼
editor (VS Code extension, other LSP clients)
```

- **S1** describes a project's translation units, module graph, toolchain, standard library and semantic options. A conforming document is a valid P2977R2 build database and exports to `compile_commands.json`.
- **S2** is how a consumer finds an S1 database: explicit configuration, a discovery command speaking JSON over standard input and output, or a known build directory.
- **S3** is what a module-aware language server adds to LSP 3.18, negotiated through `experimental.cxxModules`.
- **S4** is the manifest of a data-only semantic kit that stands in for a compiler installation. The kits mcppls distributes are published as `mcppls-kit`.
- **S5** is what a coding agent or a script asks a language server about the code — symbols, references, calls, modules, build contexts, fresh diagnostics — and the shape of the answers, bound to the Model Context Protocol and a command line.

## Relationship to standardization work

These specifications are self-contained. They are compatible with, and borrow their vocabulary from, WG21 P2977R2 (build database files), P1689R5 (module dependency information), P3286 / EcoStd RFC #3 (module metadata), CPS and LSP 3.18. P2977 is an SG15 proposal: it is not part of any standard and has no active standardization vehicle, so S1 defines every field it uses rather than referring to P2977. S1 and S2 are intended for submission to EcoStd after their 1.0 releases.

## Layout and change process

| Path | Content |
|---|---|
| `s1-…md` to `s4-…md` | Specification text |
| `schema/` | JSON Schema (draft 2020-12) documents for S1, S2 and S4 |
| `examples/` | Example documents; every example validates against its schema |
| `CHANGELOG.md` | Changes per specification and version |

- A change to a specification changes its text, its schema, its examples and the affected conformance fixtures in the same commit. CI validates every example against its schema.
- Each specification is versioned independently. S1 and S2 use semantic versioning; S3 and S4 use integer versions. MINOR versions add optional fields only; incompatible changes require a MAJOR version (or a new integer version) with migration notes.
- Releases are tagged `spec-s1-v<version>`, `spec-s2-v<version>`, `spec-s3-v<version>` and `spec-s4-v<version>`.
- S1 reaches 1.0 only after at least two producers (mcpp and a CMake adapter) and one consumer (mcppls) pass the conformance suite.

## Rule identifiers and traceability

Every requirement of S1 to S4 written with a capitalized RFC 2119 keyword (MUST, MUST NOT, REQUIRED, SHALL, SHOULD, SHOULD NOT, RECOMMENDED) carries an identifier `S<n>-<section>-<ordinal>` where it is stated, rendered as a superscript with an anchor, for example [`S2-3.3-8`](s2-discovery.md#S2-3.3-8). Identifiers are stable: a new rule takes the next ordinal of its section, and a removed rule's identifier is not reused.

[`conformance/traceability.json`](../../conformance/traceability.json) maps each identifier to its evidence: a check of `tools/validate.py` (schema, example and semantic checks), a unit test, a conformance fixture check, a line of a script that enforces the rule, or, rarely, the reason no automated evidence can exist. `tools/validate.py` fails when a keyword has no identifier, an identifier has no evidence, or evidence names a check, test, fixture or script line that does not exist. A known gap may be listed under `$pending` with what is missing; it is printed on every run until it has evidence.

## License

The specification text, schemas and examples are licensed under the Apache License 2.0, like the rest of this repository. The license may be adjusted as EcoStd requires when a specification is submitted there.
