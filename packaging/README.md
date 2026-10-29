# Packaging

Everything mcppls ships besides its own source: **one payload** for every platform, holding the
server, the runner of every system, the maintained clangd (mcppls-clangd, LLVM 23.1) of every
platform and the openkal semantic kit; and the **semantic kit of each target**, published on its own
(ecosystem design 2026-10-29 §9). The VS Code extension and the CLion plugin carry the payload; the
xlings packages carry the server's openkal tree and the kits.

```
packaging/
  payload.lock.json        every upstream input, with its sha256, and the one table of platforms
  release.manifest.json    what a release carries
  xlings/                  xpkg descriptor templates for mcpp-language-server and the kits
  kits/README.md           what each kit contains and why
```

This directory holds data only. The code that reads it is `modules/pack`
(`mcppls.pack.*`), run through `mcppls-devtools` (docs/93-devtools.md):

| Module | Command | Does |
|---|---|---|
| `mcppls.pack.fetch`, `.lock` | (inside the others) | download an input named by the lock and prove it by sha256 |
| `mcppls.pack.clangd` | `payload --only clangd --platform P` | take the maintained engine part as built, or reduce an official clangd release to what is shipped |
| `mcppls.pack.openkal` | `kit --openkal` | assemble the openkal kit from mcpp's build database of each openkal target |
| `mcppls.pack.kit` | `kit --platform P` | assemble the semantic kit of one platform's target (kits/README.md) |
| `mcppls.pack.payload` | `payload`, `payload --verify`, `payload --from`, `kit --verify` | put the parts into the payload layout, and verify a payload or a kit |
| `mcppls.pack.release` | `release check` | check a staged release against the manifest |

`release xlings` (devtools) renders the xim-pkgindex descriptors from a release's assets. It is C++
on openkal like the server, so a machine that can build mcppls can package it: no interpreter.

## The payload layout

The server and every editor rely on this layout; `payload.json` is its manifest.

```
<payload>/
  payload.json                          payload-version 5: the server, the kit, and per platform
                                        (`platforms.<platform>`) its runner, its engine with version,
                                        path and identity, and its integrity-checked files
  bin/mcppls.okb                        the server: its image for x86_64 and for aarch64, one okb
                                        (`mcpp pack --pkg-format okb`)
  libexec/okrun/<system>-<arch>/okrun[.exe]
                                        the runner of each system, which starts it: linux-x86_64,
                                        linux-aarch64, macos-aarch64, windows-x86_64
  clangd/<platform>/bin/clangd[.exe]    the engine of each platform: linux-x64, linux-arm64,
                                        darwin-arm64, win32-x64
  clangd/<platform>/lib/clang/<major>/include/
                                        clang's builtin headers, found beside clangd
  clangd/<platform>/engine.json         the maintained engine's source and binary identity
  kit/kit.json + kit data               the openkal kit (spec S4, kit-version 2)
  licenses/                             mcppls and LLVM license texts
```

Every program and editor takes the entry of the platform it runs on — the server its clangd, an
editor the runner that starts the server — and refuses a payload that has none for it, naming the
platforms it carries. The server takes the payload named by `--payload`, or else the payload that
contains its own image. `--clangd` and `--kit` replace one part of it. A part that is still missing
is looked for outside a payload: clangd on `PATH`, and the kits installed by xlings.

## The kits

The payload carries the **openkal kit**: libc++'s headers and the `std` and `std.compat` module
sources of openkal-platform's C++ run-time, openkal-musl's headers and the generated headers of each
architecture. It is made from what mcpp compiles an ok image with: `mcpp emit build-database --spec
compile-commands --target <arch>-openkal` names the system include directories of the run-time's
std module, which are copied once — shared when both targets read them, under `include/<target>/`
otherwise — and a unit of the project gives the defines a unit reads with. Without the target's own
kit a project reads with it: `import std` and the standard library work, the target's system
headers and macros do not.

The **target kits** — `mcppls-kit-<target>` for each platform's target — are made per platform
(`kit --platform`, kits/README.md) and published as `mcppls-kit-<target>-<version>.tar.gz`, one top
directory holding kit.json. The server installs the kit of a project's target with xlings when the
editor asks, into `<store>/xim-x-mcppls-kit-<target>/<version>/`, where it also finds one unpacked
by hand.

## Inputs: `payload.lock.json`

| Entry | What | Used by |
|---|---|---|
| `mcppls-clangd-<platform>` | the maintained engine, mcppls-clangd 0.0.17's `clangd-23.1.0-mcppls.5-<platform>.tar.gz`: clangd, its builtin headers, licenses and `engine.json` (source and binary identity), taken byte for byte | `mcppls.pack.clangd` |
| `llvm-project-src` | llvm-project 23.1.0 source archive | `mcppls.pack.kit`, recipe `libcxx-source` |
| `llvm-mingw` | llvm-mingw 20260826 (LLVM 23.1.0), UCRT, Linux x86_64 host | `mcppls.pack.kit`, recipe `llvm-mingw` |

`platforms` is the one table of platforms. Each row, named `<os>-<arch>` as VS Code names extension
targets, gives the clangd entry the payload's entry of that platform carries, the target kit (recipe,
source, target triple, and `name`, the published kit `mcppls-kit-<name>`), and `server-target`, the
mcpp `--target` the platform's programs and tests are built for (CI's `build` job). The VS Code
extension, the CLion build, `release.manifest.json` and CI's per-platform jobs list the same
platforms, and `mcppls-devtools check platforms` fails when one of them does not. Versions change here
and nowhere else. Each sha256 was checked against two independent downloads and against the digest
GitHub records for the release asset. The server's version is read from `mcpp.toml`; the openkal kit's
is the platform's, which mcpp reports (`mcpp self env --target x86_64-openkal`).

## Building a payload

One command packs the server, fetches the engines, makes the openkal kit, assembles and verifies; the
downloads are cached in `.payload-cache`. CI runs the same command (`.github/workflows/ci.yml`, job
`payload`), naming the okb and the tree its `server` job packed once:

```bash
mcpp run -p devtools -- payload                                   # everything packed and made here
mcpp run -p devtools -- payload --server-image mcppls-<version>.okb \
    --server-tree mcppls-<version>-openkal.zip --kit kit-openkal
mcpp run -p devtools -- payload --verify target/pack/payload
```

With nothing named, the tool packs both with one build: `mcpp pack --target openkal --mode
self-contained --systems all --pkg-format zip,okb`, the tree's `libexec/okrun/<system>-<arch>/`
holding the runners. A platform whose system the tree does not run on by itself (its
`mcpp-package.json` `runs_on.self`) is refused. Any host that builds mcppls assembles the payload:
the engines are taken byte for byte and the openkal kit is copied, so nothing is configured.

The target kits need their hosts:

| Kit | Host | Tools |
|---|---|---|
| `linux-x64` | Linux with `dpkg` and the `libc6-dev` and `linux-libc-dev` packages installed | cmake, ninja, a C and C++ compiler for libc++'s configure checks |
| `linux-arm64` | Linux arm64 with the same packages; CI uses `ubuntu-24.04-arm` and the devtools built for `aarch64-linux-musl`. Another host passes `--sysroot-include` with aarch64 C headers, because `dpkg` lists only the host's own | the same as `linux-x64` |
| `win32-x64` | any; CI uses Linux | nothing beyond mcpp |
| `darwin-arm64` | macOS | cmake, ninja, and the Xcode command line tools |

The server is built with the **release** profile, and CI runs the unit tests in both profiles on
every system: release-only defects in the runtime have happened.

To run the VS Code extension against a local payload, put it at `editors/vscode/payload` or point
`MCPPLS_PAYLOAD` at it.

## What the verification checks

`mcppls-devtools payload --verify` fails on the first build that would not work on a user's machine:

- `payload.json` is version 5, the server is `bin/mcppls.okb` with a version, and it serves at least
  one platform.
- For every platform: its runner is `libexec/okrun/<system>-<arch>/okrun[.exe]` and its clangd
  `clangd/<platform>/bin/clangd[.exe]`, both present and executable; clang's builtin headers are there
  for the clangd major version; the engine's identity names that platform and the binary's bytes;
  its integrity files match.
- The kit (`kit --verify` checks any kit by the same rules): kit-version 1 or 2, every include
  directory of the top level and of each `targets` entry, the sysroot and every license exist; the
  module manifest provides `std` and every module source it names exists; nothing in it is a
  program, a library or a script (S4-4-1); a libc++ kit's version is the `_LIBCPP_VERSION` of its
  headers (S4-4-5); a macOS kit requires the SDK and carries none of it.
- **No two paths differ only in case.** A VSIX refuses such pairs and the file systems of Windows
  and macOS fold them into one. The Linux kernel headers carry several (`xt_mark.h` and `xt_MARK.h`);
  the target kit's build keeps the all-lowercase name and lists what it dropped.

## Sizes

In MiB, measured on the 0.0.20 payload built from this tree (`mcppls-devtools payload`); `gz` is the
directory compressed with gzip.

| Part | Size | Files |
|---|---|---|
| Server okb (`bin/mcppls.okb`, both images) | 14.8 | 1 |
| Runners (`libexec/okrun/`, four systems) | 3.3 | 4 |
| clangd `linux-x64` / `linux-arm64` / `darwin-arm64` / `win32-x64` | 69 / 64 / 69 / 61 | about 310 each |
| openkal kit | 19 (1.9 gz) | 2 127 |
| **Payload** | **298 (86 gz)** | **3 365** |

Up to 0.0.19 each platform had a payload of its own, measured at 96 to 217 MiB (25.6 to 41.9 MiB
gz), each with a copy of the server and its platform's kit of 12 to 93 MiB; one payload now replaces
them, so a VSIX or CLion package is about the four of 0.0.19 combined less three servers and four
target kits. The target kits are
published beside it (0.0.19's measures, kits/README.md: 12.4 to 93.3 MiB, 1.7 to 10.4 MiB gz); the
sizes of 0.0.20's are those of its release page.

## Releases

`.github/workflows/release.yml` runs the pre-release test (all of CI, the editor plugins installed,
performance and stability) and publishes the candidate it checked, with `MANIFEST.md` and
`SHA256SUMS`; [docs/92-release.md](../docs/92-release.md) has the process.

`mcppls-devtools release xlings --release DIR --out DIR` renders `xlings/*.lua.in` from a release's
directory:

```
mcpp-language-server.lua        one asset, mcppls-<version>-openkal.zip, for Linux, macOS and Windows; each system
                                names the architectures the tree's runs_on.self covers; xvm registers bin/mcppls
                                (bin/mcppls.exe on Windows); depends on llvm-tools (clangd on PATH) and mcppls-kit-openkal
mcppls-kit-<name>.lua           one per kit, mcppls-kit-<name>-<version>.tar.gz, one asset for every system
```

Open VSX and the xlings index are published to by hand after the release (docs/92-release.md).
