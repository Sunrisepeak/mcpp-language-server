# openkal-linux

The reference implementation of [openkal](https://github.com/mcpplibs/openkal)
for Linux, written on the kernel's own system-call interface.

```toml
[dependencies]
openkal = "0.14.0"

[target.'cfg(os = "linux")'.dependencies]
openkal-linux = "0.14.0"
```

## Why it does not use a C library

Version 0.4 of this implementation was written upon the C library of the host,
and that is a correct implementation of openkal. It is not a correct
implementation for every program above openkal, and the case where it fails is
the case the specification cares most about.

A C library ported onto openkal defines `write`, `malloc` and `open`. So does
the host's. Two definitions of one name cannot both be reached from one program,
so an implementation that calls the host's calls the ported one instead — and
the ported one calls openkal, which calls the implementation, which calls it
again. The recursion is unbounded and neither side reads as though anything is
wrong.

The remedy is not a linking arrangement. It is that an implementation which
sits beneath a C library must not depend upon one. This implementation contains
the kernel's calling convention and no reference to any C library symbol, and
CI asserts that by examining the undefined symbols of the objects it produces.

A second consequence is less obvious and equally binding. A structure passed to
the kernel has the kernel's layout, which is not the layout of the same-named
structure in any particular C library: musl's `struct stat` and glibc's differ,
and an implementation compiled against one and linked with the other would read
the wrong fields. The kernel's own layouts are therefore declared in `src/sys.h`.

The specification says an implementation may be built upon a C library, beneath
one, or without one. This is the first implementation that demonstrates the
second and third.

## Interfaces provided

All eight. `tools/check-surface.sh --complete` in the specification package
compares the exported names against `SURFACE.txt`.

## Conformance

The suite lives in the specification package and is the same suite every
implementation runs. That it is the same suite is the point: a conformance suite
that differed between implementations would be testing implementations rather
than the specification.

```bash
git clone https://github.com/mcpplibs/openkal .spec
bash .spec/tools/run-conformance.sh openkal-linux . full
```

This package's own `tests/` are additional, and examine the operations version
0.5 added rather than repeating what the shared suite already observes.

## The `standalone` feature

Whether this implementation is the whole of the program's environment.

openkal says nothing about what else a program contains, and there are two
arrangements. Ordinarily a program already carries a runtime of its own; that
runtime has already received control from the environment and already creates
execution contexts, and this implementation borrows both. That is the default.

Sometimes there is no such runtime — because the program supplies one itself, or
because it has none. Then nothing in the program has received control and
nothing creates contexts, and this implementation does both:

| | |
| --- | --- |
| the program entry | `_start`, which finds what the kernel left on the stack, establishes the thread pointer, and hands control to whatever the program calls its beginning |
| thread-local storage | a block per execution context, built from the program's own `PT_TLS` segment, installed through the register the processor reserves |
| execution contexts | `clone` directly |

The feature is a statement about the program, not a smaller or faster variant of
this implementation, and the consumer that knows which arrangement holds is the
one that declares it. `openkal-musl` declares it, because a program above a C
library carries no other runtime by construction.

## Points of interest for other implementations

**Allocation is not built upon a C library's allocator.** Clause 7.3 requires
that where the environment already provides an allocator, this one be built upon
it. The environment here is the kernel, and the kernel provides mappings rather
than an allocator, so `src/memory.cpp` supplies one. The hazard the clause names
is two claimants upon one region on a system whose heap grows by extending a
single region — that is, `brk`. Nothing here uses `brk`, so a program containing
both this allocator and a C library's has two allocators drawing on disjoint
mappings, and neither can shorten the other's.

**The working directory is reported under its own name.** `kal_fs_preopen(0)`
reports the absolute path rather than `"."`. A C library above openkal must both
resolve an absolute path and report one, and a name of `"."` leaves it able to
do only the first — which version 0.4 did, and which is why `getcwd` could not
have worked above it.

**Interruption is retried, not reported.** A caller cannot distinguish an
interrupted call from a genuine failure without knowledge of the platform, and
an implementation that reports it produces short transfers on any system that
delivers signals — a defect unlikely to appear during testing.

**Errors are translated through a table.** The kernel's values are mapped onto
the closed set the specification defines. Translation preserves the naturalness
requirement of clause 7.1; reconstructing a foreign namespace would not.

**Positioning is absent from `openkal.stream`, and its absence is confirmed
here.** Whether a stream can be repositioned is a property of the individual
descriptor: the same implementation succeeds for a regular file and fails for a
pipe. Had `openkal.stream` offered positioning, this implementation could
neither claim it honestly nor withhold it usefully.

**The compiler is not permitted to reach for a runtime the program may supply.**
The implementation is compiled without exceptions, without run-time type
information and without the stack protector, and the flags are attached to this package's own sources rather
than to the whole build: a module interface records the dialect it was compiled
under, so a translation unit compiled without exceptions cannot import one
compiled with them, and the conformance tests are consumers rather than parts of
the implementation. For the same reason the implementation includes the
specification's C headers rather than importing its modules.

The four names the compiler may emit calls to — `memcpy`, `memmove`, `memset`
and `memcmp` — are the exception a freestanding implementation is permitted to
require, and they cannot re-enter this implementation because they compute
rather than call.

## Architectures

`x86_64` and `aarch64`. Adding one is `src/sys.h`: a calling convention, a table
of system-call numbers, the kernel's `struct stat`, and the two thread-pointer
conventions in `src/tls.h`.

## Verification

`mcpp test` runs six suites. Five cover the interfaces; the sixth covers the
operations version 0.5 added, and each of its assertions is written so that it
can fail — the three conditions `kal_fs_open` exists to express are observed by
their effect rather than by a return value, because a truncation that did not
happen leaves a longer file while every call reports success.

## License

Apache-2.0.
