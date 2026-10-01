# Zed extension

C++20 named modules in [Zed](https://zed.dev): navigation, completion and diagnostics that survive
a change of compiler, from mcpp-language-server.

The extension is deliberately small — it finds `mcppls` and starts it: the worktree's PATH first,
then the server `--install` puts at `<user data>/mcppls/payload` (`~/.local/share` on Linux,
`~/Library/Application Support` on macOS, `%LOCALAPPDATA%` on Windows). All the work is in the
server, which drives its own pinned clangd with the module database it built.

## Install

**1. The extension and the server.**

```bash
mcpp run -p devtools -- extension --editor zed --install
```

That compiles the crate to WebAssembly (`wasm32-wasip2`, into `extension.wasm`, which is what Zed
loads), assembles the server, puts it at `<user data>/mcppls/payload`, and names the one step left,
which is Zed's own and the recommended one: command palette → **zed: install dev extension** →
this directory. Zed compiles the extension itself for that, so it needs Rust installed through
rustup.

To skip the palette, add `--link`: it makes the same link the palette would,
`<Zed data>/extensions/installed/mcppls` → this directory, and a running Zed picks it up at once.
Where a link cannot be made (Windows without developer mode) it copies the two files instead.

**2. Put mcppls first and clangd off.** Open the settings (command palette → **zed: open
settings**) and add:

```json
{
  "languages": {
    "C++": { "language_servers": ["mcppls", "!clangd"] },
    "C":   { "language_servers": ["mcppls", "!clangd"] }
  }
}
```

Zed ships clangd for C and C++. Without this step it runs beside mcppls, two engines answering one
question, and diagnostics can appear twice. mcppls still works that way — CI opens a project in a
real Zed with Zed's default settings and checks that mcppls answers — but it starts clangd itself,
with a module database clangd would not otherwise have, so Zed's own copy adds nothing.

To remove it again:

```bash
mcpp run -p devtools -- uninstall --editor zed     # the extension, its work directory, and the
                                                   # server when CLion no longer uses it either
```

Removing it from Zed's Extensions view works as well.

## Keeping mcppls off for one project

Zed reads a project's `.zed/settings.json` over your own settings. Naming the server with a `!` turns it
off for that project's C and C++ files, and `"..."` keeps whatever else is enabled:

```json
{
  "languages": {
    "C++": { "language_servers": ["!mcppls", "clangd", "..."] },
    "C":   { "language_servers": ["!mcppls", "clangd", "..."] }
  }
}
```

Naming `clangd` here matters if your own settings turned it off in favour of mcppls (as above): a
project's list replaces yours, and without it the project would have no C++ server at all. Remove
the block to bring mcppls back.

## Resetting a workspace's cache

If preparation never finishes or clangd keeps crashing, a cache left by an earlier session may be the
cause. VS Code and Neovim have a command for it (the server does the deleting). A Zed extension
cannot add a command to the palette, so from a terminal, after closing Zed:

```bash
mcppls cache                      # the workspaces that have a cache, with their names and sizes
mcppls cache --clean <name>       # remove one by name prefix; `all` removes every workspace's
```

The logs are kept; the next start prepares the modules again from a clean state.

## Status

The extension builds, loads as a dev extension and starts the server; it is newer and less exercised than the VS Code one, which is
where the status bar, the diagnostic report and the conflict handling live. If something behaves
differently here, that is worth an issue.

It is tested in two layers. `cargo test` in this directory covers the choice of server (PATH, else
the installed payload, else the error that names the fix) on every pull request. `tests/smoke.sh`
runs a real Zed (the stable release pinned as `ZED_VERSION` in `.github/versions.env`, on Linux,
under xvfb with software Vulkan) on a project with the extension installed, once with the settings
of step 2 and once with Zed's defaults, and checks from the conversation that Zed initialized
mcppls, sent it the open file and received diagnostics, that Zed's own clangd is off or on as the
settings say, and that mcppls exits with Zed. `ZED_EDITOR=<zed>/libexec/zed-editor
MCPPLS_PAYLOAD=<payload> MCPPLS_DEVTOOLS=<mcppls-devtools> tests/smoke.sh recommended|default` runs
it locally against an X display.
