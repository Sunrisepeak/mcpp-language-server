# #59 performance evidence

Status: implementation and verification in progress. Source/test changes in the shared worktree
are under review; they are not a merged fix or release. RSS-trend gating has not been adopted
or implemented. Final claims require the original gates and replay evidence.

Local Linux measurements use the released v0.0.16 payload (clangd 23.1.0-mcppls.4), the
original pinned source fixtures and explicit CPU affinity. `environment.json` identifies
producer versions, CPU topology and limitations. These are local reproductions, not claims
about unrun GitHub, macOS, Windows or Termux environments.

## U15

`mcpp-cold.json` reproduces the historical 117-second symptom: 118.22 seconds without a
completed preparation node. `mcpp-pinned-cold.json` uses the workflow's actual producer pin
2026.9.30.2 and measures a 70.49-second node gap, with 1.458 CPU cores consumed in that gap,
no restart, and first ready at 101.27 seconds. Both keep the original 60-second threshold.

`u15-pinned-build-completions.json` preserves the 176 actual module completions observed in
the pinned run. The slowest single build took 4.349 seconds; std took 3.709 seconds. A whole
closure completed while its prime TU had not yet published diagnostics. The engine previously
mistook this for no progress and could raise `preparation-stalled`. The fix recognizes genuine
completed BMIs for wanted modules, without advancing the independent preparation-node counter.
Unrelated work, reuse, old-generation events and an otherwise silent unfinished closure remain
non-progress; native mock-process tests cover these distinctions.

The runner preserves `longestStallSeconds` and adds `longestEngineInactivitySeconds`. A gate
on actual inactivity falls back conservatively to the original node gap if logs/history are
missing. The old gap is still independently gateable; it is never replaced with a heartbeat.

`u15-recovered-two-core-cold.json` repeats the complete original mcpp cold stage after the
environment rebuild, with current server, released .4 engine, explicit producer 2026.9.30.2
and two logical CPUs (4–5). All seven checks pass. The raw prime-node gap is still 77.73 s
with 1.51 CPU cores consumed, while genuine module completions keep engine inactivity to
4.00 s; first ready is 113.35 s, with zero preparation-stalled samples and zero restarts.
This preserves the original symptom in the measurement while proving the activity fix.

## U13

`xlings-long.json` is explicitly a **cold-cache idle control**, not the original nightly's
cold/warm/edits/long cache lineage. It reproduces 0.07 cores and memory growth. Trace comparison
finds finite background indexing and an implementation sweep, rather than repeated producer
calls: auto priming opened about 120 units and built 135 modules; priming off opened one std
unit and built one module. CPU affinity classes differ for some controls, so their timings do
not establish a controlled speedup.

The resource check now drains explicit preparation/index/implementation work with a bounded
wait, records that phase separately, and then measures the same 600-second, 0.01-core, 10%-RSS
idle budgets. It never chooses an idle baseline by requiring CPU to pass its own budget.

The .17 checkpoint still shows a finite RSS jump after known background work becomes idle:
about 472 MiB to 562 MiB over approximately 30 seconds, followed by a flat multi-minute period.
The new task-state eligibility therefore cannot yet be claimed to fix the RSS budget. The same
600-second replay and the original cold/warm/edits/long lineage are still running; their final
results must be reviewed before changing any memory metric.

`xlings-recovered-cold-idle.json` is a new independent cold-cache control after the environment
was rebuilt, with the explicit historical producer 2026.9.30.2 and released .4 engine. It
passes the unchanged 600-second idle gates: 0.002835 CPU cores and RSS 967.586 → 967.738 MiB
(+0.0157%). The task-state drain records 68.36 seconds of finite scheduled work before the
baseline; 119 implementation units and 135 module builds are complete. Its 1-second smaps
samples do not reproduce the prior +19% jump. Inspection of the exact released engine source
confirms the memo worker constructs all memo JSON every 30 seconds before comparing versions,
but this control does not prove that code caused the earlier jump. No memo-publication
readiness workaround or new memory metric has been introduced. The original staged cache
lineage is recorded separately below.

The complete original xlings cold → 300-second warm → full edits → long sequence now passes
all local checks with the released .4 engine and explicit historical producer 2026.9.30.2
on CPUs 12–15. `recovered-xlings-staged-{cold,warm,edits,long}.json` preserve every check.
The original 600-second U13 gate reports 0.007208 CPU cores and RSS 1438.777 → 715.398 MiB
(-50.28%). The baseline follows the existing 60-second warm-up and the task-state drain;
it was not selected from CPU/RSS samples. U12 checkout and U15-long pass too.

A separate isolated reproduction does establish an engine limitation: unchanged persistent
memo snapshots allocate every 30 seconds even with no opened documents or background index.
`u13-memo-only-summary.json`, `u13-memo-only-samples.json` and `u13-memo-worker.md` show
176.832 → 266.754 MiB (+50.85%) during 180 seconds after warm adoption, with zero writes.
This is not a claim that the staged gate failed: that gate starts before a later large RSS
release, so its net growth is negative. It also does not mean the memo limitation is fixed.
The exact engine-source diagnosis and smallest engine fix are recorded without changing
clangd, disabling persistence, adding pending heuristics/delays, or changing the idle budgets.

## U8

A small isolated replay opening only the binfmt source reused the old configuration with zero
new completed builds. This narrow result does not cover the original importer working set.
The stronger replay opens the original eight U7 importers with clean buffers, plus main and
execute, before release → dev → release. It observes 20 actual module completions during undo
(counter 58 → 78), while the older source/size file heuristic reports 19. These counters alone
do not distinguish a previously built release cache key from a first build under that key.
The final module-cache disk usage is 304 MiB, so a 3 GiB payload quota alone does not explain
the builds.
`u8-clean-importers-summary.json` and `mcpp-u8-clean-importers.json` preserve the evidence.
The new `u8-owned-cache-control.json` and `u8-owned-cache-summary.json` add a fresh-cache
control with the released .4 engine and explicit historical producer 2026.9.30.2. The full
release closure builds 176 modules, dev builds 176, and undo builds zero. The owned ledger has
176 → 352 → 352 stable keys; the release engine CDB is byte-identical after undo. This proves
reuse of the fully built original closure in this control. Its cache lineage differs from the
earlier warm/edit experiment and does not establish the cause of those 20 builds. No upstream
defect or definitive project-cache root cause has been claimed.

The stronger original two-logical-CPU cold → 300-second warm → full edits sequence does
reproduce a failure: `recovered-mcpp-two-core-edits.json` reports 28 completed builds during
undo (26 from the older physical heuristic), with the original zero budget unchanged.
`u8-two-core-failure-attribution.json` retains the individual owned keys and PCM metadata.
All 218 old release stable entries survive; the engine CDB is byte-identical after undo.
Before undo the runner nevertheless observes `ready` with preparation done=0/wanted=140,
running=1 and 11 files still awaiting diagnostics. The runner's former settle predicate had
not drained that work.

This is more than a mislabeled count. While the old dev process continues, the replacement
release CDB is published in place. Its new platform PCM has release optimization metadata
but retains dev prerequisite producers. The later release process reuses the original valid
release std/platform entries. The working-tree fixes separately add task-state drains around
the U8 experiment and defer replacement CDB publication until the old process is reaped.
The original failure is retained; fixed full two-core replay is still required.

The same original two-core workspace then ran faults with an independent cache-b:
`recovered-mcpp-two-core-faults.json` passes every check in 262.84 seconds. U15-faults records
59.66 seconds of raw node stall, 7.42 seconds of genuine engine inactivity, zero stalled
samples and no clangd restarts (the fault checks deliberately restart the server).

A full original mcpp edits run under GDB completed with no runner crash, but that replay used
unrestricted CPU affinity. One earlier two-logical-CPU runner process exited 139 without a core
or stack; it remains unconfirmed, not a proven fixed defect.

The earlier complete staged xlings replay passed. U8 now has artifact-backed attribution
to unfinished dev work and release CDB publication while that process remained alive; native
regressions cover the driver fix and the separate measurement drain. Full original two-core
and xlings chains against the final frozen bytes are running in new validated directories.
The original `/tmp/mcppls-perf59` traces were lost when the environment was rebuilt; the JSON
evidence here survives. New raw logs, source/ledger snapshots and smaps samples are retained
in the ignored, persistent `.payload-cache/59-63-performance/` directory. This record will be
completed with final replay results before submission.
