# modules

Library packages this repository owns, each a separate mcpp package that `mcpp.toml` depends on by
path. They are here rather than under `src/` because they are *packages*, not source files of the
server: each has its own manifest and could be consumed on its own.

| Directory | What | Depended on as |
|---|---|---|
| `os/` | Three platform-constant packages (`linux`, `macos`, `windows`). One is chosen by the build target and read with `if constexpr`, so platform differences are values rather than `#ifdef` | `[target.'cfg(...)'.dependencies]` |
| `testing/` | `mcppls.testing`, the minimal named-module test harness every `tests/test_*.cpp` imports | `[dev-dependencies]` |
