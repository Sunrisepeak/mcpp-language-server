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
`test_model.cpp`, `test_spec.cpp`) start copies of *themselves* as the child
process under test, branching on an early command-line argument before the test
framework ever runs. That is deliberate, not an accident of copy-paste: it lets
process, environment and pipe behavior be exercised on every platform without
depending on a shell or a system utility (`sh`, `cat`, `sleep`, ...) being present
or behaving the same way everywhere.
