# S4 — Semantic Kit

| | |
|---|---|
| Specification | S4 |
| Kit version | 2 |
| Status | Draft |
| Schema | [`schema/s4-kit.schema.json`](schema/s4-kit.schema.json) |
| Examples | [`examples/s4-kit-linux-x64.json`](examples/s4-kit-linux-x64.json), [`examples/s4-kit-win32-x64.json`](examples/s4-kit-win32-x64.json), [`examples/s4-kit-darwin-arm64.json`](examples/s4-kit-darwin-arm64.json), [`examples/s4-kit-openkal.json`](examples/s4-kit-openkal.json) |
| License | Apache-2.0 |

## Abstract

A semantic kit is a data-only package that gives a Clang-based semantic engine what it needs to analyze C++ modules code on a machine without a compiler: standard library headers, the sources of the `std` and `std.compat` modules with their module manifest, and C library headers. This specification defines the kit's layout and its manifest, `kit.json`, how a consumer chooses the kit of a project's target among the kits it has, and how it turns a kit into engine compile commands. A kit serves one target, or several with one set of headers (kit version 2). The kits distributed with mcppls are the openkal kit, which every mcppls payload carries, and a kit per target, published as `mcppls-kit-<target>`.

## 1. Conventions

- The key words **MUST**, **MUST NOT**, **REQUIRED**, **SHALL**, **SHALL NOT**, **SHOULD**, **SHOULD NOT**, **RECOMMENDED**, **MAY** and **OPTIONAL** are to be interpreted as described in BCP 14 (RFC 2119, RFC 8174) when, and only when, they appear in all capitals.
- JSON is as defined by RFC 8259 and encoded in UTF-8.
- "Kit root" is the directory that contains `kit.json`. A **kit path** is a relative path, resolved against the kit root, that uses `/` as its separator, is not absolute, has no drive or volume prefix and has no `..` component.

## 2. Layout

```
<kit root>/
  kit.json                 the manifest (section 3)
  licenses/                license texts of every upstream component
  ...                      headers, std module sources and module manifest, located by kit.json
```

## 3. `kit.json`

| Field | Type | Requirement | Description |
|---|---|---|---|
| `kit-version` | integer | MUST | Manifest format version: `1`, or `2` for a kit that uses `targets`. <a id="S4-3-1"></a><sup>S4-3-1</sup> |
| `name` | string | MUST | Kit name, for example `libcxx-23.1.0-x86_64-w64-mingw32`. <a id="S4-3-2"></a><sup>S4-3-2</sup> |
| `target` | string | MUST | Target triple the kit's headers are configured for. <a id="S4-3-3"></a><sup>S4-3-3</sup> |
| `stdlib` | object | MUST | `name` (string, MUST: `libc++`, `libstdc++`, `msvc-stl` or `other`), `version` (string, MUST) and `module-metadata` (kit path, MUST) of a module manifest in P3286 shape. <a id="S4-3-4"></a><a id="S4-3-5"></a><a id="S4-3-6"></a><a id="S4-3-7"></a><sup>S4-3-4, S4-3-5, S4-3-6, S4-3-7</sup> |
| `system-include-directories` | kit path[] | MUST | System header directories, in the order they are passed to the engine. <a id="S4-3-8"></a><sup>S4-3-8</sup> |
| `sysroot` | kit path or `null` | MAY | Sysroot passed to the engine. Absent is the same as `null`. |
| `arguments` | string[] | MAY | Additional engine arguments, for example `-nostdinc++`. |
| `requires` | object[] | MAY | External prerequisites. Each element has `kind` (string, MUST). This version defines the kind `macos-sdk`. <a id="S4-3-9"></a><sup>S4-3-9</sup> |
| `licenses` | kit path[] | MUST | License files of the kit's contents. <a id="S4-3-10"></a><sup>S4-3-10</sup> |
| `targets` | object | MAY (version 2) | The targets of a kit that serves several, keyed by the target as its build tool names it (`x86_64-openkal`). Each value has a `target` (string) and `system-include-directories` (kit path[]), and optionally `sysroot` and `arguments`, which take the place of the top-level fields for that target. <a id="S4-3-11"></a><sup>S4-3-11</sup> |

Example (Windows semantics with libc++ over MinGW-w64):

```json
{
  "kit-version": 1,
  "name": "libcxx-23.1.0-x86_64-w64-mingw32",
  "target": "x86_64-w64-mingw32",
  "stdlib": { "name": "libc++", "version": "23.1.0",
              "module-metadata": "x86_64-w64-mingw32/lib/libc++.modules.json" },
  "system-include-directories": ["generic-w64-mingw32/include/c++/v1", "generic-w64-mingw32/include"],
  "sysroot": null,
  "arguments": ["-nostdinc++", "-nostdlibinc"],
  "licenses": ["licenses/LLVM-LICENSE.TXT", "licenses/mingw-w64-COPYING"]
}
```

A kit of several targets (kit version 2) holds one standard library and gives each target its own include directories: a C library's headers of an architecture are read before its generic ones, so a target's list is complete and in its own order, not added to the top-level one. The top-level `target`, `system-include-directories`, `sysroot` and `arguments` **MUST** equal those of one of its `targets`. <a id="S4-3-12"></a><sup>S4-3-12</sup> The openkal kit ([example](examples/s4-kit-openkal.json)) serves `x86_64-openkal` and `aarch64-openkal`; their engine targets are `x86_64-unknown-none-elf` and `aarch64-unknown-none-elf`.

### 3.1 Module manifest

The file named by `stdlib.module-metadata` uses the P3286 module metadata format: an object with `version`, `revision` and `modules`, where each module has `logical-name`, `source-path` (relative to the manifest's directory), `is-std-library`, and optionally `local-arguments.system-include-directories` (relative to the manifest's directory). The standard library's own manifest, as installed by libc++ or libstdc++, satisfies this without change.

## 4. Rules

1. A kit **MUST** contain only data files. It **MUST NOT** contain executables, shared libraries or scripts, and a consumer **MUST NOT** execute anything from a kit. <a id="S4-4-1"></a><a id="S4-4-2"></a><a id="S4-4-3"></a><sup>S4-4-1, S4-4-2, S4-4-3</sup>
2. A kit **MUST** keep the module manifest and the module sources at the relative positions the manifest refers to. <a id="S4-4-4"></a><sup>S4-4-4</sup>
3. The `stdlib.version` of a libc++ kit **MUST** be the version of the libc++ headers it carries, the `_LIBCPP_VERSION` of their `__config` (`220108` is `22.1.8`). A kit distributed in a payload is used with the payload's engine whatever their versions: the openkal kit carries the libc++ of its platform's C++ run-time (22.1.8 with clangd 23.1.0). A target kit installed on its own is taken at the LLVM base version of the engine, which is its package version: the pinned clangd version without a valid `-mcppls.<number>` suffix, so `23.1.0-mcppls.1` takes `mcppls-kit-<target>@23.1.0`. The complete engine version remains its identity and does not imply any capability. <a id="S4-4-5"></a><sup>S4-4-5</sup>
4. A kit for macOS **MUST** declare `"requires": [{ "kind": "macos-sdk" }]` and **MUST NOT** contain the macOS SDK, whose license does not permit redistribution. Its C library headers come from the SDK installed on the user's machine. <a id="S4-4-6"></a><a id="S4-4-7"></a><sup>S4-4-6, S4-4-7</sup>
5. A kit for Windows provides MinGW-w64 runtime semantics. The MSVC STL depends on the Visual Studio toolset and the Windows SDK, which cannot be redistributed; a consumer that finds Visual Studio installed **SHOULD** use it instead of the kit. <a id="S4-4-8"></a><sup>S4-4-8</sup>
6. Every path in `kit.json` **MUST** be a kit path. <a id="S4-4-9"></a><sup>S4-4-9</sup>

### Identified engine payloads

A payload with `payload-version: 4` **MUST** carry the selected engine's immutable identity under `engines.clangd.identity`, matching the complete engine version, platform and binary SHA. Assembly rejects a maintained engine without this identity, and verification rejects missing, malformed or mismatching values. <a id="S4-4-10"></a><sup>S4-4-10</sup>

The identity follows [s4-engine-identity.schema.json](schema/s4-engine-identity.schema.json):
`engine-version`, `llvm-base-version`, `llvm-commit`, `fork-commit`,
`patch-series-sha256`, `platform`, `sha256`, and `features`. The SHA identifies
final distributed bytes. The complete version is preserved; libc++ compatibility
uses its LLVM base. Features describe separately proved capabilities; a version
suffix grants none. Legacy payloads keep their previous parsing and have no
identified-engine capabilities. An explicit external `--clangd` overrides the
bundled engine and does not inherit its identity or features.

An engine may be the ok image `clangd.okb`, which a platform's runner starts (`okrun clangd.okb <arguments>`). Its
entry names the runner under `engines.clangd.runner`, and its identity's `platform` is `openkal`, whichever platform's
entry selects it; one image serves every entry that names it. An entry without a runner is a program of its platform,
and its identity names that platform. <a id="S4-4-12"></a><sup>S4-4-12</sup>

### Capability selected formatting

Automatic `mcpp` formatting fallback **MUST** require a verified `format-style-mcpp` capability and `mcpp.toml` at the workspace root, preserve explicit user fallback settings and project `.clang-format` precedence, and grant no bundled capability to an external engine. Selecting this fallback writes no project configuration. <a id="S4-4-11"></a><sup>S4-4-11</sup>

## 5. Consumer procedure

A consumer uses a kit when no suitable build toolchain is available and the user has not disabled kits.

### 5.1 The kit of a target

The kit answers the question: what does the code read like for the target it is compiled for. The target of a project is the one its build tool is asked for (an mcpp project's selected target), else the consumer's own system's (`x86_64-linux-gnu`, `aarch64-linux-gnu`, `x86_64-w64-mingw32` for every Windows target, `arm64-apple-darwin`). A consumer **MUST** choose the kit per project target, and choose again when the target changes. <a id="S4-5.1-1"></a><sup>S4-5.1-1</sup> Among the kits it has, in order:

1. a kit the user named explicitly, for every target;
2. the kit of the target: a kit of one target that names it, or a kit with an entry for it in `targets`;
3. the kit of the consumer's own system;
4. the openkal kit's entry for the consumer's architecture.

When the kit chosen stands in for the target's own (step 3 or 4) and a kit is published for the target, the consumer **SHOULD** say which kit is missing, what reading with the stand-in leaves out, and the command that installs it (the S3 issue `kit-missing`). <a id="S4-5.1-2"></a><sup>S4-5.1-2</sup> It **MAY** install the kit when the person agrees, and **MUST NOT** install one without that consent; until the kit is there, it goes on reading with the stand-in. <a id="S4-5.1-3"></a><sup>S4-5.1-3</sup> With the openkal kit, `import std` and the standard library are those of the openkal platform's C++ run-time; the headers and macros of the target's own system (`windows.h`, `_WIN32`, glibc's and the Linux kernel's headers, `__linux__`) are not available.

### 5.2 Engine commands

1. Read `kit.json`. Reject the kit if `kit-version` is greater than the highest version the consumer implements, if any path is not a kit path, or if a `requires` element has a `kind` the consumer does not recognize. For a kit of several targets, take the entry of the chosen target in place of the top-level `target`, `system-include-directories`, `sysroot` and `arguments`.
2. Satisfy each requirement. For `macos-sdk`, locate an installed SDK (for example with `xcrun --show-sdk-path` when the Command Line Tools are present, or at `/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk`) and pass `-isysroot <sdk>`. If a requirement cannot be satisfied, report it (the S3 issue code `sdk-missing` for `macos-sdk`) and do not use the kit.
3. Build the baseline command for every project unit:
   ```
   <driver> --no-default-config --target=<target> -std=<standard> <arguments...>
            -isystem <kit root>/<dir>   (for each system-include-directories entry, in order)
            [--sysroot=<kit root>/<sysroot>]
   ```
   `<driver>` is a path to a file named `clang++` that need not exist: the engine uses it only to select its driver mode and never runs it. `<standard>` is the project's language standard, `c++23` when unknown.
4. Add `-x c++-module` to units whose role provides an importable interface (S1 section 8.2).
5. For each module in the module manifest, add one unit whose source is the manifest entry's `source-path`, with the baseline command, an `-isystem` for each of the entry's `local-arguments.system-include-directories`, and `-Wno-reserved-module-identifier -x c++-module`. Units are added once per distinct baseline command.

## 6. Kits distributed

| Kit | Package | Target | Contents |
|---|---|---|---|
| openkal | in every mcppls payload (`kit/`); `mcppls-kit-openkal`, versioned by its platform | `x86_64-openkal`, `aarch64-openkal` | libc++ headers and the `std` and `std.compat` module sources of openkal-platform's C++ run-time, openkal-musl's headers, and the generated headers of each architecture (`include/<target>/`), with a module manifest written for them |
| linux-x64 | `mcppls-kit-x86_64-linux-gnu` | `x86_64-unknown-linux-gnu` | libc++ 23.1.0 headers and std module sources; glibc and Linux kernel headers under `sysroot/` |
| linux-arm64 | `mcppls-kit-aarch64-linux-gnu` | `aarch64-unknown-linux-gnu` | the same for aarch64 |
| win32-x64 | `mcppls-kit-x86_64-w64-mingw32` | `x86_64-w64-mingw32` | libc++ 23.1.0 headers and std module sources and MinGW-w64 UCRT headers, from llvm-mingw built on LLVM 23.1.0 |
| darwin-arm64 | `mcppls-kit-arm64-apple-darwin` | `arm64-apple-darwin` | libc++ 23.1.0 headers and std module sources; requires `macos-sdk` |

A kit is data, so its package installs on every system: a project on one system may be read for another system's target. The openkal kit is built from what the build tool compiles an ok image with: the system include directories of its std module, shared when every target reads them and under `include/<target>/` otherwise; a run-time's own sources are left out. Its manifest is [`examples/s4-kit-openkal.json`](examples/s4-kit-openkal.json); the target kits' are [`examples/s4-kit-linux-x64.json`](examples/s4-kit-linux-x64.json), [`examples/s4-kit-win32-x64.json`](examples/s4-kit-win32-x64.json) and [`examples/s4-kit-darwin-arm64.json`](examples/s4-kit-darwin-arm64.json).

## 7. Versioning

`kit-version` is an integer. A new version is issued for any change a consumer of the previous version would misread. Optional fields MAY be added within a version; consumers **MUST** ignore fields they do not know. <a id="S4-7-1"></a><sup>S4-7-1</sup> Version 2 adds `targets`: a consumer of version 1 would read such a kit as one target's. A consumer that implements version 2 reads version 1 kits unchanged.
