# tests

Unit tests for the server, one `test_<subject>.cpp` per subject in `src/` (for
example `test_normalize.cpp` covers `src/normalize/`, `test_process.cpp` covers
`src/platform/process.cppm`). Each file is its own `main()`, built and run with:

```bash
mcpp test
```

`mcpp` discovers every `tests/**/*.cpp`, builds each as its own program against
the server's modules and `mcppls-testing` (`testing/README.md`), runs it, and a
test passes when the program exits zero.

The one thing to expect: several tests (`test_process.cpp`, `test_env.cpp`,
`test_model.cpp`, `test_spec.cpp`, `test_conformance.cpp`) start copies of *themselves* as the child
process under test, branching on an early command-line argument before the test
framework ever runs. That is deliberate, not an accident of copy-paste: it lets
process, environment and pipe behavior be exercised on every platform without
depending on a shell or a system utility (`sh`, `cat`, `sleep`, ...) being present
or behaving the same way everywhere.

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
