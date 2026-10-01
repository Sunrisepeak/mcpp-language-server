# Project kinds

mcppls does not ask you to describe your build. It finds it, and says what it found in the status
bar. This is what "finding it" means for each kind of project, and what you get when it cannot.

The status bar's `L1`..`L4` is the *tier*: how the project was described — L1 a build database (mcpp's
`emit build-database`, or one of your own), L2 CMake's own database, L3 a bare `compile_commands.json`
(including the one an mcpp too old to emit a build database leaves, and the ones xmake and meson
write), L4 sources only (an untrusted
workspace is always L4, whatever else is on disk). It is not the same number as the `level`
`mcppls check` and `cxxModules/status` also carry, which is [S1](specs/s1-build-database.md)'s own
1..4 for how completely a database's *document* is structured; a hand-written level-3 database and an
mcpp build database are both L1, and a CMake project without `FILE_SET CXX_MODULES` is L2 even at
level 1. The status bar and the editor plugins show `L<tier>` only, to keep the two apart. A model
is never replaced by one of a worse tier: if the build tool later answers less completely than the
model in hand, that model is kept and the status says it may be stale.

## mcpp

mcppls asks mcpp for the build description directly:

```
mcpp emit build-database --format json
```

mcpp answers with a document that names every translation unit, its module role, its arguments and
the standard library module it uses — without building anything and without writing into your
project. That is the best case: the description comes from the tool that owns the build.

A few things are worth knowing:

- **How long it may take, and when it runs again.** The build tool's hard deadline is 5 minutes the
  first time, then three times what its last run took, between 1 and 10 minutes; the project's cached
  model serves meanwhile, or its sources when there is none (`mcppls.producerTimeout` sets a fixed
  limit instead). Saving a source does not run it: a saved source whose module declaration and
  imports are unchanged only updates the index. The build tool is asked again when a build file, a
  non-source input or a source's module structure changes, and edits that keep coming are one run —
  the wait before it grows with how long the build tool takes, up to a minute.
- **The run is offline.** A build description is a question about the project, not an errand, so
  the server asks it with `MCPP_OFFLINE` set. If the project's dependencies are not on the machine
  yet, mcpp says so, and nothing waits for you to decide anything:
  - the project is served from its sources at once (L4), and whatever mcpp could describe is used;
  - a notification in the corner offers **Download and Continue** (the build tool may reach the
    network, this once), **Run in Terminal** (where your proxy and credentials are), or **Don't Ask
    Again**. You can leave it unanswered forever; it is asked once per workspace and set of missing
    things;
  - the description is asked again, offline, after 30 s, 1 and 2 minutes and then every 5, and at once
    when `mcpp.toml` or `mcpp.lock` changes — so if you build in your own terminal instead, the
    project upgrades by itself and the question no longer applies.

  See [30-settings.md](30-settings.md) for `mcppls.buildTool` and `mcppls.buildDiscovery.askBeforeDownload`.
- **What a build rule generates.** A rule package (`mcpp:plugins`' `rules-qt`, say) turns `.ui`,
  `.qrc` and `.ts` files into headers and sources when the project builds. mcpp describes the build
  without running those steps, so a form's `ui_*.h` does not exist yet in what it describes. mcppls
  leaves the rule's inputs out (they are not C++), reads the generated files from your project's own
  `target/` when a build has written them, and otherwise says which are missing, with **Build in
  Terminal**; once a build writes them, the files that include them get their semantics without a
  restart.
- **An older mcpp** without `emit build-database` is not the end of it: if a newer mcpp is installed
  elsewhere on the machine (the xlings package store, mcpp's own registry store), mcppls asks *that*
  one instead, read-only and offline the same way, only to describe the project — the project still
  builds with the mcpp it pins. The status says "described by mcpp X (the project pins Y)" when this
  happens. Only when no installed mcpp can answer is the pinned one asked to configure instead
  (`mcpp build --configure-only`), which does write into the project; the status says so then, and
  updating mcpp is the fix.
- **A stale database** — one whose entries name files a `target/` a build once wrote and the project
  later deleted, or a `compile_commands.json` checked in from another machine — is used for what
  still exists; the status notes it is stale rather than failing outright. A module that build writes
  only when it runs (a dependency's own `std`, a code generator's output) is looked for where builds
  leave it — the project's own build directory, and mcpp's build-database cache, which survives a
  deleted `target/` — before mcppls falls back to an empty stand-in for it.

## CMake

If the build directory has a `build_database.json` (CMake 4.4+ with Ninja, `FILE_SET CXX_MODULES`),
mcppls reads it. Otherwise it reads `compile_commands.json` and expands the `@modmap` files the
generator wrote.

A build directory is looked for in `build*/`, `out/build/*`, `cmake-build-*`, and where the first
configure preset of `CMakePresets.json` (or `CMakeUserPresets.json`) puts its `binaryDir`.

With no build directory at all and a trusted workspace, mcppls configures one **of its own**, under
its cache directory — never in your project — following that preset's generator, toolchain file and
cache variables, so it describes the build you would get. That configure is **disconnected**
(`-DFETCHCONTENT_FULLY_DISCONNECTED=ON`), the first time too: a `FetchContent` dependency that is not
on the machine stops it, and you get the same non-blocking offer as for mcpp above (**Download and
Continue** configures once with the network, in that private directory).

## xmake

`xmake.lua` makes an xmake project. mcppls asks xmake itself, with its own command,
`xmake project -k compile_commands`, which compiles nothing — but it configures and scans modules, so
mcppls points xmake's configuration and build directories at its cache (`XMAKE_CONFIGDIR`,
`--builddir`) and your project stays untouched: nothing is generated, changed or deleted in it, your
own `compile_commands.json` included. It runs offline (`--policies=package.fetch_only,network.mode:private`):
a package that is not installed stops it with the same offer as above. The first description takes a few
seconds (about 6–8 s measured, most of it xmake detecting the toolchain); the project is served from its
sources meanwhile. Module roles come from scanning, so an xmake project is L3.

The model follows what you do, with nothing to run by hand: a change to any `xmake.lua` describes the
project again, and so does your own `xmake f` — mcppls reads what it left in
`.xmake/<plat>/<arch>/xmake.conf` (read only, the newest one when there are several) and configures its
private run the same way: platform, architecture, mode (`-m debug` gives `-O0 -g`, not the release
flags), toolchain, SDK, runtimes, kind and the options your own `xmake.lua` declares. If xmake refuses
one of those options (one that `xmake.lua` no longer declares), mcppls configures again with the
standard ones only and the status says which were left out.

A `compile_commands.json` of yours, at the root or in `.vscode/` (where xmake's VS Code plugin writes
one), is **not** read while mcppls can run xmake: it only follows your last `xmake project`, not your
`xmake.lua`, so the two would take turns. It is read as it is when mcppls cannot run xmake — the
workspace is not trusted, xmake is not on `PATH`, or `mcppls.buildTool` is `off` — and then it is
watched, and a notice says so once when it is older than an `xmake.lua` (or your `xmake.conf`): run
`xmake project -k compile_commands` to update it. To have mcppls read your own file on purpose, set
`mcppls.buildTool` to `off`.

## meson

`meson.build` makes a meson project. An existing build directory (`builddir/`, `build/`, or any
directory with `meson-private/`) is read for its `compile_commands.json`. Otherwise mcppls runs
`meson setup` into its cache with `--wrap-mode=nodownload`; a subproject that would have to be
downloaded stops it with the same offer. L3, like xmake.

## Turning discovery off

`mcppls.buildDiscovery.mode = off` makes mcppls detect no build system at all: nothing is read or run
implicitly, and only a database you name with `mcppls.database` is used, else the sources are scanned
(L4). `mcppls.buildDiscovery.providers` leaves out single build systems instead — for example, only
ever read an existing CMake build directory and never run xmake. `mcppls.buildTool = off` is the
narrower switch: build systems are still detected and their existing output read, only never run. See
[30-settings.md](30-settings.md).

## compile_commands.json

Any `compile_commands.json` works, whoever wrote it. mcppls scans the sources named in it to
recover module roles the database does not carry, and probes the compilers it names for their
module facts.

## No build system

Sources are scanned, module roles inferred from what they declare, and a compiler is looked for on
the machine. This is the fallback, and it is the reason opening a directory of `.cppm` files gives
something useful rather than nothing.

## No compiler either

The bundled **semantic kit** takes over: libc++ built as modules, with a manifest that says where
each module is. `import std` resolves, and so do the modules of your own code. Diagnostics come from
a standard library that is not the one you build with, so they can differ — the status bar says the
profile is a semantic kit rather than a build toolchain.

## Which C++ standard, and C++26

The standard is the build's: whatever `-std=` (or `/std:`) a unit's command says, mcppls gives
clangd; `/std:c++latest` is C++26. Three rules go further.

- **One standard per context for module units.** A module's BMI can only be imported under the
  standard it was built with — `import std` in a C++26 file fails outright when `std` was built as
  C++23 ("C++26 was disabled in precompiled file"). So the units of one context that import, provide
  or belong to a module are read with the newest standard among them, and a plain unit keeps its own;
  the log says when some were raised, and the report's `plan.languageStandard`,
  `plan.standardsSeen` and `plan.standardsRaised` say which. The status profile names the standard.
- **A command that names no standard** (xmake without `set_languages`, for one). Its module units --
  anything that imports, provides or belongs to a module, `std`'s own unit included -- are read as
  C++23 (`gnu++23`; `c++23` for an MSVC target), the standard `import std` is made for, whatever the
  compiler's own default: Clang's gnu++17 has no modules at all. The log says it once and the report's
  `plan.standardAssumed` is `true`; a standard the build does name is followed instead. A plain unit
  is read with its build compiler's own default where that differs from Clang's (GCC 16: gnu++20;
  C from GCC 15: gnu23), as the build compiles it. Module units a build names a standard older than
  C++20 for are said once (`module-standard-too-old`): modules need C++20.
- **Sources nothing describes are read with the newest standard their compiler takes**: C++26 with
  the semantic kit (clang 23, libc++ 23), GCC 14 and later, and Clang 17 and later (spelled `c++2c`
  before Clang 20); C++23, the oldest standard with `import std`, for older compilers.

What C++26 gives you is clangd 23.1's: pack indexing, `= delete("reason")`, placeholder `_`,
`static_assert` messages, `#embed`, variadic friends and the rest of what clang 23 implements, and
the C++26 library of the standard library you build with (the kit's is libc++ 23). **Contracts
(P2900) and reflection (P2996) are not in clang 23**: code using them shows clang's errors even
where GCC compiles it; VS Code still colors `contract_assert`, `pre` and `post`.

## Standard library macros

`std` is built with only the standard library's own configuration macros — `_ITERATOR_DEBUG_LEVEL`,
`_DEBUG`, `_GLIBCXX_ASSERTIONS`, `_LIBCPP_HARDENING_MODE`, `_HAS_*` and the like — not with every `-D`
of your project. A project macro that happens to be a header guard of the standard library would
otherwise change what `std` contains: GalTranslPP defines `_RANGES_` for its units, which is MSVC
STL's guard for `<ranges>`, and `std` built with it had no `std::views` ("no member named 'views' in
namespace 'std'"). The macros left out are named in the log. Your own units keep all their macros.

## An untrusted workspace

No build tool and no compiler is run at all — VS Code's workspace trust is respected before anything
else. The semantic kit provides the semantics and the status says why.

## What the status bar tells you

| It says | It means |
|---|---|
| A compiler and standard library | The model came from your build; that toolchain's `std` is in use |
| A semantic kit | No usable compiler was found, or the workspace is untrusted |
| "may be stale" | The build tool could not answer this time; the last model that loaded is still in use |
| "needs a download" | Planning offline stopped at something not on the machine; the project is served from its sources meanwhile, and there are actions to fix it (or build in your terminal: it upgrades by itself) |
| "files the build generates … do not exist yet" | A rule's output (a Qt form's header, say) is not built yet; build once and the files that include it get their semantics |
| "implementation unit(s) cannot be read" | An implementation unit does not build (a missing header, usually), so go-to-definition cannot reach the definitions in it |
