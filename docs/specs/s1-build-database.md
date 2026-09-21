# S1 — C++ Build Database: IDE Profile

| | |
|---|---|
| Specification | S1 |
| Profile version | 0.2.0 |
| Status | Draft |
| Schema | [`schema/s1-build-database.schema.json`](schema/s1-build-database.schema.json) |
| Examples | [`examples/s1-level3-gcc.json`](examples/s1-level3-gcc.json), [`examples/s1-level2-clang-two-sets.json`](examples/s1-level2-clang-two-sets.json) |
| License | Apache-2.0 |

## Abstract

This specification defines a JSON document that describes the C++ named modules of a project: which translation units exist, which modules they provide and require, which toolchain and standard library the build uses, and which options affect their meaning. A language server, indexer or refactoring tool can use the document to provide module-aware semantic features without running, or even having, the compiler the build uses.

The document is a profile of the build database format proposed in WG21 P2977R2. A conforming document is also a valid P2977R2 document, and a P2977R2 document produced by a build system (for example CMake's experimental build database output) is a valid level 1 input. P2977 is an SG15 proposal; it is not part of any standard and has no active standardization vehicle. This specification therefore defines every field it uses, including the fields it shares with P2977R2, and does not depend on P2977's progress. Should the two ever conflict, a MAJOR version of this profile resolves the conflict.

Design principles:

1. **Describe sources, the module graph and semantic options; never the contents of a BMI.** BMI paths are recorded as build facts only. Consumers do not use them for semantic analysis.
2. **Reuse rather than replace.** Field names and formats from existing industry work are kept wherever they fit; this profile adds only what an IDE needs and they lack.
3. **Make implicit inputs explicit.** Toolchain identity, target, sysroot, the standard library and its module manifest are stated, not inferred.
4. **Allow incremental adoption.** Conformance levels let a producer grow from "module graph only" to "live updates".

## 1. Scope

### 1.1 In scope

- Module units: primary module interface units, module partition interface units, module partition implementation units and module implementation units.
- Non-module translation units, including units that import modules.
- Standard library modules (`std`, `std.compat`) and prebuilt library modules, described through module metadata files.
- Toolchain identity and standard library selection.
- Structured semantic options.
- Visibility between sets.
- Export to `compile_commands.json`.

Locating a database and being notified of its changes are specified in [S2](s2-discovery.md).

### 1.2 Out of scope

- Header units. The role value `header-unit` is reserved; its semantics are left to a later version.
- Clang header modules (module maps).
- BMI formats.
- Link information and package description, which belong to CPS.

## 2. Conventions

- The key words **MUST**, **MUST NOT**, **REQUIRED**, **SHALL**, **SHALL NOT**, **SHOULD**, **SHOULD NOT**, **RECOMMENDED**, **MAY** and **OPTIONAL** are to be interpreted as described in BCP 14 (RFC 2119, RFC 8174) when, and only when, they appear in all capitals.
- Documents are JSON as defined by RFC 8259, encoded in UTF-8.
- Module names are logical names as written in source: `M` for a module and `M:P` for partition `P` of module `M`.
- Relative paths:
  - inside a translation unit object, relative paths are resolved against that unit's `work-directory` (as in P2977R2);
  - inside the document-level and set-level `ide` objects, relative paths are resolved against the directory containing the database file.
- Every field this profile adds lives in an object named `ide`, so fields added to P2977 in the future cannot collide with it.
- "Producer" is a tool that writes a database; "consumer" is a tool that reads one.

## 3. Relationship to other specifications

| Specification | Relationship |
|---|---|
| WG21 P2977R2, build database files | **Compatibility target.** A conforming document is a valid P2977R2 document. The P2977R2 fields are redefined here in full; readers do not need P2977. |
| CMake build database output (3.31+, experimental) | Accepted as a level 1 input. Consumers complete missing information as described in 11.2. |
| JSON Compilation Database (`compile_commands.json`) | Downstream format. Tools SHOULD be able to export a conforming document to it (section 12). <a id="S1-3-1"></a><sup>S1-3-1</sup> |
| WG21 P1689R5, dependency information for modules | Producers MAY fill `provides` and `requires` from P1689 scan results. Consumers MAY scan sources themselves when the conformance level is insufficient. |
| WG21 P3286 / EcoStd RFC #3, module metadata | Files referenced by `module-metadata` use this format. Standard library modules MUST be discoverable through it. <a id="S1-3-2"></a><sup>S1-3-2</sup> |
| CPS 0.15 | The file a CPS package names in `cpp_module_metadata` MAY be listed directly in `module-metadata`. |
| WG21 P2717 / EcoStd RFC #2, tool introspection | Toolchain objects SHOULD be filled from tool introspection where the tool supports it. <a id="S1-3-3"></a><sup>S1-3-3</sup> |
| WG21 P3335, structured core options | The SemanticOptions vocabulary is intended to converge with it. |
| LSP 3.18 | Unchanged. Editor-side extensions are specified in [S3](s3-lsp-extensions.md). |

## 4. Data model

```
Database                                     (P2977R2)
 ├─ version, revision                        (P2977R2)
 ├─ ide                                      (this profile)
 │   ├─ profile-version
 │   ├─ generator
 │   ├─ toolchains { <id>: Toolchain }
 │   └─ extensions
 └─ sets[]                                   (P2977R2)
     ├─ name, family-name, visible-sets[], baseline-arguments[]   (P2977R2)
     ├─ ide
     │   ├─ toolchain        -> Toolchain id
     │   ├─ configuration, kind
     │   ├─ options          -> SemanticOptions
     │   ├─ module-metadata[]
     │   └─ extensions
     └─ translation-units[]                  (P2977R2)
         ├─ source, work-directory, arguments[], local-arguments[],
         │  object, private, provides{}, requires[]               (P2977R2)
         └─ ide
             ├─ role
             ├─ options      -> SemanticOptions (delta relative to the set)
             └─ extensions
```

## 5. Database object

| Field | Type | Requirement | Description |
|---|---|---|---|
| `version` | integer | MUST | Format version. This profile uses `1`. <a id="S1-5-1"></a><sup>S1-5-1</sup> |
| `revision` | integer | MUST | Format revision. This profile uses `0`. <a id="S1-5-2"></a><sup>S1-5-2</sup> |
| `sets` | Set[] | MUST | The sets of translation units (section 7). Set names MUST be unique within a document. <a id="S1-5-3"></a><a id="S1-5-4"></a><sup>S1-5-3, S1-5-4</sup> |
| `ide` | object | level 2 MUST | Document-level profile data (section 5.1). <a id="S1-5-5"></a><sup>S1-5-5</sup> |

### 5.1 Document-level `ide` object

| Field | Type | Requirement | Description |
|---|---|---|---|
| `profile-version` | string | MUST | The version of this profile the document conforms to, as a semantic version, for example `"0.2.0"`. <a id="S1-5.1-1"></a><sup>S1-5.1-1</sup> |
| `generator` | object | SHOULD | The producer: `name` (string, MUST) and `version` (string, SHOULD). <a id="S1-5.1-2"></a><a id="S1-5.1-3"></a><a id="S1-5.1-4"></a><sup>S1-5.1-2, S1-5.1-3, S1-5.1-4</sup> |
| `toolchains` | object | MUST | Map from toolchain id to Toolchain object (section 6). Ids are opaque strings. <a id="S1-5.1-5"></a><sup>S1-5.1-5</sup> |
| `extensions` | object | MAY | Vendor extensions (section 13). |

## 6. Toolchain object

A Toolchain describes the compiler the **build** uses. Its `family` selects the dialect in which `arguments` are written, and it tells a consumer which standard library and target to emulate.

| Field | Type | Requirement | Description |
|---|---|---|---|
| `family` | enum | MUST | `gcc`, `clang`, `msvc`, `clang-cl` or `other`. Selects the argument dialect. <a id="S1-6-1"></a><sup>S1-6-1</sup> |
| `version` | string | MUST | The version the compiler reports. <a id="S1-6-2"></a><sup>S1-6-2</sup> |
| `build-id` | string | MAY | The compiler's build revision, for example a clang commit hash. BMI compatibility is decided by this value; equal versions do not imply compatible BMIs. |
| `driver` | string | MUST | Absolute path of the driver the build invokes. <a id="S1-6-3"></a><sup>S1-6-3</sup> |
| `target` | string | MUST | Target triple, from `-dumpmachine`, `-print-target-triple` or an equivalent query. <a id="S1-6-4"></a><sup>S1-6-4</sup> |
| `sysroot` | string | MAY | The sysroot the build uses. |
| `stdlib` | object | conditional MUST | REQUIRED when any unit using this toolchain requires `std` or `std.compat`, unless those modules are provided by translation units the requiring units can see (section 10, steps 1 and 2): a dependency package may ship the standard library's module sources itself (section 6.1). <a id="S1-6-5"></a><a id="S1-6-6"></a><sup>S1-6-5, S1-6-6</sup> |
| `config-files` | string[] | SHOULD | Implicit configuration inputs known to the producer, such as a `.cfg` file next to a clang driver or a GCC specs file. An empty array states that there are none. <a id="S1-6-7"></a><sup>S1-6-7</sup> |
| `introspection` | object[] | MAY | The queries run to obtain the fields above, for reproduction. Each element has `command` (string[], MUST) and `output` (string, MAY). <a id="S1-6-8"></a><sup>S1-6-8</sup> |

### 6.1 `stdlib` object

| Field | Type | Requirement | Description |
|---|---|---|---|
| `name` | enum | MUST | `libstdc++`, `libc++`, `msvc-stl` or `other`. <a id="S1-6.1-1"></a><sup>S1-6.1-1</sup> |
| `version` | string | SHOULD | Standard library version. <a id="S1-6.1-2"></a><sup>S1-6.1-2</sup> |
| `module-metadata` | string | MUST | Path of the standard library module manifest, for example `libstdc++.modules.json`. The manifest has either the P3286 shape (a `modules` array of objects with `logical-name` and `source-path`) or the shape the MSVC STL ships in `<toolset>/modules/modules.json`: `{"library": "microsoft/STL", "module-sources": ["std.ixx", "std.compat.ixx"]}`, in which each source provides the module its file name spells without the extension. <a id="S1-6.1-3"></a><sup>S1-6.1-3</sup> |

Rationale: some clang distributions place a default configuration file next to the driver that silently switches the standard library from libstdc++ to libc++. A consumer that does not know which standard library the build actually uses produces wrong semantics without reporting any error. The standard library is therefore stated explicitly.

A standard library whose module sources come from a dependency package rather than from the toolchain (for example a runtime package's own `std.cppm`) has no manifest to name. A producer then lists those sources as translation units with `provides` `std` and `std.compat` in a set that every requiring set can see, and omits `stdlib` or its `module-metadata`. When units provide a standard library module that a manifest also lists, a consumer **MUST** use the units. <a id="S1-6.1-4"></a><sup>S1-6.1-4</sup>

## 7. Set object

A set is, approximately, all translation units of one build target in one configuration.

| Field | Type | Requirement | Description |
|---|---|---|---|
| `name` | string | MUST | Unique name of the set within the document. <a id="S1-7-1"></a><sup>S1-7-1</sup> |
| `family-name` | string | SHOULD | Name shared by sets that are variants of the same target, for example the same target in two configurations. <a id="S1-7-2"></a><sup>S1-7-2</sup> |
| `visible-sets` | string[] | MUST | Names of the sets whose non-private modules units of this set may import. MUST list the complete visibility closure (section 10). <a id="S1-7-3"></a><a id="S1-7-4"></a><sup>S1-7-3, S1-7-4</sup> |
| `baseline-arguments` | string[] | SHOULD | Arguments common to every unit of the set, in the dialect of the set's toolchain. <a id="S1-7-5"></a><sup>S1-7-5</sup> |
| `translation-units` | TranslationUnit[] | MUST | The units of the set (section 8). <a id="S1-7-6"></a><sup>S1-7-6</sup> |
| `ide` | object | level 2 MUST | Set-level profile data (section 7.1). <a id="S1-7-7"></a><sup>S1-7-7</sup> |

How units are grouped into sets follows how the build resolves imports. A build that resolves every import against one flat module graph, in which any module of the project, of its dependencies and of the standard library can be imported from any unit, may group units per package rather than per target: for example one set for a package, one for its tests and one for the standard library modules the build compiles (mcpp writes `<package>`, `<package>:test` and `mcpp:std`). The `visible-sets` of each such set then lists every other set. That is the complete visibility closure of such a build (section 10); a narrower list would describe a rule the build does not apply.

### 7.1 Set-level `ide` object

| Field | Type | Requirement | Description |
|---|---|---|---|
| `toolchain` | string | MUST | Id of an entry in the document-level `toolchains` map. <a id="S1-7.1-1"></a><sup>S1-7.1-1</sup> |
| `configuration` | string | SHOULD | Configuration name, for example `debug`, `release` or `dev`. <a id="S1-7.1-2"></a><sup>S1-7.1-2</sup> |
| `kind` | enum | SHOULD | `library`, `executable`, `test` or `other`. Consumers use it to choose a default context. <a id="S1-7.1-3"></a><sup>S1-7.1-3</sup> |
| `options` | SemanticOptions | level 3 MUST | Structured form of `baseline-arguments` (section 9). <a id="S1-7.1-4"></a><sup>S1-7.1-4</sup> |
| `module-metadata` | string[] | MAY | Module metadata files of external modules visible to this set, excluding the toolchain's standard library. |
| `extensions` | object | MAY | Vendor extensions. |

## 8. Translation unit object

| Field | Type | Requirement | Description |
|---|---|---|---|
| `source` | string | MUST | Path of the source file. <a id="S1-8-1"></a><sup>S1-8-1</sup> |
| `work-directory` | string | MUST | Absolute path of the directory the compiler runs in. <a id="S1-8-2"></a><sup>S1-8-2</sup> |
| `arguments` | string[] | MUST | The complete command line, driver first, in the dialect of the set's toolchain. <a id="S1-8-3"></a><sup>S1-8-3</sup> |
| `local-arguments` | string[] | SHOULD | Arguments of this unit that are not in the set's `baseline-arguments`. <a id="S1-8-4"></a><sup>S1-8-4</sup> |
| `object` | string | MAY | Path of the object file the build writes. |
| `private` | boolean | SHOULD | `true` when the modules this unit provides are not visible to other sets. Absent means `false`. <a id="S1-8-5"></a><sup>S1-8-5</sup> |
| `provides` | object | level 1 MUST for module units | Map from each module name the unit provides to the path of the BMI the build writes for it. A producer that performs no build MAY use an empty string. <a id="S1-8-6"></a><sup>S1-8-6</sup> |
| `requires` | string[] | level 1 MUST for units that import | Module names the unit imports, with partitions written in full (`M:P`). <a id="S1-8-7"></a><sup>S1-8-7</sup> |
| `ide` | object | level 2 MUST | Unit-level profile data (section 8.1). <a id="S1-8-8"></a><sup>S1-8-8</sup> |

### 8.1 Unit-level `ide` object

| Field | Type | Requirement | Description |
|---|---|---|---|
| `role` | enum | MUST | The unit's role (section 8.2). <a id="S1-8.1-1"></a><sup>S1-8.1-1</sup> |
| `options` | SemanticOptions | MAY | Delta relative to the set's `options`, corresponding to `local-arguments`. |
| `extensions` | object | MAY | Vendor extensions. |

### 8.2 Roles

| Value | Source form | Provides an importable interface |
|---|---|---|
| `module-interface` | `export module M;` | yes |
| `module-partition-interface` | `export module M:P;` | yes |
| `module-partition-implementation` | `module M:P;` | yes, importable only by units of module `M` |
| `module-implementation` | `module M;` | no |
| `non-module` | no module declaration; MAY contain `import` declarations | no |
| `unknown` | the producer cannot decide, for example because the module declaration is inside a conditional inclusion block | decided by the consumer |
| `header-unit` | reserved | reserved |

A producer **MUST** determine `role` from the source content, not from the file extension. A producer that cannot decide **MUST** write `unknown` rather than guess. A consumer that reads `unknown` **SHOULD** scan the source itself and report that it is working in a degraded mode (section 11.2). <a id="S1-8.2-1"></a><a id="S1-8.2-2"></a><a id="S1-8.2-3"></a><sup>S1-8.2-1, S1-8.2-2, S1-8.2-3</sup>

## 9. SemanticOptions object

SemanticOptions describe, independently of any compiler dialect, the options that affect parsing, semantics and BMI compatibility. Options that do not affect meaning — optimization level, debug information, output paths, dependency file generation and most warnings — **SHOULD NOT** appear. <a id="S1-9-1"></a><sup>S1-9-1</sup>

| Field | Type | Description | GCC / Clang source | MSVC source |
|---|---|---|---|---|
| `language-standard` | string | `c++20`, `c++23`, `c++26` | `-std=` | `/std:` |
| `language-extensions` | enum | `none`, `gnu`, `ms` | `-std=gnu++NN` | `/permissive` and related |
| `macros` | object[] | Ordered sequence of `{"define": "N", "value": "V"}` and `{"undefine": "N"}`. A `value` of `null` defines the macro without a value. | `-D`, `-U` | `/D`, `/U` |
| `include-directories` | object | Four ordered arrays: `user`, `quote`, `system`, `after` | `-I`, `-iquote`, `-isystem`, `-idirafter` | `/I`, `/external:I` |
| `forced-includes` | string[] | Headers included before the source | `-include` | `/FI` |
| `exceptions` | boolean | Whether exceptions are enabled | `-fno-exceptions` | `/EH` |
| `rtti` | boolean | Whether RTTI is enabled | `-fno-rtti` | `/GR-` |
| `raw-semantic-arguments` | object | Map from toolchain family to an array of raw arguments that affect meaning but have no structured form, for example `-fchar8_t` or `/Zc:` options | — | — |

Rules:

1. When `options` is present a consumer **MUST** use it. When it is absent a consumer **MUST** parse `arguments` according to `toolchain.family`. <a id="S1-9-2"></a><a id="S1-9-3"></a><sup>S1-9-2, S1-9-3</sup>
2. The effective options of a unit are the set's `options` merged with the unit's `options`: objects merge key by key recursively; arrays concatenate with the set's elements first; scalars take the unit's value.
3. A producer **MUST NOT** place BMI location arguments (a module mapper, `-fmodule-file=`, `/reference` and similar) in `options`. <a id="S1-9-4"></a><sup>S1-9-4</sup>

Structuring can be left to the reader. A consumer, or a library it uses, that parses `arguments` by `toolchain.family` as rule 1 requires can complete a level 2 document to level 3: a set's `options` from its `baseline-arguments`, or, without those, from the arguments all of its units share; a unit's delta from its `local-arguments`, or from what its arguments add to the set's. Options derived this way restate `arguments` and add nothing to them, so a consumer that derives them may keep compiling `arguments`; rule 1 concerns the options a producer states. A producer may therefore write level 2 and rely on such a library, as mcpp does.

## 10. Module name resolution

For a translation unit `T` in set `S`, each module name `N` in `T.requires` resolves by the first step that finds a provider:

1. Translation units in `S` whose `provides` contains `N`.
2. Translation units in the sets listed in `S.visible-sets` whose `private` is `false` and whose `provides` contains `N`.
3. Modules whose `logical-name` is `N` in the files listed in `S.ide.module-metadata`.
4. Modules whose `logical-name` is `N` in the `stdlib.module-metadata` of the toolchain named by `S.ide.toolchain`.

Constraints:

- A producer **MUST** list the complete visibility closure in `visible-sets`. A consumer **MUST NOT** derive visibility transitively. <a id="S1-10-1"></a><a id="S1-10-2"></a><sup>S1-10-1, S1-10-2</sup>
- When one step finds more than one provider, a consumer **MUST** report an ambiguity diagnostic and **MAY** continue with the first provider in document order. <a id="S1-10-3"></a><sup>S1-10-3</sup>
- When no step finds a provider, a consumer **MUST** report an unresolved-module diagnostic at the corresponding `import` declaration. <a id="S1-10-4"></a><sup>S1-10-4</sup>
- A partition `M:P` is importable only by units of module `M`. A consumer **SHOULD** diagnose an import of another module's partition. <a id="S1-10-5"></a><sup>S1-10-5</sup>
- A module unit found through module metadata is compiled with the effective options of the importing set plus the `local-arguments` of its metadata entry.

## 11. Conformance

### 11.1 Producer levels

| Level | Name | Requirements |
|---|---|---|
| 1 | Graph | A valid P2977R2 document in which every unit that provides or requires a named module carries `provides` and `requires`. |
| 2 | IDE | Level 1, plus `ide.profile-version`, `ide.toolchains`, `ide.toolchain` on every set and `ide.role` on every unit (`unknown` is allowed). Standard library modules resolve by section 10. **Every translation unit of the project** is in the document, so that it can replace `compile_commands.json`. |
| 3 | Structured | Level 2, plus `ide.options` on every set, and an `ide.options` delta on every unit whose local arguments differ from the set's. |
| 4 | Live | Level 3, plus a discovery command ([S2](s2-discovery.md)), and an atomic rewrite of the database whenever the build description changes. |

A level describes what a document contains, not who wrote each part: a level 2 document that a consumer completes as section 9 describes is a level 3 project model for that consumer.

### 11.2 Consumers

A consumer:

- **MUST** ignore unknown fields and unknown extensions; <a id="S1-11.2-1"></a><sup>S1-11.2-1</sup>
- **MUST NOT** require that the BMI paths in `provides` exist; <a id="S1-11.2-2"></a><sup>S1-11.2-2</sup>
- **MUST NOT** use build BMIs for semantic analysis unless all of the following hold: the consumer's semantic engine matches the toolchain's `family`, `version` and `build-id` exactly; the user has explicitly enabled an authoritative mode; and the consumer can detect that a BMI is stale relative to its sources; <a id="S1-11.2-3"></a><sup>S1-11.2-3</sup>
- **MUST** interpret `arguments` in the dialect of `toolchain.family`; <a id="S1-11.2-4"></a><sup>S1-11.2-4</sup>
- **MUST** resolve modules and report unresolved and ambiguous names as specified in section 10; <a id="S1-11.2-5"></a><sup>S1-11.2-5</sup>
- **SHOULD** accept level 1 documents, inferring `role` by lexical scanning and completing toolchain information by querying the driver, and **SHOULD** tell the user that it is working in a degraded mode. <a id="S1-11.2-6"></a><a id="S1-11.2-7"></a><sup>S1-11.2-6, S1-11.2-7</sup>

## 12. Export to `compile_commands.json`

So that tools that do not implement this profile keep working, producers and conversion tools **SHOULD** offer an export: <a id="S1-12-1"></a><sup>S1-12-1</sup>

- Each translation unit becomes one entry: `directory` is `work-directory`, `file` is `source`, `arguments` is `arguments` and `output` is `object`.
- When the same source file appears in more than one set, the exporter **MUST** either let the caller choose the set or export only the entries of the default context. <a id="S1-12-2"></a><sup>S1-12-2</sup>
- The export loses the module graph, toolchain and role information. The exporter **SHOULD** say so in its documentation or log. <a id="S1-12-3"></a><sup>S1-12-3</sup>

## 13. Versioning and extensions

- `version` and `revision` keep their P2977R2 meaning.
- `ide.profile-version` is a semantic version: MAJOR for incompatible changes, MINOR for added fields, PATCH for clarifications.
- Vendor extensions live in the `extensions` object of the document, set and unit `ide` objects. Keys are reverse domain names or registered short names, for example `"io.github.mcpp-community"`.
- If this profile enters a standardization process, adopted `ide` fields are expected to move into P2977 itself or its EcoStd successor, and this profile publishes a MAJOR version that aligns with it.

## 14. Security considerations

- Toolchain queries and discovery commands execute external programs. A consumer **MUST** run them only in a trusted workspace and **SHOULD** restrict driver queries to an allow-list, in the manner of clangd's `--query-driver`. <a id="S1-14-1"></a><a id="S1-14-2"></a><sup>S1-14-1, S1-14-2</sup>
- Standard library module sources usually lie outside the workspace. A consumer **MUST** access them read-only. <a id="S1-14-3"></a><sup>S1-14-3</sup>
- Paths in a database may point anywhere. A consumer **MUST NOT** write to a location because a database names it. <a id="S1-14-4"></a><sup>S1-14-4</sup>

## 15. Complete example

The example project below is built with GCC 16. It has module `hello.greet` with partition `:detail` and the entry point `main.cpp`. The document conforms to level 3, as a producer that states `ide.options` writes it; mcpp writes level 2 for the same project, with a set per package (section 7), as the S2 single-document example shows. Paths are shortened.

```json
{
  "version": 1,
  "revision": 0,
  "ide": {
    "profile-version": "0.2.0",
    "generator": { "name": "example-producer", "version": "1.0.0" },
    "toolchains": {
      "gcc-16.1.0-x86_64-linux-gnu": {
        "family": "gcc",
        "version": "16.1.0",
        "driver": "/opt/xpkgs/gcc/16.1.0/bin/g++",
        "target": "x86_64-linux-gnu",
        "sysroot": "/opt/subos/default",
        "stdlib": {
          "name": "libstdc++",
          "version": "16.1.0",
          "module-metadata": "/opt/xpkgs/gcc/16.1.0/lib64/libstdc++.modules.json"
        },
        "config-files": []
      }
    }
  },
  "sets": [
    {
      "name": "hello@dev",
      "family-name": "hello",
      "visible-sets": [],
      "baseline-arguments": ["-std=c++23", "-fmodules", "-O0", "-g"],
      "ide": {
        "toolchain": "gcc-16.1.0-x86_64-linux-gnu",
        "configuration": "dev",
        "kind": "executable",
        "options": {
          "language-standard": "c++23",
          "language-extensions": "none",
          "macros": [],
          "include-directories": { "user": [], "quote": [], "system": [], "after": [] },
          "forced-includes": [],
          "exceptions": true,
          "rtti": true
        }
      },
      "translation-units": [
        {
          "source": "src/greet/detail.cppm",
          "work-directory": "/home/u/hello",
          "arguments": ["/opt/xpkgs/gcc/16.1.0/bin/g++", "-std=c++23", "-fmodules", "-O0", "-g",
                        "-c", "src/greet/detail.cppm", "-o", "target/obj/detail.m.o"],
          "local-arguments": [],
          "object": "target/obj/detail.m.o",
          "private": false,
          "provides": { "hello.greet:detail": "target/gcm.cache/hello.greet-detail.gcm" },
          "requires": ["std"],
          "ide": { "role": "module-partition-interface" }
        },
        {
          "source": "src/greet/greet.cppm",
          "work-directory": "/home/u/hello",
          "arguments": ["/opt/xpkgs/gcc/16.1.0/bin/g++", "-std=c++23", "-fmodules", "-O0", "-g",
                        "-c", "src/greet/greet.cppm", "-o", "target/obj/greet.m.o"],
          "local-arguments": [],
          "object": "target/obj/greet.m.o",
          "private": false,
          "provides": { "hello.greet": "target/gcm.cache/hello.greet.gcm" },
          "requires": ["hello.greet:detail", "std"],
          "ide": { "role": "module-interface" }
        },
        {
          "source": "src/main.cpp",
          "work-directory": "/home/u/hello",
          "arguments": ["/opt/xpkgs/gcc/16.1.0/bin/g++", "-std=c++23", "-fmodules", "-O0", "-g",
                        "-c", "src/main.cpp", "-o", "target/obj/main.o"],
          "local-arguments": [],
          "object": "target/obj/main.o",
          "private": true,
          "provides": {},
          "requires": ["hello.greet", "std"],
          "ide": { "role": "non-module" }
        }
      ]
    }
  ]
}
```

By section 10, `main.cpp`'s requirement on `std` has no provider in its set and no visible set, so resolution reaches the toolchain's `stdlib.module-metadata` and finds `bits/std.cc` in `libstdc++.modules.json`.

A second example with two sets, `visible-sets` and a clang toolchain using libc++ is [`examples/s1-level2-clang-two-sets.json`](examples/s1-level2-clang-two-sets.json).

## 16. Open questions

1. Submission to EcoStd: one RFC that ports P2977 together with the `ide` fields, with the P2977 authors, or two RFCs ("build database" and "IDE fields").
2. Whether `options` adopts P3335's field names now or keeps this vocabulary until P3335 settles.
3. How header units are modelled: a dedicated `role`, or a P1689-style `lookup-method` on `requires`.
4. How a default context is chosen when one database holds several configurations, as multi-config generators such as Ninja Multi-Config produce.
5. Whether libraries that are part of the project should also be exposed through P3286 metadata, or only through `visible-sets`.

## Appendix A. Conformance suite outline (informative)

The specification ships with a public conformance suite. Each case has a source project, the expected database per toolchain (GCC 16, Clang 22/23, MSVC), the expected resolution results (providers, ambiguity, unresolved names) and the expected LSP results (diagnostics, cross-module definition, hover, completion, references, module-name definition, propagation of unsaved edits).

| Case | Covers |
|---|---|
| F1 | One module and `import std` |
| F2 | Partition interface, partition implementation and `export import :part` |
| F3 | Module implementation unit `module M;` |
| F4 | Two sets, import through `visible-sets` |
| F5 | Two private sets that each provide a module of the same name (variants) |
| F6 | External prebuilt library imported through P3286 metadata |
| F7 | `import` inside conditional inclusion, testing scanner precision and degradation |
| F8 | Code only GCC accepts, testing diagnostic source attribution |
| F9 | Implicit toolchain configuration (a `.cfg` next to the driver), testing explicit standard library selection |
