# S2 — Build Database Discovery Protocol

| | |
|---|---|
| Specification | S2 |
| Version | 0.2.0 |
| Status | Draft |
| Schema | [`schema/s2-discovery.schema.json`](schema/s2-discovery.schema.json) |
| Examples | [`examples/s2-request.json`](examples/s2-request.json), [`examples/s2-messages.jsonl`](examples/s2-messages.jsonl), [`examples/s2-envelope.json`](examples/s2-envelope.json) |
| License | Apache-2.0 |

## Abstract

This specification defines how a consumer — typically a language server — locates an [S1](s1-build-database.md) build database, how it asks a producer to write or refresh one, and how it learns when the database must be read again. The command has two modes. In stream mode it follows the shape of rust-analyzer's project discovery command and Go's `GOPACKAGESDRIVER`: a child process, one JSON request on standard input, and a stream of JSON messages on standard output. In single-document mode it follows a build tool's machine-output envelope, such as mcpp's: one JSON document on standard output that carries the database inline, and a protocol description the consumer reads before running anything.

## 1. Conventions

- The key words **MUST**, **MUST NOT**, **REQUIRED**, **SHALL**, **SHALL NOT**, **SHOULD**, **SHOULD NOT**, **RECOMMENDED**, **MAY** and **OPTIONAL** are to be interpreted as described in BCP 14 (RFC 2119, RFC 8174) when, and only when, they appear in all capitals.
- JSON is as defined by RFC 8259 and encoded in UTF-8. A JSONL stream is a sequence of JSON objects, each serialized on a single line and terminated by a line feed (U+000A); a carriage return before the line feed is permitted and ignored.
- "Consumer" is the tool that starts the command and reads the database. "Producer" is the discovery command.

## 2. Locating a database

A consumer looks for a database in this order and uses the first that applies:

1. A path the user configured explicitly.
2. The path returned by a discovery command (section 3).
3. A `build_database.json` in a build directory the consumer knows for the project's build system, for example a CMake binary directory.

A producer **SHOULD** update a database atomically, by writing a temporary file in the same directory and renaming it over the database. <a id="S2-2-1"></a><sup>S2-2-1</sup>

## 3. Discovery command

### 3.1 Invocation

The command line is configured by the user or is a convention of a producer, for example `mcpp emit build-database --format jsonl`. The consumer:

1. starts the command with the workspace root as its working directory;
2. writes exactly one request object (section 3.2) to its standard input as a single JSON line;
3. closes its standard input;
4. reads messages (section 3.3) from its standard output until the terminal message or the end of the stream.

Standard error is free-form diagnostic text. A consumer **MAY** log it and **MUST NOT** interpret it. <a id="S2-3.1-1"></a><sup>S2-3.1-1</sup>

### 3.2 Request

| Field | Type | Requirement | Description |
|---|---|---|---|
| `workspace` | string | MUST | Absolute path of the workspace root. <a id="S2-3.2-1"></a><sup>S2-3.2-1</sup> |
| `files` | string[] | SHOULD | Absolute paths of the files the consumer currently needs, for example the files open in the editor. A producer MAY use them to prioritize work and MUST NOT limit the database to them. An empty array means no particular file. <a id="S2-3.2-2"></a><a id="S2-3.2-3"></a><sup>S2-3.2-2, S2-3.2-3</sup> |
| `configuration` | string | MAY | The build configuration the consumer wants, for example `debug`. Absent means the producer's default. |
| `profile-version` | string | MUST | The highest S1 profile version the consumer understands, for example `"0.2.0"`. <a id="S2-3.2-4"></a><sup>S2-3.2-4</sup> |
| `network` | boolean | MAY | Whether the consumer allows this run to reach the network. `false` asks the producer to describe the build from what is already on the machine. A producer **MAY** ignore it; a consumer **MUST NOT** rely on it as the only means of keeping a run offline, because a producer that does not know the field will not act on it. Absent means the producer decides. <a id="S2-3.2-5"></a><a id="S2-3.2-6"></a><sup>S2-3.2-5, S2-3.2-6</sup> |

Example:

```json
{ "workspace": "/abs/path/to/workspace",
  "files": ["/abs/path/to/workspace/src/greet/greet.cppm"],
  "configuration": "debug",
  "network": false,
  "profile-version": "0.2.0" }
```

### 3.3 Messages

Every line of standard output is one message object with a `kind` field.

| `kind` | Fields | Meaning |
|---|---|---|
| `progress` | `message` (string, MUST); `done`, `total` (non-negative integers, MAY) | Work is under way. <a id="S2-3.3-1"></a><sup>S2-3.3-1</sup> |
| `finished` | `database` (string, MUST); `watch` (string[], MUST); `profile-version` (string, SHOULD) | The database at `database` is complete. `profile-version` is the S1 version of the written document. <a id="S2-3.3-2"></a><a id="S2-3.3-3"></a><a id="S2-3.3-4"></a><sup>S2-3.3-2, S2-3.3-3, S2-3.3-4</sup> |
| `error` | `message` (string, MUST); `code` (string, MAY) | Discovery failed. <a id="S2-3.3-5"></a><sup>S2-3.3-5</sup> |

The last message **MUST** be a terminal message, `finished` or `error`, and a producer **MUST NOT** write anything after it. `database` **MUST** be an absolute path. Each element of `watch` is an absolute path or a glob pattern relative to `workspace`, using the glob syntax of LSP 3.18 (`*`, `**`, `?`, `{a,b}`, `[...]`). <a id="S2-3.3-6"></a><a id="S2-3.3-7"></a><a id="S2-3.3-8"></a><sup>S2-3.3-6, S2-3.3-7, S2-3.3-8</sup>

Example stream:

```text
{"kind": "progress", "message": "configuring"}
{"kind": "progress", "message": "scanning module sources", "done": 12, "total": 40}
{"kind": "finished", "database": "/abs/path/to/workspace/target/build_database.json", "watch": ["mcpp.toml", "mcpp.lock", "src/**/*.cppm"], "profile-version": "0.2.0"}
```

### 3.4 Single-document mode

A producer that already prints machine-readable envelopes offers discovery as one command whose standard output is one JSON object (mcpp's wire protocol version 1 is an example, `mcpp emit build-database --format json`):

| Field | Type | Requirement | Description |
|---|---|---|---|
| `schemaVersion` | integer | MUST | The envelope version; this section describes version `1`. <a id="S2-3.4-1"></a><sup>S2-3.4-1</sup> |
| `kind` | string | MUST | A name ending in `.build-database`, for example `mcpp.build-database`. <a id="S2-3.4-2"></a><sup>S2-3.4-2</sup> |
| `kindVersion` | integer | MUST | `1`. <a id="S2-3.4-3"></a><sup>S2-3.4-3</sup> |
| `effects` | string[] | MUST | What running the command did, for example `read-project`. <a id="S2-3.4-4"></a><sup>S2-3.4-4</sup> |
| `data` | object | conditional MUST | Present when the command succeeded: `database` (object, MUST), the S1 document; `watch` (string[], MUST), as in section 3.3; `inputs-fingerprint` (string, SHOULD), a digest of the inputs `watch` names. <a id="S2-3.4-5"></a><a id="S2-3.4-6"></a><a id="S2-3.4-7"></a><a id="S2-3.4-8"></a><sup>S2-3.4-5, S2-3.4-6, S2-3.4-7, S2-3.4-8</sup> |
| `diagnostics` | object[] | MUST | Each with `code`, `severity` (`error`, `warning` or `note`) and `message`. <a id="S2-3.4-9"></a><sup>S2-3.4-9</sup> |

The consumer writes nothing to the command's standard input. Before running it, the consumer reads the producer's protocol description, `<producer> --protocol-version`: a JSON object whose `kinds` maps kind names to versions and whose `commands` maps command names to the `effects` they may have. A consumer **MUST** use single-document mode only when `kinds` contains the build-database kind, and **MUST** run the command only when the workspace is trusted and the listed effects are acceptable. A command without `data` has failed; its `diagnostics` say why. <a id="S2-3.4-10"></a><a id="S2-3.4-11"></a><sup>S2-3.4-10, S2-3.4-11</sup>

In this mode the producer does not write a database file. The consumer keeps the document where it keeps its own state.

Example: [`examples/s2-envelope.json`](examples/s2-envelope.json).

## 4. Producer requirements

A producer:

- **MUST NOT** require a full build. It performs only what is needed to know the translation units, their arguments and the module graph: configuration and dependency scanning. <a id="S2-4-1"></a><sup>S2-4-1</sup>
- In stream mode, **MUST** write the database before emitting `finished`, and **SHOULD** write it atomically (section 2). In single-document mode, **MUST NOT** write into the workspace to answer. <a id="S2-4-2"></a><a id="S2-4-3"></a><a id="S2-4-4"></a><sup>S2-4-2, S2-4-3, S2-4-4</sup>
- **MUST** write a database that conforms to S1 at level 1 or higher, with a `profile-version` no higher than the request's when it can write that version. <a id="S2-4-5"></a><sup>S2-4-5</sup>
- **MUST** list in `watch` every input whose change can change the database, such as build description files, lock files and module sources whose declarations determine the graph. <a id="S2-4-6"></a><sup>S2-4-6</sup>
- **SHOULD** emit a `progress` message whenever work takes noticeably long, so that a consumer can show that discovery is alive. <a id="S2-4-7"></a><sup>S2-4-7</sup>
- **MUST** exit with status 0 after `finished` and with a non-zero status after `error`. <a id="S2-4-8"></a><sup>S2-4-8</sup>

## 5. Consumer requirements

A consumer:

- **MUST** watch the `database` file and every `watch` entry, and when any of them changes, run the discovery command again or reload the database. It **SHOULD** debounce bursts of changes. <a id="S2-5-1"></a><a id="S2-5-2"></a><sup>S2-5-1, S2-5-2</sup>
- **MUST** treat the end of the stream without a terminal message, a line that is not a valid message, or a `finished` message whose database cannot be read as an error. <a id="S2-5-3"></a><sup>S2-5-3</sup>
- **MUST** bound the time a discovery command may run, **MUST** terminate the command when the bound expires and treat the expiry as an error. A default of 120 seconds is RECOMMENDED; progress messages **MAY** extend the bound. <a id="S2-5-4"></a><a id="S2-5-5"></a><a id="S2-5-6"></a><sup>S2-5-4, S2-5-5, S2-5-6</sup>
- **MUST** ignore unknown fields in messages and **MUST** treat a message of unknown `kind` that is not the last line as progress without a message. <a id="S2-5-7"></a><a id="S2-5-8"></a><sup>S2-5-7, S2-5-8</sup>
- **SHOULD** keep using the last database that loaded successfully while discovery is running or after it fails, and tell the user that the model may be stale. <a id="S2-5-9"></a><sup>S2-5-9</sup>

## 6. Security considerations

- A discovery command is an arbitrary program chosen by project content or user configuration. A consumer **MUST** run it only in a workspace the user trusts. <a id="S2-6-1"></a><sup>S2-6-1</sup>
- A consumer **MUST NOT** pass secrets in the request. <a id="S2-6-2"></a><sup>S2-6-2</sup>
- A consumer **SHOULD** start the command with the environment of the user's session, and **MUST NOT** add credentials of its own. <a id="S2-6-3"></a><a id="S2-6-5"></a><sup>S2-6-3, S2-6-5</sup>
- Where the consumer's own environment is not the user's --- an editor started from a desktop entry, a Dock icon or a command-line wrapper carries the desktop session's environment, not the login shell's --- the consumer **SHOULD** start the command with the login shell's environment instead. <a id="S2-6-6"></a><sup>S2-6-6</sup>
- Variables set by hand in one terminal belong to no session a consumer can reproduce. A consumer **MUST NOT** treat starting the command with the user's environment as a means of controlling what it does with the network; `network` in the request (section 3.2) and the consumer's own bounds are that means. <a id="S2-6-7"></a><sup>S2-6-7</sup>
- Paths returned in `database` and `watch` may point outside the workspace. A consumer **MUST** only read and watch them, never write to them. <a id="S2-6-4"></a><sup>S2-6-4</sup>
