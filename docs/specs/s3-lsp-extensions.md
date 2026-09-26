# S3 — Language Server Protocol Extensions for C++ Modules

| | |
|---|---|
| Specification | S3 |
| Protocol version | 1 |
| Status | Draft |
| Base protocol | Language Server Protocol 3.18 |
| License | Apache-2.0 |

## Abstract

This specification defines the messages a C++ language server and an editor exchange, beyond the Language Server Protocol (LSP), to present C++ named modules: the state of the project model and of the semantic engine, the module graph, the providers of a module, and the choice of a context for a file that belongs to more than one set. Everything that standard LSP can express is expressed with standard LSP; section 6 lists those features.

## 1. Conventions

- The key words **MUST**, **MUST NOT**, **REQUIRED**, **SHALL**, **SHALL NOT**, **SHOULD**, **SHOULD NOT**, **RECOMMENDED**, **MAY** and **OPTIONAL** are to be interpreted as described in BCP 14 (RFC 2119, RFC 8174) when, and only when, they appear in all capitals.
- Structures are written as TypeScript interfaces, as in the LSP specification. `DocumentUri`, `Range`, `Command`, `TextDocumentIdentifier` and `TextDocumentPositionParams` are the LSP 3.18 types of those names.
- "Set", "context" and "role" have the meanings of [S1](s1-build-database.md). A context is the set the server uses for a file; it determines the file's options and the modules visible to it.

## 2. Principles

1. Features that standard LSP can express use standard LSP. Module-name definition, import completion, module-name hover and document outline are standard requests.
2. Custom capabilities are negotiated through `experimental.cxxModules` in `initialize`. A server does not send custom messages to a client that did not declare them.
3. Every custom message uses the `cxxModules/` method prefix.
4. The protocol version only grows. Fields added in later versions are optional.

## 3. Capability negotiation

```ts
// Client → server: InitializeParams.capabilities.experimental
interface ClientCxxModulesCapabilities {
  cxxModules?: {
    version: 1;
    status?: boolean;     // the client presents cxxModules/status
    graph?: boolean;      // the client can present a module graph
    contexts?: boolean;   // the client offers a context selection
  };
}

// Server → client: InitializeResult.capabilities.experimental
interface ServerCxxModulesCapabilities {
  cxxModules?: {
    version: 1;
    databaseSpec: string; // range of S1 profile versions the server reads, e.g. ">=0.2 <1"
  };
}
```

Rules:

- A server **MUST NOT** send `cxxModules/status` unless the client declared `status: true`. <a id="S3-3-1"></a><sup>S3-3-1</sup>
- A client **MUST NOT** send a `cxxModules/` request unless the server declared `cxxModules` in its capabilities. <a id="S3-3-2"></a><sup>S3-3-2</sup>
- `version` is the highest protocol version the sender implements. Both sides use the lower of the two versions.
- `databaseSpec` is a space-separated list of comparisons (`>=`, `>`, `<=`, `<`, `=`) against semantic versions, all of which must hold.

## 4. `cxxModules/status` notification

Direction: server → client.

```ts
interface CxxModulesStatusParams {
  state: "starting" | "loading" | "preparing" | "ready" | "degraded" | "error";
  project: {
    root: DocumentUri;             // the workspace folder's URI exactly as the client sent it
    source: "mcpp" | "cmake" | "build-database" | "compile-commands" | "inferred";
    level?: 1 | 2 | 3 | 4;        // S1 conformance level of the project model's *document*
    tier?: 1 | 2 | 3 | 4;         // which kind of source it came from: 1 build database, 2 CMake's
                                   // own database, 3 compile_commands.json, 4 sources only
  };
  profile: SemanticProfile;        // semantic profile of the default context
  engine: { name: string; version: string };   // the core semantic engine, e.g. "clangd"; "none" when there is none
  engines?: EngineStatus[];        // every engine serving the root (overall design 5)
  progress?: { done: number; total: number };
  issues?: CxxModulesIssue[];      // reasons for degradation; absent or empty when there are none
  notices?: CxxModulesIssue[];     // facts worth showing that reduce no feature, e.g. a producer that writes into the project
}

interface EngineStatus {
  name: string;                    // e.g. "clangd", or "mcppls" for the server's own module engine
  version: string;
  role: "core" | "modules" | string;
  state: "starting" | "ready" | "preparing" | "unavailable" | string;
}

interface SemanticProfile {
  kind: "build-toolchain" | "semantic-kit";
  compiler?: string;               // e.g. "gcc 16.1.0"; absent for a semantic kit
  stdlib: string;                  // e.g. "libstdc++ 16.1.0" or "libc++ 23.1.0"
  target: string;                  // e.g. "x86_64-linux-gnu"
  standard?: string;               // the C++ standard the context's module units are read with, e.g. "c++26"
}

interface CxxModulesIssue {
  code: "unresolved-module" | "ambiguous-module" | "engine-timeout" | "engine-crashed"
      | "toolchain-not-found" | "sdk-missing" | "untrusted-workspace" | "module-build-failed"
      | "model-stale"               // the producer failed to answer again; the last model is kept (S2 5)
      | "std-fallback-kit"          // the engine could not build the toolchain's standard library module; a semantic kit reads the files
      | "file-quarantined"          // the engine stopped answering for some files; they are answered from the module index until they change
      | "engine-incompatible"       // the engine cannot run on this machine at all (its program loader refused it); module-level features remain
      | "modules-doomed"            // modules that cannot be prepared because a module they import does not compile
      | string;
  message: string;
  command?: Command;               // an optional action that fixes the issue
  category?: "code"                // the user's own source is wrong: told as diagnostics where it is
           | "engine"              // a semantic engine lost something (stopped responding, restarted too often)
           | "environment"         // the machine, the payload or the workspace's trust
           | "project"             // the build description (stale, ambiguous, slow)
           | string;
}
```

States:

| State | Meaning |
|---|---|
| `starting` | Initialization and capability negotiation. |
| `loading` | Detecting the project and loading or inferring its model. |
| `preparing` | The semantic engine is building the modules a file imports. |
| `ready` | All features are available. |
| `degraded` | Some features are reduced, for example an inferred model or an engine timeout. `issues` says why. Issues of category `code` alone never make a root `degraded`. |
| `error` | Only syntactic features remain. `issues` says why. |

A server **MUST** send the notification whenever any field changes, **SHOULD** coalesce changes that occur within a short interval, and **MUST** send at least one notification after `initialized`. `project.source` names where the model came from: an mcpp project, a CMake project, an S1 database, a `compile_commands.json`, or inference from sources alone. `profile.kind` is `semantic-kit` when the server analyzes the project with an [S4](s4-semantic-kit.md) semantic kit because no suitable compiler was found. <a id="S3-4-1"></a><a id="S3-4-2"></a><a id="S3-4-3"></a><sup>S3-4-1, S3-4-2, S3-4-3</sup>

`project.tier` and `project.level` answer different questions: `level` is how completely the *document* a source produced is structured (S1's own 1..4, which a hand-written database can reach without a build tool at all), while `tier` is which *kind* of source produced it at all (mcpp or an explicit build database, CMake's own database, a bare `compile_commands.json`, or sources alone), independent of how that source's document happened to be structured. A build database (an explicit one, or mcpp's `emit build-database`) is tier 1, while an mcpp project the server could only describe through the `compile_commands.json` an mcpp too old to emit a build database wrote is tier 3, because a database plus scanning is what it got; a CMake project without `FILE_SET CXX_MODULES` is tier 2 even when its document is level 1, because CMake's own generator wrote it without the server having to scan or probe anything; an untrusted workspace is tier 4 by definition, since it is sources only regardless of what a database elsewhere on disk might say. `tier` is additive and optional: a client from before it existed sees no field and nothing else changes, and a server **MAY** omit it for a root it has not tiered yet, the same as `level`. A client that presents both **SHOULD** label them distinctly (for example "tier 1" and "S1 level 3") rather than show one bare number twice. <a id="S3-4-8"></a><a id="S3-4-9"></a><sup>S3-4-8, S3-4-9</sup>

A server that manages more than one workspace root (multiple `workspaceFolders`, or folders added or removed later through `workspace/didChangeWorkspaceFolders`) **MUST** send one notification per root, each with that root's own `project.root`, rather than one notification describing all of them; a client that presents status per folder tells them apart by it. This is a backward-compatible addition: `project.root` already existed in protocol version 1, and a single-root server's one notification already satisfied "at least one notification" above. A request that names a document (for example `cxxModules/setContext`) is answered by the root that owns it; `cxxModules/graph` and a bare-name `cxxModules/moduleInfo` name no document and so, until a later protocol version adds a way to select one, are answered by the first root. <a id="S3-4-4"></a><sup>S3-4-4</sup>

Each issue **SHOULD** carry a `category` saying whose problem it is. <a id="S3-4-10"></a><sup>S3-4-10</sup> A problem of the user's own source — a syntax error, an import of a module nothing provides, a module that does not compile — is category `code`: a server **MUST NOT** report `degraded` or `error` because of `code` issues alone <a id="S3-4-11"></a><sup>S3-4-11</sup>, and **SHOULD** report such a problem as a diagnostic at its location instead, though it may still list the issue. <a id="S3-4-12"></a><sup>S3-4-12</sup> A client **MUST** treat an issue without `category` as not `code` (servers from before the field existed) <a id="S3-4-13"></a><sup>S3-4-13</sup>, and **SHOULD NOT** present a `code` issue as a loss of features. <a id="S3-4-14"></a><sup>S3-4-14</sup>

A server **SHOULD NOT** send `degraded` for a condition that ends by itself within a few seconds (a file set aside and handed back while the user types): it holds a change to `degraded` until it has lasted a short interval, and sends `error` at once. <a id="S3-4-15"></a><sup>S3-4-15</sup>

A server whose semantic capabilities come from more than one engine **SHOULD** list each in `engines` with its role and state, and **MUST** name the engine that provides the core C++ semantics in `engine`, or `"none"` when the root has none. A client **MUST** accept engine names other than `"clangd"`. <a id="S3-4-5"></a><a id="S3-4-6"></a><a id="S3-4-7"></a><sup>S3-4-5, S3-4-6, S3-4-7</sup>

## 5. Requests

### 5.1 `cxxModules/graph`

Direction: client → server. Returns the module graph of a context.

```ts
interface CxxModulesGraphParams {
  context?: string;                // a context id; absent means the default context
}

interface CxxModulesGraph {
  modules: {
    name: string;                  // "M" or "M:P"
    external: boolean;             // provided through module metadata or the standard library
    units: { uri: DocumentUri; role: ModuleUnitRole }[];
  }[];
  imports: { from: DocumentUri; module: string; range: Range }[];
}

type ModuleUnitRole = "module-interface" | "module-partition-interface"
  | "module-partition-implementation" | "module-implementation" | "non-module" | "unknown";
```

`range` is the range of the module name in the `import` declaration. An unknown `context` is answered with the LSP error `InvalidParams`.

### 5.2 `cxxModules/moduleInfo`

Direction: client → server. Describes one module, named directly or by a position on a module name in a module or import declaration.

```ts
type CxxModulesModuleInfoParams = TextDocumentPositionParams | { name: string; context?: string };

interface CxxModulesModuleInfo {
  name: string;
  providers: { uri: DocumentUri; role: ModuleUnitRole; set: string }[];
  resolvedFrom?: "set" | "visible-set" | "module-metadata" | "stdlib";
  ambiguous: boolean;
}
```

- The result is `null` when a position is given and it is not on a module name.
- `resolvedFrom` is the S1 resolution step that found the providers. It is absent, and `providers` is empty, when the name does not resolve.
- `ambiguous` is `true` when that step found more than one provider.
- `set` is the name of the set that contains the providing unit. For a provider found through module metadata or the standard library it is the name of the importing set.

### 5.3 `cxxModules/contexts`

Direction: client → server. Lists the contexts available for a file.

```ts
interface CxxModulesContextsParams { textDocument: TextDocumentIdentifier }

interface CxxModulesContexts {
  current: string;
  available: { id: string; label: string; profile: SemanticProfile }[];
}
```

`available` contains every set that includes the file. For a file in no set it contains the single context the server created for it, which is also `current`.

### 5.4 `cxxModules/setContext`

Direction: client → server. Selects the context of a file.

```ts
interface CxxModulesSetContextParams { textDocument: TextDocumentIdentifier; context: string }
// Result: null
```

After answering, the server rewrites the engine's input for the new context and sends `cxxModules/status` as the engine prepares. A `context` that is not in the file's `available` list is answered with the LSP error `InvalidParams`.

### 5.5 `cxxModules/report`

Direction: client → server. What a report of a problem needs, gathered by the server for a person or a bug report.

```ts
interface CxxModulesReportParams { redact?: boolean }   // default true
interface CxxModulesReport {
  generatedAt: string;             // UTC, ISO 8601
  server: { name: string; version: string; platform: string; uptimeSeconds: number; logLevel: string; logFile: string };
  client: { name: string; version?: string } | null;   // the client's clientInfo, as it sent it
  roots: object[];                 // one entry per workspace root
  logTail: string[];               // the latest lines of the server's log
}
```

A server **SHOULD** answer at once with what it knows rather than wait for its engines. <a id="S3-5.5-1"></a><sup>S3-5.5-1</sup>

The content of each `roots` entry is the server's own and may change between server versions: a client **MUST NOT** base features on it. <a id="S3-5.5-2"></a><sup>S3-5.5-2</sup>

A report is made to be shared, so unless `redact` is `false` a server **SHOULD** replace in it the user's home directory (by `~`), the user's and the machine's names and anything it recognizes as a secret (by placeholders such as `<user>` and `<redacted>`), in every spelling a path takes in it, and use the same placeholder for the same original throughout. <a id="S3-5.5-3"></a><sup>S3-5.5-3</sup> Paths of the project itself are kept: they are what a report is read for.

## 6. Module features through standard LSP

| Feature | Standard message | Answered by |
|---|---|---|
| Go to the primary interface unit or partition declaration from a module name | `textDocument/definition` | the server's module index |
| Import completion: module names and partitions of the same module | `textDocument/completion` | the server's module index |
| Module-syntax keywords where a declaration can begin (6.2) | `textDocument/completion` | the server, merged with the semantic engine's result |
| Module-name hover: providers, role, semantic profile | `textDocument/hover` | the server's module index |
| Module declaration as a top-level outline node | `textDocument/documentSymbol` | merged with the semantic engine's result |
| Search by module name | `workspace/symbol` | merged with the semantic engine's result |
| Unresolved and ambiguous modules, import of another module's partition | `textDocument/publishDiagnostics` | the server's module index, with `source` `"mcppls"` |
| Changes to build descriptions | `workspace/didChangeWatchedFiles`, registered dynamically by the server | the editor watches the files |

Diagnostics produced from the module index use these `code` values: `unresolved-module`, `ambiguous-module` and `partition-outside-module`. A server **SHOULD** name the semantic profile in the `source` of diagnostics it forwards from the semantic engine, for example `"mcppls · gcc 16"`, so that a user can tell which compiler's semantics a diagnostic reflects. <a id="S3-6-1"></a><sup>S3-6-1</sup>

### 6.1 Module syntax in semantic tokens

A C++ semantic engine may send no tokens for module syntax at all (clangd 23.1 does not), so a server **SHOULD** add them to `textDocument/semanticTokens` results: the `export`, `module` and `import` keywords of module declarations and import declarations as the standard type `keyword`, and module and partition names. Where the core engine's tokens cover a position, the core engine's win; the result stays sorted and non-overlapping. When the core engine gives no result, the module-syntax tokens are the result. <a id="S3-6.1-1"></a><sup>S3-6.1-1</sup>

```ts
// Client → server: InitializeParams.initializationOptions
interface CxxModulesInitializationOptions {
  semanticTokens?: {
    modules?: boolean;     // default true: the server adds module-syntax tokens
    moduleType?: boolean;  // default false: the client knows the token type "module" and the modifier "partition"
  };
}
```

A module name is sent with the token type `module`, and a partition name also with the modifier `partition`, only to a client that declared `moduleType: true`; to any other client a server **MUST** send module and partition names as `namespace`, so that a theme that knows only the standard types still colors them. A server **MUST NOT** add module-syntax tokens for a client that declared `modules: false`. The legend is the server's: it maps the core engine's token types and modifiers into it by name. <a id="S3-6.1-2"></a><a id="S3-6.1-3"></a><sup>S3-6.1-2, S3-6.1-3</sup>

### 6.2 Completion of module syntax

A space typed after `import` is where a person expects the module names. A server **MAY** add `" "` to `completionProvider.triggerCharacters` for it. A server that does **MUST** answer a completion request triggered by a space (`context.triggerKind` 2, `context.triggerCharacter` `" "`) whose line, up to the position, is anything but optional white space, an optional `export` and white space, `import` and exactly one white-space character, at once with an empty result, without giving it to the semantic engine. <a id="S3-6.2-1"></a><sup>S3-6.2-1</sup>

Every space typed anywhere reaches the server as such a request, so a client that is sent the trigger should drop the others itself before they are sent. A server **MUST NOT** add `" "` for a client that declared `completion.triggerOnSpace: false`, and **SHOULD NOT** add it for a client that did not declare `true`, unless the server knows that client drops them. <a id="S3-6.2-2"></a><a id="S3-6.2-3"></a><sup>S3-6.2-2, S3-6.2-3</sup>

```ts
// Client → server: InitializeParams.initializationOptions
interface CxxModulesInitializationOptions {
  completion?: {
    triggerOnSpace?: boolean;   // true: a space after `import` triggers completion; false: never
  };
}
```

A semantic engine may offer some module-syntax keywords, not their combined forms, and nothing while it cannot answer for a file. Where a module declaration or an import declaration can begin, a server **SHOULD** offer the keywords that can appear there — `import`; `export import` in a module interface unit; `module;` before anything else in the file; `export module` and `module` in a file with no module declaration; `module :private;` in a primary module interface unit without one — merged with the semantic engine's result without duplicate labels, and **SHOULD** still offer them when the semantic engine gives no result in time. <a id="S3-6.2-4"></a><a id="S3-6.2-5"></a><sup>S3-6.2-4, S3-6.2-5</sup> No module name is offered after `export module`: the name is being declared, not referred to.

## 7. Versioning

The protocol version is an integer. Version 1 is defined by this document. A later version adds optional fields and new messages only; a change that is not backward compatible requires a new method prefix.
