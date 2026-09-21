# S5 — Semantic Queries for C++ Code

| | |
|---|---|
| Specification | S5 |
| Version | 0.1.0 |
| Status | Draft |
| Bindings | Model Context Protocol (2024-11-05, 2025-03-26, 2025-06-18); command line |
| License | Apache-2.0 |

## Abstract

This specification defines the questions a coding agent, a script or an editor asks a C++ language server about a workspace, and the shape of the answers: symbols found by name, position or identifier, their references, callers and callees, file outlines, modules and the module graph, a file's build context, and diagnostics computed for the content files have now. Results name locations the way a reader counts them, say which state of the workspace they describe, and say when they are incomplete. Section 3.7 verifies an edit, and section 5 reviews a change: findings of deterministic rules, each with the evidence it rests on, in S5, LSP and SARIF forms. Section 6 binds all of it to the Model Context Protocol (MCP), section 7 to a command line.

## 1. Conventions

- The key words **MUST**, **MUST NOT**, **REQUIRED**, **SHALL**, **SHALL NOT**, **SHOULD**, **SHOULD NOT**, **RECOMMENDED**, **MAY** and **OPTIONAL** are to be interpreted as described in BCP 14 (RFC 2119, RFC 8174) when, and only when, they appear in all capitals.
- Structures are written as TypeScript interfaces. A field marked `?` may be absent.
- "Set", "role" and "module unit" have the meanings of [S1](s1-build-database.md); "status" and "context" those of [S3](s3-lsp-extensions.md). A module name is written `m`, a partition `m:p`.
- The *core engine* is the component that answers C++ semantics (clangd in mcppls); the *module index* is the server's own knowledge of module declarations and imports. A workspace may run without a core engine.

## 2. Result conventions

### 2.1 Locations

```ts
interface Location {
  file: string;       // relative to the workspace root, '/'-separated; absolute outside the root
  line: number;       // from 1
  column: number;     // from 1, in Unicode scalar values
  text: string;       // the whole line, without its line terminator
  endLine?: number;   // the end of a range, when the location is one
  endColumn?: number;
}
```

- Lines and columns **MUST** count from 1, a column counting Unicode scalar values, not bytes or UTF-16 code units. <a id="S5-2.1-1"></a><sup>S5-2.1-1</sup>
- A location **MUST** carry the text of its line, so a reader need not open the file to see what it names. <a id="S5-2.1-2"></a><sup>S5-2.1-2</sup>
- A file inside the workspace root **MUST** be named relative to the root with `/` separators on every platform. <a id="S5-2.1-3"></a><sup>S5-2.1-3</sup>

### 2.2 Snapshot

```ts
interface Snapshot {
  generation: number;   // advances whenever a document, a watched file, the project model or the plan changes
  overlays: string[];   // files whose unsaved content, not their content on disk, was read
  preparing: boolean;   // the project model or modules were still being prepared
  indexing: boolean;    // the core engine's index was still being built
}
```

- Every query result **MUST** carry the snapshot it was computed from. <a id="S5-2.2-1"></a><sup>S5-2.2-1</sup>
- A server **MUST** read a file's content on disk at the time of the query, unless a client of the same session holds unsaved content for it, which it then lists in `overlays`. <a id="S5-2.2-2"></a><sup>S5-2.2-2</sup>

### 2.3 Symbol identifiers

A symbol's `id` is the Unified Symbol Resolution string (USR) the core engine reports for it.

- A server **MUST** resolve an `id` produced by the same session without its name. <a id="S5-2.3-1"></a><sup>S5-2.3-1</sup>
- A caller outside that session **SHOULD** pass the symbol's name with its `id`; the server then finds the named symbols and keeps the one whose USR matches. <a id="S5-2.3-2"></a><sup>S5-2.3-2</sup>

### 2.4 Limits

Lists are bounded by `maxResults`. A bounded result carries `total`, the number of items before the bound, and `truncated`.

- A server **MUST** order list items deterministically: by module, then file, then line and column. <a id="S5-2.4-1"></a><sup>S5-2.4-1</sup>

### 2.5 Failures

```ts
interface Failure {
  code: "invalid-arguments" | "not-found" | "ambiguous" | "unavailable" | "timeout" | "untrusted" | "unknown-tool";
  message: string;
  candidates?: Symbol[];   // for ambiguous
}
```

- A query that names more than one symbol where one is needed **MUST** fail with `ambiguous` and list the candidates with their identifiers. <a id="S5-2.5-1"></a><sup>S5-2.5-1</sup>
- A query that needs the core engine in a workspace without one **MUST** fail with `unavailable`, not return an empty result. <a id="S5-2.5-2"></a><sup>S5-2.5-2</sup>

## 3. Queries

### 3.1 Symbols

Input: `name` (unqualified, or qualified with `::`), or `id`, or `file`, `line` and `column`; optionally `kind` and `module` to narrow a name.

```ts
interface Symbol {
  id?: string;
  name: string;
  qualifiedName: string;
  kind: string;              // an LSP SymbolKind in kebab case: function, method, class, struct, namespace, ...; or module
  module?: string;           // of the unit that declares it
  declaration?: Location;
  definition?: Location;
  signature?: string;
  type?: string;             // a function's return type, or a variable's type
  documentation?: string;
}
interface Symbols { snapshot: Snapshot; symbols: Symbol[]; total: number; truncated: boolean }
```

- A name **MUST** match a symbol's own name exactly, and a qualified name also the scope that contains it. <a id="S5-3.1-1"></a><sup>S5-3.1-1</sup>
- `kind` values **MUST** distinguish a struct from a class. <a id="S5-3.1-2"></a><sup>S5-3.1-2</sup>
- `module` **MUST** name the partition when the declaration is in a partition. <a id="S5-3.1-3"></a><sup>S5-3.1-3</sup>
- A symbol defined in another unit of its module than the one declaring it (an implementation unit) **MUST** have that `definition`, also when it was found by a position in an importer. <a id="S5-3.1-4"></a><sup>S5-3.1-4</sup>
- A symbol the core engine's index names twice (by declaration and by definition) **MUST** be one result. <a id="S5-3.1-5"></a><sup>S5-3.1-5</sup>

### 3.2 References

```ts
interface ReferenceGroup { module?: string; file: string; references: { line: number; column: number; text: string }[] }
interface SearchScope { searchedFiles: number; complete: boolean; unsearched: string[] }
interface References { snapshot: Snapshot; symbol: Symbol; groups: ReferenceGroup[]; total: number; truncated: boolean; scope: SearchScope }
```

A core engine's index can miss references in units whose imports it does not build. The *search scope* of a symbol declared in module `m` is: the units of `m`, every unit that imports `m`, and, for every module that re-exports a module already in the scope, every unit that imports it.

- A server **MUST** find references in every unit of the search scope, within its budget, including units that reach the symbol only through a re-exporting module. <a id="S5-3.2-1"></a><sup>S5-3.2-1</sup>
- When the budget left part of the search scope unsearched, `scope.complete` **MUST** be false, with those units in `scope.unsearched`. <a id="S5-3.2-2"></a><sup>S5-3.2-2</sup>

### 3.3 Calls

Input: a symbol, and `direction`: `callers` or `callees`.

```ts
interface Call { symbol: Symbol; sites: Location[] }   // sites are in the caller
interface Calls { snapshot: Snapshot; symbol: Symbol; direction: "callers" | "callees"; calls: Call[]; total: number; truncated: boolean; scope: SearchScope }
```

A caller is the function, method or constructor that encloses a reference. Two programs of a workspace each have their own `main`, with one identifier between them; they are two callers.

- Callers **MUST** be searched in the search scope of 3.2, each enclosing function of a file its own caller. <a id="S5-3.3-1"></a><sup>S5-3.3-1</sup>
- A call's `qualifiedName` **MUST** include the scope the function is declared in. <a id="S5-3.3-2"></a><sup>S5-3.3-2</sup>

### 3.4 File outline

```ts
interface OutlineEntry { name: string; kind: string; line: number; detail?: string; children?: OutlineEntry[] }
interface Outline { snapshot: Snapshot; file: string; module?: string; symbols: OutlineEntry[] }
```

- An outline of a module unit **MUST** include its module declaration. <a id="S5-3.4-1"></a><sup>S5-3.4-1</sup>

### 3.5 Modules

Input: a module `name` (a partition names its module), or a `file` of the module.

```ts
interface ModuleDescription {
  snapshot: Snapshot;
  name: string;
  external: boolean;                 // provided by a standard library or module metadata
  resolvedFrom: "set" | "stdlib" | "module-metadata";
  units: { file: string; name: string; role: string; sets?: string[] }[];
  partitions: string[];
  exportedPartitions: string[];      // re-exported by the primary interface
  imports: string[];                 // modules its units import, its own partitions excepted
  importedBy: string[];              // modules whose units import it
  importingFiles: string[];           // units that import it and belong to no module
}
interface ModuleGraph { snapshot: Snapshot; modules: { name: string; external: boolean; files: string[] }[]; imports: { from: string; to: string }[]; total: number; truncated: boolean }
```

- A description **MUST** list every unit of the module with its role, implementation units included. <a id="S5-3.5-1"></a><sup>S5-3.5-1</sup>
- Module queries **MUST** be answered without a core engine. <a id="S5-3.5-2"></a><sup>S5-3.5-2</sup>

### 3.6 Diagnostics

Input: `files`, and `fresh` (default true).

```ts
interface Diagnostic { severity: "error" | "warning" | "information" | "hint"; message: string; code?: string; source?: string; location: Location; related?: { location: Location; message: string }[] }
interface FileDiagnostics { file: string; complete: boolean; reason?: string; diagnostics: Diagnostic[] }
interface DiagnosticsReport { snapshot: Snapshot; files: FileDiagnostics[]; counts: { error: number; warning: number; information: number; hint: number }; semanticSource: string }
```

- With `fresh`, a server **MUST** wait, within its deadline, until the core engine's diagnostics are computed for the content each file has now. <a id="S5-3.6-1"></a><sup>S5-3.6-1</sup>
- A file whose diagnostics are not computed for its current content when the result is returned **MUST** have `complete: false` and a `reason`. <a id="S5-3.6-2"></a><sup>S5-3.6-2</sup>

### 3.7 Verification

Input: changed `files`, or `changed` (the working tree's changes) or `base` (the changes since a revision), and a `budget` of files; or a `snippet`: `file`, `line` (the snippet goes before it), `replaceLines` (default 0) and `code`.

```ts
interface Verification {
  snapshot: Snapshot;
  verdict: "pass" | "errors" | "incomplete";
  changed: string[];
  removed: string[];
  checked: { file: string; why: "changed" | "importer"; complete: boolean; reason?: string; diagnostics: Diagnostic[] }[];
  unchecked: string[];       // beyond the budget
  counts: { error: number; warning: number; information: number; hint: number };
  semanticSource: string;
}
interface SnippetVerification {
  snapshot: Snapshot;
  verdict: "pass" | "errors" | "incomplete";
  file: string;
  line: number;
  endLine: number;           // the snippet's last line in the candidate file
  complete: boolean;
  inSnippet: Diagnostic[];
  introduced: Diagnostic[];  // elsewhere in the file, and not there before
  counts: { error: number; warning: number; information: number; hint: number };
}
```

- Verifying a changed interface unit **MUST** check the units of its search scope (3.2) that exist, each reported with `why: "importer"`. <a id="S5-3.7-1"></a><sup>S5-3.7-1</sup>
- An importer the core engine built before the change **MUST** be built again before its diagnostics count. <a id="S5-3.7-2"></a><sup>S5-3.7-2</sup>
- A snippet verification **MUST** leave the file on disk, and the content the engines see for it afterwards, as they were. <a id="S5-3.7-3"></a><sup>S5-3.7-3</sup>
- A diagnostic outside the snippet that the file had before **MUST NOT** be reported as introduced. <a id="S5-3.7-4"></a><sup>S5-3.7-4</sup>
- Unless an error was found, the verdict **MUST** be `incomplete` when a checked file's diagnostics are not complete or files were left unchecked. <a id="S5-3.7-5"></a><sup>S5-3.7-5</sup>
- Changes read from git **MUST** be refused with `untrusted` in a workspace that is not trusted. <a id="S5-3.7-6"></a><sup>S5-3.7-6</sup>

## 4. Contexts

### 4.1 Build context

```ts
interface BuildContext {
  snapshot: Snapshot;
  file: string;
  module?: string;
  inModel: boolean;                                // some set of the project model builds the file
  sets: { name: string; kind: string; role: string }[];
  contextSet: string;                              // S3 context; "default" for all sets
  projectSource: string;                           // mcpp | cmake | compile-commands | build-database | inferred
  semanticSource: "build-toolchain" | "semantic-kit";
  compiler?: string;
  toolchain?: { family: string; version: string };
  target: string;
  stdlib: string;
  languageStandard?: string;
  macros: string[];                                // NAME, NAME=value, or -NAME for an undefinition
  includeDirectories: string[];
  excludedFromEngine: boolean;                     // left out of the core engine's database
  engine: string;                                  // "clangd 23.1.0", or "none"
  issues: { code: string; message: string }[];
}
```

- A build context **MUST** give the file's role in each set that builds it. <a id="S5-4.1-1"></a><sup>S5-4.1-1</sup>
- A file no set builds **MUST** have `inModel: false` and an issue `not-in-model`. <a id="S5-4.1-2"></a><sup>S5-4.1-2</sup>

### 4.2 Module interface

What `import m;` brings in, without implementations.

```ts
interface InterfaceDeclaration {
  kind: "function" | "class" | "struct" | "union" | "enum" | "concept" | "alias" | "variable" | "other";
  name: string;
  qualifiedName: string;
  declaration: string;       // without body or initializer, whitespace collapsed
  documentation?: string;    // the comment right above it
  unit: string;              // the unit that exports it: m, or m:p
  location: Location;
  conditional?: boolean;     // inside #if, #ifdef or #ifndef: the summary does not preprocess
}
interface ModuleInterface {
  module: string;
  files: string[];           // interface units read, the primary interface first
  reexports: string[];       // other modules `export import` brings in; not expanded
  declarations: InterfaceDeclaration[];
  total: number;
  truncated: boolean;
  documentationOmitted: boolean;
}
```

Input: a budget in tokens, four characters counting as one.

- A summary **MUST** include the declarations of every partition the primary interface re-exports, transitively, each with the unit that exports it. <a id="S5-4.2-1"></a><sup>S5-4.2-1</sup>
- When the budget does not hold every declaration with its documentation, documentation **MUST** be left out before any declaration is. <a id="S5-4.2-2"></a><sup>S5-4.2-2</sup>
- Without a core engine, or for a name the core engine does not know, a symbol lookup by name (3.1) **MUST** find the declarations module interfaces export under that name. <a id="S5-4.2-3"></a><sup>S5-4.2-3</sup>

## 5. Review

### 5.1 Findings

A review reports findings. Every producer of findings, rules and models alike, uses this data.

```ts
interface Evidence { id: string; kind: "reference" | "diff" | "diagnostic" | "module-graph" | "import" | "test"; location: Location; detail?: string }
interface Finding {
  id: string;
  rule: string;                                    // e.g. module/export-removed-in-use
  severity: "error" | "warning" | "information" | "hint";
  message: string;
  location: Location;
  evidence: Evidence[];
  origin: "rule" | "model";
  fix: null | { description: string; edits: { file: string; line: number; column: number; endLine: number; endColumn: number; newText: string }[] };
  fingerprint: string;                             // "sha256:" and 64 hexadecimal digits
  model?: { source: string; name: string; template: string };
  confidence?: number;
}
```

- A fingerprint **MUST** be computed from the rule, the file and the whitespace-normalized text of the location and of the evidence, so it does not change when lines move or are reindented. <a id="S5-5.1-1"></a><sup>S5-5.1-1</sup>
- Every finding **MUST** carry at least one evidence item. <a id="S5-5.1-2"></a><sup>S5-5.1-2</sup>

### 5.2 Reviewing a change

Input: the revision the change is against (`base`, default `HEAD`; the working tree, untracked files included, is the change), or `files` to take against it; a `budget` of units. A review runs git, and so only in a trusted workspace.

Steps: collect the changed files with their content before and after; diff each C++ unit semantically (module declaration, imports, exported declarations, read by the server's own engine from both texts); find the impact (the search scope of 3.2 of every module whose interface changed, the uses there of every export that went away or changed, the sets and the tests of kind `test` involved); have the core engine build the changed units and the search scope, within the budget; run the rules.

```ts
interface UnitDiff {
  file: string;
  module?: { before: string; after: string };                    // what the unit provides, "m" or "m:p"
  role?: string | { before: string; after: string };
  imports: { change: "added" | "removed"; module: string; exported: boolean; location: Location }[];
  exports: { change: "added" | "removed" | "changed"; qualifiedName: string; kind: string; before?: string; after?: string;
             baseLocation?: Location; headLocation?: Location }[];
  interfaceChanged: boolean;
}
interface Impact {
  modules: string[];                                              // whose interface changed
  files: string[];                                                // their search scope
  unsearched: string[];
  sets: string[];
  tests: { set: string; files: string[]; changed: boolean }[];
  names: { qualifiedName: string; module: string; semantic: boolean; uses: Location[] }[];
}
interface Review {
  snapshot: Snapshot;
  base: string;
  files: { path: string; change: "added" | "modified" | "removed" | "renamed" | "untracked";
           hunks: { baseStart: number; baseCount: number; headStart: number; headCount: number }[] }[];
  diffs: UnitDiff[];
  impact: Impact;
  findings: Finding[];
  counts: { error: number; warning: number; information: number; hint: number };
  unbuilt: string[];
  complete: boolean;
}
```

| Rule | Severity | Reported when |
|---|---|---|
| `module/export-removed-in-use` | error | an export went away, or a module is no longer provided, and the search scope still uses it |
| `module/export-signature-changed` | warning | an export's declaration changed and the search scope uses it |
| `module/partition-misuse` | error | the change exports an implementation partition, or imports a partition as `import m:p;` |
| `module/import-unresolved` | error | the change imports a module no unit and no standard library provides, or several units do |
| `build/diagnostic-introduced` | the diagnostic's | the core engine reports an error or a warning on a line the change added or altered |
| `build/toolchain-divergence` | error | with `toolchains` (an mcpp project, two or more toolchains): one toolchain rejects a changed line, or a unit of the search scope, while another builds the project |
| `test/exported-change-untested` | information | a module's exports changed and no unit of a test set changed |

- A review **MUST** be refused with `untrusted` in a workspace that is not trusted. <a id="S5-5.2-1"></a><sup>S5-5.2-1</sup>
- A review **MUST** leave every file of the workspace and of its repository as it was. <a id="S5-5.2-2"></a><sup>S5-5.2-2</sup>
- The semantic diff **MUST** report a removed and an added export of one qualified name and kind as one changed export. <a id="S5-5.2-3"></a><sup>S5-5.2-3</sup>
- Uses of an export that went away **MUST** be looked for in the text of the search scope as it is now, identifiers in code only. <a id="S5-5.2-4"></a><sup>S5-5.2-4</sup>
- `module/export-removed-in-use` **MUST** carry the uses it found as `reference` evidence, through re-exporting modules too. <a id="S5-5.2-5"></a><sup>S5-5.2-5</sup>
- `module/export-signature-changed` **MUST** carry the declaration before and after as `diff` evidence. <a id="S5-5.2-6"></a><sup>S5-5.2-6</sup>
- `module/partition-misuse` **MUST** be reported for both of its forms. <a id="S5-5.2-7"></a><sup>S5-5.2-7</sup>
- `module/import-unresolved` **MUST** be limited to imports the change added or altered. <a id="S5-5.2-8"></a><sup>S5-5.2-8</sup>
- `build/diagnostic-introduced` **MUST** be limited to lines the change added or altered. <a id="S5-5.2-9"></a><sup>S5-5.2-9</sup>
- A compiler diagnostic on the line of another rule's finding **MUST** be that finding's `diagnostic` evidence rather than a finding of its own. <a id="S5-5.2-10"></a><sup>S5-5.2-10</sup>
- `test/exported-change-untested` **MUST NOT** be reported when a unit of a test set that uses the module is part of the change. <a id="S5-5.2-11"></a><sup>S5-5.2-11</sup>
- `complete` **MUST** be false when units of the search scope were not searched or not built. <a id="S5-5.2-12"></a><sup>S5-5.2-12</sup>
- Builds with other toolchains **MUST** happen outside the workspace, in a copy under the user cache. <a id="S5-5.2-13"></a><sup>S5-5.2-13</sup>
- `build/toolchain-divergence` **MUST** be reported only where another toolchain built the whole project, so that an error is not taken for a divergence because the other build stopped before reaching it. <a id="S5-5.2-14"></a><sup>S5-5.2-14</sup>

### 5.3 Forms

| Finding | LSP Diagnostic | SARIF 2.1.0 result |
|---|---|---|
| `rule` | `code` | `ruleId`, with the rule in `tool.driver.rules` |
| `severity` | `severity` | `level`: `error`, `warning`, or `note` for information and hint |
| `location` | `range` | `locations`, relative to `%SRCROOT%` |
| `evidence` | `relatedInformation` | `relatedLocations` |
| `fingerprint` | `data.fingerprint` | `partialFingerprints["mcppls/v1"]` |
| `origin`, `model` | `source`: `mcppls review`, or `mcppls review · <model>` | `properties` |

- A SARIF log **MUST** declare `columnKind: "unicodeCodePoints"`, since S5 columns count Unicode scalar values. <a id="S5-5.3-1"></a><sup>S5-5.3-1</sup>
- An LSP diagnostic of a finding **MUST** give its range in UTF-16 code units, from the location's line text. <a id="S5-5.3-2"></a><sup>S5-5.3-2</sup>

### 5.4 A model's judgement

A review can add a model's findings to its rules' ones. The model reads the *change context* — the semantic diffs, the impact, the rules' findings, and evidence items numbered `E1` to `En` across the context (the findings' own evidence, and every line the change added or removed) — through a versioned template, and answers with JSON matching the template's output schema.

| Source | What happens |
|---|---|
| `none` (default) | No model |
| `agent` | Nothing is sent anywhere: the result carries `model.agentContext`, the rendered messages and the output schema, for the agent that asked to judge itself |
| `gateway` | The model gateway process (`mcppls-model`, `model-gateway/PROTOCOL.md`) asks a local or remote model |
| `mcp-sampling` | The MCP client's own model is asked through `sampling/createMessage`, with its user's consent |
| `client` | An editor's model interface; not bound in this version |

A result with a model carries `model`: `source`, `enabled`, `budgetExceeded`, `cacheHit`, `schemaErrors`, `droppedReasons`, `findings`, `usage`, and `explanation` when only what would be sent was asked for.

- A model source other than `none` and `agent` **MUST** be enabled by whoever starts the server or runs the command, never by a request. <a id="S5-5.4-1"></a><sup>S5-5.4-1</sup>
- A model finding whose evidence is empty, or cites an id the change context does not have, **MUST** be dropped. <a id="S5-5.4-2"></a><sup>S5-5.4-2</sup>
- An answer that does not match the output schema **MUST** be dropped whole. <a id="S5-5.4-3"></a><sup>S5-5.4-3</sup>
- The evidence of a file an excluded pattern matches **MUST NOT** reach a model. <a id="S5-5.4-4"></a><sup>S5-5.4-4</sup>
- A fix a model proposes **MUST NOT** be reported unless it was verified. <a id="S5-5.4-5"></a><sup>S5-5.4-5</sup>
- A prompt **MUST** keep code inside delimited data blocks, escaped so that nothing in the code can close one. <a id="S5-5.4-6"></a><sup>S5-5.4-6</sup>
- A prompt over the token budget **MUST NOT** be sent. <a id="S5-5.4-7"></a><sup>S5-5.4-7</sup>

## 6. MCP binding

A server runs as an MCP server over standard input and output (`mcppls mcp`): one JSON-RPC message per line.

| Tool | Query |
|---|---|
| `cxx_symbol` | 3.1 |
| `cxx_references` | 3.2, and 3.3 with `direction` |
| `cxx_outline` | 3.4 |
| `cxx_module` | 3.5 with 4.2 as `interface` (unless `interface: false`); `graph: true` for the graph |
| `cxx_build_context` | 4.1 |
| `cxx_diagnostics` | 3.6 |
| `cxx_verify` | 3.7 |
| `cxx_impact` | 5.2, its first steps: `files`, `diffs` and `impact` |
| `cxx_review` | 5.2; `format: "sarif"` gives the SARIF log, `"markdown"` a report |

- Every tool **MUST** be annotated `readOnlyHint: true`, and leave every file of the workspace as it was. <a id="S5-6-1"></a><sup>S5-6-1</sup>
- A tool result **MUST** carry its S5 result as JSON text, and also as `structuredContent` when the negotiated protocol version is 2025-06-18 or later. <a id="S5-6-2"></a><sup>S5-6-2</sup>
- A failure of a query **MUST** be a tool result with `isError: true` and `{"error": Failure}`, not a JSON-RPC error; a JSON-RPC error is for an unknown tool or method. <a id="S5-6-3"></a><sup>S5-6-3</sup>
- A server **MUST** answer `initialize` with the client's protocol version when it supports it, and otherwise with the latest version it supports. <a id="S5-6-4"></a><sup>S5-6-4</sup>
- A server **MUST NOT** write anything but MCP messages to its standard output. <a id="S5-6-5"></a><sup>S5-6-5</sup>

### 6.1 The workspace daemon

One process per workspace can serve every MCP connection to it from one session whose engines stay warm (`mcppls daemon run`; `mcppls mcp --daemon` relays its standard streams to it, starting it when none runs). The daemon listens on the loopback interface and writes `daemon.json` — `port`, `token`, `version`, `root` — in the workspace's directory of the user cache. A connection's first line is `{"token": string, "session": "mcp" | "control"}`, answered `{"ok": true}` or `{"ok": false, "error": string}`; an MCP connection then carries MCP messages one per line, and a control connection one request per line: `{"method": "status"}` or `{"method": "stop"}`.

- A daemon **MUST** listen on a loopback address only. <a id="S5-6.1-1"></a><sup>S5-6.1-1</sup>
- A daemon **MUST** refuse a connection whose first line does not carry its token. <a id="S5-6.1-2"></a><sup>S5-6.1-2</sup>
- An entry **MUST NOT** use a daemon of another version. <a id="S5-6.1-3"></a><sup>S5-6.1-3</sup>
- A daemon **MUST** exit when no connection has been open for its idle time, removing its `daemon.json` unless another daemon replaced it. <a id="S5-6.1-4"></a><sup>S5-6.1-4</sup>
- Each MCP connection **MUST** keep its own protocol state (the negotiated version and client capabilities), whatever the daemon's other connections negotiated. <a id="S5-6.1-5"></a><sup>S5-6.1-5</sup>

## 7. Command-line binding

| Command | Query |
|---|---|
| `mcppls query symbol <name> [--at FILE:LINE:COLUMN] [--id ID] [--kind K] [--module M]` | 3.1 |
| `mcppls query refs <name> [--no-declaration]` | 3.2 |
| `mcppls query calls <name> [--callees]` | 3.3 |
| `mcppls query outline <file>` | 3.4 |
| `mcppls query module <name> [--file F] [--graph]` | 3.5 |
| `mcppls diagnostics <file>... [--no-fresh]` | 3.6 |
| `mcppls verify [<file>...] [--changed] [--base REV] [--budget N]`, `mcppls verify --snippet FILE:LINE --code TEXT [--replace-lines N]` | 3.7 |
| `mcppls query context <file>` | 4.1 |
| `mcppls daemon run\|start\|status\|stop [--root DIR]` | 6.1 |
| `mcppls impact [<file>...] [--base REV] [--budget N]` | 5.2, its first steps |
| `mcppls review [<file>...] [--base REV] [--budget N] [--format json\|text\|sarif\|markdown] [--output FILE]` | 5.2, 5.3 |

Every command takes `--root`, `--timeout` and `--format json|text`.

- With `--format json` (the default) a command **MUST** print exactly one JSON document: the S5 result, or `{"error": Failure}`. <a id="S5-7-1"></a><sup>S5-7-1</sup>
- A command **MUST** exit with 0 when it has a result, 1 when the query found nothing, diagnostics include an error or a review has a finding of severity error, and 2 when the command itself failed, diagnostics that could not be completed in time included. <a id="S5-7-2"></a><sup>S5-7-2</sup>
