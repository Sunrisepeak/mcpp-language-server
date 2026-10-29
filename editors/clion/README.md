# CLion plugin

C++20 named modules in CLion through mcpp-language-server: the plugin registers `mcppls` as a
language server for C and C++ using the IntelliJ platform's LSP API, which the paid IDEs have.

It is deliberately small: its job is to start the server it carries, to stay out of the way of
CLion's own engine on the projects CLion models, and, when the user says so, to keep CLion's engine
out of the files mcppls answers. Everything else happens in the server.

## Build

```bash
mcpp run --features clion -p devtools -- extension --editor clion
```

That puts this machine's payload in place (assembled, or the one `--payload` names) and runs
`gradle buildPluginVariants_<os>_<arch>` here with `-Pmcppls.payload.<platform>=<payload>`: the
package of this system, carrying the payload at `mcppls-clion/payload/`, written to
`target/pack/mcppls-clion-<version>-<platform>.zip`. `--payloads DIR` (a directory of
`<platform>/payload`) builds the package of every platform it holds, which is what a release does.
Each package's `plugin.xml` depends on its system's `com.intellij.modules.os.*` and
`com.intellij.modules.arch.*`, by which the JetBrains Marketplace gives CLion 2026.1 and later the
package of its own system. Without a payload, `gradle buildPlugin` makes a plugin that carries no
server and starts `mcppls` from PATH (a build for development).
Gradle is installed for you if it is missing — `xim:gradle` declares the JDK it runs on, so the one
package brings both.

`--features clion` is what asks for it. `[xlings.workspace]` is provisioned as a set, so an
unconditional Gradle entry would make every other `mcpp run` in this project wait for it — or fail,
on a machine whose index does not carry it. Behind the feature, it costs only the person building
this plugin.

The first build downloads the CLion SDK it compiles against, so it is long and needs the network.

## Install

```bash
mcpp run --features clion -p devtools -- extension --editor clion --install
```

That builds this system's package and unpacks it into the plugins directory of every CLion that has
run on this machine — what **Install Plugin from Disk** does — so restart CLion afterwards.
`--clion-dir DIR` names one plugins directory instead; `--plugin FILE` installs a package you
already have, such as a release's `mcppls-clion-<version>-<platform>.zip`.

```bash
mcpp run -p devtools -- uninstall --editor clion
```

## Use

The plugin starts, in this order (`ServerLaunch.kt`):

1. `MCPPLS_SERVER`, a program's absolute path (development and tests);
2. the payload it carries, `<plugin>/payload`, as its `payload.json` says: the platform's runner
   `bin/okrun` starting the server's image `bin/mcppls.okb`. A payload built for another system is
   refused, with what to install instead; a runner or clangd that lost its executable bit in the
   installation gets it back;
3. `mcppls` on the PATH the IDE sees (`xlings install mcppls`, the universal package or a single
   file), for a build without a payload.

With none of them, the plugin says so once per project and starts nothing.

CLion 2026.1 and later are supported (`sinceBuild` 261). The plugin is built against CLion 2026.2.3
and tested inside it; the Plugin Verifier checks it against 2026.1 as well.

## One engine per file

CLion has its own C/C++ engine, and two engines answering the same file means two lists of
completions and two sets of diagnostics. So a file is answered by one of them:

- a project CLion models itself -- a loaded CMake, compilation database or Makefile workspace, or a
  `CMakeLists.txt` at the project root -- is CLion's, and mcppls does not start for its files;
- every other project (mcpp, xmake, a plain folder of sources) is mcppls's. CLion's engine has
  little to say about those anyway: it does not know how they are built.

Settings | Tools | mcppls has a checkbox, **Also for projects CLion models**. With it on, mcppls
answers the first kind too, which is useful when the project's modules are what CLion cannot follow.
The setting applies to files opened afterwards.

### CLion's engine in the files mcppls answers

CLion's engine also analyses a folder no build system configures, with the default compiler, and it
answers a modelled project's files too when the checkbox above is on. So in the files mcppls answers,
problems and completions would come twice. The first time a project has such a file the plugin asks,
once: **Use mcppls only in this project**, **Always use mcppls only** (every project without an
answer of its own), or **Keep both**. Nothing is changed until it is answered (`OneEngine.kt`).

"mcppls only" does two things in those files:

- their highlighting level is **Syntax** (`FileHighlightingSetting.SKIP_INSPECTION`, the level of the
  inspection widget in the editor's corner). CLion Nova's frontend passes a file's level to its
  backend, which then reports syntax errors only; the platform's inspections and external tools stop.
  Syntax colouring, gutter icons and mcppls's diagnostics stay: the LSP highlighting pass does not
  depend on the level;
- completion keeps mcppls's items and live templates (`McpplsOnlyCompletion.kt`), so CLion's engine's
  items, and another plugin's, are left out.

Settings | Tools | mcppls changes the answer for every project and for this one; **Tools | mcppls**
has *Use mcppls Only in This Project* and *Restore CLion's Engine in This Project*. Every level the
plugin lowered is remembered with what it was before, and restoring puts that back, except in a file
whose level was changed since. Another plugin that may run a language server for C and C++ — LSP4IJ,
the list is in `OneEngine.kt` — is named once per project with the way to turn its server off: a
plugin cannot stop another plugin's server.

"CLion models the project" is asked of CLion's workspace API (`CidrWorkspaceManager`), which lives in
its C/C++ plugin and is reached through an optional dependency (`mcppls-cidr.xml`): where that API is
missing or has changed, the plugin still loads and the `CMakeLists.txt` check alone decides.

## Keeping mcppls off for one project

To keep it off for one project whatever it is, open Settings | Plugins, find this plugin, and use
the arrow beside its checkbox to disable it for the current project only (the wording depends on the
IDE version; an IDE without that choice can only disable the plugin for every project). Reopen the
project for it to take effect.

## Resetting a workspace's cache

If preparation never finishes or clangd keeps crashing, a cache left by an earlier session may be the
cause. VS Code and Neovim have a command for it (the server does the deleting). This plugin has no
action for it yet — the IntelliJ platform's LSP API does not yet offer a stable way to send a server
its own command — so from a terminal, after closing CLion:

```bash
mcppls cache                      # the workspaces that have a cache, with their names and sizes
mcppls cache --clean <name>       # remove one by name prefix; `all` removes every workspace's
```

The logs are kept; the next start prepares the modules again from a clean state.

## Tests

```bash
cd editors/clion
MCPPLS_PAYLOAD=/path/to/payload gradle verifyPlugin test
```

`verifyPlugin` runs the Plugin Verifier against CLion 2026.1 and 2026.2.3. `test` starts a headless
CLion 2026.2.3 with the IntelliJ Platform test framework and opens the projects in
`src/test/fixtures/`: an mcpp package, where it asserts that the server is Running within a minute,
that the wrong import gets mcppls's diagnostic, that `import hel` completes `hello.greet`, and that
closing the project ends the process; and a CMake project, which gets no server by default and gets
one, with the one question, when the setting is on; and in the mcpp project, that "mcppls only" sets
the open files to Syntax with mcppls's diagnostic still there, and that restoring puts each level back
unless the user changed it. What CLion's own engine answers for the same files is printed to the log.
`MCPPLS_PAYLOAD` names this machine's payload, so the sandbox holds this system's package with the
payload inside it, and the plugin starts that payload's server as its `payload.json` says (the runner
and the okb); the PATH the tests see has neither `mcppls` nor `mcpp`. CI runs `verifyPlugin` on every pull request and
the tests when the change reaches the plugin, the protocol or the server, and on release branches,
nightly and releases (`.github/workflows/ci.yml`, `clion-verify` and `clion-e2e`). The CLion
downloads are about 1.9 GB each and are cached by version.

### In a CLion with its engine running

Headless CLion's own engine does not answer in the tests (its completion and highlighting stay empty),
so what "mcppls only" does to it is checked by hand once per release, in CLion 2026.2 with Nova:

1. Open an mcpp project (`mcpp new hello`), open `src/main.cpp`, add `import missing.module;`. Answer
   *Keep both*: the wrong import is reported by mcppls and by CLion, and typing `std::` lists items of
   both engines.
2. Tools | mcppls | *Use mcppls Only in This Project*: the inspection widget shows Syntax, the import
   is reported once (mcppls's message), and the `std::` list holds mcppls's items and live templates.
3. Tools | mcppls | *Restore CLion's Engine in This Project*: the widget is back at All Problems and
   both engines answer again.
