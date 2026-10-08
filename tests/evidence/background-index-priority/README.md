# Cold module work and background index priority

Run [37703379435](https://github.com/Sunrisepeak/mcpp-language-server/actions/runs/37703379435),
Darwin x64 job 113077471287, accepted references request 20 for `detail.cppm`
at 00:13:19.516 and answered at 00:13:39.388 (19,861 ms). The unchanged stress
assertion allowed 15,000 ms. The request waited for the AST rather than spending
that time finding references: detail's prerequisite modules took 20.90 s, the
std primer took 21.17 s, and main's prerequisites took 23.27 s. The trace also
records two waits on the same std module lock. Concurrent background indexing
of std.cppm ran from 00:13:15.28 to 00:13:34.763 and produced 15,466 symbols,
264,045 references and 1,354 files. Generated prime-file indexing was under 50 ms,
so removing it would not address the dominant cold cost.

[darwin-cold-timeline.json](darwin-cold-timeline.json) retains the actual log
lines and original line numbers. Full local downloaded log:
`/tmp/mcpp-artifact-11519443946/mcppls-e2e-cache-kZxypi/logs/server-20261008-001313.877-e6ab.log`.

The product now requests clangd's `background` index priority by default,
while an explicit `MCPPLS_ENGINE_ARGUMENTS` priority retains precedence. It
keeps background indexing, every provider, and foreground module preparation.
This changes OS scheduling priority rather than coalescing compiler builds or
caching scan results. LLVM's `llvm/lib/Support/Unix/Threading.inc` maps `low`
(the engine default) to macOS Utility QoS and `background` to Background QoS.
On Linux both currently map to `SCHED_IDLE`; no Linux speedup is claimed.

Focused validation: `mcpp test server` passed 54 cases / 483 assertions, including
default priority and explicit joined/separate arguments with both supported
flag prefixes. A fresh Linux stress fixture using the rebuilt product server,
the existing 23.1.0-mcppls.0 payload, and VS Code 1.132.0 answered all 40 requests
with seed 7 under the original 15 s assertion (p90 17ms, maximum 2,326 ms). The
actual clangd startup argv contains the new default. Details and raw local
report paths are in [linux-validation.json](linux-validation.json).

This is a source-backed scheduling improvement, not proof that the failing
native Darwin request now meets 15 s. That requires a native rerun with the
same engine/payload; Windows index throughput has also not been measured.
Background index completion can take longer under sustained foreground load.
No deadline or stress assertion was changed.
