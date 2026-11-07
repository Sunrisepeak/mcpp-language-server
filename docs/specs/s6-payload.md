# S6 — Payload Manifest

| | |
|---|---|
| Specification | S6 |
| Payload version | 4 and 5 |
| Status | Draft |
| Schema | [`schema/s6-payload.schema.json`](schema/s6-payload.schema.json) |
| Examples | [`examples/s6-payload-v4-linux-x64.json`](examples/s6-payload-v4-linux-x64.json), [`examples/s6-payload-v5.json`](examples/s6-payload-v5.json) |
| License | Apache-2.0 |

## Abstract

A payload is the directory an editor integration starts mcppls from: the server, its semantic engine, a semantic kit and their licenses. Its manifest, `payload.json`, states where each of them is and how the server is started on each system. This specification defines the manifest of payload versions 4 and 5 and the rule by which a client — an editor extension, an IDE backend or a package recipe — turns a payload into the command that starts the server. Version 4 is a payload of one platform (mcppls 0.0.18 and 0.0.19); version 5 is one payload for every platform, whose server and engine are ok images started by each system's runner (mcppls 0.0.20 and later).

## 1. Conventions

- The key words **MUST**, **MUST NOT**, **REQUIRED**, **SHALL**, **SHALL NOT**, **SHOULD**, **SHOULD NOT**, **RECOMMENDED**, **MAY** and **OPTIONAL** are to be interpreted as described in BCP 14 (RFC 2119, RFC 8174) when, and only when, they appear in all capitals.
- JSON is as defined by RFC 8259 and encoded in UTF-8.
- "Payload root" is the directory that contains `payload.json`. A **payload path** is a relative path, resolved against the payload root, that uses `/` as its separator, is not absolute, has no drive or volume prefix and has no `..` component.
- A **platform key** names a system and an architecture as VS Code names extension targets: `linux-x64`, `linux-arm64`, `darwin-arm64`, `darwin-x64`, `win32-x64`, `win32-arm64`.

## 2. Layout

```
<payload root>/
  payload.json             the manifest (section 3)
  bin/mcppls.okb           the server (version 5; version 4 also) or bin/mcppls[.exe] (a program)
  libexec/okrun/<system>-<arch>/okrun[.exe]   each system's runner (version 5)
  clangd/...               the semantic engine
  kit/kit.json             the semantic kit (S4)
  licenses/                license texts
```

## 3. `payload.json`

Every path in `payload.json` **MUST** be a payload path. <a id="S6-3-1"></a><sup>S6-3-1</sup>

### 3.1 Fields of every version

| Field | Type | Requirement | Description |
|---|---|---|---|
| `payload-version` | integer | MUST | `4` or `5`. <a id="S6-3.1-1"></a><sup>S6-3.1-1</sup> |
| `server` | object | MUST | `path`, the payload path of the server, an ok image (`.okb`) or a program, and `version`, the mcppls version; both are required. <a id="S6-3.1-2"></a><sup>S6-3.1-2</sup> |
| `kit` | object | MUST | `name` (string) and `path` (payload path) of the payload's semantic kit, an S4 kit. <a id="S6-3.1-3"></a><sup>S6-3.1-3</sup> |
| `files` | object | MUST | Integrity of files every platform uses: payload path to `{ "size": integer, "sha256": string }`. <a id="S6-3.1-4"></a><sup>S6-3.1-4</sup> |
| `build` | object | MAY | Provenance: `builtAt`, `commit`, `dirty`, `source`. Not read to start the server. |

### 3.2 Version 4: one platform

| Field | Type | Requirement | Description |
|---|---|---|---|
| `platform` | platform key | MUST | The platform the payload serves. <a id="S6-3.2-1"></a><sup>S6-3.2-1</sup> |
| `server.runner` | payload path | MAY | The runner that starts an image server (`bin/okrun`). Absent: `server.path` is a program. |
| `engines.clangd` | object | MUST | `path` (payload path) and `version` of the engine, its `identity` (S4 section 4, S4-4-10), and `runner` when the engine is an ok image. <a id="S6-3.2-2"></a><sup>S6-3.2-2</sup> |
| `clangd` | object | MAY | `path` and `version` of the engine, as versions 1 to 3 wrote them; a reader of version 4 takes `engines.clangd`. |

### 3.3 Version 5: one payload for every platform

| Field | Type | Requirement | Description |
|---|---|---|---|
| `platforms` | object | MUST | Keyed by platform key, one entry per platform the payload serves. <a id="S6-3.3-1"></a><sup>S6-3.3-1</sup> |
| `platforms.<key>.runner` | payload path | MUST when `server.path` is an image | The runner of that platform, which starts the server image. <a id="S6-3.3-2"></a><sup>S6-3.3-2</sup> |
| `platforms.<key>.engines.clangd` | object | MUST | The engine of that platform, as in version 4. One engine image may serve several entries. <a id="S6-3.3-3"></a><sup>S6-3.3-3</sup> |
| `platforms.<key>.files` | object | MUST | Integrity of the files only that platform uses, as `files`. <a id="S6-3.3-4"></a><sup>S6-3.3-4</sup> |

A version 5 manifest has no top-level `platform`, `server.runner` or `engines`: what differs between systems is under the system's entry. <a id="S6-3.3-5"></a><sup>S6-3.3-5</sup>

## 4. Starting the server

A client knows the platform key of the system it runs on, the **host key**. It reads `payload.json` and builds the command from it alone; it **MUST NOT** infer a runner or a server from file names. <a id="S6-4-1"></a><sup>S6-4-1</sup>

1. **Version 5.** The entry is `platforms[<host key>]`. When the entry has a `runner`, the command is `<root>/<runner> <root>/<server.path>`; otherwise it is `<root>/<server.path>`. <a id="S6-4-2"></a><sup>S6-4-2</sup>
2. **Version 4.** The payload serves the host when `platform` is the host key. When `server.runner` is present, the command is `<root>/<server.runner> <root>/<server.path>`; otherwise it is `<root>/<server.path>`. <a id="S6-4-3"></a><sup>S6-4-3</sup>
3. **Another platform.** When the payload does not serve the host — version 5 without an entry for the host key, version 4 naming another `platform` — the client **MUST** refuse to start it, and the refusal **MUST** name the host key and the platforms the payload serves. It **MUST NOT** start another platform's server or engine. <a id="S6-4-4"></a><a id="S6-4-5"></a><a id="S6-4-7"></a><sup>S6-4-4, S6-4-5, S6-4-7</sup>
4. **Another version.** A client **MUST** refuse a `payload-version` it does not implement, naming the version and the ones it reads. <a id="S6-4-6"></a><sup>S6-4-6</sup>

The server's own arguments follow the command (`serve`, which speaks LSP over standard input and output). A client that also starts a server found on `PATH` or installed by a package manager treats it as a program and needs no manifest.

## 5. Starting the engine

The server starts its engine from the same entry: in version 5 `platforms[<host key>].engines.clangd`, in version 4 `engines.clangd`. The engine command is `<root>/<runner> <root>/<path>` when the entry names a `runner`, else `<root>/<path>`. The engine's identity **MUST** be the one its entry states, and the server **MUST** refuse a payload whose identity is missing or malformed (S4-4-10). <a id="S6-5-1"></a><a id="S6-5-2"></a><sup>S6-5-1, S6-5-2</sup>

## 6. Integrity

The files a platform's server and engine read are `files` and that platform's `files`. The server verifies each before it serves and reports a payload whose file differs from its stated size or digest as corrupt. A client **MAY** verify them too; it does not need to in order to start the server. <a id="S6-6-1"></a><sup>S6-6-1</sup>

## 7. Versions

| Version | mcppls | Shape |
|---|---|---|
| 1 to 3 | 0.0.11 to 0.0.17 | one platform, a program server; not specified here |
| 4 | 0.0.18, 0.0.19 | one platform; an image server and its runner under `server`; engine identity |
| 5 | 0.0.20 and later | every platform; each platform's runner and engine under `platforms` |

A client that reads version 5 and version 4 starts every published payload from 0.0.18 on.
