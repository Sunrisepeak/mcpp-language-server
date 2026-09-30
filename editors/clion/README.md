# CLion plugin

C++20 named modules in CLion through mcpp-language-server: the plugin registers `mcppls` as a
language server for C and C++ using the IntelliJ platform's LSP API, which the paid IDEs have.

Like the Zed extension, it is deliberately small: its job is to start the server, and to stay out of
the way of CLion's own engine on the projects CLion models. Everything else happens in the server.

## Build

```bash
mcpp run --features clion -p devtools -- extension --editor clion
```

That runs `gradle buildPlugin` here and would leave a plugin archive in `build/distributions/`.
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

That builds the plugin, puts the server at `<user data>/mcppls/payload`, and unpacks the plugin
into the plugins directory of every CLion that has run on this machine — what **Install Plugin from
Disk** does — so restart CLion afterwards. `--clion-dir DIR` names one plugins directory instead;
`--plugin FILE` installs an archive you already have, such as a release's.

```bash
mcpp run -p devtools -- uninstall --editor clion
```

## Use

The plugin starts `mcppls` from the PATH the IDE sees, or else the one `--install` put in place.

CLion 2025.2 and later are supported (`sinceBuild` 252). The plugin is built against CLion 2026.2.3
and tested inside it; the Plugin Verifier checks it against 2025.2 as well.

## One engine per file

CLion has its own C/C++ engine, and two engines answering the same file means two lists of
completions and two sets of diagnostics. So a file is answered by one of them:

- a project CLion models itself -- a loaded CMake, compilation database or Makefile workspace, or a
  `CMakeLists.txt` at the project root -- is CLion's, and mcppls does not start for its files;
- every other project (mcpp, xmake, a plain folder of sources) is mcppls's. CLion's engine has
  little to say about those anyway: it does not know how they are built.

Settings | Tools | mcppls has one checkbox, **Also for projects CLion models**. With it on, mcppls
answers the first kind too, which is useful when the project's modules are what CLion cannot follow;
the plugin then says once per project that both engines are answering, and for module questions the
one to trust is mcppls. The setting applies to files opened afterwards.

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
MCPPLS_PAYLOAD_BIN=/path/to/payload/bin gradle verifyPlugin test
```

`verifyPlugin` runs the Plugin Verifier against CLion 2025.2 and 2026.2.3. `test` starts a headless
CLion 2026.2.3 with the IntelliJ Platform test framework and opens the projects in
`src/test/fixtures/`: an mcpp package, where it asserts that the server is Running within a minute,
that the wrong import gets mcppls's diagnostic, that `import hel` completes `hello.greet`, and that
closing the project ends the process; and a CMake project, which gets no server by default and gets
one, with one notice, when the setting is on. What CLion's own engine answers for the same files is
printed to the log. `MCPPLS_PAYLOAD_BIN` is put first on the PATH the tests see, the way the plugin
finds `mcppls`; `mcpp` is deliberately not on it. CI runs `verifyPlugin` on every pull request and
the tests when the change reaches the plugin, the protocol or the server, and on release branches,
nightly and releases (`.github/workflows/ci.yml`, `clion-verify` and `clion-e2e`). The CLion
downloads are about 1.9 GB each and are cached by version.
