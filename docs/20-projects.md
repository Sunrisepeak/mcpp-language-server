# Project kinds

mcppls does not ask you to describe your build. It finds it, and says what it found in the status
bar. This is what "finding it" means for each kind of project, and what you get when it cannot.

The status bar's `L1`..`L4` is the *tier*: which kind of source described the project — L1 an mcpp
build database, L2 CMake's own database, L3 a bare `compile_commands.json`, L4 sources only (an
untrusted workspace is always L4, whatever else is on disk). It is not the same number as the `level`
`mcppls check` and `cxxModules/status` also carry, which is [S1](specs/s1-build-database.md)'s own
1..4 for how completely a database's *document* is structured; a hand-written level-3 database and an
mcpp project are both L1, and a CMake project without `FILE_SET CXX_MODULES` is L2 even at level 1.
The status bar and the editor plugins show `L<tier>` only, to keep the two apart.

## mcpp

mcppls asks mcpp for the build description directly:

```
mcpp emit build-database --format json
```

mcpp answers with a document that names every translation unit, its module role, its arguments and
the standard library module it uses — without building anything and without writing into your
project. That is the best case: the description comes from the tool that owns the build.

Two things are worth knowing:

- **The run is offline.** A build description is a question about the project, not an errand, so
  the server asks it with `MCPP_OFFLINE` set. If the project's dependencies are not on the machine
  yet, mcpp says so and the status bar offers to run the build tool in your terminal — where your
  proxy and credentials are. See [30-settings.md](30-settings.md) for `mcppls.buildTool`.
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

With no build directory at all and a trusted workspace, mcppls configures one **of its own**, under
its cache directory — never in your project. The first such configure may download what the project
declares (`FetchContent`, `ExternalProject`); every later one adds
`-DFETCHCONTENT_UPDATES_DISCONNECTED=ON`.

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

## An untrusted workspace

No build tool and no compiler is run at all — VS Code's workspace trust is respected before anything
else. The semantic kit provides the semantics and the status says why.

## What the status bar tells you

| It says | It means |
|---|---|
| A compiler and standard library | The model came from your build; that toolchain's `std` is in use |
| A semantic kit | No usable compiler was found, or the workspace is untrusted |
| "may be stale" | The build tool could not answer this time; the last model that loaded is still in use |
| "needs a download" | Planning offline stopped at something not on the machine; there is an action to fix it |
