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
| L3 | only `compile_commands.json` | the database plus scanning | arguments per file, module roles recovered by scanning, compilers probed |
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
`x86_64-linux-gnu`, `aarch64-macos` and `x86_64-windows-gnu`.

## 3. Robustness

A module that does not build, an engine that stalls, a missing SDK or a build tool that hangs must
cost precision, never the session.

**Principles.** A fault only affects where it is (P1). Missing parts are supplied, not units removed
(P2). Restarting clangd is the last resort, rate-limited and never oscillating (P3). Every request
has a deadline and a fallback; an unknown stall isolates one file, not the engine (P4). Compatibility
by construction, with the kit as fallback for a failing toolchain `std` (P5). Bounded resources (P6).
Visible degradation: the status says which modules, how many files, and why (P7).

**External programs** (build tools, compiler probes, CMake, git) all go through one runner: each run
gets its own process unit, soft and hard deadlines end the unit with everything it started, reads
are bounded, and every run is recorded (command, environment source, offline or not, duration,
outcome, tail of stderr). Build tools run with the login shell's environment on POSIX; clangd keeps
the editor's. Implicit runs are offline (`mcppls.buildTool = offline`); a run that needs a download
reports `producer-needs-download` with a "run it in a terminal" action.

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
| BD5 | Deadlines: producer soft 5 s / hard 60 s, toolchain probe 20 s, login shell 10 s, 10 s wait for a producer when nothing is cached |
| BD7 | The first configure of the private CMake build directory may download (FetchContent); later ones are disconnected |
| BD8 | No minimum-version table for build tools; a hang, timeout or download need suggests updating |
| T1 | Tooling: the server is not split internally; devtools depends on no server code and is the one entry for repository work (`mcpp run -p devtools -- ...`) |
| T3 | Cache inspection belongs to the server (`mcppls cache`), since only the server knows its cache layout |
| T5 | The specification schema check (`docs/specs/tools/validate.py`) is the one script kept, until a C++ JSON Schema 2020-12 validator exists |

## 7. Known limits

- The CLion plugin builds and installs but has not been exercised in a running CLion.
- clangd 23.1 rejects MSVC STL's aligned allocation; the plan turns aligned allocation off for
  units using MSVC STL (`msvcStlNeedsNoAlignedAllocation`) until upstream fixes it.
- An mcpp project built for Windows through openkal needs `--target x86_64-windows-gnu`, which no
  editor setting passes to mcpp yet.
- openkal cannot lower a child's scheduling priority, so clangd's cold-start module builds compete
  with the editor.

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
bounds (clangd `-j` = cores/4, preparation concurrency) · C8 degradation in the status · C9
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

**"tooling architecture".** 3.2 the workspace layout · 5.1 what mcpp, mcppls and devtools each do ·
5.5 how devtools finds the server it just built · M0–M6 its migration steps.
