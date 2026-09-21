# S4 — Semantic Kit

| | |
|---|---|
| Specification | S4 |
| Kit version | 1 |
| Status | Draft |
| Schema | [`schema/s4-kit.schema.json`](schema/s4-kit.schema.json) |
| Examples | [`examples/s4-kit-linux-x64.json`](examples/s4-kit-linux-x64.json), [`examples/s4-kit-win32-x64.json`](examples/s4-kit-win32-x64.json), [`examples/s4-kit-darwin-arm64.json`](examples/s4-kit-darwin-arm64.json) |
| License | Apache-2.0 |

## Abstract

A semantic kit is a data-only package that gives a Clang-based semantic engine what it needs to analyze C++ modules code on a machine without a compiler: standard library headers, the sources of the `std` and `std.compat` modules with their module manifest, and C library headers. This specification defines the kit's layout and its manifest, `kit.json`, and how a consumer turns a kit into engine compile commands. The kits distributed with mcppls are published under the package name `mcppls-kit`.

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
| `kit-version` | integer | MUST | Manifest format version. This specification defines `1`. <a id="S4-3-1"></a><sup>S4-3-1</sup> |
| `name` | string | MUST | Kit name, for example `libcxx-23.1.0-x86_64-w64-mingw32`. <a id="S4-3-2"></a><sup>S4-3-2</sup> |
| `target` | string | MUST | Target triple the kit's headers are configured for. <a id="S4-3-3"></a><sup>S4-3-3</sup> |
| `stdlib` | object | MUST | `name` (string, MUST: `libc++`, `libstdc++`, `msvc-stl` or `other`), `version` (string, MUST) and `module-metadata` (kit path, MUST) of a module manifest in P3286 shape. <a id="S4-3-4"></a><a id="S4-3-5"></a><a id="S4-3-6"></a><a id="S4-3-7"></a><sup>S4-3-4, S4-3-5, S4-3-6, S4-3-7</sup> |
| `system-include-directories` | kit path[] | MUST | System header directories, in the order they are passed to the engine. <a id="S4-3-8"></a><sup>S4-3-8</sup> |
| `sysroot` | kit path or `null` | MAY | Sysroot passed to the engine. Absent is the same as `null`. |
| `arguments` | string[] | MAY | Additional engine arguments, for example `-nostdinc++`. |
| `requires` | object[] | MAY | External prerequisites. Each element has `kind` (string, MUST). This version defines the kind `macos-sdk`. <a id="S4-3-9"></a><sup>S4-3-9</sup> |
| `licenses` | kit path[] | MUST | License files of the kit's contents. <a id="S4-3-10"></a><sup>S4-3-10</sup> |

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

### 3.1 Module manifest

The file named by `stdlib.module-metadata` uses the P3286 module metadata format: an object with `version`, `revision` and `modules`, where each module has `logical-name`, `source-path` (relative to the manifest's directory), `is-std-library`, and optionally `local-arguments.system-include-directories` (relative to the manifest's directory). The standard library's own manifest, as installed by libc++ or libstdc++, satisfies this without change.

## 4. Rules

1. A kit **MUST** contain only data files. It **MUST NOT** contain executables, shared libraries or scripts, and a consumer **MUST NOT** execute anything from a kit. <a id="S4-4-1"></a><a id="S4-4-2"></a><a id="S4-4-3"></a><sup>S4-4-1, S4-4-2, S4-4-3</sup>
2. A kit **MUST** keep the module manifest and the module sources at the relative positions the manifest refers to. <a id="S4-4-4"></a><sup>S4-4-4</sup>
3. The `stdlib.version` of a libc++ kit **MUST** equal the version of the semantic engine it is distributed with; for mcppls this is the pinned clangd version. <a id="S4-4-5"></a><sup>S4-4-5</sup>
4. A kit for macOS **MUST** declare `"requires": [{ "kind": "macos-sdk" }]` and **MUST NOT** contain the macOS SDK, whose license does not permit redistribution. Its C library headers come from the SDK installed on the user's machine. <a id="S4-4-6"></a><a id="S4-4-7"></a><sup>S4-4-6, S4-4-7</sup>
5. A kit for Windows provides MinGW-w64 runtime semantics. The MSVC STL depends on the Visual Studio toolset and the Windows SDK, which cannot be redistributed; a consumer that finds Visual Studio installed **SHOULD** use it instead of the kit. <a id="S4-4-8"></a><sup>S4-4-8</sup>
6. Every path in `kit.json` **MUST** be a kit path. <a id="S4-4-9"></a><sup>S4-4-9</sup>

## 5. Consumer procedure

A consumer uses a kit when no suitable build toolchain is available and the user has not disabled kits.

1. Read `kit.json`. Reject the kit if `kit-version` is greater than the highest version the consumer implements, if any path is not a kit path, or if a `requires` element has a `kind` the consumer does not recognize.
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

## 6. First-batch kits

| Platform | Kit name | Target | Contents |
|---|---|---|---|
| linux-x64 | `libcxx-23.1.0-x86_64-unknown-linux-gnu` | `x86_64-unknown-linux-gnu` | libc++ 23.1.0 headers and std module sources; glibc and Linux kernel headers under `sysroot/` |
| win32-x64 | `libcxx-23.1.0-x86_64-w64-mingw32` | `x86_64-w64-mingw32` | libc++ 23.1.0 headers and std module sources and MinGW-w64 UCRT headers, from llvm-mingw built on LLVM 23.1.0 |
| darwin-arm64 | `libcxx-23.1.0-arm64-apple-darwin` | `arm64-apple-darwin` | libc++ 23.1.0 headers and std module sources; requires `macos-sdk` |

Their manifests are [`examples/s4-kit-linux-x64.json`](examples/s4-kit-linux-x64.json), [`examples/s4-kit-win32-x64.json`](examples/s4-kit-win32-x64.json) and [`examples/s4-kit-darwin-arm64.json`](examples/s4-kit-darwin-arm64.json).

## 7. Versioning

`kit-version` is an integer. A new version is issued for any change a consumer of the previous version would misread. Optional fields MAY be added within a version; consumers **MUST** ignore fields they do not know. <a id="S4-7-1"></a><sup>S4-7-1</sup>
