# Linux process-tree resource preparation

This observer is staged for an independent final72 resource replay. It has not
qualified real clangd, the product, process-tree regressions or physical cache
allocation. All elapsed times from its runs are excluded from latency evidence.
Engine source, patch series, binaries and running clean build were unchanged.

The actual Python controls exercise a parent and fork child allocating memory,
CPU work, reaping and PID/start-time identity rejection. The framed mock LSP
control exercises ordinary/deferred request boundaries and owned-process cleanup.
It is not a C++ semantic control. The original timestamp negative is retained;
current observer records observation time before releasing the snapshot lock.

Read-only audit of the recorded control:

```sh
python3 audit-tree-controls.py
```

Live control drivers require a fresh directory with the scripts, because they
refuse to overwrite existing fixtures/results. The resource-only project wrapper
uses the local engine repository's replay/probe implementation and must be run
with `--resources`. It adds an explicit `latency_qualification: false` marker.

CPU intervals include own plus waited-child counters with quantization bounds.
Membership/reap races reject snapshots; stable-window gaps reject qualification.
PSS apportions mapped shared pages; summed RSS double counts them. Neither is the
physical allocation count of a completion cache. Periodic observation cannot
prove all fast unsampled child lifetimes/escapes. Real engine/product baselines,
1000 saves, long soak and cache budgets remain unverified.
