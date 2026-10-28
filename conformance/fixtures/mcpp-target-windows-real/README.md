# Real Windows target following

This fixture requires Windows, Visual Studio C++ tools and Windows SDK, real mcpp,
and `llvm@23.1.3`. Supply the MSVC environment captured by the Windows conformance job:

```bash
stage/mcppls-conformance.exe run --server payload/bin/mcppls.okb --payload payload \
  --fixture conformance/fixtures/mcpp-target-windows-real \
  --msvc-env "$RUNNER_TEMP/msvc.env" \
  --workspace-dir "$RUNNER_TEMP/target-real/workspace" \
  --cache-dir "$RUNNER_TEMP/target-real/cache" --keep --verbose --timeout 180
```

`MCPPLS_REAL_MCPP` can select a real mcpp executable. The scenario never fixes the
server target. Its preparation runs real configure-only builds for i686 and x64,
copies each complete published CDB unchanged, and configures i686 again. During
one LSP session it publishes these original databases in the order 32 → 64 → 32.
The manifest default is x64, so the initial 32-bit result proves that auto mode
uses the published CDB rather than that default. Checks cover the requested and
effective targets, actual engine command, architecture-dependent module constant,
module navigation, and diagnostics in both source files. The constant also has a
pointer-size assertion, making inconsistent architecture preprocessing visible.
The module does not import `std`: Clang does not support coroutines for 32-bit
x86 MSVC targets and does not define `__cpp_impl_coroutine` there, so the MSVC
STL's `<coroutine>` is empty and its `std` module (`<generator>`) cannot be built
by any Clang for i686, clangd included. mcppls then reads the project with the
semantic kit (`std-fallback-kit`), which is the right answer for such a project
but would hide the target switch this fixture checks. The standard library module
set of a 64-bit MSVC target is covered by `mcpp-llvm-msvc` and `mcpp-split-msvc`.

The empty `[target.i686-windows-msvc]` section is a legitimate project custom
target declaration: mcpp's built-in target list does not include this spelling.
It does not imply that every compiler triple is a built-in mcpp target. Real
mcpp versions 2026.9.30.2 and 2026.10.21.1 accepted the canonical
`i686-pc-windows-msvc` spelling supplied to `emit build-database` with this
manifest, resolving it to the declared target before checking SDK availability.

The strict expected CDB flags are `--target=i686-pc-windows-msvc` and
`--target=x86_64-pc-windows-msvc`. In the pinned mcpp v2026.9.30.2 sources,
`src/build/prepare/toolchain.cpp` derives `crossTargetFlag` using `llvm_triple()`
whenever a target is explicitly selected with a Clang driver; this condition
does not require the selected target to differ from the host;
`modules/toolchain-model/src/triple.cppm` adds `-pc-windows-msvc`;
`src/toolchain/hostflags.cppm` passes that token into the compile flags; and
`src/build/compile_commands.cppm` writes those tokens into `arguments`.
Native execution must still verify the generated databases and effective driver
facts. This fixture contains no build program and does not test host build
context filtering.

Keep the complete `recorded-i686.json`, `recorded-x86_64.json`, final
`compile_commands.json`, model reports and logs as native CI artifacts. For
replay evidence, compute hashes without altering any rows:

```bash
cmake -E sha256sum "$RUNNER_TEMP/target-real/workspace/mcpp-target-windows-real/recorded-i686.json" \
  "$RUNNER_TEMP/target-real/workspace/mcpp-target-windows-real/recorded-x86_64.json"
```

Local preflight on Linux reached real LLVM target resolution for both targets,
then stopped because Visual Studio tools, MSVC STL and Windows SDK were absent.
The pinned producer also accepted the canonical i686 alias before reporting the
same missing SDK. No CDB was generated locally, so there are no local generated
CDB hashes or full end-to-end pass claims. Windows CI is the native acceptance
gate; a Linux SDK failure does not establish that gate has passed.

A separate Linux preflight with pinned mcpp 2026.9.30.2 and explicit
`x86_64-linux-gnu` confirmed that this project without `import std` produces
only its project set. With the standard import and type assertion present, real
`emit build-database` produces both the project set and `mcpp:std` (two units),
and the existing specification validator accepts the complete envelope. This
checks the standard-library contract on Linux, not the Windows target chain.

The same Linux preflight ran real `build --configure-only --target
x86_64-linux-gnu`: both project CDB entries retained the explicit
`--target=x86_64-unknown-linux-gnu` token despite selecting the host architecture.
This supports the explicit-host flag path; native Windows CDB spelling remains
a strict CI assertion.
