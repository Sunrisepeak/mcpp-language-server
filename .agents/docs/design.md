# mcpp-language-server design

The one design record for mcppls: what it is for, how it is built, the decisions that shaped it, and
an index of the labels source comments cite ("usable plan W9.3", "robustness design C6", ...). User
documentation is in [`docs/`](../../docs/README.md); the normative parts are the specifications in
[`docs/specs/`](../../docs/specs/README.md).

## 1. Purpose

C++20 named modules are standard; the rest is not. BMI formats differ per compiler, dependency
scanning differs, and `import std` comes from somewhere else in each toolchain, so module code that
builds fine gives an editor nothing as soon as the compiler changes.

mcppls is a language server that fixes this at the one place it is broken: it turns any build —
mcpp, CMake, a bare `compile_commands.json`, or nothing — into one module description, drives a
pinned clangd with it, and answers the module-level requests clangd does not. It has no compiler
front end of its own and never builds the project.

The same facts serve three kinds of client through one kernel: people in an editor (LSP), coding
agents (MCP), and CI and scripts (the command line). It is a semantic fact, verification and review
layer — not an agent: no agent loop, no chat, no autonomous edits.

**Non-goals.** Replacing clangd's C++ semantics; building the user's project; talking to a network
from the server process.

**The bar every change is held to.** Two project shapes — all-`.cppm`, and interface `.cppm` with
implementation `.cpp` — keep working on Linux, macOS and Windows, with the conformance fixtures'
results unchanged.

## 2. Architecture

```
editor / coding agent / CI
        |  LSP, MCP, command line          (src/lsp, src/ai/mcp, src/cli)
   orchestrator  one workspace per root: model, plan, documents, status, snapshots
        |
   project       how is this built? mcpp, CMake, compile_commands.json, or scanning
   toolchain     what does that compiler do with modules? probed and cached
        |
   normalize     one engine plan: arguments every engine can take (S1 level 3)
        |
   engines       mcppls's own module engine, clangd beside it
```

- **Layers depend strictly downward**, and the repository is an mcpp workspace split by what each
  part links: `modules/base` (text, paths, errors) → `modules/platform` (the only place openkal is
  imported) → the server in `src/`. `modules/pack` (downloads, archives, payload assembly) is linked
  by `tools/devtools` only, so the server carries no TLS or archive code. `modules/os/{linux,macos,
  windows}` differ in six constants and nothing else. `mcppls-devtools check layers` enforces this.
- **Engine abstraction.** Every semantic answer comes from an engine that declares its capabilities;
  the orchestrator routes and merges. The mcppls engine (in process) answers module-level requests
  — module-name navigation, `import` completion, the module graph, unresolved / ambiguous /
  cross-module-partition diagnostics — from a syntax index it owns. clangd answers C++ semantics from
  a database mcppls writes for it. A clice engine was considered and is not implemented.
- **One process shape per client**: `mcppls serve` (LSP over stdio), `mcppls mcp` (MCP over stdio, or
  `--daemon` to share one workspace between several agents), and one-shot commands (`mcppls check`,
  `report`, `cache`, `symbol`, `review`, ...).

### 2.1 Where the module description comes from

| Level | Project | Source | Result |
|---|---|---|---|
| L1 | mcpp | `mcpp emit build-database`, run offline | every unit, its role, arguments and that toolchain's `std` (S1 level 3 after structuring) |
| L2 | CMake with `FILE_SET CXX_MODULES` | the build directory's database, or a private configure | the generator's answer, `@modmap` files expanded |
| L3 | only `compile_commands.json`, or xmake / meson | the database (or the one xmake or meson writes into a private directory) plus scanning | arguments per file, module roles recovered by scanning, compilers probed |
| L4 | sources only, or no usable compiler | scanning and the bundled semantic kit | modules resolve and `import std` works, with libc++ diagnostics |

An untrusted workspace is L4 by definition: no build tool and no compiler runs, and — the rule an
incident against a real project (openxlings/xlings) sharpened — nothing already on disk from a
trusted session, or from a build run outside mcppls entirely, is read either; a leftover
`compile_commands.json` is still a fact about a build, and reading it would make "untrusted" mean
"unless something is already sitting there".

This table's L1..L4 is `project.tier` (S3); the S1 profile's own 1..4 conformance level
(`project.level`, how completely a *document* is structured) answers a different question and
happens to share the same range — an mcpp project and a hand-written level-3 database are both tier
1, and a CMake project without `FILE_SET CXX_MODULES` is tier 2 even at level 1. The status bar and
the editor plugins show `L<tier>`, never `level`, because showing both as a bare number invited
reading one as the other.

**Model sources are ordered** cache > producer > inferred, each with an input fingerprint. A cached
model is used at once and confirmed in the background; a worse source never replaces a better result
or overwrites a better cache. Within "producer", the mcpp a project pins is not the only one asked:
an mcpp that cannot `emit build-database` is not the end of it if a newer one is installed elsewhere
on the machine (the xlings package store, mcpp's own registry store) and advertises the kind — that
one describes the project instead, read-only and offline the same way, while the project still
builds with the mcpp it pins (`mcppls.project.mcpp::other_mcpp_executables`). A database entry naming
a file that no longer exists is used for what remains rather than discarded outright, and a module a
dependency's build generates is looked for where builds leave it — the project's own build
directory, and mcpp's build-database cache, which survives a `target/` the project later deleted —
before an empty stand-in is created for it (`mcppls.project.generated`).

### 2.2 The payload

What ships is one payload per platform: the server, a trimmed and hash-pinned clangd 23.1, and the
semantic kit (libc++ headers and modules built for that target, spec S4), so `import std` resolves
with no usable compiler on the machine. `payload.json` records the size and sha256 of every file; the
server verifies them before use. The VS Code extension carries a payload inside its VSIX; Zed and
CLion start `mcppls` from PATH, or else from `<user data>/mcppls/payload`.

The whole product — server, tools, model gateway — is C++23 modules only, built by mcpp on openkal
(an LLVM runtime with its own libc and kernel layer), cross-built from one Linux host for
`x86_64-linux-gnu`, `aarch64-linux-musl`, `aarch64-macos` and `x86_64-windows-gnu`. The platforms a
release ships are the rows of `packaging/payload.lock.json`; `mcppls-devtools check platforms` holds
the extension, the release manifest and every per-platform CI job to them.

## 3. Robustness

A module that does not build, an engine that stalls, a missing SDK or a build tool that hangs must
cost precision, never the session.

**Principles.** A fault only affects where it is (P1). Missing parts are supplied, not units removed
(P2). Restarting clangd is the last resort, rate-limited and never oscillating (P3). Every request
has a deadline and a fallback; an unknown stall isolates one file, not the engine (P4). Compatibility
by construction, with the kit as fallback for a failing toolchain `std` (P5). Bounded resources (P6).
Visible degradation: the status says which modules, how many files, and why (P7).

**What clangd reads is not only what it is given** (0.0.5, `.agents/docs/2026-09-26-issue-23-fix-plan.md`).
clangd reads a file's imports from disk (UP-14 in issue #24), so every save and watched change is
checked: a file whose text on disk would stall clangd (a module name ending in `.`, an import the
database clangd has read has no unit for) is set aside before clangd builds it and handed back when
the disk or the database makes it safe. Every command clangd is given compiles only (`-c`), so no
link-phase check of the driver can fail its module scan (issue #23). Restarts have a budget per
cause (plan, recovery, crash) and are backed off past it, never refused (refining P3); a person's
own restart and a switch of toolchain, profile or context are never counted. A crash sets aside the
file clangd names in its crash context. With nothing cached, the build tool has 2.5 s to describe
the project; after that both engines serve it from its scanned sources (L4) while the build tool goes
on, and its model replaces the provisional one in one switch that is never counted against the budget
(0.0.6, revising 0.0.5's "clangd only with the build tool's model"). A report that a module has no
unit is weighed against the database clangd has actually read, and the kit replaces the toolchain's
`std` only when std's own unit fails to compile or does not exist (0.0.6).

**What clangd indexes is not what it can open** (0.0.6, `.agents/docs/2026-09-27-qt-demo-navigation-discovery-plan.md`).
clangd's background index compiles a module unit without building the modules it imports, so a
definition in an implementation unit is indexed apart from its declaration, or not at all, until the
unit has been open (WA-CLANGD-008). The server builds implementation units through clangd's
foreground, a few at a time -- the units of an opened file's module and of the modules it imports
first, a unit changed on disk again, the rest once clangd is idle -- and closes them; their symbols
stay in clangd's index. A definition request that still lands on a declaration in a module interface
searches the module's units by name.

**What the person feels comes first** (0.0.7, `.agents/docs/2026-09-30-stability-performance-plan.md`).
Scheduling, timeouts and restarts are decided by what the person in the editor notices: nothing
waits, nothing restarts under their hands, nothing flaps. A request they wait on has a budget for
clangd (completion and signature help 1 s, hover 2 s, definitions 10 s); past it mcppls answers
with what it has -- a completion the file's words as an incomplete list the client asks again for --
and clangd's request is cancelled, while clangd's own watchdogs still see it until clangd lets go.
clangd gets the threads but one (2 to 8, capped by memory); preparation takes every worker but one
while an open file waits on it and half otherwise; implementation units are built for the index one
at a time and only after 10 s without typing, opening or asking. A restart for the plan, or for work
clangd would not let go of, waits for typing to pause 3 s (60 s at most); a crash or a clangd that
answers nobody restarts at once. A timeout behind the server's own background work is not held
against the file. Conditions that pass by themselves turn the status degraded only after 30 s. A
clangd that keeps crashing is backed off (1, 2, 4, 8 minutes) instead of given up, and its crash
issues clear once it has stayed up a minute. Stale module locks are cleared before clangd starts and
when clangd says it waits on one another process holds. Each unit keeps the BMIs of its two newest
commands. `std` is built with only the standard library's own configuration macros, never the
project's (GalTranslPP's `_RANGES_` is MSVC STL's `<ranges>` header guard). The model cache keeps the
options derived from the database, so a warm start gives clangd the same commands and rebuilds
nothing (#30). The build tool's hard deadline follows how long it took last time (5 minutes the first
time, then three times the last run, 1 to 10 minutes), and a saved source reruns it only when its
module structure changes.

**Observability.** One occurrence must be enough to see why. clangd logs at `info` into a ring in
memory; crashes, stuck or spinning clangd, files set aside, backed-off restarts and a workaround
whose premise is seen broken each write an incident under the workspace's cache (the newest twenty,
for a week): what led up to it, clangd's log, each file's editor-versus-disk lines, their commands,
clangd's recent states per file, and which of its threads used the CPU. Every change of the engine
database is logged with what changed. A diagnostic bundle (`mcppls.exportBundle`, `mcppls report
--bundle`) zips the report, environment, logs, incidents and engine database, with the user's home,
names and secrets replaced, and is not written at all when any is left; it is never uploaded. What
nothing recovers from by itself (a crash loop, an engine that cannot start or run, a corrupt payload,
stalled preparation) writes one at once (`<cache>/bundles/auto-<code>-<time>.zip`, the newest five),
named in the status issue's `bundle` (S3-4-22); the log and the editor say where it is, how to report
it, and that turning mcppls off for the workspace (`mcppls.enable`) works around it meanwhile.

**External programs** (build tools, compiler probes, CMake, git) all go through one runner: each run
gets its own process unit, soft and hard deadlines end the unit with everything it started, reads
are bounded, and every run is recorded (command, environment source, offline or not, duration,
outcome, tail of stderr). Build tools run with the login shell's environment on POSIX; clangd keeps
the editor's. Implicit runs are offline (`mcppls.buildTool = offline`); a run that needs a download
reports `producer-needs-download` with a "run it in a terminal" action and, for a client that knows
`askOnline`, the offer to fetch it once with the network (`mcppls.describeOnline`). The offer never
blocks anything; the offline description is asked again on a backoff and whenever a watched input
changes, so a build the person runs in their own terminal upgrades the project by itself. Build
systems are providers behind one interface (detect from files, read existing output, describe into a
private directory offline or online); none writes into the workspace. Every configurable behaviour is
one row of `src/config/settings.cppm`, from which the command line, `initializationOptions`,
`workspace/didChangeConfiguration`, the report and `docs/30-settings.md` are derived.

## 4. AI-facing capabilities (`src/ai/`)

Query (symbols, references and callers followed through imports), context packs, verification of an
edit against fresh diagnostics, and review. **Review is evidence-driven**: deterministic findings
first (semantic diff, impact, module rules, diagnostics, cross-compiler results); a model's finding
must cite that evidence, and a fix is verified before it is shown. Model access is optional, off by
default and never from the server process: `tools/model-gateway` (`mcppls-model`) is the only program
that reaches a model endpoint. Results are specified as S5.

## 5. Specifications and evaluation

Five specifications, versioned apart from the product, with rule identifiers (`S1-6-1`, ...) mapped
to evidence in `conformance/traceability.json`: S1 build database, S2 discovery, S3 LSP extensions
(`cxxModules/status`, `report`, `setContext`, ...), S4 semantic kit, S5 semantic queries.

Three kinds of evidence: conformance fixtures (the facts are right, against real toolchains on three
hosts), the review fixtures (precision and recall per rule), and the agent task benchmark
(`tools/bench`). A pre-release workflow runs all of CI, installs every editor plugin from the release
archives, holds startup medians to a budget and repeats the key fixtures three rounds in a row
before anything is published (`docs/92-release.md`).

## 6. Decisions

| # | Decision |
|---|---|
| D26 | MSVC STL `import std` is in scope for the first version (clang-cl and MSVC projects on Windows) |
| D27 | Pick Visual Studio automatically on Windows for a project with no build system |
| D29 | Names: project `mcpp-language-server`; executable, modules, settings prefix `mcppls`; extension `sunrisepeak.mcpp-language-server` (the Marketplace reserved `sunrisepeak.mcppls` after its removal); kit package `mcppls-kit` (`docs/91-naming.md`) |
| D30 | The engine abstraction: all semantics come from engines; mcppls's own engine is first class from day one, clangd supplies C++ core semantics |
| D31 | The kit is its own package, versioned as the libc++ of the engine it serves, selected by exact engine version |
| D33, D34 | AI modules live in `src/ai/`; the direction is review, not code generation; no agent is implemented |
| D35 | One kernel, three entry points (LSP, MCP, command line) |
| D36 | Evidence-driven review |
| D37 | Model access through replaceable sources, off by default; the server process never touches the network |
| D38 | Several clients: coordinate instances per workspace first, then a workspace daemon |
| D39 | Agent queries and review results are specified (S5) |
| BD1 | `mcppls.buildTool` defaults to `offline` |
| BD2–3 | Build tools get the login shell's environment on POSIX; clangd keeps the editor's |
| BD4 | A cached model is used immediately and confirmed in the background |
| BD5 | Deadlines: producer soft 5 s / hard 5 minutes the first time, then three times the last run, 1 to 10 minutes (60 s fixed until 0.0.7, plan 2026-09-30 G-4); toolchain probe 20 s, login shell 10 s, 2.5 s wait for a producer when nothing is cached (10 s until 0.0.6, plan 2026-09-27 D5) |
| BD7 | Withdrawn in 0.0.6 (plan 2026-09-27 D1): every configure of the private CMake build directory is disconnected (`FETCHCONTENT_FULLY_DISCONNECTED`); fetching is the person's choice (BD9) |
| BD8 | No minimum-version table for build tools; a hang, timeout or download need suggests updating |
| T1 | Tooling: the server is not split internally; devtools depends on no server code and is the one entry for repository work (`mcpp run -p devtools -- ...`) |
| T3 | Cache inspection belongs to the server (`mcppls cache`), since only the server knows its cache layout |
| T5 | The specification schema check (`docs/specs/tools/validate.py`) is the one script kept, until a C++ JSON Schema 2020-12 validator exists |
| RD1 | Revised in 0.0.6 (plan 2026-09-27 D5): past the first 2.5 s clangd serves the scanned-sources model too, and the build tool's model replaces it in one switch that is never counted against the restart budget |
| RD2 | clangd's upstream defect behind `import a.` (UP-01) is worked around, not fixed in a clangd of our own, until upstream settles (D2) |
| RD3 | clangd logs at `info` into memory; incidents on disk; nothing ever uploaded (D3, F17, F18) |
| RD4 | The space is a completion trigger only after `import `, dropped by the editor elsewhere, and advertised only to clients known to drop it (D4) |
| RD5 | Reports and bundles are redacted by default; a bundle with anything left is not written (F18) |
| RD6 | Restarts are budgeted per cause and backed off past it, never refused; the person's restart is never counted (F14). Extended in 0.0.7 to crashes: five exits in five minutes back clangd off for 1, 2, 4, then 8 minutes instead of giving it up (plan 2026-09-30 K-2) |
| BD9 | A download the build description needs is fetched only when the person accepts, once, in a notification that never blocks; the server retries offline on its own meanwhile (plan 2026-09-27 D2, §9.2) |
| BD10 | Build systems are `BuildSystemProvider`s (mcpp, CMake, xmake, meson, compile-commands); `mcppls.buildDiscovery` turns detection off, `buildDiscovery.providers` chooses them (plan 2026-09-27 B-1, B-7) |
| RD7 | The kit replaces the toolchain's `std` only when std's own unit fails to compile or the plan has none; a report about a database clangd has not read is ignored (plan 2026-09-27 D4', Q1-1, Q1-4) |
| RD8 | Generated output a producer only names in its private planning directory is read, read-only, from the project's own `target/`; missing, it is reported with a build action and watched for (plan 2026-09-27 Q1-3, mcpp-community/mcpp#724) |
| RD9 | Implementation units are built through clangd's foreground for its index (WA-CLANGD-008), until clangd's background index builds a module unit's imports (plan 2026-09-27 N-7); revised in 0.0.7: one at a time and only in the person's idle time (plan 2026-09-30 R-5) |
| RD10 | Every configurable behaviour has one definition, the registry in `src/config/settings.cppm`; command line, `initializationOptions`, `didChangeConfiguration`, report and the settings chapter are derived from it and held to it by a test (plan 2026-09-27 T1) |
| UD1 | Performance trade-offs are decided by what the person feels (fast, unnoticed), and gated by user-experience scenarios with response budgets on pinned mcpp and xlings checkouts (plan 2026-09-30 D3, D5) |
| UD2 | A request a person waits on has a budget for clangd; past it mcppls answers with what it has, marked incomplete where the protocol allows, and clangd's request is cancelled (R-7) |
| UD3 | Background work (N-7, preparation of what no open file waits on) uses idle time only and yields to the person; restarts nothing needs at once wait for typing to pause (R-1, R-5, R-8) |
| UD4 | clangd's workers: the threads but one, 2 to 8, capped by memory, `mcppls.engine.workers` overrides; revises robustness design C7's cores/4 (R-2) |
| RD11 | `std` is built with the standard library's own configuration macros only; the project's `-D`s never reach it (plan 2026-09-30 G-1) |
| RD12 | Module locks in the cache are the lease holder's: all are cleared before clangd starts, and one another process holds is removed when clangd waits on it (C-4) |
| RD13 | Each unit keeps the BMIs of its two newest commands; the rest are pruned when clangd starts (`mcppls cache --prune` for the others) (C-2) |
| RD14 | What nothing recovers from by itself writes a redacted bundle at once and names it in the status; the person reports it, restarts, or turns mcppls off for the workspace (`mcppls.enable`), and nothing is uploaded (K-7) |
| PD1 | Android under Termux (PRoot) is a supported platform: openkal-linux falls back from `execveat` to `execve` and the server detects the sandbox (plan 2026-09-30 D1, X-1..X-5) |
| UD5 | In VS Code, C and C++ open the completion list while you type, alongside inline completions: the extension contributes `editor.quickSuggestions` `{other: "on"}` as their language default (WA-VSCODE-002), since VS Code 1.125's own default waits for inline completions; a person's `[cpp]` / `[c]` value wins, and one set for every language is overridden and told in the log (plan 0.0.8 E-1, E-2) |
| RD15 | Containment (RP1.1) takes a failed module's importers from clangd, never the unit whose compile failed: clangd reads it from the editor, with its real errors and completion (plan 0.0.8 M-1); a completion clangd does not answer gets the file's words, never nothing (M-2) |
| RD16 | On the provisional model only the standard library is prepared; the project's modules wait for the build tool's commands (plan 0.0.8 R-4). The modules whose units clangd could not scan are taken together, one replan and one restart (P-1) |
| RD17 | A unit opened in clangd to prepare a module is closed as soon as the module is built; its BMI stays in clangd's module cache on disk. Held open, clangd re-checked every one of them on each save (plan 0.0.8 part 2 C-1) |
| UD6 | A completion clangd answers after its budget is not thrown away: the requests the same word makes meanwhile wait for it, and it goes to them with its edits ended at each cursor; a request in another word cancels it (plan 0.0.8 part 2 C-2) |
| RD18 | Module units whose build names no C++ standard are read as C++23, the standard `import std` is for, whatever the compiler's default; a standard any module unit names is followed instead. Other units that name none are read with their build compiler's own default where Clang's differs (GCC 16: gnu++20). Module units below C++20 are said once (plan 0.0.8 part 2 X-3) |

## 7. Known limits

- The CLion plugin builds and installs but has not been exercised in a running CLion.
- linux-arm64 ships LLVM's own clangd build, which needs glibc 2.34 and a GCC 12 libstdc++ (Ubuntu
  22.04+, Debian 12+, openEuler 24.03+); elsewhere the status says `engine-incompatible` and only
  module-level features remain. A clangd built for a lower floor is the way out, if those systems
  turn out to matter (0.0.3 plan §5.2).
- clangd 23.1 rejects MSVC STL's aligned allocation; the plan turns aligned allocation off for
  units using MSVC STL (`msvcStlNeedsNoAlignedAllocation`) until upstream fixes it.
- Every compensation for a clangd defect is a registered workaround (`WA-CLANGD-<n>`,
  `src/engine/clangd/workarounds.cpp`, import-hang plan §9); `mcppls report` lists the ones in use.
- clangd 23.1 (and main at 510126255) never finishes a file in which `import std;` comes before an
  `export import` of something nothing provides, under a command that names `std`'s unit. Only a file
  outside the database gets such a command, the one clangd interpolates from its nearest unit, so
  only a stray file with ill-formed code reaches it. The first-diagnostics guard sets it aside after
  two minutes (import-hang plan §13).
- clangd 23.1 reads an open file's imports from disk (UP-14): an import typed and not saved is not
  built until the save (told as information, `WA-CLANGD-007`), and a file whose disk text would stall
  clangd is answered by mcppls's own engine until it is saved again (fix plan F16). The upstream fix
  — scanning with the editor's buffer — is tracked in issue #24.
- clangd cannot change its log level while it runs; an incident carries its `info` log, and the
  `verbose` one needs the server started at `--log-level debug` (in VS Code, the trace setting at
  `verbose`), a restart away.
- An mcpp project built for Windows through openkal needs `--target x86_64-windows-gnu`, which no
  editor setting passes to mcpp yet.
- openkal cannot lower a child's scheduling priority, so clangd's cold-start module builds compete
  with the editor (plan 2026-09-30 R-9, deferred).
- Windows gives no CPU reading of clangd yet (plan 2026-09-30 K-1, deferred), so the stuck-clangd
  watch decides nothing there; crashes are not symbolized (K-3, deferred).
- openkal-linux is vendored (`vendor/openkal-linux`, 0.15.1 plus the `execveat` fallback) until the
  fix is released upstream.

## 8. Label index

Source comments cite the working documents this record replaces. Their labels map as follows.

**"design N" — the first design (sections).** 11 architecture and process model · 12 server:
modules, command line, concurrency (12.3 threads with blocking reads), openkal platform layer (12.4),
session state machine (12.5), routing (12.6), openkal defects K1–K15 (12.9) · 13 flows: startup,
open, edit and save, context switch, no compiler (13.5), faults (13.6) · 14 project model and
normalization: detection (14.1), mcpp as first producer (14.2), toolchain probing (14.3),
normalization rules (14.4) · 15 engines and payload: clangd adaptation (15.1), payload contents
(15.2), locating it (15.3), state directory (15.4) · 16 VS Code extension (16.5 conflicts) · 17
distribution · 18 logging, configuration and security (payload integrity, trust) · 20 build, test
and release (20.2 test tiers, clean machines).

**"usable plan W" — the first usable version.** W1 MSVC family and MSVC STL `import std` · W2
automatic Visual Studio · W3 mcpp as producer (`emit build-database`, S2 0.2) · W4 CMake build
database · W5 clean machines and first use (W5.4 the macOS SDK prompt) · W6 no unasked UI, and
testing the installed VSIX · W7 performance, cold and warm start (SC4: a warm start reuses `std`) · W8
self-hosting and large projects · W9 server completeness: W9.1 multi-root, W9.2 contexts and several
targets, W9.3 file-watch fallback, W9.4 payload integrity, W9.5 the engine interface · W10
specification traceability · W11 cross-built binaries verified natively. E1–E19 are its probe
results; U1–U10 and SC1–SC8 its acceptance scenarios and criteria.

**"overall design N" — the engine and AI design.** 4 architecture (4.4 process shapes) · 5 the
engine layer (5.2 routing and merging, 5.3 mcppls engine, 5.4 clangd engine) · 6 orchestration (6.2
snapshots, 6.3 several clients) · 7 the AI layer: 7.1 query, 7.2 context, 7.3 verify, 7.4 review, 7.5
model access, 7.6 MCP, 7.7 editor features · 8 entry points (8.2 agents) · 9 specifications · 10
evaluation (10.1 conformance, 10.2 agent benchmark, 10.3 review fixtures, 10.4 performance) · 11
security and privacy · 12 the rename.

**"robustness design".** C1 language-correct drivers, `std` taken from a C++ unit · C2 stand-in
modules for anything nobody provides · C3 failure kinds, remembered per provider fingerprint · C4 the
restart policy · C5 the kit as `std` fallback · C6 per-file watchdog and set-aside · C7 resource
bounds (clangd `-j` = cores/4 until 0.0.7, then UD4; preparation concurrency) · C8 degradation in the status · C9
observability · C10 definitions in implementation units. O1 the event timeline · O2 persistent logs ·
O3 the diagnostic report (`cxxModules/report`, `mcppls report`) · O4 what the editor shows.

**"build description design".** 4.1 model sources and state · 4.2 the external program runner · 4.3
the tool environment · 4.4 network · 4.5 what the user sees · 4.6 observability. Decisions BD1–BD9
above.

**"cold-start plan".** 4.1 standard `$/progress` for every client · 4.2 the status bar · 4.3 hover on
symbols not ready yet · 4.4 cold-start concurrency · 4.6 missing or old mcpp · 4.8 stand-ins for
modules that fail to compile · 4.10 clangd progress that starts and stops · 4.11 one C++ engine per
file.

**"real-project plan RP" — [2026-09-22-real-project-experience.md](2026-09-22-real-project-experience.md).**
RP0 real-project stress testing (the `stress` check, client profiles, the old-mcpp mock, generated
and failing-base fixtures, pinned real projects, `devtools stress`, real editors, CI) · RP1.1 a
failed module's import closure is answered by mcppls's own engine · RP1.2 no global recovery for a
local fault, restarts capped · RP1.3 doomed modules are not prepared · RP1.4 the status settles ·
RP1.5 resource budgets · RP2.1 producer negotiation · RP2.2 a worse model never replaces a better
one · RP2.3 generated-source recovery before a stand-in · RP2.4 stale databases · RP3.1 an untrusted
workspace is L4 · RP3.2 one vocabulary (`project.tier`) · RP3.3 log severities · RP3.4 a nested
project is not folded into its parent.

**"0.0.3 plan" — [2026-09-24-0.0.3-plan.md](2026-09-24-0.0.3-plan.md).** B1 a clangd that dies
before its handshake still lets the server initialize; a loader's refusal is `engine-incompatible`,
not a crash · L the icon · S being found as mcppls · P Open VSX from CI, after local verification · A
Linux arm64 with the official LLVM clangd · X one platform table.

**"import-hang plan" — [2026-09-25-import-hang-status-highlight.md](2026-09-25-import-hang-status-highlight.md).**
§1 clangd 23.1 spins on a module name ending in `.` at the end of its line · §2 why the guards did
not see it · §3 WA-CLANGD-001, the same-line `;` · §4 the spin guard, a build's budget from its own
history · §5 no stand-in for an import still being typed · §6 status issue categories, the degraded
hold · §7 module syntax colored by an injected grammar and by the server's semantic tokens · §9 the
workaround registry and its canaries · §10 living with other C++ extensions.

**"plan 2026-09-30" — [2026-09-30-stability-performance-plan.md](2026-09-30-stability-performance-plan.md).**
X-1..X-5 Termux / PRoot (#32) · W-1..W-5 the warm start that rebuilt every BMI (#30) and other command
churn · G-1 `std` and the project's macros · G-2 JSON `{…}` errors on GalTranslPP · G-3 slow and
"dying" completion · G-4 the adaptive producer deadline · G-5 edits that reran the build tool · C-1
reset the workspace's cache · C-2 BMI pruning · C-4 stale module locks · C-5 a lease that knows its
owner · R-1 the background budget · R-2 clangd's workers · R-3 timeouts behind background work · R-5
N-7 in idle time · R-6 stand-ins that are no change · R-7 request budgets · R-8 restarts at idle · K-2
crash loops backed off · K-4, K-5 guards that misjudged a busy clangd · K-6 the passing-degraded hold ·
K-7 the automatic bundle · U1–U15 user-experience scenarios on mcpp and xlings · O-1..O-6 Code - OSS
and the extension.

**"plan 0.0.8" — [2026-09-30-0.0.8-plan.md](2026-09-30-0.0.8-plan.md).** E-1..E-4 the completion list VS
Code kept closed for inline completions (WA-VSCODE-002) · M-1 the unit that fails keeps clangd ·
M-2 completion always has the file's words · M-3 no preparation stall while its module is edited · M-4 U16
writing a module with autosave · K-3 crash evidence for upstream · R-4 no project preparation on the
provisional model · P-1 stand-ins together · K-6 the settled-preparing hold · O-6 the Open VSX listing wait.

**"plan 0.0.8 part 2" — [2026-10-01-0.0.8-part2-plan.md](2026-10-01-0.0.8-part2-plan.md).** C-1 prepared units
close at once · C-2 late completions reach their word · C-4 slow files and build times in the report · X-1..X-7 xmake
followed without touching the project · X-3 the build compiler's own standard · X-4 a standard library unit that does
not scan · I-1 include cleaner stays on · CL-1..CL-4 CLion 2026.2.3, one engine per file · Z-1..Z-3 Zed in CI.

**"tooling architecture".** 3.2 the workspace layout · 5.1 what mcpp, mcppls and devtools each do ·
5.5 how devtools finds the server it just built · M0–M6 its migration steps.
