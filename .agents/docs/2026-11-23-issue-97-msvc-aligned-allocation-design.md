# Issue 97: aligned allocation under the MSVC STL, and a workaround the maintained engine no longer needs

Status: implemented in server 0.24.1 / VS Code 0.24.1 · supplements the workaround register (#24 UP-05,
`WA-CLANGD-005`) · evidence: the report (mcppls 0.23.0, clangd `23.1.0-mcppls.12`, Windows 11, MSVC 14.52,
Slint 1.19 through `mcpplibs.mwrap.slint`), the clang 23.1.0 sources, the mcppls-clangd patch series, and a
platform-neutral reproduction on the bundled engine (§2). **Server only: no engine change, no change to Slint or
slint-m.**

## 1. What is wrong

`WA-CLANGD-005` gives every unit of an MSVC STL context `-fno-aligned-allocation`. It was added for
llvm-project#218152 (mcpp-community/mcpp#640): clang 23.1.0 declares an implicit `std::align_val_t` when aligned
allocation is on, does not merge it with the one MSVC's `vcruntime_new.h` exports from the `std` module's purview,
and every lookup of `align_val_t` in `std.ixx` is ambiguous.

The flag removes clang's implicit declaration, and with it the predefined `__cpp_aligned_new`
(`InitPreprocessor.cpp`: the macro is defined only when `AlignedAllocation` is on). `vcruntime_new.h` declares
`std::align_val_t` and the aligned `operator new`/`delete` only `#ifdef __cpp_aligned_new`. A header that uses them
unconditionally -- standard C++17, as Slint 1.19's `slint/private/vtable.h` does -- then fails in clangd only:
"no type named 'align_val_t' in namespace 'std'", and every module that includes it "did not compile". The build
(mcpp's clang 23.1.3) passes no such flag and compiles it.

Two facts make the flag unnecessary for the engine mcppls bundles:

- **The maintained engine carries the fix.** mcppls-clangd's `patches/0001-UP-05-backport-std-predefined-decl-merge.patch`
  is llvm-project#219151 (commit `52774473867e`, released in 23.1.1), with its test `clang/test/Modules/pr218152.cppm`,
  in every release since v0.0.2. `23.1.0-mcppls.<n>` has never had the defect.
- **The register does not know it.** `WA-CLANGD-005` is retired by `fixedIn = "23.1.1"`, a comparison of LLVM base
  versions. `23.1.0-mcppls.12` does not parse as a release of the line (`0-mcppls` is no number), so `needs()` answers
  "needed", and the flag has been passed for a defect the engine does not have.

So the fault is mcppls's, and so is the fix: retiring the workaround for the maintained engine is a root fix for it,
not a further compensation. Neither the engine nor Slint is at fault: Slint's code is conforming, and the engine
compiles it once mcppls stops turning aligned allocation off.

## 2. Evidence

On the bundled engine (`clangd --check`, the 0.24.0 payload, Linux host), a fake `std` module shaped like `std.ixx`
(headers in the purview, declarations exported inside `extern "C++"`, guarded `#ifdef __cpp_aligned_new` as
`vcruntime_new.h` guards them) and an importer that uses `std::align_val_t` the way Slint's `vtable.h` does:

| Engine arguments | Result |
|---|---|
| none (aligned allocation on) | 0 errors |
| `-fno-aligned-allocation` (0.24.0, `WA-CLANGD-005`) | `no type named 'align_val_t' in namespace 'std'` -- the report's error |
| `-fno-aligned-allocation -D__cpp_aligned_new=201606L` | 0 errors |

The upstream minimal reproducer of #218152 gives no diagnostic on the bundled engine with aligned allocation on, for
an x86_64-linux and an x86_64-pc-windows-msvc target: the backport is in effect.

Not run locally: the real MSVC STL (no MSVC on the machine). That is what the Windows conformance fixtures
`mcpp-msvc` and `cmake-msvc-std` (`import std` with the MSVC STL, built by clangd) and the new fixture of §4 prove
in CI.

## 3. Changes

**R1 -- the maintained engine retires `WA-CLANGD-005`.** The register gets `fixedInMaintained`: the maintained
engine (`<line>.<z>-mcppls.<n>`, mcppls-clangd) carries the fix in every release. `needs()` answers "not needed" for
such a version. Nothing else of the register changes; a workaround without the field is decided as before.

Not by an engine feature (`engine.json`'s `features`, the way `WA-CLANGD-001` is retired): the engine declares no
feature for the backport, and declaring one is an engine change this fix does not need. A later engine release may
add one; the register then names it in `fixed_by_verified_features` and the version rule can go.

**R2 -- where the workaround still applies, it keeps the standard library's declarations.** A clangd of the line
without the backport (an upstream 23.1.0 the person names with `mcppls.clangd`) still needs aligned allocation
off. With it the server now also defines `__cpp_aligned_new=201606L`: clang declares no implicit `std::align_val_t`
(that needs `AlignedAllocation`, `SemaExprCXX.cpp`), so nothing is ambiguous, and `vcruntime_new.h` declares its own.
The macro is a standard library macro for `std_unit_arguments`, so `std` and its importers agree. Not added when the
build itself turns aligned new off (`/Zc:alignedNew-`, `-fno-aligned-allocation`): that is the project's choice, and
its headers are compiled without the declarations in the build too.

What R2 costs: a new-expression of an over-aligned type does not call the aligned allocation function in clangd's
reading. clangd generates no code; the warning clang may give for it was already there under `WA-CLANGD-005`.

**R3 -- words.** CHANGELOG (`server 0.24.1`, `vscode 0.24.1`), the troubleshooting entry for "no type named
'align_val_t'", the register entry, #24's UP-05 comment, and an answer on #97.

Upgrade cost of R1: the commands of MSVC STL units lose `-fno-aligned-allocation`, so the first session after the
upgrade builds `std` and the modules once more ("command" rebuild reason).

## 4. Tests

- `tests/unit/test_workarounds.cpp`: `WA-CLANGD-005` is needed for `23.1.0`, not for `23.1.0-mcppls.12`, not for
  `23.1.1`; the traits of `23.1.0-mcppls.12` have `msvcStlNeedsNoAlignedAllocation` off.
- `tests/unit/test_normalize.cpp`: an MSVC STL unit under the workaround has `-fno-aligned-allocation` and
  `-D__cpp_aligned_new=201606L`, and `std`'s entry keeps the macro; without the workaround, neither; with
  `/Zc:alignedNew-` in the build, the flag and no macro.
- Conformance, `win32-x64`: `mcpp-msvc-aligned-new`, an mcpp project on the MSVC STL whose module includes a header
  that uses `std::align_val_t` and `::operator new(size, std::align_val_t)` unconditionally, as Slint 1.19 does, and
  imports `std`: no diagnostics, and the engine command has no `-fno-aligned-allocation`. The existing `mcpp-msvc`
  and `cmake-msvc-std` keep proving that `std` builds without the flag.

## 5. Release

Server 0.24.1 and VS Code 0.24.1 (`version --bump server`, `--bump vscode`), released as `vscode-v0.24.1`: a
plugin's patch carries the newest server patch of its series (docs/92-release.md "Versions"), so the VSIX carries
server 0.24.1. CLion and Zed get the server patch with their next patch.
