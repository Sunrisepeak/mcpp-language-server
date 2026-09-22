# Conformance

The executable part of the specifications: each fixture is a small project and
a `scenario.json` that drives a language server through the Language Server
Protocol and checks what comes back. The same fixtures run locally and in CI.

```bash
mcpp build
bin=target/<triple>/<fingerprint>/bin
$bin/mcppls-conformance run --server $bin/mcppls --fixture conformance/fixtures/inferred \
    --payload editors/vscode/payload            # or --clangd PATH --kit DIR
```

The runner copies the fixture to a scratch directory, runs its `prepare`
commands there, starts the server with a private cache directory
(`MCPPLS_CACHE_DIR`), advertises `experimental.cxxModules`, and prints one
`PASS`/`FAIL` line per check. It exits non-zero when a check fails. Once the
server has exited, or has left two requests in a row unanswered, the remaining
checks fail at once with that reason instead of each waiting out its timeout.

## Fixtures

| Fixture | What it covers |
|---|---|
| `inferred` | Loose module sources, no build system and no compiler: the semantic kit provides libc++ semantics (design 13.5) |
| `engine-none` | The `inferred` project with `--engine none` (overall design 5.6): no core engine, so mcppls's own engine alone answers module navigation, hover, outline, import completion and module diagnostics; the status names `none` as the core engine and lists `mcppls` in `engines` (S3-4-5, S3-4-6). `inferred` checks the same fields with clangd, and starts a second server on the same workspace and cache, which must report the `shared-workspace` notice (design 6.3). Its S5 checks: module descriptions and interface summaries, exported symbols found without clangd, `unavailable` for references, and a file written to disk read by the next query |
| `verify-changes` | `mcpp-split`'s project in a git repository, for S5's verification after an edit: snippets checked in place (passing, failing, and leaving no unsaved content behind), a partition interface renamed on disk that breaks the implementation unit using it, the working tree's changes from git, and the restored file passing again once its importer is built again |
| `untrusted` | An mcpp package in an untrusted workspace: nothing is executed, the kit answers, the status is `degraded` with the reason |
| `mcpp-gcc` | mcpp with GCC 16, described by mcpp's own `emit build-database` (mcpp 2026.9.15.1): level 3 once the S1 library has structured mcpp's level 2 document, GCC arguments translated for clangd (P1), libstdc++'s `std` from its manifest, a test that imports the package's module across sets, and the workspace unchanged |
| `mcpp-llvm` | The same with LLVM 22: libc++ selected through include paths, BMI arguments removed (P3) |
| `mcpp-split` | The shape of a project that separates interfaces from implementations: interface units and an interface partition in `.cppm`, two implementation units (`module hello.greet;`) in `.cpp`, and an implementation partition; from mcpp's own build database. Declarations and definitions are reached from importers, definitions in implementation units before any of them is open, implementation units navigate into partitions and complete module-internal names, edits reach importers, and a file outside the build (`apps/gui/main.cpp`, importing a module nothing provides) is answered by clangd. `mcpp-split-gcc` (Linux) and `mcpp-split-msvc` (Windows) are the same project with GCC 16 and with `msvc@system` |
| `mcpp-all-cppm` | The shape of a project whose module units are all `.cppm`, implementations written inside the interfaces: a primary interface re-exporting two partitions and a second module; navigation into partitions and through the re-exports, completion through the re-exported module, edits propagated to importers |
| `mcpp-watch` | S2 5 with mcpp itself: a new module interface, a broken `mcpp.toml` and its repair are inputs mcpp names in `watch`; the broken manifest keeps the last model, `degraded` with `model-stale` and mcpp's own `MCPP_BUILD_DATABASE_PLAN_FAILED` (S2-5-9). CI also runs it as `mcpp-watch@polling` on Linux |
| `mcpp-emit` | mcpp's `emit build-database --format json` (mcpp-community/mcpp#636), simulated by `mcppls-mock-mcpp` from `mcpp-mock.json`, which records what mcpp 2026.9.15.1 prints for the project (paths as `${root}` and `${env:HOME}`): a level 2 document without `ide.options`, one set per package plus `hello:test` and `mcpp:std`, each seeing every other set. The S1 library completes it to a level 3 model; a test imports the package's module across sets, and the workspace stays unchanged. The simulated fixtures cover what a real mcpp cannot be made to do on demand |
| `mcpp-emit-package-std` | The same with `std` and `std.compat` provided by translation units of `mcpp:std` from a dependency package instead of by the toolchain's manifest, built in an mcpp std cache directory that does not exist yet |
| `mcpp-emit-broken` | The same mcpp answering with an error: the status carries mcpp's own diagnostic, sources are scanned meanwhile, and nothing is configured in its place (the mock's `build` would leave a `compile_commands.json` that `workspace-unchanged` sees) |
| `mcpp-emit-unavailable` | A project whose `.xlings.json` asks for an mcpp that is not installed: xlings answers every mcpp command in its place and runs nothing, and the status carries xlings's explanation (`mcpp-no-database`) while sources are scanned |
| `mcpp-emit-watch` | The inputs mcpp names in `watch` (S2 5, S2-5-1): writing one loads the model again, and an unchanged answer is recognized without rebuilding the index; when mcpp then fails the last model is kept, `degraded` with `model-stale` (S2-5-9), until it answers again. CI also runs it as `mcpp-emit-watch@polling`, with `--no-dynamic-watch` |
| `mingw` | A compile database for `x86_64-windows-gnu`: MinGW-w64 GCC semantics through `--sysroot` (P2) |
| `cmake-clang` | CMake 3.28+ with `FILE_SET CXX_MODULES`, Clang and Ninja: `@modmap` expansion, partitions |
| `cmake-clang-bdb` | The same project, but the prepare steps configure with CMake's experimental build database (`CMAKE_EXPERIMENTAL_EXPORT_BUILD_DATABASE`, CMake 4.4+ only) and build its `build_database.json` target: the model is read from that file instead of `compile_commands.json` (usable plan W4) |
| `cmake-msvc` | The same project built by cl.exe on Windows: cl.exe arguments translated into clang++ arguments, `-interface`, `-ifcOutput` and `-reference` removed from the expanded `.modmap` files, the Visual Studio toolset and Windows SDK passed explicitly (P7) |
| `cmake-msvc-bdb` | `cmake-msvc`'s build-database counterpart: the prepare steps build `build_database.json` with cl.exe and CMake 4.4+, the server under test still without a developer environment (D30) |
| `cmake-msvc-std` | CMake's `import std` with cl.exe and an `.ixx` interface: the build's own `std.ixx` units are replaced by the MSVC STL manifest's (P7) |
| `cmake-clangxx-msvc` | CMake modules built by clang++ for the MSVC ABI (P5) |
| `cmake-clang-cl` | CMake 4.4 modules built by clang-cl (P6), the first CMake that scans clang-cl module sources |
| `compdb-clangxx-msvc-std` | clang++ for the MSVC ABI with `import std`, built by the fixture's own script (P5) |
| `compdb-clang-cl-std` | clang-cl with `import std`, built by the fixture's own script (P6) |
| `mcpp-msvc` | mcpp with `msvc@system` and `import std`, described by mcpp's own `emit build-database`: level 3, a test across sets, the workspace unchanged |
| `mcpp-llvm-msvc` | mcpp's default Windows toolchain, LLVM for `x86_64-windows-msvc`, with the MSVC STL (P5), described the same way |
| `inferred-msvc` | Loose module sources on a machine with Visual Studio: MSVC STL semantics without a build system (design 9.3, D27) |
| `inferred-no-sdk` | macOS with the Command Line Tools and Xcode hidden: degraded with `sdk-missing` and its install command, a file importing `std` answered at once, module-level features (usable plan W5, U7) |
| `inferred-discover` | The `inferred` project with compiler discovery on, on clean machines: a Linux container without a compiler and Windows with Visual Studio hidden (usable plan W5) |
| `self-mcpp` | The mcpp repository at a fixed commit, about 170 modules (nightly, W8). Its `.xlings.json` asks for mcpp 2026.9.21.1, which xlings runs inside it |
| `self-mcppls` | Real-project plan RP0/RP3.4: mcpp-language-server's own repository at a fixed commit, described by its own `mcpp.toml` (no xlings pin). A stress check carries the plan's acceptance budget (0 timeouts, 60s max stall). Nightly and pre-release, alongside `self-mcpp` |
| `real-xlings` | Real-project plan RP0: the xlings repository at a fixed commit (about 110 modules, with a dependency that generates a module at build time), as committed: `projectScope` false, so the mcpp on PATH describes it. Module-name and symbol definition, hover, and a stress check (p90 ≤ 3 s, no request outliving the server's limit). Nightly |
| `real-xlings-old-mcpp` | Real-project plan RP0/RP2.1, the incident the plan starts from: the same commit with `projectScope` removed, so xlings' pinned mcpp 2026.8.8.4 — too old to emit a build database — is the project's mcpp. It must be described by a newer installed mcpp (tier 1, notice `producer-negotiated`, no stand-in) and pass the same checks. Nightly and pre-release |
| `timing` | Startup timing (usable plan W7): the `inferred` project opened and navigated at once; run cold, then warm with the same workspace and cache |
| `module-faults` | Faults stay where they are (robustness design): a module chain whose first unit imports a module nothing provides, a module that does not compile and its importer, and a file importing both a broken chain and a working module. Every file keeps its features, a module nothing provides gets a stand-in, and a module that breaks and heals while the server runs neither stalls clangd nor leaves the project without it. Runs with the semantic kit on every host. Also carries a `stress` check, so real-project stress testing (below) runs on every PR, on every platform |
| `failure-at-base` | Real-project plan RP1.1/RP1.3, at the scale of the xlings incident the plan is named for: a generated straight import chain of 100 modules whose base does not compile. A deep importer, opened first so the whole chain is wanted, is answered within a second and carries a `module-failed` diagnostic; files entirely outside the chain keep answering normally; the status settles to `degraded` naming `modules-doomed` within 60s; nothing restarts clangd. Also carries a `stress` check with the plan's acceptance budget |
| `generated-module` | Real-project plan RP0: a package module generated at build time (as libxpkg's `build.mcpp` writes `mcpplibs.xpkg.lua_stdlib` into `MCPP_OUT_DIR`) and a sibling module of the project's own that imports it; mcpp's build database (simulated) names the generated unit directly, at its real path under `target/.build-mcpp/deps/<pkg>@<ver>/out/`. Hover and definition into it must reach that real file, never a stand-in |
| `generated-module-old-mcpp` | The same project, but mcpp is too old to advertise `mcpp.build-database` and its `build --configure-only` is rejected (`mcpp-mock.json`'s `oldProtocol`, like mcpp 2026.8.8.4): the server's L2 fallback reads the project's own `compile_commands.json` (built for real by the fixture's own `prepare` step) instead, and the generated module's real source is still what hover and definition reach. `isolate-home` keeps the fixture's tier 3 deterministic on a machine that has a real, working mcpp installed |
| `generated-module-negotiated` | The same old-mcpp project, but a newer mock mcpp is installed where producer negotiation looks (its own isolated HOME's `xim-x-mcpp/9999.0.0/bin/mcpp`, the `producer-candidate` prepare step): the server must find it, describe the project through it (tier 1, level 3, notice `producer-negotiated`), and never fall back to `compile_commands.json` |
| `s1-two-sets` | A workspace carrying its own S1 build database (`--database`, usable plan W9.2): two sets compile the same file under `-DVARIANT=1` and `-DVARIANT=2`; `cxxModules/setContext` switches which one answers |
| `watch-polling` | Run with `--no-dynamic-watch` (usable plan W9.3): a new module interface written straight into the workspace must still reach the module graph within seconds, through the polling fallback rather than a client-driven `workspace/didChangeWatchedFiles` |
| `payload-corrupt` | Its `prepare` step copies the payload the runner was given and truncates clangd in the copy (usable plan W9.4); `server-arguments` then points `--payload` at that broken copy, and status must reach `error` with issue `payload-corrupt` |
| `multi-root` | Two workspace folders (usable plan W9.1): an `inferred` root and an mcpp-built `mcpp-llvm` root (level 3, from mcpp's own build database), each getting its own project model and clangd, each `cxxModules/status` telling them apart by `project.root` |

Before the fixtures run, CI records what `mcpp emit build-database --format json` prints for each of the host's mcpp fixtures that has no `mcpp-mock.json` and passes those envelopes to `docs/specs/tools/validate.py`, which checks them like the simulated data: the S2 envelope and S1 schemas, S1's semantic rules and the contract of mcpp-community/mcpp#636 (level 2, sets per package, every set seeing every other). The producer's side and the consumer's side of the loop are thus both checked against the same mcpp.

On Windows every server runs without a developer environment, as it does when an editor starts it. Fixtures whose own build needs one declare `"prepare-environment": "msvc"`, and the runner is given the environment with `--msvc-env FILE` (`NAME=value` lines, the output of `set` after `vcvars64.bat`); only the prepare steps see it. `--no-dynamic-watch` makes the runner declare no `workspace.didChangeWatchedFiles.dynamicRegistration` support at all, the way an editor without it would, exercising the server's own polling fallback (usable plan W9.3) instead of dynamic registration. Without it the runner acts as an editor's file watcher: it keeps the watchers the server registers (glob strings and relative patterns alike) and reports its own `write-file` writes that they cover through `workspace/didChangeWatchedFiles`. In CI's fixture lists, `NAME@polling` runs fixture `NAME` with `--no-dynamic-watch`.

## Startup timing

A run can reuse its workspace and the server's cache, so a second run measures a warm start:

```bash
$bin/mcppls-conformance run --server $bin/mcppls --payload payload --fixture conformance/fixtures/timing \
    --workspace-dir /tmp/timing/workspace --cache-dir /tmp/timing/cache --measure cold.json --navigation-budget 15
$bin/mcppls-conformance run --server $bin/mcppls --payload payload --fixture conformance/fixtures/timing \
    --workspace-dir /tmp/timing/workspace --cache-dir /tmp/timing/cache --measure warm.json --navigation-budget 2 --expect-warm
```

| Option | Effect |
|---|---|
| `--workspace-dir DIR` | The fixture is copied to `DIR/<name>` and prepared once; later runs use it as it is |
| `--cache-dir DIR` | The server's cache directory, instead of a fresh one per run |
| `--measure FILE` | JSON with seconds from `initialize` to the first `ready` state, the first diagnostics and the first navigation that answered, and each check's duration |
| `--navigation-budget SECONDS` | The run fails when the first navigation took longer |
| `--expect-warm` | A `module-cache-reused` check fails unless an earlier run left the module's files in the cache |

CI runs the pair on every host with budgets of 15 and 5 seconds and uploads the measure files; nightly records three runs per host.

## Scenario format

```json
{
  "name": "inferred",
  "prepare": [["cmake", "-S", ".", "-B", "build"]],
  "remove": ["mcpp.toml"],
  "server-arguments": ["--no-discover"],
  "checks": [ { "id": "C2", "kind": "definition", "file": "src/main.cpp", "at": [4, 31], "expect": "src/greet/greet.cppm" } ]
}
```

In `prepare` and `server-arguments`, `{exe}` expands to the platform executable suffix,
`{env:NAME|fallback}` to an environment variable, `{workspace}` to the fixture's scratch copy,
`{runner-dir}` to the directory of the runner executable, and `{payload}` to the runner's own
`--payload` directory (usable plan W9.4: a fixture's `prepare` step can copy and mutate it, then
point `server-arguments`' own `--payload` at the mutated copy — a later `--payload` wins), and
`{conformance}` to the runner itself, and `{home}` to the isolated HOME a fixture with
`"isolate-home": true` gets (empty otherwise). A fixture that has to generate something before the server
sees it names `["{conformance}", "prepare", "<kind>", ...]`: the generators live in the runner
(`mcppls-conformance prepare --help`), so a conformance host needs nothing the runner does not
bring — no interpreter. Positions
are `[line, character]`, zero-based, UTF-16. A check with `"text"` opens its file with that unsaved
content; a check with `"optional": true` reports `SKIP` instead of failing, and `"timeout": SECONDS`
waits less than the run's `--timeout`. `"file"` and `"folder"` on a check, like every other path a
scenario names, are relative to the fixture's own root, never to a specific workspace folder.

A fixture whose result depends on what mcpp or xlings a machine happens to have installed sets
`"isolate-home": true` (real-project plan RP2.1): the server under test, and every process it
starts, gets a private HOME (and, on Windows, USERPROFILE) under the fixture's own scratch
workspace, empty until the fixture's own `prepare` populates it. Producer negotiation
(`other_mcpp_executables`) searches `<home>/.xlings/data/xpkgs` and `<home>/.mcpp/registry/data/xpkgs`,
so `generated-module-old-mcpp` no longer risks being negotiated to a real mcpp a host or CI runner
happens to have installed, and `generated-module-negotiated`'s `producer-candidate` prepare step
(`["{conformance}", "prepare", "producer-candidate", "{home}"]`) installs a second mock mcpp exactly
there to prove negotiation itself, rather than only its fallback. A `status` check's optional
`"tier"` asserts `project.tier` (the README's L1..L4, real-project plan RP3.2), distinct from `"level"`.

A fixture with more than one workspace folder (usable plan W9.1) names them, relative to its own
root, in a top-level `"folders"` array; without one, the fixture's root is the only folder, as it
always has been.

```json
{ "name": "multi-root", "folders": ["inferred", "mcpp-llvm"], "checks": [
  { "id": "S1", "kind": "status", "folder": "mcpp-llvm", "state": "ready" } ] }
```

| Kind | Passes when |
|---|---|
| `status` | `cxxModules/status` reaches `ready`, `degraded` or `error` and matches `source`, `profile-kind`, `state`, `level`, `tier` (`project.tier`, the README's L1..L4, real-project plan RP3.2), `issue-code` (with `issue-command`, that issue's command; with `issue-message`, a part of its message), `notice-code` and `engine-name`/`engines-include` when given, and a `profile-compiler` prefix (a settled status that does not match yet is looked at again for up to three seconds, since a server coalesces changes that keep its state); `"folder"` picks one root's own status in a multi-root fixture (usable plan W9.1), absent picks whichever root's arrived most recently |
| `workspace-unchanged` | no file under the workspace was added, changed or removed after the prepare steps |
| `responds` | a request (`method`, default `textDocument/definition`) at `at` is answered, empty answers included, within the check's time |
| `module-cache-reused` | every file clangd published for `module` (default `std`) before the server started is still there unchanged, and none was added (SC4); passes on a cold start unless `--expect-warm` |
| `diagnostics-empty` | the file's diagnostics, after the engine has published them, contain no errors |
| `diagnostic-code` | a diagnostic with code `expect` is published for the file |
| `definition` / `declaration` | a location ends with `expect` |
| `definition-any` | there is at least one location |
| `hover-contains` | the hover text contains `expect`, or any one of them when `expect` is a list |
| `completion-contains` | a completion label starts with `expect`; `insert: [line, text]` adds a line first, `edit` changes another open buffer without saving it |
| `references-span` | the references include every path in `expect` |
| `document-symbol-contains` | the outline has a top-level symbol named `expect` |
| `module-graph-contains` | `cxxModules/graph` lists module `expect`; retries within the check's own timeout, so it doubles as "a change reaches the graph within N seconds" (usable plan W9.3's `watch-polling`) |
| `set-context` | sends `cxxModules/setContext` with `"context"` (usable plan W9.2), then a hover at `"at"` contains `expect`, retried the same way as `hover-contains` |
| `write-file` | writes `"content"` (default: a fresh `export module <module>;`; `"content-from"` copies another workspace file) to `"file"` directly, the way a file system watcher — or, without one, the server's own polling fallback — would notice it, without the runner opening it as a document (usable plan W9.3); with `"expect-reload": true`, also waits for the status to pass through `loading` again (S2-5-1) |
| `second-instance` | a second server on the same workspace and cache reports the notice `notice-code` (default `shared-workspace`) in its status (overall design 6.3) |
| `mcp` | S5 section 6: `mcppls mcp`, started once per fixture with the fixture's server arguments beside the language server, answers the tool call `"tool"` with `"arguments"` (or, with `"method"` and `"params"`, another request) with a result meeting `"expect"`; `"is-error": true` expects a tool error instead; the call is repeated until the expectations hold or the check's time is up, unless `"retry": false`; with `"via": "daemon"`, through `mcppls mcp --daemon` and the workspace daemon it starts (S5 6.1) |
| `execute-command` | `workspace/executeCommand` with `"command"` and `"arguments"` is answered without an error (the editor's review commands, design 7.7) |
| `cli` | S5 section 7: `mcppls <args>` with the runner's payload and the fixture's server arguments, run to completion in the workspace, exits with `"exit"` (default 0) and prints one JSON document meeting `"expect"` |
| `stress` | real-project stress testing (real-project plan RP0): seeded random use — see below — meets every key present in `"budget"` |
| `report` | robustness design O3: `cxxModules/report` meets `"expect"`, retried within the check's time like an `mcp`/`cli` result (a plan or an engine may still be on its way) |

An expectation of `mcp`, `cli` and `report` names a JSON pointer in `"path"`, where a `*` segment stands for every
element of an array, and one of `"equals"` (a value the pointer names equals it), `"contains"` (a string
contains it, or an array has an element that includes all its members), `"min-items"`, `"max-items"`, `"exists"` or
`"absent"`; it holds when any value the pointer names satisfies it.

The check identifiers C1–C9 are the core navigation and diagnostics checks every fixture can
use; M-checks cover the module features of S3 section 8.5.

## Stress checks

A `stress` check drives the server the way a person's first few minutes with a project do: files
matching `"files"` (glob, default `["src/**/*.cppm", "src/**/*.cpp"]`) are opened — some in quick
succession, without waiting for an answer — and at random identifier positions one of hover,
definition, references, completion or documentSymbol is asked, for `"actions"` rounds (default 60)
seeded by `"seed"` (default 1: the same seed always produces the same sequence of files, positions
and methods). `"requestTimeout"` (default 10s) bounds each request. Its `"detail"`, and the
combined `--measure` JSON's matching entry, carry:

```json
{ "methods": { "textDocument/hover": { "answered": 7, "empty": 0, "timeout": 0, "error": 0, "p50": 0.001, "p90": 0.01, "max": 0.4 }, "...": "..." },
  "timeouts": 0, "errors": 0, "p90": 0.02, "maxStallSeconds": 0.9, "finalState": "ready",
  "cpuSeconds": 9.1, "cpuSecondsPerMinute": 548.4, "rssMB": 893.2 }
```

`p50`/`p90`/`max` are per-method answered-or-empty latency in seconds. `maxStallSeconds` is the
longest interval, over the check's own window, with no `cxxModules/status` change and no
`$/progress` while the project was not `ready` — the status timeline design 1 asks for.
`cpuSeconds`/`rssMB` are the server's process tree's CPU seconds and peak RSS over that window,
sampled from `/proc` where the platform is Linux and the server's OS pid is known (POSIX only);
`null`, never a failure, wherever a platform cannot say. A `"budget"` object enforces whichever of
`timeouts`, `p90`, `maxStallSeconds`, `cpuSecondsPerMinute` and `rssMB` it names (a budget key with
no matching measurement, because a platform could not measure it, is not enforced):

```json
{ "id": "ST1", "kind": "stress", "actions": 60, "seed": 7, "requestTimeout": 10,
  "files": ["src/**/*.cppm", "src/**/*.cpp"],
  "budget": { "timeouts": 0, "p90": 3, "maxStallSeconds": 60, "cpuSecondsPerMinute": 90, "rssMB": 3000 } }
```

## Client profiles

`--client vscode|neovim|zed|plain` sends the capabilities that editor actually advertises, instead
of this runner's own long-standing default (the full `experimental.cxxModules` block, no
`initializationOptions`), so a fixture is run the way each real client talks to the server:

| `--client` | `experimental.cxxModules` | `initializationOptions` |
|---|---|---|
| (none) | `{ version: 1, status: true, graph: true, contexts: true }` | none |
| `vscode` | the same, as `editors/vscode/src/extension.ts` sends it | `{ conflictArbitration: "client" }` |
| `neovim` | `{ version: 1, status: true }`, as `editors/nvim/lua/mcppls/init.lua` sends it | `{ conflictArbitration: "client" }` |
| `zed` | none | none |
| `plain` | none | none |

`--client zed` and `--client plain` behave exactly as `--plain-client` always has (kept as its
alias): no `cxxModules/status` arrives, so a `status` check is skipped rather than run, and the run
checks that standard `$/progress` still arrived instead.
