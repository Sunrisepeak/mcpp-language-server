# testing

`mcppls-testing`, a minimal named-module test harness (`import mcppls.testing;`),
the sole dev-dependency of `tests/` (`mcpp.toml`'s `[dev-dependencies]`).

```cpp
import mcppls.testing;
using namespace mcppls::testing;
int main() {
    "adds"_test = [] { expect(1 + 1 == 2) << "arithmetic"; };
    return report();
}
```

It provides `"name"_test = []{...}` to register and run a named case, `expect(cond)`
for a non-fatal check and `expect(fatal(cond))` for one that aborts the current case
on failure, both returning an `Expectation` that `<<` can append a message to on
failure, and `report()`, which prints a summary and returns the process exit code.

It exists in place of a third-party framework because boost.ut 2.3.1 does not
compile with clang 22.1.8 on a Windows host — a frontend crash in code generation,
in both the dev and release profiles (design doc issue log, K5).
