# U13: unchanged persistent memo snapshots allocate during idle

This is local evidence for the released `23.1.0-mcppls.4` engine, fork
`92a4c630f4691890e6133c87d3e4591bc5fd8258`. It is not a claim that the original
600-second U13 gate has failed in the new staged replay; that result is separate.

## Isolated reproduction

`u13-memo-only-summary.json` and `u13-memo-only-samples.json` preserve the result.
The exact released engine loaded a separate copy of the staged session's 30,298,617-byte
memo file, then received only initialize/initialized. Background indexing was disabled and
no document was opened. Shutdown/exit followed after 180 seconds. Its memo store adopted
253 validation verdicts and 329 scans, with no unreadable entries.

RSS after loading was 176.832 MiB and stayed there until the first timer cycle. At about
30.7 seconds it peaked at 284.105 MiB, retaining 220.684 MiB. After the second cycle it
retained 266.754 MiB and remained there. The end-to-baseline increase is 50.85%; measured
CPU after the load is 0.008274 cores. There were zero memo-written log lines and no
compiler/index work. The original U13 budgets have not been altered to produce these results.

## Source attribution

The payload's engine manifest names the same fork commit as the inspected primary source:
[patch 0030](https://github.com/Sunrisepeak/mcppls-clangd/blob/92a4c630f4691890e6133c87d3e4591bc5fd8258/patches/0030-UP-35-module-input-sources-and-memos.patch).
The store's worker wakes every 30 seconds. Its `write()` builds the complete JSON value
through `memos()` before comparing the validation/scan version counters with the last
written counters. Therefore the unchanged path still snapshots and converts every memo.

Both caches' adoption methods insert retained entries without incrementing their change
counters. With no new compiler work the counters remain zero, matching the initially zero
written counters; the expensive snapshots are then discarded. Waiting for a written log
after warm adoption can wait forever. A stored-to-written boolean also cannot establish
snapshot coverage when stores race with serialization.

The separate original-lineage run shows a 1439 → 507 → 715 MiB trajectory. Its actual
measurement baseline precedes that drop, so a local 507 → 715 segment must not be passed
off as the formal U13 growth. It supports investigating this timer, while the isolated
reproduction establishes idle allocation without compiler work.

## Smallest correct fix

The engine should read each cache's cheap change counter before constructing snapshots.
If both equal the last written counters, return immediately. If changed, construct the
snapshot, publish it and retain the counters belonging to that snapshot. A concurrent store
must remain distinguishable on a later cycle. This preserves existing publication semantics
and avoids allocating the unchanged JSON; it does not change the interval or resource budget.
A regression should load nonempty retained memos, keep versions unchanged across timer
cycles, and prove no snapshot construction. Changed versions and stores during publication
still need their own publication checks. Then repeat the real 600-second original U13 gate.

mcppls currently has only the process-start memo-directory argument; it cannot stop or
reorder an already running engine's private worker. Turning the entire persistence feature
off would also remove warm validation/scan reuse and requires an explicit product tradeoff
and its warm-start evidence. Adding a pending heuristic, delay or RSS-selected baseline does
not fix the unchanged-snapshot allocation. No engine source or mcppls workaround has been
modified on the strength of this diagnosis, and no upstream issue/comment has been posted.
