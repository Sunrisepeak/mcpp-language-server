# Packaging

Everything mcppls ships besides its own source: a **payload** per platform,
holding the server, a trimmed clangd 23.1 and the `mcppls-kit` semantic kit.
The VS Code extension carries a payload inside its platform VSIX; the xlings
packages carry the same parts as two archives.

```
packaging/
  payload.lock.json        every upstream input, with its sha256
  release.manifest.json    what a release carries
  xlings/                  xpkg descriptor templates for mcpp-language-server and mcppls-kit
  kits/README.md           what each platform's kit contains and why
```

This directory holds data only. The code that reads it is `modules/pack`
(`mcppls.pack.*`), run through `mcppls-devtools` (docs/93-devtools.md):

| Module | Command | Does |
|---|---|---|
| `mcppls.pack.fetch`, `.lock` | (inside the others) | download an input named by the lock and prove it by sha256 |
| `mcppls.pack.clangd` | `payload --only clangd` | reduce an official clangd release to what is shipped |
| `mcppls.pack.kit` | `kit` | assemble the semantic kit for one platform (kits/README.md) |
| `mcppls.pack.payload` | `payload`, `payload --verify`, `payload --from` | put the three parts into the payload layout, and verify one |
| `mcppls.pack.release` | `release check`, `release xlings` | check a staged release; split payloads into xlings-res archives |

It is C++ on openkal like the server, so a machine that can build mcppls can
package it: no interpreter.

## The payload layout

The server and the VS Code extension both rely on this layout; `payload.json` is
its manifest.

```
<payload>/
  payload.json                      payload-version 3, platform, the version and
                                    relative path of each part, and `engines`: per
                                    engine its executable, version and matching kit
  bin/mcppls[.exe]
  clangd/bin/clangd[.exe]
  clangd/lib/clang/<major>/include/  clang's builtin headers, found beside clangd
  kit/kit.json + kit data            spec S4
  licenses/                          mcppls and LLVM license texts
```

The server takes the payload named by `--payload`, or else the payload that
contains its own executable. `--clangd` and `--kit` replace one part of it. A
part that is still missing is looked for outside a payload: clangd on `PATH`,
and the kit installed by xlings.

## Inputs: `payload.lock.json`

| Entry | What | Used by |
|---|---|---|
| `clangd-linux`, `clangd-mac`, `clangd-windows` | clangd 23.1.0 release archives | `mcppls.pack.clangd` |
| `llvm-project-src` | llvm-project 23.1.0 source archive | `mcppls.pack.kit`, recipe `libcxx-source` |
| `llvm-mingw` | llvm-mingw 20260826 (LLVM 23.1.0), UCRT, Linux x86_64 host | `mcppls.pack.kit`, recipe `llvm-mingw` |

`platforms` maps each payload platform to its clangd entry and its kit recipe,
source and target triple. Versions change here and nowhere else. Each sha256 was
checked against two independent downloads and against the digest GitHub records
for the release asset. The server's version is read from `mcpp.toml`.

## Building a payload

One command fetches, trims clangd, builds the kit, assembles and verifies; the
downloads are cached in `.payload-cache`. CI runs the same command
(`.github/workflows/ci.yml`, job `payload`), naming the server it cross-built:

```bash
mcpp run -p devtools -- payload                                   # this host, release server built for you
mcpp run -p devtools -- payload --platform win32-x64 --server target/x86_64-windows-gnu/<fingerprint>/bin/mcppls.exe
mcpp run -p devtools -- payload --verify target/pack/payload
```

| Platform | Host | Tools |
|---|---|---|
| `linux-x64` | Linux with `dpkg` and the `libc6-dev` and `linux-libc-dev` packages installed | cmake, ninja, a C and C++ compiler for libc++'s configure checks; `strip` if available |
| `win32-x64` | any; CI uses Linux and cross-builds the server | nothing beyond mcpp |
| `darwin-arm64` | macOS | cmake, ninja, and the Xcode command line tools (`lipo`, `strip`, `codesign`) |

The server is built with the **release** profile, and CI runs the unit tests in
both profiles on every host: release-only defects in the runtime have happened.

To run the VS Code extension against a local payload, put it at
`editors/vscode/payload` or point `MCPPLS_PAYLOAD` at it.

## What the verification checks

`mcppls-devtools payload --verify` fails on the first build that would not work on a
user's machine:

- `payload.json` has the expected version, platform and part paths, and every
  part has a version.
- The server and clangd exist and are executable, and clang's builtin headers
  are present for the clangd major version.
- `kit.json` names the same kit as the manifest, uses kit-version 1, and every
  include directory, sysroot and license it lists exists.
- The kit's module manifest provides `std`, and every module source and module
  include directory it names exists.
- **No two paths differ only in case.** A VSIX refuses such pairs and the file
  systems of Windows and macOS fold them into one. The Linux kernel headers
  carry several (`xt_mark.h` and `xt_MARK.h`); the kit build keeps the
  all-lowercase name and lists what it dropped.

## Sizes

In MiB, with a dev-build server carrying debug information; a release server is
a few MB. `gz` is the payload directory compressed.

| Platform | Server | clangd | Kit | Total | gz | Kit gz | Files |
|---|---|---|---|---|---|---|---|
| linux-x64 | 30.6 | 74.5 | 20.8 | 125.9 | 35.6 | 3.9 | 3630 |
| win32-x64 | 58.8 | 64.9 | 93.3 | 217.1 | 41.9 | 10.4 | 3904 |
| darwin-arm64 | 7.2 | 76.7 | 12.4 | 96.3 | 25.6 | 1.7 | 2163 |

## Releases

`.github/workflows/release.yml` runs the pre-release test (all of CI, the editor
plugins installed, performance and stability) and publishes the candidate it
checked, with `MANIFEST.md` and `SHA256SUMS`; [docs/92-release.md](../docs/92-release.md)
has the process.

`mcppls-devtools release xlings` splits each payload into the two archives the xlings
ecosystem installs, named by the xlings-res convention, and renders
`xlings/*.lua.in` with their hashes:

```
mcpp-language-server-<version>-<os>-<arch>.tar.gz   the server (mcppls) and its license
mcppls-kit-<kit version>-<os>-<arch>.tar.gz          the semantic kit
```

clangd is not part of either archive: the xlings package depends on
`llvm-tools`, which puts clangd on `PATH`. The VS Code Marketplace, Open VSX and
the xlings index are not published to yet.
