# UP20 original fanout save passes with published prerequisite generations

Original recovered U7b source/CDB/scenario/product and CPU affinity are unchanged; the cache is fresh. Private joint source7214ddce4 (through0063/0064), clangdSHA9052437d175c75b2b4f4a87fa12baef74bc9028b4ac4f527a360ae8a5408ea96, matching builtin resources. The original check passes in61.59s: zero crashes/restarts, all8 importers republish6.313s after save, probe clears2.306s, recovery6.481s against60s. The maintained59 control had6 crashes and319.50s recovery failure.

Patch0063 binds consumer cache admission to published prerequisite generations. A separate normal std→Consumer profile-change control fails before and passes after, while a changed temporary read-copy retains warm published reuse. Root integration passes all64 prerequisite units923ms and produces the identical agent binary SHA.

[Exact result and invocation](result.json). Raw log is retained privately by SHA; no forced kill qualified this run. This is one Linux development artifact/save result, not proof of native cross-platform fixes,1000 saves or latency distributions. Seven interactive requests receive replies, engineShare75%; their p951.14s does not pass the completion performance gate. Current GCC/libstdc++16.1 dependencies differ from the historical profile. Both PRs remain draft.
