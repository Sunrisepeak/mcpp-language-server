# CLion plugin

C++20 named modules in CLion through mcpp-language-server: the plugin registers `mcppls` as a
language server for C and C++ using the IntelliJ platform's LSP API, which the paid IDEs have.

Like the Zed extension, it is deliberately small — five files whose whole job is to start the
server. Everything else happens in the server.

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

CLion has its own C++ engine. This plugin adds a second one over the same files, which is useful
when the project's modules are what CLion cannot follow; if the two disagree, the one to trust for
module questions is this one.

## Keeping mcppls off for one project

The plugin has no settings of its own. To keep it off for one project, open Settings | Plugins,
find this plugin, and use the arrow beside its checkbox to disable it for the current project only
(the wording depends on the IDE version; an IDE without that choice can only disable the plugin for
every project). Reopen the project for it to take effect.

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

## Status

Built, and installed into a plugins directory by the tool; not yet loaded in a running CLion here
(no CLion on the machine it was written on). If it needs an adjustment for your CLion version, that
is expected, and worth an issue.
