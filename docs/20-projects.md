# Project kinds

mcppls does not ask you to describe your build. It finds it, and says what it found in the status
bar. This is what "finding it" means for each kind of project, and what you get when it cannot.

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
- **An older mcpp** without `emit build-database` is asked to configure instead
  (`mcpp build --configure-only`), which does write into the project. The status says so when it
  happens. Updating mcpp is the fix.

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
