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
| `save-plan` | Actual autosave of a module's new export reaches an importer without changing module names or imports; autosave of a new exported import still updates the plan and makes its exports available. Uses the semantic kit and no build system |
| `xmake-basic` | 0.0.8 part 2 X-1, X-6, X-7: an xmake project with GCC 16 and `import std`, described by xmake run privately (source xmake, L3, std prepared, navigation works); the provisional model never says `untrusted-workspace`; nothing is written into the project. Linux, needs `xmake` on PATH |
| `xmake-late-config` | X-1, X-5 (E1): no `set_languages`, and a stale `compile_commands.json` in the root (`prepare xmake-stale-compdb`); the engine commands are xmake's, not the file's; adding `set_languages("c++23")` to xmake.lua reaches the engine within 15 s with no `compdb-invalid`, `model-stale` or `preparation-stalled`; the project directory is unchanged once the edit is put back |
| `xmake-needs-download` | Plan 0.0.9 D-1, D-2, D-3, D-6, against a stand-in `xmake` script (`fake-bin/`, first on the server's PATH by the scenario's `server-path-prepend`; POSIX only, so in the Linux fixture lists). Offline, missing packages: `producer-needs-download` naming them, that build tools are included, that the run stayed offline and that the system package manager works too, `askOnline`, the project untouched. `mcppls.describeOnline` and the install fails: `producer-install-failed` with xmake's error lines and its install log, no ask. Missing at the `xmake project` stage: needs-download, not `xmake-configure-failed`. `.xmake/*/*/xmake.conf` holds `proxy`, `dotnet` and `dotnet_sdkver` (which `xmake f` refuses) and a project option: `xmake f` runs once per load, with no `xmake-options-left-out` |
| `xmake-user-mode` | X-2 (E5): `.xmake/linux/x86_64/xmake.conf` says `mode = "debug"` and `fancy = true`; the engine commands have `-O0 -g` and the project's `-DFANCY_PART`, and no `-DNDEBUG`; the project directory, `.xmake/` included, is unchanged |
| `xmake-no-standard` | X-3, X-7 (E4): no `set_languages`, GCC 16, `main.cpp` alone imports std (no module file, so xmake names no standard); the module units are read as C++23 -- the engine command carries `-std=gnu++23`, std is prepared and `std::println` is found -- and the report says the standard was mcppls's choice |
| `compdb-midwrite` | X-5: a `compile_commands.json` cut to half and written complete 300 ms later (and again with a writer that takes 2.5 s): no `compdb-invalid`, no `model-stale`, the model follows the complete file, the slow writer's half is read again a second later (`model-reread` event) |
| `inferred` | Loose module sources, no build system and no compiler: the semantic kit provides libc++ semantics (design 13.5) |
| `build-discovery-off` | Plan 2026-09-27 B-7: `mcppls.buildDiscovery = off` on an mcpp project whose producer would answer: nothing is detected, read or run, the model is scanned sources (L4), the report says discovery is off, and module features still work |
| `engine-none` | The `inferred` project with `--engine none` (overall design 5.6): no core engine, so mcppls's own engine alone answers module navigation, hover, outline, import completion, module-syntax keywords and module diagnostics, and a space off an import line is answered with nothing (S3-6.2); the status names `none` as the core engine and lists `mcppls` in `engines` (S3-4-5, S3-4-6). `inferred` checks the same fields with clangd, and starts a second server on the same workspace and cache, which must report the `shared-workspace` notice (design 6.3). Its S5 checks: module descriptions and interface summaries, exported symbols found without clangd, `unavailable` for references, and a file written to disk read by the next query |
| `verify-changes` | `mcpp-split`'s project in a git repository, for S5's verification after an edit: snippets checked in place (passing, failing, and leaving no unsaved content behind), a partition interface renamed on disk that breaks the implementation unit using it, the working tree's changes from git, and the restored file passing again once its importer is built again |
| `untrusted` | An mcpp package in an untrusted workspace: nothing is executed, the kit answers, the status is `degraded` with the reason |
| `mcpp-gcc` | mcpp with GCC 16, described by mcpp's own `emit build-database` (mcpp 2026.9.15.1): level 3 once the S1 library has structured mcpp's level 2 document, GCC arguments translated for clangd (P1), libstdc++'s `std` from its manifest, a test that imports the package's module across sets, and the workspace unchanged |
| `mcpp-llvm` | The same with LLVM 22: libc++ selected through include paths, BMI arguments removed (P3) |
| `mcpp-split` | The shape of a project that separates interfaces from implementations: interface units and an interface partition in `.cppm`, two implementation units (`module hello.greet;`) in `.cpp`, and an implementation partition; from mcpp's own build database. Declarations and definitions are reached from importers, definitions in implementation units before any of them is open, implementation units navigate into partitions and complete module-internal names, edits reach importers, and a file outside the build (`apps/gui/main.cpp`, importing a module nothing provides) is answered by clangd. `mcpp-split-gcc` (Linux) and `mcpp-split-msvc` (Windows) are the same project with GCC 16 and with `msvc@system` |
| `mcpp-partition-definition` | Plan 2026-09-27 N-7 (WA-CLANGD-008): a module split into partitions, functions and members declared in a partition with class-type parameters and defined in other units of the module. clangd's background index builds no module a unit imports, so these definitions stayed on their declarations (0.0.5 fails N1-N5); the server builds the module's implementation units through clangd's foreground, and go-to-definition reaches them with only the callers open |
| `mcpp-all-cppm` | The shape of a project whose module units are all `.cppm`, implementations written inside the interfaces: a primary interface re-exporting two partitions and a second module; navigation into partitions and through the re-exports, completion through the re-exported module, edits propagated to importers |
| `mcpp-watch` | S2 5 with mcpp itself: a new module interface, a broken `mcpp.toml` and its repair are inputs mcpp names in `watch`; the broken manifest keeps the last model, `degraded` with `model-stale` and mcpp's own `MCPP_BUILD_DATABASE_PLAN_FAILED` (S2-5-9). CI also runs it as `mcpp-watch@polling` on Linux |
| `mcpp-emit` | mcpp's `emit build-database --format json` (mcpp-community/mcpp#636), simulated by `mcppls-mock-mcpp` from `mcpp-mock.json`, which records what mcpp 2026.9.15.1 prints for the project (paths as `${root}` and `${env:HOME}`): a level 2 document without `ide.options`, one set per package plus `hello:test` and `mcpp:std`, each seeing every other set. The S1 library completes it to a level 3 model; a test imports the package's module across sets, and the workspace stays unchanged. The simulated fixtures cover what a real mcpp cannot be made to do on demand |
| `mcpp-emit-package-std` | The same with `std` and `std.compat` provided by translation units of `mcpp:std` from a dependency package instead of by the toolchain's manifest, built in an mcpp std cache directory that does not exist yet |
| `mcpp-emit-broken` | The same mcpp answering with an error: the status carries mcpp's own diagnostic, sources are scanned meanwhile, and nothing is configured in its place (the mock's `build` would leave a `compile_commands.json` that `workspace-unchanged` sees) |
| `mcpp-emit-partial` | S2 0.3.0 (S2-3.4-12, S2-3.4-13; mcpp-community/mcpp#699, fix plan 2026-09-26 F10): the producer answers with every member it could plan and an `error` diagnostic whose `path` names the one it could not, exiting 1. The rest is used (the model is mcpp's, not scanned sources) and the status names the missing part (`producer-partial`) |
| `mcpp-rules-generated` | Plan 2026-09-27 Q1-2, Q1-3 (mcpp-community/mcpp#724): a rule's input (a Qt form) listed as a translation unit is left out, and a header the rule generates, named in mcpp's private planning directory where no action runs, is reported missing with a build action until a build writes it into the project's own target/, after which the model is loaded again by itself and the including file has semantics |
| `mcpp-emit-partial-download` | B-8 (2026-09-27 plan §3.4, §9.3 T9): the same partial shape, but the missing member's own diagnostic is `MCPP_OFFLINE_DOWNLOAD_REQUIRED` -- a dependency it needs is not installed and offline forbids fetching it. The status carries both `producer-partial` (the rest is used) and `producer-needs-download` (what to fetch), where before it only ever saw the former |
| `mcpp-emit-unavailable` | A project whose `.xlings.json` asks for an mcpp that is not installed: xlings answers every mcpp command in its place and runs nothing, and the status carries xlings's explanation (`mcpp-no-database`) while sources are scanned |
| `mcpp-emit-needs-download` | The producer, run offline, needs a download: the status says what is missing, offers the terminal and lets a client offer to fetch it (`askOnline`, S3-4-16); `mcppls.describeOnline` then describes the project once with the network (the mock's `online` answer) and the model is mcpp's; nothing is written into the project |
| `mcpp-emit-provisioned` | Plan 2026-09-27 §9.2 rule 4: the offline description needs a download and nobody answers the offer; the person builds in their own terminal instead (the check writes `mcpp.lock`), and the server, seeing a watched build input change, asks the producer again offline and upgrades from scanned sources to mcpp's model by itself (the mock's `provisionedWhen`) |
| `mcpp-emit-watch` | The inputs mcpp names in `watch` (S2 5, S2-5-1): writing one loads the model again, and an unchanged answer is recognized without rebuilding the index; when mcpp then fails the last model is kept, `degraded` with `model-stale` (S2-5-9), until it answers again. CI also runs it as `mcpp-emit-watch@polling`, with `--no-dynamic-watch` |
| `cmake-fetchcontent-offline` | Plan 2026-09-27 B-3 (D1, BD7 withdrawn): a CMake project with a FetchContent dependency not on the machine and no build directory. The private configure is disconnected from its first run, so it stops at `producer-needs-download` naming the dependency, with `askOnline`, instead of downloading; the sources are served meanwhile and nothing is written into the project |
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
| `compdb-lto-msvc` | Issue #23: a `compile_commands.json` of clang++ for the MSVC ABI with `-flto` (the runner's `prepare compdb-lto-msvc`, no configuration file choosing lld). Every command the server gives clangd carries `-c`, so clangd's module scan does not stop at the driver's `LTO requires -fuse-ld=lld`: the module is built, the importer's import resolves and a hover across it answers. The driver raises that for the windows-msvc target on any host, so this runs on Linux |
| `mcpp-msvc` | mcpp with `msvc@system` and `import std`, described by mcpp's own `emit build-database`: level 3, a test across sets, the workspace unchanged |
| `mcpp-llvm-msvc` | mcpp's default Windows toolchain, LLVM for `x86_64-windows-msvc`, with the MSVC STL (P5), described the same way |
| `inferred-msvc` | Loose module sources on a machine with Visual Studio: MSVC STL semantics without a build system (design 9.3, D27) |
| `inferred-bom` | Issue #23: loose module sources saved with a UTF-8 byte order mark. The scanners skip the mark, so `export module greet;` after it declares the module: no false `unresolved-module`, no stand-in, the import navigates to the module and a hover across it answers |
| `inferred-cxx26` | C++26 alignment (fix plan 2026-09-26 §9): sources nothing describes are read with the newest standard their compiler takes, C++26 for the semantic kit; C++26 language features in a module's interface and its importer, `import std` and `std::saturating_add` under C++26, and `standard` in the status profile. 0.0.4 read them as C++23 |
| `compdb-mixed-standards` | C++26 alignment: a `compile_commands.json` (`prepare compdb-mixed-standards`) whose module and a user of std are C++23 and whose application importing both is C++26. A BMI is only imported under the standard it was built with (0.0.4: "C++26 was disabled in precompiled file"); the module units of a context are read with the newest standard they name, and the report says which and how many were raised |
| `inferred-no-sdk` | macOS with the Command Line Tools and Xcode hidden: degraded with `sdk-missing` and its install command, a file importing `std` answered at once, module-level features (usable plan W5, U7) |
| `inferred-discover` | The `inferred` project with compiler discovery on, on clean machines: a Linux container without a compiler and Windows with Visual Studio hidden (usable plan W5) |
| `self-mcpp` | The mcpp repository at a fixed commit, about 170 modules (nightly, W8). Its `.xlings.json` asks for mcpp 2026.9.21.1, which xlings runs inside it |
| `self-mcppls` | Real-project plan RP0/RP3.4: mcpp-language-server's own repository at a fixed commit, described by its own `mcpp.toml` (no xlings pin). A stress check carries the plan's acceptance budget (0 timeouts, 60s max stall). Nightly and pre-release, alongside `self-mcpp` |
| `real-xlings` | Real-project plan RP0: the xlings repository at a fixed commit (about 110 modules, with a dependency that generates a module at build time), as committed: `projectScope` false, so the mcpp on PATH describes it. Module-name and symbol definition, hover, and a stress check (p90 ≤ 3 s, no request outliving the server's limit). Nightly |
| `real-xlings-old-mcpp` | Real-project plan RP0/RP2.1, the incident the plan starts from: the same commit with `projectScope` removed, so xlings' pinned mcpp 2026.8.8.4 — too old to emit a build database — is the project's mcpp. It must be described by a newer installed mcpp (tier 1, notice `producer-negotiated`, no stand-in) and pass the same checks. Nightly and pre-release |
| `ux-mcpp` | 0.0.7 plan 6.4, U1 to U15 (and 0.0.8 plan M-4, U16, writing a module interface with autosave): the user-experience scenario tests on the mcpp repository at the commit `self-mcpp` pins (176 modules), on a four-vCPU machine: first answers, latency during the preparation, in the background and idle, typing with half identifiers, an unclosed brace and a half-typed `import a.b.` (also with autosave), saving widely imported interfaces, build-graph changes, clangd killed, the server killed and started at once, stale module locks, a truncated module file, a `git checkout` twenty commits back, ten idle minutes, and the status timeline through all of it. Stages (`--stage`, below); the `long` stage is nightly's only, because ten idle minutes and a checkout would take the pull request job past its hour |
| `ux-xlings` | The same on the xlings repository at the commit `real-xlings` pins (about 110 modules, one generated at build time): its own files and budgets, `xlings.platform` (the most imported interface) for the fan-out save, a compile flag added to `mcpp.toml` for the configuration change, and no second fan-out (the clangd crash on `mcpp.manifest.types` is mcpp's) |
| `timing` | Startup timing (usable plan W7): the `inferred` project opened and navigated at once; run cold, then warm with the same workspace and cache |
| `module-faults` | Faults stay where they are (robustness design): a module chain whose first unit imports a module nothing provides, a module that does not compile and its importer, and a file importing both a broken chain and a working module. Every file keeps its features, a module nothing provides gets a stand-in, and a module that breaks and heals while the server runs neither stalls clangd nor leaves the project without it. Runs with the semantic kit on every host. Also carries a `stress` check, so real-project stress testing (below) runs on every PR, on every platform |
| `failure-at-base` | Real-project plan RP1.1/RP1.3, at the scale of the xlings incident the plan is named for: a generated straight import chain of 100 modules whose base does not compile. A deep importer, opened first so the whole chain is wanted, is answered within a second and carries a `module-failed` diagnostic; files entirely outside the chain keep answering normally; the status settles to `degraded` naming `modules-doomed` within 60s; nothing restarts clangd. Also carries a `stress` check with the plan's acceptance budget |
| `generated-module` | Real-project plan RP0: a package module generated at build time (as libxpkg's `build.mcpp` writes `mcpplibs.xpkg.lua_stdlib` into `MCPP_OUT_DIR`) and a sibling module of the project's own that imports it; mcpp's build database (simulated) names the generated unit directly, at its real path under `target/.build-mcpp/deps/<pkg>@<ver>/out/`. Hover and definition into it must reach that real file, never a stand-in |
| `generated-module-old-mcpp` | The same project, but mcpp is too old to advertise `mcpp.build-database` and its `build --configure-only` is rejected (`mcpp-mock.json`'s `oldProtocol`, like mcpp 2026.8.8.4): the server's L2 fallback reads the project's own `compile_commands.json` (built for real by the fixture's own `prepare` step) instead, and the generated module's real source is still what hover and definition reach. `isolate-home` keeps the fixture's tier 3 deterministic on a machine that has a real, working mcpp installed |
| `generated-module-negotiated` | The same old-mcpp project, but a newer mock mcpp is installed where producer negotiation looks (its own isolated HOME's `xim-x-mcpp/9999.0.0/bin/mcpp`, the `producer-candidate` prepare step): the server must find it, describe the project through it (tier 1, level 3, notice `producer-negotiated`), and never fall back to `compile_commands.json` |
| `s1-two-sets` | A workspace carrying its own S1 build database (`--database`, usable plan W9.2): two sets compile the same file under `-DVARIANT=1` and `-DVARIANT=2`; `cxxModules/setContext` switches which one answers |
| `watch-polling` | Run with `--no-dynamic-watch` (usable plan W9.3): a new module interface written straight into the workspace must still reach the module graph within seconds, through the polling fallback rather than a client-driven `workspace/didChangeWatchedFiles` |
| `clangd-cannot-load` | 0.0.3 plan B1: its `prepare` step puts a stand-in clangd in the workspace (mcppls-mock-mcpp with an `unavailable` config) that writes a loader's message, a `GLIBCXX` version not found, to standard error and exits 1; `--clangd` points the server at it. `initialize` must be answered within 20 s (it used to wait for good), the status must reach `error` with issue `engine-incompatible`, and mcppls's own module features must work |
| `typing-import` | Import-hang plan §8: `import hello.greet;` in `main.cpp` and `export module hello.greet;` in its interface typed one key at a time, through `import hello.` and `export module hello.`, which clangd 23.1 never finishes building (WA-CLANGD-001); again with every step saved, as autosave does. Every request is answered within 5 s, the status never turns `degraded`, hover works right after, and nothing restarts clangd or is set aside |
| `typing-import-spin` | The same typing with WA-CLANGD-001 turned off (`--disable-workaround`), so clangd really spins (Linux, macOS; on Windows the same text crashes it instead): a spin is found within its 20 s budget (event `engine-spin`), the file set aside with that text remembered and clangd restarted, a crash is restarted as before (`engine-exit`), and either way features come back while typing goes on (import-hang plan §4). If a clangd update removes the defect, its T2 check fails as well |
| `typing-autosave` | Fix plan 2026-09-26 F16, F13, F11, F12 (the hello project with `files.autoSave`): a new import line typed below the others, every step saved to disk, staying on `import hello.` for twelve seconds and then on a module nothing provides. clangd reads a file's imports from disk (UP-14), so 0.0.4 left every request for the file unanswered; the file is now set aside before clangd builds it and handed back once its disk text, or the database clangd read, makes it safe, with an incident recording the broken premise and no restart but for a spin. A directive missing its `;` is reported on the directive (WA-CLANGD-006) and an import only in the unsaved buffer is information (WA-CLANGD-007) |
| `module-edit-autosave` | 0.0.8 plan M-1 to M-4 (the hello project with `files.autoSave afterDelay`): a module interface is written function by function for 30 s while its importer is open with a probe line `int probe_value = 1; probe_v` in `main()`, and every pause with half a statement on the screen is saved to disk a second after the last key. 0.0.7 took the module's own file from clangd with its importer (RP1.1): one position-less `module-failed` diagnostic instead of its real errors, and completions from mcppls's own engine, empty, until the next save handed it back. The unit whose compile failed keeps clangd (no `module-failed` diagnostic for the typed file); the importer is still contained while the module does not compile, and no completion in it may be empty (the file's words, M-2); no status sample may say `preparation-stalled`, and clangd may not be restarted nor a request time out. Runs the `typing` kind with `autosave-idle-ms` and `importer` |
| `partial-scan-standins` | 0.0.8 plan P-1 (issue #34-2, GalTranslPP run 36676829043): three module interfaces include a header no command can reach after a generated chain of headers that takes clangd none, about two and about four seconds to preprocess, and `main.cpp` imports all three, so clangd names one more module it cannot find each time. 0.0.7 gave each its stand-in as it was named and restarted clangd for it; a restart ends the scans still running, so the next module was named by the next session (two restarts here, four rounds and 144 s on GalTranslPP). The reports are gathered and all the stand-ins enter the database once: at most one restart for a module's unit leaving the engine database (`restarts/*/reason`, `max-matches`) |
| `provisional-no-prime` | 0.0.8 plan R-4 (issue #34-1), Linux and macOS: the producer (`prepare delayed-producer 8`: a shell script around `mcppls-mock-mcpp` that waits eight seconds before `emit build-database`) answers late and there is no cached model, so the scanned sources are the provisional model. Six seconds in (`not-before-since-start`), `cxxModules/report` must show at most two units wanted for preparation (std, std.compat), not the project's modules, which are prepared for commands the build tool's model will replace (0.0.7 wanted three); then the model comes (`source: mcpp`) and the importer's hover works |
| `clangd-crash-context` | Fix plan 2026-09-26 F3 (issue #23), Linux and macOS: a stand-in clangd (`prepare clangd-crash-context`) prints a crash context naming a file nobody opened while `main.cpp` is typed in, then dies. Only the named file is set aside, the exit is reported with it and the status names it; 0.0.4 set aside `main.cpp` |
| `compdb-rejected-command` | Fix plan 2026-09-26 F6: a `compile_commands.json` whose commands carry a value the compiler rejects (`-std=c++99999`, `prepare compdb-rejected-command`), as issue #23's did: every module scan fails, and the status says the command was rejected, as an environment issue in the compiler's words |
| `mcpp-emit-wait` | Fix plan 2026-09-26 F4 (D1): the producer hangs past the ten seconds after which scanned sources are planned, to its 20 s bound; mcppls's own engine answers meanwhile and clangd is given no plan until the producer answers or is given up |
| `completion-keywords` | Fix plan 2026-09-26 F9 and F15, as VS Code (`client-info`): the space is a completion trigger character; a space after `import ` or `export import ` opens the module list from mcppls's own index, and one typed anywhere else, or after `import  ` or `export module `, is answered at once with nothing. The module-syntax keywords are offered where each can begin a declaration and not inside a function body, merged with clangd's answer, and on their own within 1.5 s for a file clangd cannot answer for yet (a new file waiting for clangd to read a database that has it); the report counts what the space trigger cost (S3-6.2-1 to S3-6.2-5) |
| `workaround-canaries` | Import-hang plan §9, 0.0.9 plan M-3: one check per registered workaround with a canary, run against the payload's clangd. WA-CLANGD-001 is a `clangd-check`; WA-CLANGD-009 (completion with `--experimental-modules-support` takes over 1.8 times as long on a file including 120 generated headers, `prepare heavy-headers`) and WA-CLANGD-010 (misc-const-correctness on a filter view and a view over one) are `clangd-lsp` checks, on Linux only. A failure here means a clangd update fixed that defect and the workaround it names can be removed. WA-CLANGD-001 and 010 are `retired-by` capabilities mcppls-clangd declares, so with the bundled engine they report themselves retired |
| `no-modules` | 0.0.9 plan M-3, WA-CLANGD-009: sources and headers with no module in the plan: clangd runs without `--experimental-modules-support`, hover and definition work, and the report splits request time into clangd's and the server's (M-1); a module interface written into the workspace restarts clangd with the flag, the event's reason naming WA-CLANGD-009 |
| `tidy-const-views` | 0.0.9 plan M-3, WA-CLANGD-010: a project `.clangd` with `FastCheckFilter: None` and a `.clang-tidy` for misc-const-correctness; of a filter view, a transform view over it and an int never changed, the server shows only the int (`diagnostic-code-lines`) |
| `ux-heavy-headers` | 0.0.9 plan M-2 (issue #37): ten sources including 400 generated headers (`prepare heavy-headers 400`), no modules, no checkout. `completion-baseline` holds completion through the server, each after an edit, to 1.3 times clangd alone's p95 plus 30 ms on the very compile_commands.json the server wrote, and to a share of answers by clangd. CI's `ux` job runs its one stage (`edits`); with `--disable-workaround WA-CLANGD-009` it fails (405 ms against 14 ms locally) |
| `mcpp-emit-edits` | Plan 2026-09-30 G-5, a project being written: edits inside functions, saved, never run the producer again although it names every source as an input; an edit that changes a file's imports, and a new source, do. `write-file` with `"expect-reload": false` watches for `settle` seconds that the model is not loaded again |
| `reset-cache` | Plan 2026-09-30 C-1: `mcppls.resetCache` removes the workspace's cache (models, engine database, clangd's module cache and locks) and plans and starts again; navigation, hover and completion answer as before |
| `diagnostic-bundle` | Issue #23 fix plan F18: the diagnostic bundle an editor exports (`mcppls.exportBundle`) holds what its manifest says, digest for digest, within its 25 MB cap, and no file of it, nor `cxxModules/report`, names the user or the home directory the server runs with, the home written into the client's log it is sent included (S3-5.5-3); asked to hide project paths, not the workspace either. On a CI runner that is `runneradmin`, its `RUNNER~1` and `C:\Users\runneradmin` on Windows, `/home/runner` and `/Users/runner` elsewhere |
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
| `--stage NAME` | Runs the checks whose `"stage"` is `NAME` (a string or a list) and those with none: a fixture that has a cold start, a warm start, edits and faults (`ux-mcpp`, `ux-xlings`) is one scenario run in stages |

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
content; a check with `"only-on": ["linux", "macos", "windows"]` runs only on those operating systems and reports `SKIP` elsewhere; a check with `"optional": true` reports `SKIP` instead of failing, and `"timeout": SECONDS`
waits less than the run's `--timeout`. `"file"` and `"folder"` on a check, like every other path a
scenario names, are relative to the fixture's own root, never to a specific workspace folder.
`"initialization-options"` on the scenario is an object merged into the runner's own
`initializationOptions` (over whatever `--client` profile set), so a fixture can ask for something
`--client` does not, such as `{"semanticTokens": {"moduleType": true}}` (design doc 2026-09-25
K/§7).
`"client-info"` on the scenario is the `clientInfo` the runner sends in `initialize` (none otherwise):
what a server tells VS Code can differ from what it tells other clients (fix plan 2026-09-26 F9).
`"initialize-within": SECONDS` on the scenario fails the run when `initialize` is answered later
than that (the runner itself waits up to 120 s): a server that answers eventually is not enough
where the point is that it answers at once (`clangd-cannot-load`).

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
| `status` | `cxxModules/status` reaches `ready`, `degraded` or `error` and matches `source`, `profile-kind`, `state`, `level`, `tier` (`project.tier`, the README's L1..L4, real-project plan RP3.2), `issue-code` (with `issue-command`, that issue's command; with `issue-message`, a part of its message; with `issue-category`, its S3 category: `code`, `engine`, `environment` or `project`), `notice-code` and `engine-name`/`engines-include` when given, and a `profile-compiler` prefix (a settled status that does not match yet is looked at again for up to three seconds, since a server coalesces changes that keep its state); `"folder"` picks one root's own status in a multi-root fixture (usable plan W9.1), absent picks whichever root's arrived most recently |
| `workspace-unchanged` | no file under the workspace was added, changed or removed after the prepare steps |
| `status-never` | 0.0.8 part 2 X-6: none of the statuses the server sent (after the first `ready`, unless `"after-ready": false`) names an issue with a code in `issue-codes` |
| `engine-command` | X-7: the command clangd was given for the unit whose file ends with `file`, read from the engine database the server wrote under the cache directory, has every argument of `contains` and none of `absent`; retried within the check's time |
| `write-midway` | X-5: a file a tool is rewriting in place: `file` (with `replace` `{"from", "with"}` applied) is written cut to its first `cut` (0.5) part, then complete `after-ms` (300) later; the server is told of both writes, as an editor's watcher would; put back when the run ends |
| `responds` | a request (`method`, default `textDocument/definition`) at `at` is answered, empty answers included, within the check's time |
| `module-cache-reused` | every file clangd published for `module` (default `std`) before the server started is still there unchanged, and none was added (SC4); passes on a cold start unless `--expect-warm` |
| `diagnostics-empty` | the file's diagnostics, after the engine has published them, contain no errors |
| `diagnostic-code` | a diagnostic with code `expect` is published for the file; with `"line"` (0-based) it starts on that line, with `"severity"` it has that severity, and none of the codes in `"absent"` is published with it (fix plan 2026-09-26 F11, F12) |
| `diagnostic-code-lines` | once the file's diagnostics settle, the diagnostics with code `code` start on exactly the lines in `"lines"` (0-based; `[]`: none), or with `"subset": true` on none outside them (plan 0.0.8 part 2 I-1) |
| `definition` / `declaration` | a location ends with `expect` |
| `definition-any` | there is at least one location |
| `hover-contains` | the hover text contains `expect`, or any one of them when `expect` is a list |
| `completion-contains` | a completion label starts with `expect` (with a list, every one does; with `"exact": true`, a label is it), and none is one of `"absent"`; `insert: [line, text]` adds a line first, `edit` changes another open buffer without saving it; `"trigger"` sends the request as typing that character asked for it (`context.triggerKind` 2) |
| `completion-empty` | the completion (with `"trigger"` as above) has no items, within `"within-ms"` when given (fix plan 2026-09-26 F9: a space off an import line) |
| `capabilities` | the server capabilities `initialize` answered with meet `"expect"` (expectations as for `report`) |
| `references-span` | the references include every path in `expect` |
| `document-symbol-contains` | the outline has a top-level symbol named `expect` |
| `semantic-tokens` | `textDocument/semanticTokens/full` (or `/range`, with `"range"`) for `"file"` (optionally with an unsaved `"text"`), decoded with the legend `initialize` gave, has every entry of `"expect"` (`{"line", "text", "type", "modifiers"?}`; `"modifiers"` is a list, and optional) among its tokens (design doc 2026-09-25 K/§7) |
| `module-graph-contains` | `cxxModules/graph` lists module `expect`; retries within the check's own timeout, so it doubles as "a change reaches the graph within N seconds" (usable plan W9.3's `watch-polling`) |
| `set-context` | sends `cxxModules/setContext` with `"context"` (usable plan W9.2), then a hover at `"at"` contains `expect`, retried the same way as `hover-contains` |
| `write-file` | writes `"content"` (default: a fresh `export module <module>;`; `"content-from"` copies another workspace file) to `"file"` directly, the way a file system watcher — or, without one, the server's own polling fallback — would notice it, without the runner opening it as a document (usable plan W9.3); with `"expect-reload": true`, also waits for the status to pass through `loading` again (S2-5-1); `"replace": {"from", "with"}` rewrites part of the file as it is, and a later `"restore": true` writes back what it was (a run puts back every file it changed when it ends); `"notify": true` reports the write to the server even when it registered no watcher for it, as an editor's own watcher of build files does (an inferred model, kept while a build tool cannot answer, registers none) |
| `second-instance` | a second server on the same workspace and cache reports the notice `notice-code` (default `shared-workspace`) in its status (overall design 6.3) |
| `mcp` | S5 section 6: `mcppls mcp`, started once per fixture with the fixture's server arguments beside the language server, answers the tool call `"tool"` with `"arguments"` (or, with `"method"` and `"params"`, another request) with a result meeting `"expect"`; `"is-error": true` expects a tool error instead; the call is repeated until the expectations hold or the check's time is up, unless `"retry": false`; with `"via": "daemon"`, through `mcppls mcp --daemon` and the workspace daemon it starts (S5 6.1) |
| `execute-command` | `workspace/executeCommand` with `"command"` and `"arguments"` is answered without an error (the editor's review commands, design 7.7); `"{workspace-uri}"` in an argument is the workspace folder's URI as the client sent it, and `"expect"` names fields the answer must carry with those values |
| `cli` | S5 section 7: `mcppls <args>` with the runner's payload and the fixture's server arguments, run to completion in the workspace, exits with `"exit"` (default 0) and prints one JSON document meeting `"expect"` |
| `stress` | real-project stress testing (real-project plan RP0): seeded random use — see below — meets every key present in `"budget"` |
| `type-text` | line `line` of `file` takes each of `steps` in turn, `interval-ms` apart (default 120), the whole buffer sent each time; after each, `request` (default `textDocument/documentSymbol`) is answered within `answer-within` seconds (default 5); with `save`, each step is also written to disk and reported as saved and changed, as autosave does; fails when the status turned to a state listed in `states-never` meanwhile (import-hang plan §8) |
| `retired-by` (any check) | the capability that retires the workaround whose defect the check watches: the check holds, saying why, when the runner uses the payload's clangd and its verified identity declares that capability (as the server retires the workaround there); against any other clangd it runs |
| `clangd-check` | the runner's own clangd (`--clangd`, else the payload's) run with `--check` on `file` does not finish (`expect: "hangs"`: not finished after `seconds`, default 10, or crashed) or finishes normally (`"finishes"`); a workaround's canary expects its defect, and fails with `says` once a clangd update fixed it (import-hang plan §9) |
| `clangd-lsp` | 0.0.9 plan M-3: the runner's clangd driven over LSP, the server not involved, started with `arguments` (`{workspace}` stands for the workspace; the isolated HOME applies). `"action": "diagnostics"`: `file` is opened and, once clangd's diagnostics settle (`quiet-ms`, 3000, after the first), `code` is on exactly the 0-based `lines`, or with `"includes": true` on at least those; `"completion-ratio"`: the median of `rounds` (10) completions at `at`, with `arguments`, over the same without them (`baseline-arguments`), `passes` (2) alternating sessions each, over `min-ratio`. A canary holds while the defect is there and fails with `says`, as `clangd-check` does; `only-on` keeps one off a platform it was not verified on. |
| `completion-baseline` | 0.0.9 plan M-2: after the server settles, clangd alone (`baseline-arguments`; `{engine-database}` is the directory of the compile_commands.json the server wrote for its own clangd) is timed over `rounds` (40) completions at `at`, each right after the buffer was edited (`interval-ms` apart), then the server over the same. Budget: `ratio` (1.3) and `slackMs` (30) of clangd's p95, `engineShare` (a minimum), `maxEmpty`; the measure carries both p50 and p95 and the report's own `engineP95Ms` and `overheadP95Ms` for completion |
| `report` | robustness design O3: `cxxModules/report` meets `"expect"`, retried within the check's time like an `mcp`/`cli` result (a plan or an engine may still be on its way) |
| `bundle` | issue #23 fix plan F18: `workspace/executeCommand` `mcppls.exportBundle` with `"arguments"` (and a client log naming the home directory) answers with the path of a zip under 25 MB whose `manifest.json` lists every other file with its SHA-256, which has every file of `"expect-files"`, and in which no file names the server's home directory (either separator), a distinctive user name (`USER`, `USERNAME`, `LOGNAME`) or its 8.3 form, or, with `"forbid-workspace": true`, the workspace; neither may `cxxModules/report` (the workspace aside) |

An expectation of `mcp`, `cli` and `report` names a JSON pointer in `"path"`, where a `*` segment stands for every
element of an array, and one of `"equals"` (a value the pointer names equals it), `"contains"` (a string
contains it, or an array has an element that includes all its members), `"min-items"`, `"max-items"`, `"at-least"`
(a number at least it), `"at-most"` (a number at most it), `"exists"` or `"absent"`; it holds when any value the pointer names satisfies it.
`"max-matches"` and `"min-matches"` count instead: with `"equals"` or a string `"contains"` beside them, at most (at least) that many of the values the pointer names satisfy it
(0.0.8: `restarts/*/reason` containing a reason, at most once), and without either they count the values the pointer names. `"each-contains"` is the one that
every value the pointer names must satisfy instead (each is a string containing it), and it holds when
the pointer names none.

The check identifiers C1–C9 are the core navigation and diagnostics checks every fixture can
use; M-checks cover the module features of S3 section 8.5.

## Stress checks

A `stress` check drives the server the way a person's first few minutes with a project do: files
matching `"files"` (glob, default `["src/**/*.cppm", "src/**/*.cpp"]`) are opened — some in quick
succession, without waiting for an answer — and at random identifier positions one of hover,
definition, references, completion or documentSymbol is asked, for `"actions"` rounds (default 60)
seeded by `"seed"` (default 1: the same seed always produces the same sequence of files, positions
and methods). `"requestTimeout"` (default 10s) bounds each request; a check whose budget allows no timeout at all
sets it above the server's own request limit (60s), so a timeout there means the server itself never
answered, while `"p90"` and `"maxStallSeconds"` carry how fast it did. Its `"detail"`, and the
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

## Scenario checks: the user-experience tests

`ux-mcpp` and `ux-xlings` (0.0.7 plan 6.4, U1 to U15; 0.0.8 plan M-4, U16) hold the server to what a person notices: how long a first answer, a
completion while typing or a recovery takes, measured against the mcpp and xlings repositories at pinned commits. They are
the merge gate of CI's `ux` job and nightly's `ux` rounds, and each check writes everything it measured (budgeted or not)
into `--measure`, in the check's `"measure"` object.

Their checks are the kinds below. Every number a budget names is a maximum unless it says otherwise; a key a check has no
measurement for (a platform that cannot read `/proc`, a fault it cannot cause) is not enforced, as `stress` does, and a
platform that cannot cause a fault reports it as skipped in the check's detail. Every measurement carries `load` (the first field of
`/proc/loadavg` when the host has one): a run on a busy machine explains itself.

| Kind | What it does and what it measures |
|---|---|
| `latency` | Repeats `requests` (`{"kind": "completion" \| "hover" \| "definition" \| "semanticTokens" \| "documentSymbol", "file", "at", "name"?, "expect"?, "trigger"?}`; `expect` is a path suffix for a definition, a label prefix for a completion, a text for a hover) one after another, `interval-ms` (700) apart, in a `phase`: `cold` (while the preparation runs: from the first `preparing` until it stops), `background` (from ready for `seconds`, 45: the implementation index is being built), `idle` (after the project settled: ready, and clangd under 0.3 of a core for eight seconds; `rounds`, 20) or `now` (at once, `rounds` times). Per request kind it reports p50, p95 and max seconds, timeouts, errors, empty answers and answers that missed `expect`, and, from `cxxModules/report`'s per-method `answeredBy`, the share of `completion` and `hover` answers (`engine-share-kinds`) that were not mcppls's own fallback. `first-answers` (`{"name", "kind", "file", "at", "expect", "within"}`) are asked in every round until each is answered as it should be, and the seconds since `initialize` of the first right answer are held to `within`. Budget: `p50`, `p95`, `max` (for every kind, or `<kind>.p95` for one), `timeouts`, `wrong`, `maxEmptyShare`, and `engineShare` (a minimum, 0 to 1) |
| `typing` | Types `scripts` (`{"line", "text"}`: a new line at `line` is typed one character at a time at `hz` (10), left with its broken text on screen for a moment, erased and removed; the whole buffer is sent after every key) for `seconds` (120), with a completion asked at the cursor every `completion-every-ms` (1000) without waiting for it; with `autosave-ms` the file is also written and saved that often, as autosave does, and put back when the check ends; with `autosave-idle-ms` it is written and saved once that long has passed since the last key and the buffer differs from what was last saved (`files.autoSave: afterDelay`, independent of `autosave-ms`); with `importer` (`{"file", "line", "text"}`) a second file that imports the typed one is opened with `text` inserted as a new line at `line` (0-based, in the editor's buffer only), a completion at the end of that line is asked alongside each of the typed file's, its latencies and results are kept apart, and its text is put back when the check ends. Then a real mistake (`diagnostic-probe`: `{"line", "text", "expect"}`) is typed and the time until its diagnostic arrives is measured. Reports completion p50, p95, max, the clangd restarts, files set aside and request timeouts (the report's counters, before and after), the engine share of the completions and the states the status went through. Also reports the files doomed (`file-doomed` events), the `module-failed` diagnostics published for the typed file and, apart, for the importer, the status samples naming `preparation-stalled`, and the importer's completion p95 and empty answers. Budget: `p95`, `max`, `restarts`, `filesSetAside`, `timeouts`, `engineShare` (a minimum), `diagnosticsRefresh`, `doomed` (the most `file-doomed` events), `moduleFailedDiagnostics` (the most publications for the typed file that carry a diagnostic with code `module-failed`: a file taken from clangd gets one), `stalled` (the most status samples with the issue `preparation-stalled`), `importerP95` (seconds) and `importerEmpty` (the most empty importer completions); `states-never` lists states the status must not reach With `"firstOpenSeconds"` in the budget, the file is first asked for a completion once a second until clangd answers one (issue #50: mcppls-clangd waits for a module importer's first preamble), that wait is held to the budget and reported, and the typing and its other budgets start after it. |
| `edit-save` | Opens `importers` (the first carrying `probe.append`, a use of what `insert` adds) and `file`, waits for the project to settle, inserts `insert` after `marker` in `file`, saves it (on disk, `didChange`, `didSave`, `didChangeWatchedFiles`) and times until the probe's diagnostic (`probe.expect`) clears and until every importer's diagnostics were published again, while a completion (`interactive`: `{"file", "at", "every-ms"}`) is asked every second. With `recover-probe` (a request as in `fault`) it also times until that request is answered as it should be. The interface is put back afterwards. Budget: `republishSeconds`, `probeSeconds`, `p95`, `max`, `recoverSeconds`, `errorStates`, `restarts`, `engineShare` |
| `fault` | Causes a fault (`action`) and times the recovery to `recover-probe` (`{"kind": "completion" \| "definition" \| "hover", "file", "at", "expect"}`: a request whose right answer shows the project usable again; a completion of a std name only clangd can give is the honest one, a jump is often answered by mcppls's own engine before clangd is up) and to the status `ready` (a `ready` that lasts less than a second and a half does not count: 0.0.6 says `ready` for a moment before `preparing` when nothing is built). `open` lists the documents opened first, since the preparation is of what an editor has open. `kill-clangd` (`at-progress`: while the preparation is that far along, taking `progress-fallback-seconds` in `preparing` for it where the status reports no progress, or, without it, once the project settled; `count`, more than one at a further tenth each) reports how long the new clangd took (`newClangdSeconds`), how many clangds the server started for the kills (`clangdStarts`) and whether the preparation went on; `kill-server` kills the server (SIGKILL) and starts another at once (`gap-ms`) on the same workspace and cache, and reports whether it used the workspace's cache (`usesPrimaryCache`) and whether clangd was left running (`orphans`); `lock` stops the server, removes `module`'s published files and writes a stale lock for them (`variant`: `dead-pid`, `live-pid` or `foreign-host`), the shape clangd 23.1 leaves (`.locks/<hash>.lock`, a symlink to `<hash>.lock-<random>` holding "hostname pid"), and starts another; `truncate-pcm` truncates `module`'s files to half instead; `git-checkout` checks out `rev` (`HEAD~20`) in the workspace, tells the server the files that changed and does the same back, reporting `replanSeconds` and clangd restarts of both legs and the probe's time after the second. A fault always reports whether the module files built again were more than the cache had (`newBmi`), and fails when the probe or the status is not back within `wait-seconds` (120). Budget: `answerSeconds` (the probe), `readySeconds`, `recoverSeconds` (the later of the two), `newClangdSeconds`, `clangdStarts`, `newBmi`, `orphans`, `lockWaitLines`, `usesPrimaryCache` (`true`), `replanSeconds`, `restarts` |
| `graph-edit` | A build-graph change and how the server follows: `add` a module interface (`file`, `module`) or `remove` it (times until `cxxModules/graph` lists or drops `module`, and counts the clangd restarts within `quiet-seconds`, 10); `config` rewrites part of a build input (`replace`: `{"from", "with"}`) and, once settled, undoes it, timing how soon the project is described again each time and how many module files the undoing built (`revertNewBmi`). Budget: `graphSeconds`, `restarts`, `changeSeconds`, `revertSeconds`, `revertNewBmi` |
| `timeline` | The whole run's `cxxModules/status` history (across the server restarts a fault caused): the longest time not `ready` while `loading`, `preparing` or `preparation-stalled` without `progress.done` advancing, the state changes in the busiest minute, the times the state was `error`, clangd's restarts, the seconds until the first `ready` that held (a `ready` under a second and a half long, which 0.0.6 says before `preparing` when nothing is built, is not one) and until the last one after a preparation (a project whose model is replaced is prepared twice). `observe-seconds` first watches that long. Budget: `maxStallSeconds`, `maxChangesPerMinute`, `errorStates`, `restarts`, `firstReadySeconds`, `preparedSeconds` |
| `bmi-reuse` | The module files under the cache directory built since the server started (new, or with another size or time: clangd's timestamped copies are not builds) and the `Built module` lines its own output logged (from the server's log files, which needs the fixture's `--log-level debug`; a log without clangd's output is not counted as zero). Passes on a cold start unless `--expect-warm`, like `module-cache-reused` (which checks one module). Budget: `newBmi`, `builtLines` |
| `resources` | clangd's peak resident memory over the run (`/proc`, Linux) and, with `idle-seconds`, its CPU in cores while nobody asked and the growth of its memory over that time (the window starts once the project settled and `warmup-seconds`, 60, more have passed). Budget: `rssMB`, `idleCpuCores`, `rssGrowth` |

Any check may carry `"not-before-since-start": SECONDS` (the runner keeps serving the server until that long after `initialize` before it runs the check, to look at it in a window,
0.0.8) and `"within-since-start": SECONDS` (the check holds that soon after `initialize`; a retried check ends when it
first holds) and `"max-seconds": SECONDS` (the check itself took no longer). A `write-file` check may `replace` part of a file
(`{"from", "with"}`) and a later one `restore` it; whatever a check changed is also put back when the run ends, so the next
stage finds the prepared workspace.

`--stage NAME` runs the checks whose `"stage"` is that name (a string or a list) and those with none. The two fixtures use
five stages, run in this order, `cold` and `warm` and `edits` sharing one `--cache-dir` and `faults` using another (all sharing
the `--workspace-dir`, which is prepared once):

| Stage | Cache | Scenarios |
|---|---|---|
| `cold` | empty | U1 first responses and the first clangd answers, U4 during the preparation and after it, U3 idle, U14 memory, U15 |
| `warm` (`--expect-warm`) | what `cold` left | U2 ready within 5 s, no module built again, no clangd restart in five minutes |
| `edits` (`--expect-warm`) | the same | U5 and U6 typing (and autosave), U16 writing a module interface with autosave and its importer open (the typed file keeps clangd, nothing stalled, no empty completion in the importer), U7 saving a widely imported interface (`ux-mcpp` also `mcpp.manifest.types`, where clangd crashed in 0.0.6), U8 a module added, removed and a build input changed |
| `faults` | a second, empty one | U9 clangd killed mid-preparation, U10 the server killed and started at once, U11 stale locks (dead pid, live pid, another host) and a truncated module file |
| `long` (`--expect-warm`) | the first one again | U13 ten idle minutes, U12 a `git checkout` twenty commits back and forth; nightly only |

Run one on a laptop-sized machine the way CI does (four vCPU, which `taskset` gives a bigger machine; the server and clangd
inherit the pin, and the preparation's parallelism follows it):

```bash
bin=target/<triple>/<fingerprint>/bin
ws=/tmp/ux/workspace
run() {   # run STAGE CACHE [runner options]
  stage=$1; cache=$2; shift 2
  taskset -c 0-3 $bin/mcppls-conformance run --server $bin/mcppls --payload editors/vscode/payload \
      --fixture conformance/fixtures/ux-mcpp --workspace-dir $ws --cache-dir /tmp/ux/$cache \
      --stage $stage --timeout 900 --measure /tmp/ux/ux-mcpp-$stage.json "$@"
}
run cold a; run warm a --expect-warm; run edits a --expect-warm; run faults b     # long: run long a --expect-warm
```

The budgets are the plan's initial values (6.4): they are tightened once rounds have been measured, and never loosened. A
failure names the check, the measurement and the budget it is over; the measure file has the rest.

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


`format-mcpp` checks the pinned mcpp fallback and project configuration override
through actual LSP formatting edits. It requires a payload with verified
`format-style-mcpp` capability. `format-explicit` checks explicit user fallback
and project precedence and also runs with stock clangd. `format-external` is run
with `--clangd` pointing to stock clangd and checks that it receives no bundled
preset. These fixtures disable the build tool deliberately: the inferred model
is usable and reports the expected degraded state for that choice.

`formatting-equals` applies returned TextEdits using UTF-16 positions, rejects
invalid/overlapping ranges, and compares the complete output against `expect`
or `expected-file`; an empty response cannot pass an unformatted golden.

The focused installed-VSIX counterpart runs with
`MCPPLS_E2E_SCENARIO=formatting MCPPLS_E2E_VSIX=<candidate.vsix> npm test`
in editors/vscode. It requires the maintained formatting capability and uses
three independent isolated workspaces for auto preset, explicit Google style
and project LLVM style. It compares the editor provider's full edits to the
same goldens and verifies that every workspace remains unchanged.
