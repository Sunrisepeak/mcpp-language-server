# The semantic kits

A semantic kit (spec [S4](../../docs/specs/s4-semantic-kit.md)) gives clangd a
standard library with module semantics when the project's own compiler cannot
be used: there is none installed, discovery is turned off, or the workspace is
not trusted. It is data only — standard library headers, the `std` and
`std.compat` module sources, their module manifest, C library headers where they
may be redistributed, license texts and `kit.json`. Nothing in it is compiled.
The server chooses the kit of each project's target (S4 5.1).

| Kit | Made by | Shipped as |
|---|---|---|
| the openkal kit | `mcppls-devtools kit --openkal` (`modules/pack/src/openkal.cpp`), from mcpp's build database of `x86_64-openkal` and `aarch64-openkal` | in every payload (`kit/`); `mcppls-kit-openkal-<platform version>.tar.gz` and `mcppls-kit-openkal` |
| a target kit | `mcppls-devtools kit --platform P` (`modules/pack/src/kit.cpp`), from the inputs in `packaging/payload.lock.json`; the recipe of each platform is named there | `mcppls-kit-<target>-<version>.tar.gz` and `mcppls-kit-<target>` (`--archive DIR` writes the archive) |

## The openkal kit

Kit version 2, one kit for both openkal targets: libc++ 22.1.8's headers and
module sources as openkal-platform 0.19.1's C++ run-time (openkal-llvm-runtime
0.16.11) configures them, openkal-musl 0.23.1's headers, openkal's and the image
loader's, and under `include/<target>/` what differs between the architectures
(musl's generated and per-architecture headers). Each entry of `targets` lists
its include directories in the order the build reads them, with the defines a
unit of an ok image reads with (`__OPENKAL__`, `__unix__`, `_XOPEN_SOURCE=700`)
and `--target=<arch>-unknown-none-elf`. The module manifest
`lib/libc++.modules.json` names `std` and `std.compat`, whose sources sit beside
the `std/*.inc` they include. Its `stdlib.version` is read from the headers'
`__config`, not from clangd: 19 MiB, 2 127 files, 1.9 MiB compressed.

## The target kits

Each is published as `mcppls-kit-<name>`, the `kit.name` of its row in
`packaging/payload.lock.json`.

| Platform | Package | Recipe | Target | Standard library | C library headers |
|---|---|---|---|---|---|
| `linux-x64` | `mcppls-kit-x86_64-linux-gnu` | `libcxx-source` | `x86_64-unknown-linux-gnu` | libc++ 23.1.0, configured from the llvm-project source and installed as headers and module sources | glibc and Linux kernel headers from the build host's `libc6-dev` and `linux-libc-dev` packages, in `sysroot/usr/include`, with their copyright files |
| `linux-arm64` | `mcppls-kit-aarch64-linux-gnu` | `libcxx-source` | `aarch64-unknown-linux-gnu` | the same, for aarch64 | the same, from an arm64 host's packages |
| `win32-x64` | `mcppls-kit-x86_64-w64-mingw32` | `llvm-mingw` | `x86_64-w64-mingw32` | libc++ 23.1.0 headers and module sources from llvm-mingw 20260826, built on the same LLVM | mingw-w64 headers from `generic-w64-mingw32/include`, without `*.idl`, `*.tlb` and `*.def`, with the mingw-w64 `COPYING` files |
| `darwin-arm64` | `mcppls-kit-arm64-apple-darwin` | `libcxx-source` | `arm64-apple-darwin` | libc++ 23.1.0, configured for arm64 macOS | none: Apple's SDK license does not allow a kit to carry them, so the kit requires the SDK installed on the user's machine |

Measured for 0.0.19, when each payload carried its platform's kit:

| Platform | Size, MiB | Compressed, MiB |
|---|---|---|
| linux-x64 | 20.8 | 3.9 |
| win32-x64 | 93.3 | 10.4 |
| darwin-arm64 | 12.4 | 1.7 |

### `libcxx-source`

LLVM's `runtimes` build is configured for libc++ and libc++abi with
`LIBCXX_INSTALL_MODULES=ON`, and only the `install-cxx-headers`,
`install-cxxabi-headers` and `install-cxx-modules` targets run, which copy files
and compile nothing. The configure checks still need cmake, ninja and a working
C and C++ compiler on the build host. Linux configures with a per-target runtime directory,
so its `__config_site` sits in `include/x86_64-unknown-linux-gnu/c++/v1`. The
darwin-arm64 kit must be built on macOS.

The Linux kernel headers include pairs whose names differ only in case, such as
`linux/netfilter/xt_mark.h` and `xt_MARK.h`. A VSIX cannot hold both and the
file systems of Windows and macOS fold them into one, so the builder keeps the
all-lowercase name of each pair and lists the files it dropped.

### `llvm-mingw`

The headers, `share/libc++/v1` and the `x86_64-w64-mingw32` module manifest are
extracted from the llvm-mingw release archive, on any host. The same
case-insensitive rule applies to the extracted headers.

## `kit.json` as built

linux-x64:

```json
{
  "kit-version": 2,
  "name": "mcppls-kit-libcxx-23.1.0-x86_64-unknown-linux-gnu",
  "target": "x86_64-unknown-linux-gnu",
  "stdlib": {
    "name": "libc++",
    "version": "23.1.0",
    "module-metadata": "lib/x86_64-unknown-linux-gnu/libc++.modules.json"
  },
  "system-include-directories": ["include/c++/v1", "include/x86_64-unknown-linux-gnu/c++/v1"],
  "sysroot": "sysroot",
  "arguments": ["-nostdinc++"],
  "licenses": ["licenses/LLVM-LICENSE.TXT", "licenses/libc6-dev-copyright.txt", "licenses/linux-libc-dev-copyright.txt"]
}
```

The other two differ in these fields:

| Field | win32-x64 | darwin-arm64 |
|---|---|---|
| `target` | `x86_64-w64-mingw32` | `arm64-apple-darwin` |
| `module-metadata` | `x86_64-w64-mingw32/lib/libc++.modules.json` | `lib/libc++.modules.json` |
| `system-include-directories` | `generic-w64-mingw32/include/c++/v1`, `generic-w64-mingw32/include` | `include/c++/v1` |
| `sysroot` | `null` | `null` |
| `arguments` | `-nostdinc++`, `-nostdlibinc` | `-nostdinc++` |
| `licenses` | LLVM, and the mingw-w64 `COPYING` files | LLVM |
| `requires` | absent | `[{ "kind": "macos-sdk" }]` |

## How the server uses a kit

The kit is chosen when no translation unit set has a usable, probed toolchain.
The status then reports the profile `semantic-kit` with the kit's standard
library and target. For each translation unit the engine database carries:

```
--no-default-config --target=<target> -std=<the unit's standard, else c++23>
<kit arguments>
-isystem <kit>/<each system include directory>
--sysroot=<kit>/<sysroot>                  when the kit has one
-isysroot <macOS SDK>                      when the kit requires macos-sdk
```

The kit's module manifest makes `std` and `std.compat` visible to every unit,
and their sources are added to the engine database once. For `macos-sdk` the
server looks at `SDKROOT`, then `xcrun --show-sdk-path`, then the Command Line
Tools and Xcode SDK locations.

With xlings, the kit is the `mcppls-kit` package, installed under
`<xlings data>/xpkgs/xim-x-mcppls-kit/<version>`, where the server looks for
it when no payload provides one.
