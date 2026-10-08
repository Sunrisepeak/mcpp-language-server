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

Native follow-up: run [37709654683](https://github.com/Sunrisepeak/mcpp-language-server/actions/runs/37709654683)
at product 759deb6 passes the Darwin x64 editor job. All 40 seed-7 requests
answer within the unchanged 15 s budget (p90 368 ms, maximum 11,954 ms).
The actual startup argv contains the new priority; the cold references request
19 reports 11,853 ms. Retained artifact/log identity and exact lines are in
[darwin-native-validation.json](darwin-native-validation.json). All five
platform editor jobs pass this run; the overall run still has generated-settings
unit failures, repaired separately by 42eea9e and requiring a new CI verdict.

This is one native passing execution with the locked product payload. Different
CI hosts/runs do not establish a controlled causal speedup, statistical latency
acceptance or final maintained-engine joint qualification. Windows index
throughput has not been measured. Background index completion can take longer
under sustained foreground load. No deadline or stress assertion changed.

The later native run37714360156 at21a3675 fails the unchanged cold references
limit again. The engine receives detail.cppm references(19) at02:03:23.391 and
replies at02:03:44.949, reporting21546ms with actual namespace references.
Detail prerequisites take19.62s and the std primer21.17s; two std lock waits
are observed. This reinforces that the previous priority pass did not close
cold preparation latency. [Exact retained timeline](darwin-native-recurrence.json).
No symbol location is removed, empty result fabricated, or15s assertion changed.
