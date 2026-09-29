# vendor

Upstream data and source this repository carries a copy of, with its licence. Nothing here is
written by this project except where a row below says it is patched.

| Directory | What | Used by |
|---|---|---|
| `lsp-metamodel/` | The Language Server Protocol 3.18 meta model (`metaModel-3.18.json`), Microsoft's machine-readable description of the protocol, with its licence | `mcppls-lspgen` generates `src/lsp/protocol.cppm` and `.cpp` from it; CI regenerates and fails if the result differs from what is committed |
| `openkal-linux/` | **Patched.** `mcpplibs/openkal-linux` 0.15.0, the Linux implementation of openkal beneath musl, as 0.15.1 with two fixes for termux / PRoot (below). Its `LICENSE` (Apache-2.0) is kept | The root `mcpp.toml` declares it by `path`, so every Linux build compiles this copy instead of the registry's 0.15.0 |

The generator itself is ours and lives with the rest of the source, at `src/bin/lspgen.cpp`.

## openkal-linux 0.15.1: why it is here

termux with PRoot is a supported environment (issue #32), and openkal-linux 0.15.0 cannot start a
single program in it. Two defects, both in that package, found with the reproducers in the 0.0.7
plan (`.agents/docs/2026-09-30-stability-performance-plan.md` §1.2):

1. **`execveat` (X-1).** `kal_process_spawn` always starts a program with `execveat(dirfd, relative
   name)`. termux's PRoot answers ENOSYS to `execveat` with a real directory descriptor, and the
   result is that every start fails as "not supported". *Changed (`src/process.cpp`, `src/sys.h`
   `join_under`)*: when the child reports ENOSYS the parent records it in a process-wide flag and
   starts the program again with `execve` and an absolute name (`/proc/self/fd/<dir>` read and joined
   in the parent before the duplication, so the child still allocates nothing). `#!` scripts work,
   because the interpreter is handed a real path. `okl_execveat_unavailable(int)` exposes the flag:
   `mcppls.platform` sets it under PRoot (proot-me's ptrace mode lets the kernel run an untranslated
   `execveat` and the program dies at its first `brk`), and `tests/test_process_fallback.cpp` sets it to
   test the path on a system that has the call.
2. **Registers PRoot rewrites (X-4).** `okl::sys` declared the argument registers input-only. In its
   default seccomp mode PRoot rewrites RSI for `openat` (the path pointer) and never restores it, so a
   value the compiler kept "from RSI" after the call was garbage; the `"/"` preopen lost its name and
   every program path was "outside every preopened directory". *Changed (`src/sys.h`)*: every
   argument register is an in/out operand (`"+D"`, `"+S"`, `"+d"`, `"+r"` for r10/r8/r9 on x86_64; x1
   to x5 on aarch64). That is what the kernel ABI allows anyway and it costs nothing.

Not copied: upstream's `tests/`, `examples/` and CI workflow. The package's own `mcpp.toml` says
0.15.1 and carries the same explanation.

**To upstream and remove.** Both changes are to be sent to `mcpplibs/openkal-linux`. When a release
carries them: delete `vendor/openkal-linux/`, delete the `openkal-linux = { path = ... }` line and its
comment from the root `mcpp.toml`, delete the NOTICE entry and this section. `mcppls.platform` warns at
startup when no preopened directory is named `/`, so a regression of the second defect shows there
first.

`openkal-musl` needs no change: it replaces musl's `syscall_arch.h` and reaches the kernel through
openkal (`__okm_syscall` calls `kal_*`), so it issues no raw system call of its own.
