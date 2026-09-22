# Real-project experience: findings and plan

Status: proposal for review · 2026-09-22 · measured on mcppls 0.0.1 (linux-x64 payload)

The goal this plan serves: **mcppls needs no configuration and never gets in the way.** Whatever the
project's state (an old pinned mcpp, a stale compile database, a module that does not compile, no
build tool at all), the user gets answers at once, at the best level the machine can give, and never
a spinning "preparing modules" or a machine at 400% CPU. "Degraded" is allowed; "stuck" is not.

## 1. What happened on xlings

Opening a few files of `openxlings/xlings` left the status at *preparing modules* for good, hover and
definition timed out, and clangd used ~4 cores.

**Why it changed.** The committed `.xlings.json` sets `"projectScope": false`, so `mcpp` in that
directory resolved to the global `2026.9.21.3`. An uncommitted local edit (2026-09-18) removed that
line; the project's pin `2026.8.8.4` took effect (verified: `mcpp --version` in a copy of each
file). That mcpp has no `emit build-database`.

**What mcppls did with it** (each step verified in the code or reproduced):

1. `load_mcpp` fell back to mcpp's `compile_commands.json` and stamped the model level 2
   (`src/project/mcpp.cpp:301-317`, `src/project/model.cpp:281`). The newer mcpp on the same
   machine was never considered (`find_tool`, `src/project/provider.cpp:10-17`).
2. That database lacks modules a dependency **generates at build time**: libxpkg's `build.mcpp`
   writes `mcpplibs.xpkg.lua_stdlib` into `MCPP_OUT_DIR`. mcppls put in a stand-in,
   `export module mcpplibs.xpkg.lua_stdlib;` and nothing else (`src/normalize/plan.cpp:505-519`).
   The same generated file existed on disk in mcpp's cache
   (`~/.mcpp/cache/build-database/*/target/.build-mcpp/deps/xpkg@0.0.57/out/`), and the root
   `compile_commands.json` named it (under a `target/` since deleted) — neither was used.
3. `xpkg-executor.cppm` uses what that module declares (`detail::log_lua`, ...), so it cannot compile
   (reproduced with `clangd --check`: 12 `no member` errors), and every importer up to `xlings.cli`
   and `main.cpp` fails with it.
4. `main.cpp` is an importer, not a unit of the failed module, so after 120 s it is "set aside" and
   clangd is **restarted** (`src/engine/clangd.cpp:869-901`, `Reclaim::if_busy`). A restart resets
   module preparation (`primer_.reset()`), which starts over, fails the same way, and restarts again
   with backoff and no cap (`src/engine/clangd/guard.cppm`).

**The blast-radius mechanism held at the module level and leaked at the process level.** Files
outside the failed closure (`src/core/log.cpp`, `config.cpp`) answered normally. But restarts and
preparation are global: every restart threw away all progress, the status never left *preparing*,
and requests in the failed closure waited out the full request timeout (10 s) to get nothing.

## 2. Measurements

A random-use probe (Neovim's LSP client, headless): open files at random, sometimes three in a row
without waiting, and ask hover / definition / references / completion / documentSymbol at random
identifiers; 60 actions, 10 s request timeout, fresh cache each run; CPU and RSS summed over the
server's process tree.

| Project, scenario | Status reached | Requests waiting the full 10 s | CPU (tree) | Peak RSS |
|---|---|---|---|---|
| xlings, pinned mcpp 2026.8.8.4 (the user's state) | never ready: *preparing* to the end | hover and definition p90 = 10 s, answered empty | **408 s in 108 s** | 5.3 GB |
| xlings, `--mcpp` 2026.9.21.3 | ready in 29 s | none | 221 s in 37 s | 5.7 GB |
| xlings, `--build-tool off` | same as pinned (cached L2 model) | as pinned | 450 s | 4.9 GB |
| xlings, `--untrusted` (meant to be L4) | still mcpp L2, stuck at 25/101 | 7 definitions empty at 10 s | **682 s** | **7.3 GB** |
| xlings, `--engine none` | degraded | module features only (documentSymbol) | 2 s | 44 MB |
| mcpp repository | ready in 20 s | none; p90 ≈ 1 s | 122 s | 4.0 GB |
| mcppls repository | ready in 3 s, then *degraded* (unresolved / ambiguous modules from `conformance/fixtures`) | none; p90 ≈ 1–3 s | 148 s | 3.1 GB |

Healthy projects are fine. One broken module in a dependency takes the whole session down, and
the one setting meant to be the floor (`--untrusted`) is the most expensive of all.

## 3. Root causes

| # | Cause | Where |
|---|---|---|
| R1 | The model source is chosen once, by kind; its **quality** is never compared. A worse result (unresolved or generated modules) never makes mcppls try a better producer (a newer mcpp that is installed) or a better artifact already on disk. | `detect.cpp:49-95`, `model.cpp:266-330`, `provider.cpp:10-17` |
| R2 | A stand-in for a module that importers **use** guarantees their failure; nothing recovers the real source (generated files in mcpp's caches, a stale-but-useful root database). | `plan.cpp:298-373` |
| R3 | Recovery is global: an importer's timeout restarts clangd and resets preparation for every file; restarts are uncapped. The "no restart" rule covers only units of the failed module. | `clangd.cpp:869-901`, `guard.cppm:14-27`, `primer.cppm:50` |
| R4 | Requests for files whose import closure contains a failed module still go to clangd and wait the full request timeout; mcppls's own engine answers only after it. | request routing / timeouts |
| R5 | Preparation keeps priming transitive importers of a failed module (totals grew 21 → 101), each one a doomed compile. | primer |
| R6 | `--untrusted` still reads the L2 database and has clangd build modules; it is not the L4 the docs describe. | `detect`/`model` under untrusted |
| R7 | The status can say *preparing* indefinitely; the `level` it shows is S1's conformance level (mcpp was "L3" with a good mcpp, "L2" with the old one), not the L1–L4 of the README. | status, S3 `project.level` |
| R8 | Logs hide the cause: stand-ins are only counted ("1 stand-ins"), never named; "could not build module" and clangd's own `E[...]` lines are `info`. | `clangd.cpp:311,780,1110` |
| R9 | Nothing tests this: the mock mcpp always advertises `mcpp.build-database`, so "mcpp too old" is never exercised; no fixture has a generated module or a failure at the base of a 100-module graph; no test measures CPU, stuck states or random use on a real project; almost every run uses one client profile. | `src/bin/mockmcpp.cpp:96-108`, `conformance/fixtures` |

## 4. Principles (acceptance terms for everything below)

1. **Zero configuration.** No setting is needed to get a working session on a project that builds.
2. **Never stuck.** No state lasts more than 60 s without progress; *preparing* ends in *ready* or
   *degraded*, with the reason and the fix named.
3. **A floor that holds.** Every open file always gets at least L4 answers (mcppls's own engine and
   the kit), within 1 s, whatever clangd is doing.
4. **A fault stays where it is** — in processes too: a failed module affects its import closure only;
   nothing global (restart, preparation reset) happens because of it.
5. **Bounded cost.** CPU and memory have budgets; failed work is not repeated.
6. **Smart, not configurable.** mcppls picks the best source it can find itself, and says which.

## 5. Plan

### Phase 0 — measure first: real-project stress, engineered (devtools + CI)

Everything below is proven by this, so it lands first.

- **`stress` check kind** in `mcppls-conformance`: seeded random use — open (and fast-switch) files
  matching globs, hover / definition / references / completion / documentSymbol at random
  identifiers. Records per method *answered / empty / timeout / error* and p50/p90/max, the status
  timeline (longest time without progress, final state), and the server tree's CPU and peak RSS
  (the runner is the server's parent: `getrusage(RUSAGE_CHILDREN)` on POSIX, job-object accounting
  on Windows). Budgets in the scenario fail the check, e.g. `{"timeouts": 0, "p90": 3,
  "maxStallSeconds": 60, "cpuSecondsPerMinute": 90, "rssMB": 3000}`.
- **Client profiles** `--client vscode|neovim|zed|plain` generalizing `--plain-client`: the
  capabilities each editor actually sends (experimental `cxxModules` or not, dynamic file watching or
  polling, `conflictArbitration`, work-done progress). Every stress fixture runs under each profile.
- **Old-producer mock**: `mockmcpp` gains an "mcpp 2026.8" mode — no `mcpp.build-database` kind,
  no `--configure-only` — so the L2 fallback path is tested at all.
- **Fixtures**: `generated-module` (a dependency's build program generates a module its sibling
  uses) × {mock new mcpp, mock old mcpp}; `failure-at-base` (a failing module under a ~100-module
  graph, generated); real projects pinned like `self-mcpp`: **xlings** at a fixed commit (with and
  without `projectScope`, i.e. new and old mcpp), **mcpp**, and **mcppls itself**.
- **devtools `stress`**: `mcpp run -p devtools -- stress --project xlings|mcpp|self|fixture
  [--client ...] [--scenario ...]` runs the matrix and prints one summary table (the one in §2),
  with `--compare base.json` for before/after.
- **Real editors**: the Neovim headless probe becomes `editors/nvim/tests/stress.lua` (same metrics,
  real client); the VS Code suite gains a stress test on the `generated-module` fixture.
- **CI**: every PR — the `generated-module` and `failure-at-base` fixtures under all profiles on
  Linux, macOS and Windows (minutes, mock mcpp). Nightly and pre-release — the real projects ×
  profiles, with budgets, and the Neovim stress on each OS. Results upload as artifacts and to the
  run summary.

### Phase 1 — never stuck, bounded cost (containment)

1. **Closure-scoped failure.** When a module fails, compute its importer closure once. Files in it
   are answered by mcppls's engine immediately (no clangd wait) and marked as such in the status;
   clangd keeps serving the rest.
2. **No global recovery for local faults.** A stuck file whose closure contains a failed module is
   never a restart reason; restarts are capped (e.g. 3 per 10 min) and never reset preparation of
   modules that already succeeded.
3. **Don't prime doomed modules.** Preparation skips transitive importers of a failed module and
   retries them only when an input of that closure changes. Totals stay stable.
4. **The status settles.** After preparation ends or stalls (60 s without progress): *degraded*
   with "N modules cannot be built because M failed: <first cause>", and an action.
5. **Budgets in the server.** clangd's `-j` and background indexing are sized to the machine and
   lowered while a closure is failing.

### Phase 2 — smart source selection (fix it without the user)

1. **Producer negotiation.** Enumerate usable mcpp executables (the one PATH/xlings resolves for the
   project, and newer installed ones in the xlings store and `~/.mcpp`); use the newest that answers
   `emit build-database`, read-only and offline, for the model only — the project's pin still builds.
   The status says "described by mcpp X (the project pins Y)".
2. **Quality-aware fallback.** Score a model (unresolved and generated modules, missing files) and
   keep the best among: cache, producer L1, producer L2, root/`build/` compile database, inferred.
   A worse source never replaces a better one; a *better* one always may.
3. **Generated-source recovery.** Before creating a stand-in, look for the module's real source
   where builds leave it: the compile database's own entry if the file exists, mcpp's
   build-database cache and `target/.build-mcpp/deps/<pkg>@<ver>/out/`. A stand-in is the last
   resort, and one whose module is *used* is reported as the cause (R8).
4. **Stale database detection.** A compile database whose entries point at missing files is used
   only for what still exists, and the status says it is stale.

### Phase 3 — the floor, the words, the logs

1. `--untrusted` becomes the real L4: no producer, no compile database's commands, clangd only on
   the kit's L4 profile or not at all.
2. One vocabulary: the status shows the README's level (L1 build database … L4 sources only) and
   the S1 conformance level only in the report.
3. Logs: stand-ins and generated-module recoveries named at `warning`; "could not build module" at
   `warning`; clangd's `E[` lines at `warning`, `I[`/`V[` at `debug`.
4. The mcppls repository excludes `conformance/fixtures` from its own model (the ambiguous and
   unresolved modules measured on itself).

## 6. Acceptance (checked by Phase 0, on Linux, macOS and Windows)

| Scenario | Must hold |
|---|---|
| xlings with the old pinned mcpp | settles in ≤ 60 s (*ready* via a newer mcpp, or *degraded* naming the module); 0 requests wait out the timeout; files outside the failed closure p90 ≤ 2 s; CPU ≤ 90 s per minute after settling; no restart |
| `generated-module` fixture, old mock mcpp | the real generated source is found; no stand-in is used |
| `failure-at-base` fixture | importers answered by mcppls's engine in ≤ 1 s; totals stable; 0 restarts |
| `--untrusted` on any project | no producer run, no module builds, first answer ≤ 1 s |
| mcpp, mcppls, xlings (new mcpp) | no regression against today's numbers (§2) |

## 7. Order and size

Phase 0 (≈ 2–3 days: runner check kind + profiles + mock mode + fixtures + devtools + CI), then
Phase 1 (containment, the user-visible fix), Phase 2 (smart sources), Phase 3 (floor and words) —
each a PR of its own, each shown against the Phase 0 table.
