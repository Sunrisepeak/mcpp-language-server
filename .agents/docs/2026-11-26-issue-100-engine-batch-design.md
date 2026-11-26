# Issue 100 and one engine batch: MS inline assembly in module builds, with the engine items that ride the same rebuild

Status: **design, review decisions recorded (§9), nothing implemented** · 2026-11-26 · issue [#100](https://github.com/Sunrisepeak/mcpp-language-server/issues/100)
(32-bit Windows target, `<Windows.h>`, "MS-style inline assembly is not available: target MC unavailable") ·
related: #24 (register), #47, #50, #52, #66, #77, mcppls-clangd#19 · evidence: the clang/clangd 23.1.0 sources in
`~/workspace/engine-local/src`, the mcppls-clangd series (patches 0001-0048), the bundled engine
`23.1.0-mcppls.5` run locally, the issue text.

Decisions taken (2026-11-26): fix **B** below (not linking the target's MC); carry as many of the open
engine/server items as need the same engine rebuild; Linux first, locally, then CI on the other platforms. The
answers to the review questions are in §9.

## 0. Summary

| # | Item | Where | Disposition |
|---|---|---|---|
| E1 | **#100**: no MC for `__asm`, and a compile with an error writes no BMI | engine, new patch 0049 | **fix: link and register the x86 MC and asm parser** (§2, revised from B) |
| E2 | mcppls-clangd#19: the ok image bounds a worker's diagnostics on no system but native Linux | engine, openkal series | **fix**, depends on a spawn capability to be confirmed (§3) |
| E3 | #47 one failed module blanks all completions | engine (0028 exists) | **verify on 0.24.x, then close or reduce** (§4) |
| E4 | #50 first completion waits for the first preamble | engine (0027, 0030, 0034, 0035 exist) | **verify on 0.24.x, then close or reduce** (§4) |
| E5 | #52 items 1, 3, 6, 7 (missing prerequisites, Windows warm start, Windows heavy-header completion) | engine | **re-measure first**; only what a measurement still shows (§5) |
| S1 | #66 CI absolute paths in the release payload binaries | server release build | **fix**, build flags only (§6) |
| S2 | #71 fallback and refresh through mcpp's result record, #52 items 2, 4, 5 | server | **not in this batch** unless the baseline shows one is a speed cause (§6) |
| P | speed: #77 residuals, image-vs-native interactive latency | engine + server | **best effort, non-blocking; hard rule: no regression against 0.0.23 / 0.24.1** (§5) |

Not carried: #59 (nightly CI jobs), #48 (a Windows environment report), #44 (an enhancement), #77 itself (a
tracker; stays open).

The one rule of the batch: **an item is fixed only when its failure has been reproduced on the unmodified engine
first.** Several of these issues were written against 0.0.11-0.0.12; the series has patches since that answer them
(0027, 0028, 0030, 0034, 0035), and re-fixing a solved problem would only add risk to a release that exists for #100.

## 1. What #100 is, and what it is not

**The report's diagnosis is not what happens.** It says `clangd.okb` lacks the X86 target and asks for `X86` in
`LLVM_TARGETS_TO_BUILD`. The engine already builds `X86` (and `AArch64`): `ci/scripts/configure.sh`
(`host;X86;AArch64`), `openkal/okgen/src/main.cpp:202` (`X86;AArch64`). The error is raised for another reason:

1. clangd calls `llvm::InitializeAllTargetInfos()` and nothing else (`ClangdMain.cpp:860`). `__asm { }` is parsed
   with the target's MC (`ParseStmtAsm.cpp:512-549`): `lookupTarget` succeeds (infos are registered),
   `createMCRegInfo` returns null (no MC registered), and clang reports `err_msasm_unable_to_create_target` with
   the text "target MC unavailable".
2. **Upstream knows and filters it.** `clangd/Diagnostics.cpp:91-96` (`isExcluded`): "clang will always fail
   parsing MS ASM, we don't link in desc + asm parser" drops `err_msasm_unable_to_create_target` and
   `err_msasm_unsupported_arch` from what the editor sees; `DiagnosticsTests.cpp:1215` (`ClangdTest.MSAsm`) pins it.
   After the error clang builds an empty `MSAsmStmt` (`EmptyStmt()`, line 532/550) and carries on: the AST is whole.
3. **Module builds do not use that path.** Their gate is the error count itself:
   - the supervised worker (patch 0017): `ModuleBuildWorker.cpp:955-958`, `ExecuteAction() ||
     hasErrorOccurred() || !exists(Output)` -> "module worker compilation failed". `ExecuteAction` already returns
     `!getClient()->getNumErrors()` (`CompilerInstance.cpp:1066`), and the client is the plain
     `TextDiagnosticPrinter` (line 864), which counts the diagnostic;
   - the in-process fallback (`ModulesBuilder.cpp:1475`, `hasErrorOccurred()`), the path stock clangd also takes.

   So a diagnostic the editor never sees is a build failure for a BMI, and every module that imports it fails with
   "prerequisite BMI compilation", which is the cascade of the report.
4. Only `_M_IX86` triggers it: `winnt.h` 1018-1103 (`Int64ShllMod32` etc.) uses `__asm` there, plain C shifts under
   `_M_AMD64` (the report's workaround). Any module that includes `<Windows.h>` for a 32-bit MSVC target, and any
   hand-written `__asm` block in a module unit, hits it.

**Evidence run today** (bundled `23.1.0-mcppls.5`, Linux): a function with `__asm { mov ecx, s; shl dword ptr v, cl }`
checked as `--target=i686-pc-windows-msvc -fms-extensions` through `clangd --check`: 0 errors (the AST path
filters). A control `int y = ;` in the same file: 1 error, so errors do print. **Not yet reproduced: the worker
failure itself.** Phase 1 does that on the unmodified engine, on Linux, with no Windows SDK (the failure needs only
the triple and an `__asm` in a module unit).

**Probable upstream defect** (UP-42 to be registered on #24, unfiled): stock clangd's in-process module builder has
the same gate and no filter. To be confirmed by the same reproduction against an upstream 23.1.0 `clangd`.

## 2. E1 -- the fix

**Revised during implementation (2026-11-26): B does not work; the fix is A, x86 only.** The text below keeps what
the review decided and what the source then showed.

*What was decided (§9.1): B* -- module builds ignore the two MS-asm diagnostics the way the editor path does, with no
link change. *Why it cannot work:* `PCHGenerator::HandleTranslationUnit` (`clang/lib/Serialization/GeneratePCH.cpp:74`)
writes no module interface when `PP.getDiagnostics().hasErrorOccurred()`, unless the compile allows AST with
errors. That flag lives in the compile's `DiagnosticsEngine`, set when the error is *reported*, before any consumer
sees it; a consumer that swallows the diagnostic does not clear it. The editor path never writes a BMI, so its filter
(`isExcluded`) is enough there. The other way to keep the diagnostic from being an error is to remap it, and
`DiagnosticsEngine::setSeverity` forbids that (`Diagnostic.cpp:358`: an error can only map to error or fatal; an
assertion build would abort). So the diagnostic must not be *raised*: the target's MC has to be there.

**Change (patch 0049, UP-42).**
- `clangd/CMakeLists.txt`: when `X86` is among `LLVM_TARGETS_TO_BUILD`, clangDaemon links `X86Desc` and
  `X86AsmParser` and publishes `CLANGD_X86_MS_ASM=1`. Only x86: MS inline assembly is x86 only, and the image's link
  line (okgen takes it from the build) gets two libraries, not four targets' worth.
- `tool/ClangdMain.cpp`: after `InitializeAllTargetInfos()` and **before** the `--module-build-worker` branch,
  `LLVMInitializeX86TargetMC()` and `LLVMInitializeX86AsmParser()`. Every module build, in the supervised worker and
  in process, then parses `__asm` as clang does, so a unit builds exactly where the project's own compile does.
- `Diagnostics.cpp`: `isExcluded` stays (it still hides the diagnostic for a target without MS assembly, and in a
  build that does not link MC); its comment is corrected.

**What changes for the editor.** The AST path now parses the assembly too, so a genuine mistake inside an `__asm`
block can be reported as the real compiler reports it; before, the whole block was silently accepted. That is
clang's own behaviour (the project's build reports the same), not a new class of false positive, and it is stated in
the CHANGELOG. Cost: two more LLVM libraries in `clangd` and the image (X86 MC tables and the asm parser; measured in
T6 as the size delta of the binary and the image).

**Evidence.** The unmodified engine, Linux, a module with an `__asm` block built for `i686-pc-windows-msvc`:
supervised worker: "a.cppm:3:3: error: MS-style inline assembly is not available: target MC unavailable ... module
worker compilation failed"; `--modules-builder-worker-policy=in-process` (the path stock clangd takes): "Failed to
compile a.cppm" -- the same message stock gives, so UP-42 is probably an upstream defect, unfiled. Both ended with
`module 'a' not found` in the importer.

**Tests.** `PrerequisiteModulesTest.cpp`, class `MSAsmModulesTests`: an `__asm` module and a module that imports it
build in process; a module with an `__asm` block and a real error still fails; the supervised worker's entry
(`runModuleBuildWorker`) writes the staged interface. The conformance fixtures of the product repository cover the
rest (§7, T7): a portable `i686-windows-msvc` module with `__asm` on every system, and the real `<Windows.h>` on
Windows.

**Ledger.** New patch `0049-UP-42-ms-asm-in-module-builds.patch`, row UP-42 (`patches/PATCHES.md`, the register on
#24). A new patch rather than an edit of 0017: 0017 is `steady`.

**Original B text, kept for the record:** the predicate would be exported and applied on the compile's own engine
after `prepareCompilerInstance`; untouched in either case: `ProjectModules.cpp:524` (the P1689 scan preprocesses and
never parses a body) and the importer's own preamble (no error gate in `Preamble.cpp` or `PrecompiledPreamble.cpp`:
a plain `.cpp` that includes `<Windows.h>` for i686 already works, which is why the report sees only module builds
fail).

## 3. E2 -- bounded worker diagnostics on the ok image (mcppls-clangd#19)

**Problem.** Native Linux owns a FIFO for a worker's stderr and drains a 64 KiB prefix (0017 `WorkerDiagnostics`,
`#ifdef __linux__`). `clangd.okb` is built for openkal, not `__linux__`, so on Linux it takes the portable
transport: a regular file read with a bounded read. A worker writing without bound fills the disk before it ends.
The case `module-worker-diagnostics` (`tests/cts/module_workers.cpp:247`) proves it with an 8 MiB shell stand-in
(a replaced worker binary, so only the **transport** can bound it, not the worker's own printer).

**Design.** Replace the file with an anonymous pipe on every system, drained with the same bounds (16 x 4096 per
poll, 64 KiB prefix, truncation marker). `mkfifo` is not provided by openkal; an anonymous pipe needs none.

**To confirm before writing code (this decides the scope):** that openkal's process spawn, and the native
`llvm::sys::ExecuteNoWait` on Windows, can take a pipe as the child's stderr. If not on some system, the fallback
named in the issue applies there: a size bound on the file (the supervisor kills the worker when the file passes the
bound, as its output limit already does, `OutputLimitBytes`). If neither is feasible on a system, E2 is dropped from
the batch **for that system** (decision, §9.2) and the batch does not wait for it. The drop is written down in three
places so it cannot be forgotten: a comment at `WorkerDiagnostics` naming the systems that keep the regular-file
transport and why; the patch ledger row (`patches/PATCHES.md`, and `ci/module-worker-isolation.md` where it already
says "their diagnostic disk bound remains unqualified"); and a comment on mcppls-clangd#19, which stays open for them.

**Tests.** The existing case, with `cts::needs { "linux", "native" }` lifted to every subject and its assertions made
transport-neutral (bytes on disk bounded, prefix kept, marker present, parent answers).

## 4. E3, E4 -- items that may already be solved

Both issues were written against 0.0.11/0.0.12. The series has since gained:

- **0027** bounded wait: completion in a module importer answers within a bound while the first preamble or a new
  import is still built (this is #50's improvement 1).
- **0028** partial prerequisites: an importer is served with the modules that did build when one fails (the ask of
  #47).
- **0030** scan and validation memos kept across sessions (improvement 2 of #50); **0035** interactive slots apart from
  AST and preamble builds (improvement 3, the edited file first, in part); **0034** preamble loading for importers.

So these two are a **verification task, not a design**: reproduce each issue's scenario on the 0.24.x payload
(`ux-xlings` U5 for #50; a project with one unbuildable module, which E1's own test can supply, for #47). Outcomes:
solved -> close with the evidence; partly -> reduce the issue to the residual and add it to §5; unsolved -> becomes a
design of its own and leaves this batch (it needs more than a rebuild).

Interaction with E1: #100 *is* the "one failed module cascades" shape of #47, through a cause the filter removes. E3's
check also asserts that a **genuinely** failing module (not asm) degrades to 0028's partial service, which keeps the
class of failure covered after E1 removes this instance.

## 5. E5 and P -- speed, measured before anything is changed

What the sources already say (not re-measured by this document):

- #77: Linux x64 cold CPU of the image is 1.09-1.11x native (within 1.15); Linux ARM64 1.05-1.07; macOS cold
  readiness has an outlier history and invalid-by-spread runs; **Windows cold readiness 1.46-1.69x native**, CPU
  1.19-1.32x (a consequence of the owned cache and supervised workers against in-process builds, disclosed in the
  release policy). A traced pair showed module validation 350 ms native against 828 ms image with 405-421 ms of
  foreground run-queue delay: **one diagnostic observation of a possible scheduling issue**, not an estimate.
- #52 items 1/3/6/7 predate patches 0025-0035 and the owned cache; their present size is unknown.

**Policy (decision 9.4).** Speed is **best effort and never blocks the batch**; the one hard requirement is **no
regression against the previous release** (engine 0.0.23 / server 0.24.1). Optimising is welcome where it is cheap
and shown; it is not a condition of shipping #100.

**The Phase 1 baseline is therefore also the regression reference**: the unmodified engine, measured once on this
machine, is what Phase 2's build is compared with, quantity by quantity.

| Quantity | Tool | Compare |
|---|---|---|
| first completion in a just-opened importer; completion p95 while typing; cold/warm stages | `mcppls-bench` `ux-xlings` (U5, `tests/bench/ux.cpp`, `linux-x64`), `ux-heavy-headers` | native vs ok image; before vs after |
| cost of one completion by phase | `clangd --check --check-completion` (UP-36 cost line), `CLANGD_TRACE` spans | native vs image; before vs after |
| where CPU goes | gdb SIGINT sampling (perf is off here) | only if a gap shows |
| cold/warm readiness and CPU | the warm-prime gate's own meter | image vs native, `taskset -c 0-3` to approximate a 4-vCPU runner |

**Regression check (hard).** After Phase 2, each row is measured again, twice. A quantity regresses when it worsens
beyond the spread of the two baseline repetitions (the release policy's own rule: a measurement whose spread is out of
bound is invalid, not a pass). A regression is fixed or its change removed from the batch; it is not shipped. E1 touches
only module-build verdicts and cannot move a completion; the check exists for what Phase 2 adds beside it.

**Optimisation (soft).** A change is added when the baseline shows a gap above noise, a trace or sample locates it to
a span or function, and the fix is smaller than its evidence. Candidates that exist today only as hypotheses: the
image's foreground run-queue delay (the worker pool's priority against the interactive thread), and the validation
memo for module importers. If Phase 1 finds none that meets this, the batch ships without a speed change and says
so; the findings go to #77 for the next batch.

Windows items (cold readiness, warm start, heavy headers) cannot be measured on this machine. They go to CI as a
measurement run on the engine PR (Phase 3); anything found there is the next batch's input, not this one's.

## 6. Server side

- **S1 (#66) -- dropped from this batch (2026-11-26).** The paths that leak are those of the dependency packages
  (`<home>/.mcpp/registry/data/xpkgs/...`, `/home/runner/...`), which differ per host and are not known when
  `mcpp.toml` is written; a `-ffile-prefix-map` for them needs mcpp to supply the package roots (or a flag with the
  runner's path computed in CI), and this is not something to try blindly in a release that exists for #100. #66
  stays open with that finding.
- **For #100 itself** the server needs only words: the troubleshooting entry for "MS-style inline assembly is not
  available" (what it meant, which engine fixes it), the register row UP-42 and the answer on the issue that corrects
  the report's diagnosis (X86 is built; the filter was missing in the module builders). The `mcppls.buildDiscovery.
  mcppTarget` setting is not at fault and nothing changes in the product's target handling.
- **S2 (#71, #52 items 2/4/5)**: independent product work, not caused by and not needed for the engine rebuild; they
  stay out unless the baseline points at one.

## 7. Plan and gates

**Phase 0 -- the local machine.** *Disk done (2026-11-26):* expired `/tmp` entries older than two days (about 18 GB)
and clearly stale sibling `target/<triple>/<fingerprint>` directories were removed; `/home` has 33 GB free (it had
14). The mcppls family's own `target` directories hold one fingerprint per triple, so nothing there was stale. A
Release tree of `clangd` needs about 15-20 GB. *Still to do:* run `engine-local/build.sh clangd` once (the full cost,
paid once; later edits are incremental, minutes) from `llvm-sparse`'s `openkal-series4` through `sync.sh`.

**Phase 1 -- unmodified engine.** (a) Reproduce #100's worker failure with the E1 test project (native `clangd` and the
shipped image). (b) Run the §5 baseline, native and image. (c) Run E3/E4's reproductions. *Gate:* each item enters
the batch only with its failure shown; the document is amended with what was found.

**Phase 2 -- implement on Linux.** E1, then E2 if its spawn question resolves, then any speed item that passed §5's
rule. One patch per concern (0049 for E1; the openkal series for E2), ledger rows, cts cases, engine unit tests.
Local lit/unit run of the touched suites (`ninja clang clangd FileCheck ...` per the local-profiling note), then the
cts program against the native build. *Gate:* the new cases fail before and pass after; the baseline does not regress
beyond noise.

**Phase 3 -- the other platforms in CI.** Open the engine PR; mcppls product CI consumes its build through
`packaging/engine-candidate.json` (the candidate is tested before the engine is merged or released, the standing rule).
CI covers linux-x64/arm64, macOS arm64, Windows x64 (2022 and the 24H2 gate row), the Ubuntu 20.04 floor, and the
image acceptances; Windows gets the real-SDK i686 case and the speed measurement run of §5. *Gate:* both PRs green;
no new `unsupported` where the case was `passed` before.

**Phase 4 -- release.** Versions follow the rules, nothing special (decision 9.5): the engine's next patch
(`RELEASE_VERSION` 0.0.23 -> 0.0.24, reporting `23.1.0-mcppls.13`, because the series gains a patch); the server's
next patch of its series (0.24.1 -> 0.24.2, `mcpp run -p devtools -- version --bump server`) and the VS Code
extension's (0.24.2, which carries the newest server patch, docs/92-release.md "Versions"); CLion and Zed take the
server patch with their next patch. Engine release (the tag rebuilds the image, so bytes differ from the PR candidate; the
timing-sensitive darwin checks are where a difference shows), pin the server to it, delete the candidate file,
server patch release with the troubleshooting entry and S1. Answer #100 with the evidence; update #24, #19, and the
issues closed or reduced by Phase 1.

## 8. Risks

| Risk | Handling |
|---|---|
| ~~`setSeverity(..., Ignored)` on an error~~ | settled by reading: not allowed (`Diagnostic.cpp:358`), and a consumer filter cannot clear `hasErrorOccurred`; B replaced by A (§2) |
| E1 now parses asm, so the editor can show a real asm mistake it used to ignore | it is what the project's own compile reports; stated in the CHANGELOG; the unit test keeps a real error failing the build |
| E2's spawn capability absent on openkal | per-system fallback or drop (§3); the batch does not wait |
| A full rebuild, then a long CI cycle for one-line fix | accepted by the decision to batch; mitigated by Phase 1 being able to cut items out before Phase 3's cost |
| Reproducing a speed problem on this machine's fast cores | compare spans and counters, not wall times (the local-profiling note); Windows/macOS numbers come from CI only |
| 0049 touches files that 0017/0019/0043 also touch; the series applies in order | rebase 0049 last, run `ci/check_ledger.py` and `check_tree.py`; the hunks are a few lines at two call sites |
| The image differs from the native build locally | Linux ships the image: Phase 2 also builds `oksrc` + `okgen` once to run the cts case on the image, or states that only CI did |

## 9. Review decisions (2026-11-26)

1. **0049 is a new patch**, not an edit of 0017 (0017 stays `steady`).
2. **E2 may be dropped per system** where openkal or the platform gives the supervisor no pipe, and the batch does not
   wait for it. The drop is noted in the code comment, the ledger and #19 (§3).
3. **A real-SDK i686 fixture on Windows CI** is added, once, as a gate that may be `unsupported`, never `passed`
   without the SDK (§2 tests).
4. **Speed is best effort and non-blocking**; the one hard requirement is no regression against the previous release,
   measured against the Phase 1 baseline (§5). Optimisation where cheap and shown.
5. **Version numbers follow the rules** (§7, Phase 4): engine 0.0.24 / `23.1.0-mcppls.13`, server and VS Code 0.24.2.

## 10. Self-review

Checked against the sources after the first draft; the findings, and what each changed:

| Finding | Change |
|---|---|
| The severity mapping would have been set on the wrong engine: the worker's local `DiagnosticsEngine Diags` only parses the arguments; the compile uses the `CompilerInstance`'s | §2 now sets it on `Clang->getDiagnostics()` after `prepareCompilerInstance`, in both builders |
| It was not stated why only module builds fail | §2: the importer's preamble has no error gate (`Preamble.cpp`, `PrecompiledPreamble.cpp` checked), so a plain `.cpp` with `<Windows.h>` for i686 already works |
| The E2 test stand-in is a shell script replacing the worker binary, so a bound in the worker's own printer would not satisfy it | §3 keeps the bound in the **supervisor's transport**, as the issue's pipe option does |
| #47/#50 are probably solved by patches 0027, 0028, 0030, 0035 | §4 is a verification task; nothing is re-implemented |
| The speed policy needed a reference to compare with | §5: the Phase 1 baseline of the unmodified engine is that reference |
| Tool names: the U5 measurement is `mcppls-bench` `ux-xlings` (`tests/bench/ux.cpp`), not a cts case | §5 table corrected |

Residual uncertainty, stated rather than hidden:

- **The worker failure is not yet reproduced** (§1); the whole of E1 rests on the code reading plus the AST-path
  run. Phase 1(a) is the first thing done, and if it does not fail, this document is wrong and is amended before any
  patch is written.
- `setSeverity(..., Ignored)` on an error-class diagnostic is read from `DiagnosticIDs.cpp`, not run (§8, fallback
  ready).
- Stock clangd having the same defect (UP-42's "upstream" column) is unverified.
- Windows numbers and the real-SDK i686 case can only be seen in CI; the first look is Phase 3.
- Whether the Linux image path matters for E1: it should not (same worker code), but only the image acceptance in
  CI, or a local `oksrc` build, shows it.

## 11. Implementation plan and task dependencies

Two repositories, so **one PR in each** (the engine PR and the server PR; the server PR consumes the engine PR's build
through `engine-candidate.json` before the engine is released, the standing rule). Order of the whole run: both PRs
green -> self-review -> squash-merge the engine PR -> cancel the engine's main CI, let the **release** workflow run
first -> after the release succeeds, re-trigger main CI -> server PR re-pointed at the released engine (candidate file
deleted, lock updated) -> green -> self-review -> squash-merge -> same release-first order -> real local check of the
released payload -> report.

| Task | What | Depends on |
|---|---|---|
| T0 | worktree `fix/issue-100-msasm-module-builds` from `origin/main` of mcppls-clangd; pinned tree fetched and the 48 patches applied under `~/workspace/engine-100`; unmodified `clangd` built | -- |
| T1 | baseline on the unmodified build: reproduce #100's worker failure; the §5 speed baseline | T0 |
| T2 | patch 0049 (E1): export `isExcluded`, apply it on the compile's engine in the worker and the in-process builder | T1a |
| T3 | gtest cases in `PrerequisiteModulesTest.cpp` (worker path through `runModuleBuildWorker`, in-process path through `ModulesBuilder`; a real error still fails); a lit test is added only if the unit tests cannot show the editor-visible result | T2 |
| T4 | engine ledger and words: `patches/PATCHES.md` row, `patches/series`, UP-42, CHANGELOG 0.0.24, `RELEASE_VERSION`, `FORK_SUFFIX` `.13` wherever it is pinned, `ci/check_ledger.py` / `check_tree.py` | T2 |
| T5 | speed re-measure against T1 (hard: no regression) | T2 |
| T6 | engine PR; CI green (native builds, floors, acceptances, warm-prime) | T3, T4, T5 |
| T7 | server: `packaging/engine-candidate.json` -> T6's run; conformance fixtures: a portable `i686-windows-msvc` + `__asm` module (every system) and the real-SDK `<Windows.h>` i686 case (`win32-x64`, `unsupported` without the SDK); troubleshooting entry; register UP-42; version bump 0.24.2 (server, VS Code) with CHANGELOG | T6 (candidate run id) |
| T8 | server PR; CI green | T7 |
| T9 | self-review of both diffs against this document; squash-merge the engine PR; engine release (tag `v0.0.24`), release CI first | T6, T8 |
| T10 | server: lock to the released archives, candidate file deleted, CI green, self-review, squash-merge, release (`vscode-v0.24.2`), release CI first | T9 |
| T11 | local check of the **released** payload on this machine: the VSIX's server and engine, the i686 reproduction through the real binary, report with the resource paths | T10 |

**E2 (#19) is not in the batch.** Reading the code for T0 settled the question of §3: the ok image starts a worker
through `llvm::sys::ExecuteNoWait` with a *path* for each stream, and openkal "starts a program with its three
streams and no other descriptor" (openkal series patch 0002); an anonymous pipe as the worker's standard error needs
openkal's own process-start API (and a Windows `CreateProcess` with handles), not a change to `WorkerDiagnostics`. A
file-size bound polled by the supervisor does not bound the 8 MiB stand-in, which a writer finishes within one poll.
So the three image systems keep the regular-file transport, as the decision of §9.2 foresaw; the note is in the
`WorkerDiagnostics` comment (T2 touches that file), the ledger, and a comment on mcppls-clangd#19.

## 12. What implementation found (2026-11-26)

1. **B did not work, as §2 says**: the engine's error flag is set before any consumer sees the diagnostic.
2. **The first candidate of the fix broke the arm64 images.** The product's CI (server PR 101, run 45676721862) ended
   `clangd` by SIGBUS on `linux-arm64` and `darwin-arm64` in the VS Code end-to-end suite (twice), the proot job and the
   server's `report`; the engine's own acceptance, which passed, does not start that thread the same way. A control
   (the unmodified 0.0.23 image through the same job, rerun the same day) passed, so it was the change, not the
   runner. The crash is deterministic: the same fault address in every run (`0x6c632f3436609e`), in
   `clangdMain -> startModuleMemoStore -> pthread_create -> __clone`, where openkal-musl walks its thread list: the
   list's head, `okm_done` at the start of `.bss`, holds the tail of `x86_64/clangd.oke`'s name (`6_64/cl`), not
   zero. A probe workflow on the arm runner (a temporary branch, removed) reproduced it with `mcppls report` in about
   40 s and showed the image does not crash under qemu, under gdb, or when driven directly with the same flags: it needs
   the server's start. okrun keeps a sealed copy of an image's segments and a boot snapshot of it
   (`.okseg`, `.oksnap` under `$XDG_RUNTIME_DIR/okrun`), a different pair for the new image; whether the defect is in
   that, in openkal-musl or in the image's link was not found. The same libraries in an x86_64 image run.
3. **Decision:** the libraries are linked where the processor is not arm64 (`CMAKE_SYSTEM_PROCESSOR`), so arm64 images
   are the unmodified ones. The report's case (a Windows x64 host) is fixed; an arm64 host is not, and the limit is in
   the changelogs, the troubleshooting entry and the ledger. A follow-up should find the cause in openkal (the probe's
   method is in the memory of this work: a probe workflow that runs on `push` of a temporary branch, since a
   `workflow_dispatch` needs the file on the default branch).
4. **Not carried**, as §3 and §6 say: mcppls-clangd#19 and #66.
5. The engine's Windows comparison run needed a fix of its own: vcpkg's zlib 1.4 names its static library `libzs.lib`,
   which the workflow did not look for (and the shell step did not stop on a missing one).

## 13. The release scope was decided wrongly, and the rule is now written (2026-11-26)

The batch was first released as `vscode-v0.24.2` (a draft; a plugin patch, on the precedent of 0.24.1). That was wrong:
the change is in the server (a new engine in its payload), and **any change to the server is a series** -- here
`0.25.0`, every component with it -- while only a change confined to one plugin's own files is that plugin's patch.
`docs/92-release.md` ("Versions", "Which release a change needs", English and Chinese) did not say so, which is how
the precedent was followed. Now:

- the rule is stated once in `docs/92-release.md` and `docs/zh-CN/92-release.md` and referenced from `CONTRIBUTING.md`
  and the `mcppls-contributing` skill (`.agents/skills`);
- it is enforced: `mcpp run -p devtools -- version --bump server` is refused (unit test), and the Release workflow
  refuses `component: server` and a `server-v*` tag;
- the draft `vscode-v0.24.2` is deleted and its number is not used; this work is released as `0.25.0`.
