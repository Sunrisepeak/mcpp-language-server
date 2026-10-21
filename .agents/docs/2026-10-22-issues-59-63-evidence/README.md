# #59 performance evidence

Status: implementation and verification in progress. Source/test changes in the shared worktree
remain unsubmitted; they are not a merged fix or release. RSS-trend gating is only a proposal
and has not been implemented. Final claims require the original gates and replay evidence.

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

## U8

A small isolated replay opening only the binfmt source reused the old configuration with zero
new completed builds. This narrow result does not cover the original importer working set.
The stronger replay opens the original eight U7 importers with clean buffers, plus main and
execute, before release → dev → release. It reproduces 20 actual module completions on undo
(counter 58 → 78), while the older source/size file heuristic reports 19. The final module-cache
disk usage is 304 MiB, so a 3 GiB payload quota alone does not explain these rebuilds.
`u8-clean-importers-summary.json` and `mcpp-u8-clean-importers.json` preserve the evidence.
Exact command identity and cache provenance still need a snapshot replay; no upstream defect
or definitive project-cache root cause has been claimed.

A full original mcpp edits run under GDB completed with no runner crash, but that replay used
unrestricted CPU affinity. One earlier two-logical-CPU runner process exited 139 without a core
or stack; it remains unconfirmed, not a proven fixed defect.

The original staged xlings replay and final U8 root-cause diagnosis remain incomplete.
Raw local traces/logs/snapshots are in `/tmp/mcppls-perf59`; large traces are intentionally not
committed. This record will be completed with final replay results before submission.
