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
    level?: 1 | 2 | 3 | 4;        // S1 conformance level of the project model
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
}

interface CxxModulesIssue {
  code: "unresolved-module" | "ambiguous-module" | "engine-timeout" | "engine-crashed"
      | "toolchain-not-found" | "sdk-missing" | "untrusted-workspace" | "module-build-failed"
      | "model-stale"               // the producer failed to answer again; the last model is kept (S2 5)
      | "std-fallback-kit"          // the engine could not build the toolchain's standard library module; a semantic kit reads the files
      | "file-quarantined"          // the engine stopped answering for some files; they are answered from the module index until they change
      | string;
  message: string;
  command?: Command;               // an optional action that fixes the issue
}
```

States:

| State | Meaning |
|---|---|
| `starting` | Initialization and capability negotiation. |
| `loading` | Detecting the project and loading or inferring its model. |
| `preparing` | The semantic engine is building the modules a file imports. |
| `ready` | All features are available. |
| `degraded` | Some features are reduced, for example an inferred model or an engine timeout. `issues` says why. |
| `error` | Only syntactic features remain. `issues` says why. |

A server **MUST** send the notification whenever any field changes, **SHOULD** coalesce changes that occur within a short interval, and **MUST** send at least one notification after `initialized`. `project.source` names where the model came from: an mcpp project, a CMake project, an S1 database, a `compile_commands.json`, or inference from sources alone. `profile.kind` is `semantic-kit` when the server analyzes the project with an [S4](s4-semantic-kit.md) semantic kit because no suitable compiler was found. <a id="S3-4-1"></a><a id="S3-4-2"></a><a id="S3-4-3"></a><sup>S3-4-1, S3-4-2, S3-4-3</sup>

A server that manages more than one workspace root (multiple `workspaceFolders`, or folders added or removed later through `workspace/didChangeWorkspaceFolders`) **MUST** send one notification per root, each with that root's own `project.root`, rather than one notification describing all of them; a client that presents status per folder tells them apart by it. This is a backward-compatible addition: `project.root` already existed in protocol version 1, and a single-root server's one notification already satisfied "at least one notification" above. A request that names a document (for example `cxxModules/setContext`) is answered by the root that owns it; `cxxModules/graph` and a bare-name `cxxModules/moduleInfo` name no document and so, until a later protocol version adds a way to select one, are answered by the first root. <a id="S3-4-4"></a><sup>S3-4-4</sup>

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
// Params: {}
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

## 6. Module features through standard LSP

| Feature | Standard message | Answered by |
|---|---|---|
| Go to the primary interface unit or partition declaration from a module name | `textDocument/definition` | the server's module index |
| Import completion: module names and partitions of the same module | `textDocument/completion` | the server's module index |
| Module-name hover: providers, role, semantic profile | `textDocument/hover` | the server's module index |
| Module declaration as a top-level outline node | `textDocument/documentSymbol` | merged with the semantic engine's result |
| Search by module name | `workspace/symbol` | merged with the semantic engine's result |
| Unresolved and ambiguous modules, import of another module's partition | `textDocument/publishDiagnostics` | the server's module index, with `source` `"mcppls"` |
| Changes to build descriptions | `workspace/didChangeWatchedFiles`, registered dynamically by the server | the editor watches the files |

Diagnostics produced from the module index use these `code` values: `unresolved-module`, `ambiguous-module` and `partition-outside-module`. A server **SHOULD** name the semantic profile in the `source` of diagnostics it forwards from the semantic engine, for example `"mcppls · gcc 16"`, so that a user can tell which compiler's semantics a diagnostic reflects. <a id="S3-6-1"></a><sup>S3-6-1</sup>

## 7. Versioning

The protocol version is an integer. Version 1 is defined by this document. A later version adds optional fields and new messages only; a change that is not backward compatible requires a new method prefix.
