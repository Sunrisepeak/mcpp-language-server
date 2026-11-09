# tests

The tests of the server, by kind (mtest's layout, [docs/layout.md](https://github.com/mcpp-community/mtest/blob/main/docs/layout.md)):

| Directory | Kind | What |
|---|---|---|
| `unit/` | `UTest` | one `test_<subject>.cpp` per subject in `src/` (`test_normalize.cpp` covers `src/normalize/`), one program run by `mcpp test` |
| `isolated/` | `UTest` | the subjects whose tests run again with a cache of their own, a program each |
| `cts/` | `CTS` | the conformance fixtures as cases, `mcppls-cts` ([cts/README.md](cts/README.md)) |
| `bench/` | `Bench` | the measurements, `mcppls-bench` ([bench/README.md](bench/README.md)) |
| `harness/` | | how the cases drive the server, and the benchmark's own logic with its tests |

A unit test file declares objects, each a case and the scope of the cases its body declares:

```cpp
UTest normalize_tests{"normalize", [] {
    "a command line keeps its order"_test = [] { /* ... */ };
}};
```

```bash
mcpp test
```

The files of one directory are one program (`[test] programs = "directory"`, mcpp 2026.11.8.1):
`tests/unit/` is the program `unit`, built once against the server's modules and
[mtest](https://github.com/mcpp-community/mtest)'s library (`mcpplibs.mtest`), and run as a process
per file of its cases (`unit/test_glob`, ...), so the files keep their isolation and run in
parallel with one link. Each object and each `"name"_test` is a case that mtest and the editors list
and run alone; every run is kept in mtest's store (`target/mtest`), which CI reports per job and over
every job.

The one thing to expect: several tests (`test_process.cpp`, `test_env.cpp`, `test_spec.cpp`,
`test_bundle.cpp`) start copies of *themselves* as the child process under test. Each declares what a
copy does as an mtest `child` and starts it with `child_command` (mtest 0.5.2), so a copy never runs
the cases and no file of the program defines `main` for it. That is deliberate: it lets process,
environment and pipe behavior be exercised on every platform without depending on a shell or a system
utility (`sh`, `cat`, `sleep`, ...) being present or behaving the same way everywhere. The program
begins at `test_conformance.cpp`, whose `main` is also the server the conformance runner starts
(`serve`). The two tests that run themselves again with a cache of their own
(`isolated/save-plan`, `isolated/answer-routing`) keep their `main`, and are each a directory of
`tests/isolated/`: a program's environment does not change while it runs (mcpplibs.os).

Build the tools with `mcpp build` before running `mcpp test` to exercise the
process integration tests against `mcppls-mock-model` and `mcppls-conformance`.
The latter drives a native fake LSP server and verifies UTF-8 measurement JSON,
malformed external output, and nonzero failures when artifacts cannot be written.
Those tests print a note when the corresponding tool build is unavailable. When
several tool builds coexist, `MCPPLS_CONFORMANCE=/absolute/path/to/mcppls-conformance`
selects the exact runner build for the UTF-8 regression.

The runner's fake-LSP test also checks per-word typing traces: an incomplete answer causes one
new request, a complete answer stops reasking, and the trace records the original metadata
without completion text. `test_answer_routing` holds actual workspace engine callbacks across
cached fallbacks and proves that their full latency teaches the next request's adaptive wait.
The fake LSP also exposes a partial owned BMI copy until its reported diagnostics work finishes:
cache-reuse checks must drain that work, accept the finished copy, and still reject new builds,
missing work state and a wait that never finishes.

The same Workspace tests resolve definition and declaration ownership before a rename in a
workspace with read-only engine sources. A compiler that returns only project edits must not
rename an interface symbol; writable symbols still work. The controls cover both location
forms, shared deadlines, advisory cancellation, stale models/documents, closed-file stamps,
and synchronous callbacks. The unchanged `mcpp-build-programs` R1 fixture checks the editor
contract with the real engine.

`test_process` starts its CPU sample after both children acknowledge readiness. The busy child
then acknowledges at least 0.75 seconds of independently measured native process CPU, before
blocking for the parent sample. The original busy >0.5 / idle <0.2 CPU-second gates remain;
work and handshakes share a 30-second deadline. Existing bounded sampler calls and process
cleanup may finish after that deadline; a late sample still fails. No CPU share is assumed from wall time.
The oracle bypasses the subject reader and OpenKAL's monotonic implementation of `std::clock`.
