# Issue 95: a standard library command that does not move, and an open file the build replaces

Status: **implemented in 0.24.0** (changes A and B, with T8; change C, S2, is left for a later release as §7.5
assumed) · revision 2 of the proposal below, §10 is what implementing it found · evidence: the reporter's diagnostic
bundle (`mcppls-bundle-20261122T124704.140Z.zip`, mcppls 0.23.0, clangd `23.1.0-mcppls.12`, win32-x64, mcpp
2026.11.20.1) read against the 0.23.0 sources, one local measurement of what mcpp writes for the same project shape
(§4), and — after implementation — the loop reproduced with mcpp's own build database on Linux (§10). Target: the next
series release, server only — **no engine (mcppls-clangd) change and no mcpp change is required**. One addition is
asked of mcpp for a later step (mcpp-community/mcpp#1004, §4). §8 is the implementation plan; §9 is what a review of
revision 1 changed.

The goal this plan serves: **the command the standard library is built with is a property of the
toolchain and the context, not of whichever unit happens to import `std` this minute; and a file the
build replaces with a generated one is not an ambiguity, does not degrade the workspace, and can
still be formatted.**

| # | Item | Decision | Section |
|---|---|---|---|
| S1 | `std`'s representative unit changes when a file gains or loses `import std`, even from the edit buffer | the representative is chosen by set, then by unit; "imports `std`" is asked of the set, from the model's own imports; targets before build programs. **This is the fix of the restart loop** | §2.1 |
| S2 | `std.ixx` is built with the representative's `-I` directories | hardening for projects with several target sets: `std`'s arguments lose the project's header search paths. Waits for E2; can ship after S1 | §2.2 |
| S3 | Nothing says which unit `std` follows | the plan records the representative; the plan diff names it when it moves | §2.3 |
| G0 | mcppls does not read `ide.generated` | the model keeps a set's generated entries; they say which file a generated unit was made from | §3.0 |
| G1 | An open file no set describes, providing a module the model already provides, is `ambiguous-module` and the workspace `degraded` | it is a *shadowed source*: left out of the database, a notice, not an issue | §3.1 |
| G2 | Formatting answers nothing for any file clangd is not given | (a) the reason is said; (b) a format-only document: clangd formats the text under a scratch name. (b) waits for E1 | §3.2 |
| G3 | The shadowed source has no clangd semantics | not in this release; needs experiments and a stated `origin` (mcpp-community/mcpp#1004) | §4 |
| T | Tests | mtest cases for S1, S2, G0, G1; conformance steps for the loop and for G2 | §6 |

## 0. What the bundle says

The report reads the bundle as one chain (restart loop → `degraded` → Format Document does nothing).
It is three separate things.

### 0.1 The restart loop is `std`'s representative flipping

`plan_engine` (`src/normalize/plan.cpp:524-535`) picks a *representative unit* — the first C++ unit
that imports `std` and is not a module interface (rank 4), else the best lower rank — and builds
`std.ixx` / `std.compat.ixx` with that unit's arguments and working directory (`plan.cpp:752-787`).
`std_unit_arguments` (`plan.cpp:301`) removes `-D`, `-U`, `-include`, `-imacros`; `-I` stays.

The project has two possible representatives:

| `src/main.cpp` imports `std` directly | representative | `std`'s first argument |
|---|---|---|
| yes | `main.cpp` (set of package `hui`) | `-I…\mcpplibs.huxerui\…\include` |
| no (the targets' other `std` importers are all module interfaces) | a `.cpp` of the host build-program set | `-I…\mcpplibs-x-openkal\0.26.0\…\include` |

The bundle's last database has the openkal form. Set beside the target units of the same database,
the two forms of `std`'s command differ in more than the `-I`:

| | target set `hui` (`main.cpp`) | host build-program set |
|---|---|---|
| `-I` | four directories (huxerui, its rules, `src`, the `hrc` output) | one (openkal) |
| `-g` | yes | no |
| `-Wno-unknown-attributes` | yes | no |
| `-D__MCPP_TARGET_WINDOWS__=1` | yes (already dropped for `std`) | no |
| directory | `…\target\x86_64-windows-msvc\b0d0…` | `…\.build-mcpp\host\c33d…\target\…\455d…` |

None of this changes what `std` means, but
`ClangdEngine` (`src/engine/clangd.cpp:926` onwards) compares the argument text of every module
provider, sees one "compiled with other arguments" and restarts clangd; the BMIs of the other command are then freed ("freed 37.0 MB of BMIs
built for commands no unit has any more") and all 39 modules are prepared again.

Four flips in six minutes, each right after an edit of `main.cpp`:

| time | what happened | `std` follows |
|---|---|---|
| 12:24:39 → 12:24:40 | `main.cpp` v8 edited; replan 2 s later, model not reloaded | huxerui |
| 12:27:09 → 12:27:15 | v9 edited and saved; model reloaded and *changed* | openkal |
| 12:29:03 → 12:29:05 | v10 edited; replan 2 s later, model not reloaded | huxerui |
| 12:30:35 → 12:30:42 | v14 edited and saved; model reloaded and *changed* | openkal |

The asymmetry is the edit window: for 5 s after an edit (`EDITING_WINDOW`,
`workspace.cpp:573`, `3475-3483`) the file's buffer imports are merged into its `required`
(`plan.cpp:392-401`), and `required` is what the rank reads. An `import std` typed into the buffer
makes `main.cpp` the representative at once; removing it takes effect when mcpp describes the project
again. Inference, not observation: the log does not carry buffer text. What supports it is that the
saves of v11–v13 reloaded the model as "unchanged" and exactly these four changed it.

The earlier restarts (11:50–12:20) are the person editing `mcpp.toml` (11 → 19 → 12 → 13 sets):
correct restarts, but they are counted in the same ten-minute budget, so the flips reached the
155 s backoff quickly. After 12:33 there is no further restart; at export time the plan budget is
`recent: 0` and clangd is `ready`.

### 0.2 `degraded` and the empty formatting are the open `src/app.cppm`

`[build] sources = []` and HuxerUI's `hcg` transform: the model has
`target/.build-mcpp/out/…/hui/hcg/src_app.cppm` as the unit of module `app`; `src/app.cppm` is in no
set. Opened in the editor, it enters the plan as an open source no set describes
(`plan.cpp:473-509`), scans as `export module app`, and becomes the second provider of `app`
(`plan.cpp:564-578`): `ambiguous-module`, and the file is left out.

- `degraded` is that plan issue (`workspace.cpp:4026`), for as long as the file is open. It is not
  the restart loop.
- A file left out is not given to clangd, and `claims()` refuses its requests
  (`clangd.cpp:1317`). Nothing else formats. The report's counters: 24 `textDocument/formatting`, 16
  empty — `app.cppm` asked 16 times; the 8 answered are `main.cpp` and `build.mcpp`.
- The `.clang-format` above the root is not involved: `grants = ["all"]`, `engine.confine = false`,
  and `main.cpp` beside it formats.

### 0.3 Not the person's doing

No setting or project layout in the bundle is wrong. mcpp's failures in the log (a git dependency
without `mcpp.toml`, a dependency cycle, offline needing a download, a patch naming a missing file)
are intermediate states of `mcpp.toml` being edited and all recover. One of them names the pair of
§0.2: at 12:20:37 `emit build-database` failed with "module 'app' is provided by package 'hui'
(…\hcg\src_app.cppm) and by package 'hui' (…\src\app.cppm)". That diagnosis is correct — for that
save the manifest listed the original as a source beside the rule's copy — and it is not a defect of
mcpp.

## 1. Scope

In: `src/normalize/plan.cpp`, `plan.cppm`; `src/spec/database.cppm`, `database.cpp` (reading
`ide.generated`); `src/engine/clangd.cpp` (what the plan diff says, the format-only document);
`src/orchestrator/workspace.cpp` (state, routing of formatting); status and report texts; tests.

Out: the restart budget's accounting of restarts the person caused by editing the build description
(§7, open question); semantics for the shadowed source (§4); anything in the engine or in mcpp.

## 2. The standard library's command

### 2.1 S1 — the representative is chosen by set, from what the model says

Today the choice (`plan.cpp:524-535`) ranks *units*, and a unit's rank reads its own `required`
after the edit-window merge. Both halves are what moved `std`. The choice becomes two steps.

**Step 1, the set.** Among the sets of the context that have a C++ unit with a standard library
manifest, in this order:

1. a set that is not a build-program set (`spec::is_build_program_set`) before one that is;
2. a set in which some unit imports `std` or `std.compat` *as the model states it* before a set in
   which none does;
3. the database's own order.

**Step 2, the unit in that set.** The first C++ unit with a manifest that is not a module interface;
if the set has none, its first C++ unit. Whether this unit imports `std` is not asked.

"As the model states it" is `unit.requiredModules`, read before the edit-window merge of
`plan.cpp:392-401`. `Candidate` gets `bool importsStdDeclared` for it. The merged `required` keeps
every other use it has today (stand-ins, `anyStd`, entries' imports): only the choice of
representative stops looking at it. Two details for the implementation:

- For a unit whose model lists no imports, `required` comes from `scan()`. If the scanner reads the
  editor's buffer, `importsStdDeclared` must come from the text on disk instead; T1 checks which.
- An open source of step 1b is never the representative: it belongs to no set.

Why by set: the arguments that matter to `std` are a set's (its `baseline-arguments`), and which of
a set's units imports `std` this minute is the thing that must not move the command. With the
question asked of the set, the representative leaves a set only when the set's *last* importer of
`std` goes, as mcpp describes it — an event of the build description, not of typing.

What this does to the reported project: set `hui` always has an importer of `std` (the generated
`src_app.cppm`), so the set is `hui` and the unit is `main.cpp`, in all four edits of §0.1. `std`'s
command is the huxerui form throughout and never the openkal one. No restart.

A context with build programs only, and a project with one target set, keep the set they have
today; the unit in it can be another one (the first that is not an interface, not the first that
imports `std`), which matters only where units of one set have arguments of their own (§8.4).

### 2.2 S2 — `std`'s arguments are the toolchain's, not the project's (hardening)

S1 leaves one way for the command to move without the toolchain moving: a project with several
target sets, where the first set with an importer of `std` changes when a file is saved. The sets of
one project usually differ in `-I` and `-D`; `-D` is already dropped.

`std_unit_arguments` additionally drops the representative's own header search:
`-I<dir>` / `-I <dir>`, `-iquote`, `-F`, `-iframework`, `-idirafter`. It keeps everything that
states the language, the target or where the toolchain is: `-std=`, `--target`, `-isystem`,
`-isysroot`, `--sysroot`, `-stdlib=`, `-nostdinc*`, `-X…` (the MSVC roots), `-B`, `-f…`, `-m…`,
`-O…`, `-g…`, `-W…`, and the `_HAS_*` / `_LIBCPP_ABI_*` defines it keeps today. The manifest's own
`systemIncludeDirectories` are appended as now.

What it does not do: sets that differ in `-g`, `-O`, `-W…` or `-f…` still give different text, as
the table of §0.1 shows for the host set. Those are left alone on purpose — which of them a BMI's
importers must agree on is not something to decide by reading code — so S2 narrows the cases, and S1
is what removes the reported one.

Why it is safe: `std.ixx` and `std.cppm` include standard headers only. A project `-I` can change
what they mean in one way — by putting a file with a standard header's name ahead of the toolchain's
— and a build tool that builds `std` once per toolchain never has those directories on that command
(mcpp's own `mcpp:std` set, measured in §4: ten arguments, no `-I`).

One exception, kept deliberately narrow: a representative that states `-nostdinc`, `-nostdinc++` or
`-nostdlibinc` places its standard library through its search paths; its `-I` (and the rest) stay as
they are today. The debug line at `plan.cpp:757` names what was dropped, search paths included.

The entry's directory stays the representative's `workDirectory`; the restart decision does not
compare directories, and with S1 it no longer moves between configurations.

Cost on upgrade: `std`'s command text changes once for every project whose representative has an
`-I`, so the first session after the upgrade builds `std` and its importers again (the "command"
rebuild reason). This is why S2 waits for E2 and E3 and is its own change (§8): it can ship one
release after S1 without leaving #95 open.

### 2.3 S3 — the plan says which unit `std` follows

`EnginePlan` gets `std::string stdRepresentative` (the source). It is written to `plan.json` and the
report's `plan` object, and the "engine database changed" line and the `engine-plan-diff` event add,
when a standard library unit is among `otherArguments`, `std follows <file> now (was <file>)`. A
report like this one then names the cause in one line instead of needing the sources.

No change to the restart decision itself: a provider whose command really changed still needs a
fresh clangd (robustness design C4, fix plan F13). S1 and S2 remove the cases where it changed for
no reason.

## 3. An open file the build replaces

### 3.0 G0 — the model reads `ide.generated`

S1 §7.2 lets a set list what its build generates, and mcpp writes it (§4): for the reported shape,
an entry of `kind` `source` whose `generator.inputs` holds the authored file. mcppls parses none of
it today.

- `spec::Set` gets `std::vector<Generated> generated` — `{ path, buildPath, kind, generatorId,
  inputs, origin }`; `origin` is empty until S1 0.4.0 exists (mcpp#1004) and is read when present.
  `database.cpp` parses it and writes it back, so the model cache keeps it.
- `use_project_build_output` (`src/project/generated.cppm`) moves units from the producer's planning
  directory to the project's `target/`; a generated entry is matched to a unit by `path` *or*
  `build-path`.
- A helper answers the one question G1 asks: for a unit's source, the authored file it was made
  from — the entry's `origin`, else, as a fallback said to be one, the single input of the entry
  that is a C++ source inside the workspace root (none or several: no answer).

Nothing else changes with G0: `generated-files-missing` keeps its present source.

### 3.1 G1 — a shadowed source is not an ambiguity

Definition: a candidate that came from `input.openSources` (step 1b — no set describes it) and
provides a module for which a unit of the model is the provider in use. The model is the build's
word on who provides the module; a file the build does not compile cannot contest it. The reported
case is the origin of a generated unit; a stale copy and a file outside `sources` globs are the same
thing to the plan.

Changes:

- `Candidate` gets `bool fromOpenSource`. In the rivals loop (`plan.cpp:564-578`), a rival with
  `fromOpenSource` whose `indices.front()` is a model unit is excluded as today but is not a rival:
  no `ambiguous-module`. Two model units providing one module stay an ambiguity, unchanged. (Step 1b
  appends after every model candidate, so an open source is never `indices.front()` while the model
  has a provider.)
- It goes to a new `EnginePlan::shadowedSources` — `{ source, module, provider }` — instead of
  `excludedFiles`. `ClangdEngine` treats it as excluded (`newExcluded`, `clangd.cpp:898-903`): it is
  still not given to clangd as itself, since clangd finds module providers by scanning the
  database's files and two files declaring `app` make it pick one arbitrarily (engine patch 0014:
  "Detected multiple source files … declaring the same module").
- State: a shadowed source is a notice, not a plan issue, so `workspace.cpp:4026` no longer returns
  `degraded` for it. Code `shadowed-source`, with a `kind`:
  - `generated-from` — G0 names this file as what the providing unit was made from: "app.cppm is not
    compiled as it is: the build generates src_app.cppm from it (step hcg:app), and that is the unit
    of module app. It is formatted, and answered by mcppls's own engine";
  - `outside-build` — otherwise: "app.cppm is in no build target; module app is provided by <file>".
- `plan.json` / report: `shadowed` beside `leftOut`; `leftOut` no longer lists it.

mcppls's own engine keeps answering the file as it does today (the report shows hover, definition
and completion answered for `app.cppm`).

What G1 does not touch: an authored file that provides no module (`src/view.cpp`, transformed into
`hcg/src_view.cpp`) was never an ambiguity. Step 1b already gives it to clangd with the nearest
unit's arguments, untransformed. That is G3's question answered in the field for ordinary units, and
worth looking at before G3's experiment is designed.

### 3.2 G2 — formatting does not depend on the file's semantic state

Formatting is a function of the text, the file's name and the `.clang-format` files above it. Today
it is refused for every file clangd is not given: left out, shadowed, set aside after an engine
exit, doomed by a module that cannot be built. All of those are files a person is very likely
editing.

**G2a, the reason.** When the server answers a formatting request empty because it chose not to ask
clangd, the reply stays an empty edit list (editors show an error as a failure on every save) and
the reason goes to the log once per file and to `mcppls status`: "app.cppm is not formatted:
<reason>". This part waits for nothing.

**G2b, the format-only document.** clangd formats only a document it has open, and opening a
document builds it. So the server gives clangd the text under another name, for the length of one
request:

1. `textDocument/formatting`, `rangeFormatting`, `rangesFormatting` or `onTypeFormatting` arrives
   for a file clangd is not given, and clangd is accepting. (Read-only files of mcpp's engine
   interface are still refused first, `refuse_edit`.)
2. The engine sends `didOpen` for a scratch URI in the same directory with the same extension —
   `<dir>/<stem>.mcppls-format<ext>` — carrying the document's current text. Same directory: the
   same `.clang-format` is found. Same extension: the same language is detected. The file does not
   exist on disk and is not in the database.
3. It forwards the request with the URI replaced, then sends `didClose` whatever the answer.
4. The reply is `TextEdit[]` with no URI in it; it is returned for the real document unchanged.
5. Everything clangd says about the scratch URI — diagnostics, file status — is dropped in the
   engine and never reaches the editor, the status, the crash ledger's suspect or the incident
   counters.

Why not the real URI: opening the file as itself is exactly what exclusion, the crash ledger and the
doom set exist to prevent, and its diagnostics would be published as the file's.

Guards:

- The scratch document is never primed, never counted in `fileStates`, never a restart reason.
- A file in the crash ledger (it ended clangd) is **not** formatted this way in this release: its
  text under another name is the same text. It keeps today's empty answer, with the reason below.
- If clangd is not accepting (restarting, in backoff), the request is answered empty as today.
- In each of these cases G2a says why.

Not chosen: running a `clang-format` program beside clangd. The payload ships none, and the `mcpp`
fallback style is a preset compiled into the maintained clangd (engine FEATURE-43), so a second
formatter would format `mcpp.toml` projects differently from clangd.

G2b waits for experiment E1 (§5): that clangd answers formatting for a document whose build is still
in flight or has failed, with the style of the real file, and what the build of a scratch module
unit costs before `didClose` cancels it. E1 was not run for this revision: no engine payload is on
the machine the plan was written on. If E1 fails, G2 ships as G2a only, and formatting for such
files becomes an engine item.

## 4. G3 — semantics for the shadowed source (not in this release)

The person edits `src/app.cppm`; clangd knows `src_app.cppm`. What would serve them is clangd
reading the original with the generated unit's arguments while importers keep importing the
generated one. Two things stand in the way, and neither is settled by reading code:

- clangd takes a command for a file outside the database by interpolation from a neighbour; which
  neighbour, and whether its module hints survive, is not ours to choose without an in-memory
  database channel (`compilationDatabaseChanges`), which the server does not use today.
- Whether an untransformed HuxerUI source parses to useful semantics (the build passes
  `-Wno-unknown-attributes`, which suggests it is meant to) or to false diagnostics.

And one thing would make the relation a fact instead of the inference of §3.1. What mcpp writes
today was measured on HuxerUI's `buildtools/mcpp/examples/01-import` (mcpp 2026.11.20.1), the same
shape as the reporter's project:

- The set's `ide.generated` (S1 0.3.0 §7.2, SPEC-005 R3.12) has an entry of `kind` `source` for
  `…/hcg/src_app.cppm`, with `build-path` and `generator { id: "hcg:app", arguments, inputs }`;
  `inputs` is `[<project>/src/app.cppm, <the hcg binary>]`.
- The generated unit is `provides {app: ""}`, `ide.role` `unknown`; `src/app.cppm` is no unit.
- The generated file carries `#line` directives that name `src/app.cppm`.
- mcppls does not read `ide.generated` at all (`src/spec/` has no such field); `generatedMissing`
  comes from the private-directory path shape (`use_project_build_output`).

So the origin is already in the document, but only as one member of a flat `inputs` list beside the
tool, which is a guess for any rule with more than one C++ input and gives nothing to cross-check
for an ordinary `.cpp`. mcpp-community/mcpp#1004 asks for the relation to be stated: `origin` (and
`line-mapping`) on a Generated object, as S1 0.4.0, declared by the rule on `mcpp::action`.

G0 (§3.0) is what needs no mcpp release: the fallback reads `inputs`, and `origin` replaces it when
a producer writes it. G1 and G2 do not wait for #1004; G3 does. When mcpp answers on the names, the
S1 text and schema (`docs/specs/s1-build-database.md` §7.2, 0.4.0) are changed in this repository
first, as 0.3.0 was for mcpp#724.

## 5. Experiments before implementation

| # | Question | How | Decides |
|---|---|---|---|
| E1 | Does clangd `23.1.0-mcppls.12` answer `textDocument/formatting` for a just-opened document outside the database, with the style of the `.clang-format` above it, and what does the open cost before `didClose`? | a fixture with a module unit and a `.clang-format` in a parent directory; drive the payload's clangd over LSP directly (didOpen scratch → formatting → didClose), `CLANGD_TRACE` for the build | G2 as designed, or reason line only |
| E2 | Is the BMI of `std` built with and without the project's `-I` accepted by the same importers? | the reported shape as a fixture (targets + a build-program set), plan before/after S2, fixtures on linux-x64 and win32-x64 | S2's keep/drop list |
| E3 | Does anything else key on `std`'s command text (module memos, prefetch, the answer cache)? | grep for `moduleCommands_` uses; run the existing module fixtures with S2 | upgrade note in §2.2 |

## 6. Tests

C++ cases of mtest, in `tests/unit/test_normalize.cpp` unless said otherwise.

- S1a: targets = one interface importing `std` + `main.cpp` not importing it; a build-program set
  with a `.cpp` importing `std`. The representative is `main.cpp`.
- S1b: the same, `main.cpp` in `editingSources` and the scanner returning `import std` for it; and
  the same with `main.cpp`'s model imports listing `std`. The `std` entries are equal, argument for
  argument, in all three plans (the regression test of this issue).
- S1c: a context of build programs only: the representative is unchanged from today.
- S1d: two target sets, only the second with an importer of `std`: the representative is in the
  second; the first set's units never are.
- S2a: a representative with `-I a -iquote b -isystem c -DX -D_HAS_EXCEPTIONS=0`: `std` keeps
  `-isystem c` and `-D_HAS_EXCEPTIONS=0` only.
- S2b: a representative with `-nostdinc++ -I libcxx/include`: `-I` stays.
- S2c: two target sets differing in `-I` only: `std`'s arguments are equal whichever set is chosen.
- S3: the plan names the representative; a plan whose representative differs from the last one's
  has the "std follows" text in its diff (`tests/unit/test_exits.cpp` or where the plan diff is
  tested today).
- G0a (`tests/unit/test_spec.cpp`): a set with `ide.generated` parses, writes back equal, and the
  helper returns the authored file for the generated unit by `path` and by `build-path`; two C++
  inputs give no answer; a stated `origin` wins over `inputs`.
- G1a: an open source providing a module a model unit provides: no `ambiguous-module`, the source in
  `shadowedSources` with `kind` `generated-from` when G0 names it and `outside-build` when not; not
  in `entries`, not in `excludedFiles`.
- G1b: two model units providing one module: `ambiguous-module`, as today.
- G1c (workspace fixture, model loaded as generation 1 / from the producer): with G1a's plan the
  root's state is `ready`, and the notice is present.
- Loop (conformance, `tests/cts/editing.cpp`): a fixture of the reported shape (a target set with a
  generated-style interface importing `std`, `main.cpp`, a build-program set). `import std` is typed
  into `main.cpp`, saved, removed, saved; clangd's process id is the same at the end and no
  `restart-backoff` incident exists.
- G2a: a left-out file asked to format: empty edits, one reason line.
- G2b (conformance, after E1): a shadowed module unit in the fixture; `textDocument/formatting`
  returns edits; no diagnostics are published for any `*.mcppls-format.*` URI.

## 7. Open questions for review

Each has the answer this plan assumes until told otherwise.

1. S2's exception: is `-nostdinc*` the right and only signal that a project places its standard
   library by `-I`? Assumed: yes. (A toolchain file that uses `-isystem` is already kept.)
2. G1's breadth: every open source the model already has a provider for, or only those G0 names as
   the origin of a generated unit? Assumed: every one, with the two kinds of §3.1.
3. G2b's scratch name is visible to clangd's log and nowhere else; is `<stem>.mcppls-format<ext>`
   acceptable? Assumed: yes. Files in the crash ledger stay unformatted in this release.
4. Should restarts whose cause is a model the person changed (`mcpp.toml`, `build.mcpp` saved) be
   counted apart from the plan budget, as toolchain and context switches already are
   (`RestartCause::user`, fix plan F14)? In this session they brought the backoff to 155 s within
   four later restarts. Assumed: not in this plan; a separate item if wanted.
5. Does S2 ship with S1 or one release later? Assumed: later, when E2 and E3 are done, since it
   costs every project one rebuild of `std` and S1 alone closes the report.

## 8. Implementation plan

Two changes that do not depend on each other, a third that waits for experiments, and a track in
other repositories. Each task is one commit with its tests.

### 8.1 Tasks

| # | Task | Files | Needs | Done when |
|---|---|---|---|---|
| E1 | Experiment: formatting of a just-opened document outside the database | a payload, an LSP driver from `tests/harness` | a local engine payload | answer and cost written into §5 |
| E2 | Experiment: `std` built with and without the project's `-I` is accepted by the same importers | fixtures on linux-x64 and win32-x64 (CI) | T1 | result written into §5 |
| E3 | Check: what else keys on `std`'s command text | `clangd.cpp` (`moduleCommands_`), module memos, the answer cache | — | list in §5 |
| T1 | S1: representative by set, then unit; `importsStdDeclared`; which text `scan()` reads | `src/normalize/plan.cpp` | — | S1a–S1d pass |
| T2 | S3: `EnginePlan::stdRepresentative`; `plan.json`, report, "std follows" in the plan diff | `plan.cppm`, `plan.cpp`, `workspace.cpp:5136`, `clangd.cpp` (`note_plan_diff_`) | T1 | S3 passes |
| T3 | The loop as a conformance case | `tests/cts/editing.cpp`, a fixture under `tests/cts/fixtures` | T1 | fails on 0.23.0's plan, passes with T1 |
| T4 | G0: parse, write back and query `ide.generated` | `src/spec/database.cppm`, `database.cpp`, `tests/unit/test_spec.cpp` | — | G0a passes; a cached model round-trips |
| T5 | G1 in the plan: `fromOpenSource`, `shadowedSources` and its `kind` | `plan.cppm`, `plan.cpp` | T4 | G1a, G1b pass |
| T6 | G1 in engine and workspace: treated as excluded, a notice, state, `shadowed` in `plan.json` / report / status | `clangd.cpp:898-903`, `workspace.cpp:4026`, `:5136` | T5 | G1c passes |
| T7 | G2a: the reason an unformatted file is not formatted | `workspace.cpp` (formatting route), `clangd.cpp` (`claims`), status | T6 for the shadowed wording | G2a passes |
| T8 | G2b: the format-only document | `clangd.cpp`, `workspace.cpp` | E1 passed, T7 | G2b passes |
| T9 | S2: `std_unit_arguments` drops header search paths | `plan.cpp:301` | E2, E3, T1 | S2a–S2c pass; module fixtures pass |
| T10 | Words: CHANGELOG, `docs/50-troubleshooting.md` (+ zh-CN) for `shadowed-source` and "std follows"; the one-time rebuild note with T9; an answer on issue 95 | docs | T2, T6 | — |
| M1 | S1 0.4.0: `origin`, `line-mapping` in §7.2, schema, example, spec CHANGELOG | `docs/specs/` | mcpp's answer on mcpp#1004 | spec checks pass |
| M2 | mcpp writes `origin`; `huxerui.rules` declares it on its `hcg` edge | mcpp, HuxerUI | M1 | a database with `origin` |
| M3 | G3: experiments, then a design of its own | — | M2, T6 | — |

### 8.2 Dependencies

```
change A (restart loop)     T1 ──┬──> T2 ──┐
                                 └──> T3   ├──> T10
change B (the open file)    T4 ──> T5 ──> T6 ──┬──> T10
                                               └──> T7 ──> T8
                                                      E1 ──┘
change C (hardening)        T1 ──> E2 ──┬──> T9 ──> T10 (the rebuild note)
                                   E3 ──┘

other repositories          mcpp#1004 ──> M1 ──> M2 ──┬──> M3
                                                 T6 ──┘
```

- A and B share no code path except `plan.cpp`, in different steps of it (the representative at
  step 2, open sources at steps 1b and 3); either can merge first.
- Nothing in A, B or C waits for mcpp. T4's `origin` is read when present and absent until M2.
- E1 decides whether T8 exists. T7 is written so that B is complete without T8.

### 8.3 Order and what each change gives

| Change | Tasks | The person sees |
|---|---|---|
| A | T1, T2, T3 | no restart when `import std` comes and goes; if `std` does move, the log says after which file |
| B | T4, T5, T6, T7 (+ T8 after E1) | `src/app.cppm` open: workspace `ready`, a notice naming the generated unit and its step, a reason when formatting is not done (and formatting itself with T8) |
| C | T9 | the same stability for projects whose target sets differ in `-I` |

A first: it is the smaller change and the one that costs the person minutes at a time. If one
release can hold only part of this, it is A and B without T8; T8 and C follow when their
experiments are in.

### 8.4 Risks the plan carries

- T1 changes which unit `std` follows for existing projects where the first rank-4 unit today is
  not the unit S1 chooses and the two have different arguments (a build-program `.cpp` ahead of
  the targets is the known case; units of one set with `local-arguments` of their own is the other).
  Their first session after the upgrade rebuilds `std` once. T10 says so.
- The reading of §0.1 (an `import std` typed into `main.cpp`) is an inference. T3 is what tests it:
  the case must fail before T1.
- G1 turns an issue into a notice. A stale duplicate the person believes is compiled is now quieter
  than before; the `outside-build` text has to say plainly that the file is not part of the build.

## 9. What a review of revision 1 changed

1. **S2 did not do what revision 1 said.** It claimed that dropping `-I` made the two forms of
   `std`'s command the same text. The bundle's own database shows they also differ in `-g` and
   `-Wno-unknown-attributes` (§0.1). S1 is the fix; S2 is hardening, behind its experiments.
2. **S1 still let the representative move.** Ranking units by "imports `std`" moved it between
   `main.cpp` and `src_app.cppm` inside one set — harmless there only because the two have equal
   arguments. The choice is now by set, and the unit in the set does not depend on its imports.
3. **G1's wording was chosen by the shape of a path.** mcpp already writes which file a generated
   source was made from (`ide.generated`), and mcppls never read it. G0 is new, and G1's two texts
   are chosen by it.
4. **"Worth passing on to mcpp" was wrong.** The scanner error at 12:20:37 is a correct diagnosis of
   a manifest that listed the original beside the rule's copy (§0.3). What was asked of mcpp instead
   is narrower: state the relation (`origin`), mcpp#1004.
5. **G2 is two things.** The reason line needs no experiment and is its own task; the format-only
   document keeps its gate. A separate `clang-format` program was considered and set aside (§3.2).
6. **Still not established:** the defect was not reproduced by running; E1, E2, E3 are not run; the
   budget question (§7.4) is not designed. The plan is written so that none of these blocks change A.

## 10. What implementing it found

Done: T1–T8 and T10 in one change (0.24.0); E1 run; E2, E3 and T9 not, as §7.5 assumed.

1. **The loop is reproduced, not inferred.** `tests/cts/fixtures/mcpp-std-representative` is `mcpp-build-programs`
   (a `build.mcpp` and a build-dependency that import `std`, a target `main.cpp` that imports it) described by
   mcpp 2026.11.20.1's own `emit build-database`. Removing `import std;` from `main.cpp`, saving, putting it back and
   saving: the 0.23.0 server restarts clangd at each save, "units are compiled with other arguments
   (std.compat.cppm)" (two restarts); this change restarts it never. The unit case of §6 S1b, with the targets
   first in the database, fails on 0.23.0's ranking for the same reason.
2. **Why the reporter's build programs were in the context at all.** The default context reads a build-program set
   only when its toolchain is one of the targets' (`context_sets`, `plan.cpp`). The reporter's host builds use the
   targets' toolchain, so `std`'s candidates were the targets' units and the build programs' together, targets
   first. A unit case with a host toolchain of its own does not show the defect; §6's cases use one toolchain.
3. **`std` can still leave a set, as designed, and that is right.** In the fixture `main.cpp` is the targets' only
   importer of `std`; without it, `std` follows another target's unit (`both.cppm`, whose `std` command is the same
   text) and never a build program's. The fixture asserts that, and that no restart happens, not that the unit
   stays `main.cpp` (§2.1: "it leaves a set only when the set's last importer goes").
4. **E1 passed.** clangd `23.1.0-mcppls.12` (the 0.23.0 payload) answered `textDocument/formatting` for a scratch
   module unit outside the database 1 ms after `didOpen`, before its build, with the `.clang-format` of a parent
   directory (`IndentWidth: 7` applied), while its import of an unknown module was being kept textual. The open
   costs a preamble build of the scratch text with a command interpolated from a neighbour; after `didClose` clangd
   still publishes one empty diagnostics list for the scratch URI, which the engine drops with everything else it
   says about such a URI.
5. **G2b covers only files the plan leaves out.** A file in `doomed` (a module it imports cannot be built) is not
   formatted this way either: its scratch build would ask for the same modules. It and the crash-ledger and
   spinning files get G2a's reason (`not-formatted`) instead.
6. **`scan()` reads the buffer** (`workspace.cpp`, `index.scan_of`), as T1 had to find out: a unit the model lists
   no imports of is asked whether it imports `std` through a scanner of the text on disk (`PlanInput::diskScanner`),
   which reads only the files the editor has open.
7. **The S1 validator holds fixtures to "provides iff importable role"**; mcpp writes `ide.role: unknown` with
   `provides` for a generated unit (§4). The `shadowed-source` fixture states `module-interface`; the unit case
   keeps `unknown`, where the role comes from the scan.
8. **S1-7.2-10 to -13 stay pending**: the server reads `ide.generated` for G1 only, not yet for the files a build
   has not written (`generated-files-missing` keeps its path-shape source, §3.0).

